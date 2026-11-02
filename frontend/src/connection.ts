import { createSignal } from "solid-js";
import { Command, Dashboard, type DeepPartial } from "./proto/dashboard";
import { me, refreshMe } from "./auth";

export type Status = "connecting" | "online" | "offline";

const [dashboard, setDashboard] = createSignal<Dashboard | null>(null);
const [status, setStatus] = createSignal<Status>("connecting");
export { dashboard, status };

// Live data normally arrives over a WebSocket. Some networks (school /
// corporate wifi with a filtering proxy) block or silently break WebSockets,
// so if the socket can't deliver data we fall back to plain HTTP polling and
// keep retrying the socket in the background.
const POLL_MS = 3000;
const WS_RETRY_WHILE_POLLING_MS = 60000;
const WS_FIRST_DATA_TIMEOUT_MS = 8000;

let ws: WebSocket | null = null;
let retry = 0;
let timer: number | undefined;
let pollTimer: number | undefined;
let polling = false;
let pollInFlight = false;
let stopped = true;

function wsUrl(): string {
  const override = import.meta.env.VITE_WS_URL as string | undefined;
  if (override) return override;
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}/ws`;
}

function wsLive(): boolean {
  return !!ws && ws.readyState === WebSocket.OPEN;
}

function apply(buf: ArrayBuffer) {
  try {
    setDashboard(Dashboard.decode(new Uint8Array(buf)));
  } catch (err) {
    console.error("Protobuf decode failed", err);
  }
}

// ---- HTTP polling fallback -------------------------------------------------
async function pollOnce() {
  if (stopped || !polling || pollInFlight) return;
  clearTimeout(pollTimer);
  pollInFlight = true;
  try {
    const res = await fetch("/api/snapshot", { credentials: "same-origin", cache: "no-store" });
    if (stopped || !polling) return;
    if (res.status === 401) {
      pollInFlight = false;
      await refreshMe().catch(() => {}); // logs out -> disconnect()
      if (!stopped && polling) pollTimer = window.setTimeout(pollOnce, POLL_MS);
      return;
    }
    if (res.ok) {
      if (res.status !== 204) apply(await res.arrayBuffer());
      setStatus("online");
    } else {
      setStatus("offline");
    }
  } catch {
    if (!stopped && polling) setStatus("offline");
  } finally {
    pollInFlight = false;
  }
  if (!stopped && polling) pollTimer = window.setTimeout(pollOnce, POLL_MS);
}

function startPolling() {
  if (polling || stopped) return;
  console.warn("WebSocket unavailable, falling back to HTTP polling");
  polling = true;
  clearTimeout(pollTimer);
  void pollOnce();
}

function stopPolling() {
  polling = false;
  clearTimeout(pollTimer);
}

// ---- WebSocket ---------------------------------------------------------------
function scheduleReconnect() {
  const delay = polling
    ? WS_RETRY_WHILE_POLLING_MS
    : Math.min(15000, 1000 * 2 ** retry++); // 1s, 2s, 4s ... 15s
  clearTimeout(timer);
  timer = window.setTimeout(connect, delay);
}

export function connect() {
  stopped = false;
  clearTimeout(timer);
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

  if (!polling) setStatus("connecting");
  let sock: WebSocket;
  try {
    sock = new WebSocket(wsUrl());
  } catch {
    startPolling();
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
  }, WS_FIRST_DATA_TIMEOUT_MS);

  sock.onmessage = (ev: MessageEvent) => {
    if (!(ev.data instanceof ArrayBuffer)) return;
    if (!gotData) {
      gotData = true;
      retry = 0;
      stopPolling();
      setStatus("online");
    }
    apply(ev.data);
  };
  sock.onclose = () => {
    clearTimeout(watchdog);
    if (ws !== sock) return;
    ws = null;
    if (stopped) return;
    // Socket never delivered anything: the network likely blocks it.
    if (!gotData) startPolling();
    else if (!polling) setStatus("offline");
    // Closed because the session ended / permissions changed? Ask the server.
    refreshMe()
      .then((m) => {
        if (m && !stopped) {
          if (gotData) startPolling(); // bridge the gap until the socket is back
          scheduleReconnect();
        }
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
  if (!polling) return false;
  fetch("/api/command", {
    method: "POST",
    credentials: "same-origin",
    headers: { "Content-Type": "application/x-protobuf" },
    body: bytes,
  })
    .then(() => {
      // Pick up the result quickly instead of waiting for the next tick.
      clearTimeout(pollTimer);
      pollTimer = window.setTimeout(pollOnce, 400);
    })
    .catch(() => setStatus("offline"));
  return true;
}
