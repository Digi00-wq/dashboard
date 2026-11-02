import { createSignal } from "solid-js";
import { Command, Dashboard, type DeepPartial } from "./proto/dashboard";
import { me, refreshMe } from "./auth";

export type Status = "connecting" | "online" | "offline";

const [dashboard, setDashboard] = createSignal<Dashboard | null>(null);
const [status, setStatus] = createSignal<Status>("connecting");
export { dashboard, status };

// Live data normally arrives over a WebSocket. Some networks (e.g. school
// wifi with a TLS-inspecting firewall) break WebSockets while ordinary HTTPS
// still works, so we fall back in this order:
//   1. WebSocket        /ws           push, bidirectional
//   2. Server-Sent Events /api/events  push over plain HTTPS (rides HTTP/3)
//   3. Polling          /api/snapshot every few seconds
// Commands go over the socket when it is up, otherwise POST /api/command.
// The WebSocket keeps being retried in the background.
const FIRST_DATA_TIMEOUT_MS = 8000;
const POLL_MS = 3000;
const WS_RETRY_IN_FALLBACK_MS = 60000;

let ws: WebSocket | null = null;
let sse: EventSource | null = null;
let sseLive = false;
let polling = false;
let pollInFlight = false;
let retry = 0;
let timer: number | undefined;
let pollTimer: number | undefined;
let stopped = true;

function wsUrl(): string {
  const override = import.meta.env.VITE_WS_URL as string | undefined;
  if (override) return override;
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}/ws`;
}

const wsLive = () => !!ws && ws.readyState === WebSocket.OPEN;
const inFallback = () => !!sse || polling;

function apply(bytes: Uint8Array) {
  try {
    setDashboard(Dashboard.decode(bytes));
    setStatus("online");
  } catch (err) {
    console.error("Protobuf decode failed", err);
  }
}

function fromBase64(s: string): Uint8Array {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// ---- 3. Polling ---------------------------------------------------------------
async function pollOnce() {
  if (stopped || !polling || pollInFlight) return;
  clearTimeout(pollTimer);
  pollInFlight = true;
  let loggedOut = false;
  try {
    const res = await fetch("/api/snapshot", { credentials: "same-origin", cache: "no-store" });
    if (res.status === 401) loggedOut = true;
    else if (res.status === 200) apply(new Uint8Array(await res.arrayBuffer()));
    else if (res.status === 204) setStatus("online"); // server has no data yet
    else setStatus("offline");
  } catch {
    setStatus("offline");
  } finally {
    pollInFlight = false;
  }
  if (loggedOut) await refreshMe().catch(() => {}); // logged out -> disconnect()
  if (!stopped && polling) pollTimer = window.setTimeout(pollOnce, POLL_MS);
}

function startPolling() {
  if (polling || stopped) return;
  console.warn("Live push unavailable, polling over HTTP");
  polling = true;
  void pollOnce();
}

function stopPolling() {
  polling = false;
  clearTimeout(pollTimer);
}

// ---- 2. Server-Sent Events ----------------------------------------------------
function startSse() {
  if (sse || stopped) return;
  console.warn("WebSocket unavailable, using Server-Sent Events");
  const es = new EventSource("/api/events");
  sse = es;
  sseLive = false;
  let gotData = false;

  // Some proxies buffer streamed responses: no data in time -> poll instead.
  const watchdog = window.setTimeout(() => {
    if (sse === es && !gotData) {
      stopSse();
      startPolling();
    }
  }, FIRST_DATA_TIMEOUT_MS);

  es.onmessage = (ev: MessageEvent<string>) => {
    if (sse !== es) return;
    if (!gotData) {
      gotData = true;
      clearTimeout(watchdog);
    }
    sseLive = true;
    stopPolling();
    apply(fromBase64(ev.data));
  };
  es.onerror = () => {
    if (sse !== es) return;
    sseLive = false;
    if (!gotData || es.readyState === EventSource.CLOSED) {
      // Never worked, or the server refused it (e.g. 401): give up on SSE.
      clearTimeout(watchdog);
      stopSse();
    }
    // Bridge with polling while EventSource reconnects (it stops on new data).
    startPolling();
    // Session ended / permissions changed? Re-read them.
    refreshMe().catch(() => {});
  };
}

function stopSse() {
  sse?.close();
  sse = null;
  sseLive = false;
}

// ---- 1. WebSocket -------------------------------------------------------------
function scheduleReconnect() {
  const delay = inFallback() ? WS_RETRY_IN_FALLBACK_MS : Math.min(15000, 1000 * 2 ** retry++);
  clearTimeout(timer);
  timer = window.setTimeout(connect, delay);
}

function startFallback() {
  if (!sse) startSse();
}

export function connect() {
  stopped = false;
  clearTimeout(timer);
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

  if (!inFallback()) setStatus("connecting");
  let sock: WebSocket;
  try {
    sock = new WebSocket(wsUrl());
  } catch {
    startFallback();
    scheduleReconnect();
    return;
  }
  sock.binaryType = "arraybuffer";
  ws = sock;
  let gotData = false;

  // A proxy may accept the upgrade but never pass frames through. The server
  // sends a snapshot right after connecting, so no data = broken socket.
  const watchdog = window.setTimeout(() => {
    if (ws === sock && !gotData) sock.close();
  }, FIRST_DATA_TIMEOUT_MS);

  sock.onmessage = (ev: MessageEvent) => {
    if (!(ev.data instanceof ArrayBuffer)) return;
    if (!gotData) {
      gotData = true;
      retry = 0;
      // The socket works (again): drop the fallbacks.
      stopSse();
      stopPolling();
    }
    apply(new Uint8Array(ev.data));
  };
  sock.onclose = () => {
    clearTimeout(watchdog);
    if (ws !== sock) return;
    ws = null;
    if (stopped) return;
    if (gotData) setStatus("offline");
    // Closed because the session ended / permissions changed? Ask the server.
    refreshMe()
      .then((m) => {
        if (!m || stopped) return;
        // Never delivered anything: the network likely blocks WebSockets.
        if (!gotData) startFallback();
        scheduleReconnect();
      })
      .catch(() => {
        if (!stopped) scheduleReconnect(); // server unreachable: keep trying
      });
  };
  sock.onerror = () => sock.close();
}

export function disconnect() {
  stopped = true;
  clearTimeout(timer);
  stopSse();
  stopPolling();
  const sock = ws;
  ws = null;
  sock?.close();
  setDashboard(null);
}

// Phones freeze background tabs and silently drop sockets: reconnect on return.
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible" && me()) {
    retry = 0;
    connect();
    if (polling) void pollOnce();
  }
});
window.addEventListener("online", () => {
  if (me()) connect();
});

export function send(cmd: DeepPartial<Command>): boolean {
  const bytes = Command.encode(Command.fromPartial(cmd)).finish();
  if (wsLive()) {
    ws!.send(bytes);
    return true;
  }
  if (!inFallback()) return false;
  fetch("/api/command", {
    method: "POST",
    credentials: "same-origin",
    headers: { "Content-Type": "application/x-protobuf" },
    body: bytes,
  })
    .then((res) => {
      if (res.status === 401) return void refreshMe().catch(() => {});
      // SSE pushes the result by itself; when polling, fetch it right away.
      if (polling && !sseLive) {
        clearTimeout(pollTimer);
        pollTimer = window.setTimeout(pollOnce, 400);
      }
    })
    .catch(() => setStatus("offline"));
  return true;
}
