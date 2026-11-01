import { createSignal } from "solid-js";
import { Command, Dashboard, type DeepPartial } from "./proto/dashboard";
import { me, refreshMe } from "./auth";

export type Status = "connecting" | "online" | "offline";

const [dashboard, setDashboard] = createSignal<Dashboard | null>(null);
const [status, setStatus] = createSignal<Status>("connecting");
export { dashboard, status };

let ws: WebSocket | null = null;
let retry = 0;
let timer: number | undefined;
let stopped = true;

function wsUrl(): string {
  const override = import.meta.env.VITE_WS_URL as string | undefined;
  if (override) return override;
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}/ws`;
}

function scheduleReconnect() {
  const delay = Math.min(15000, 1000 * 2 ** retry++); // 1s, 2s, 4s ... 15s
  timer = window.setTimeout(connect, delay);
}

export function connect() {
  stopped = false;
  clearTimeout(timer);
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

  setStatus("connecting");
  const sock = new WebSocket(wsUrl());
  sock.binaryType = "arraybuffer";
  ws = sock;

  sock.onopen = () => {
    retry = 0;
    setStatus("online");
  };
  sock.onmessage = (ev: MessageEvent) => {
    if (!(ev.data instanceof ArrayBuffer)) return;
    try {
      setDashboard(Dashboard.decode(new Uint8Array(ev.data)));
    } catch (err) {
      console.error("Protobuf decode failed", err);
    }
  };
  sock.onclose = () => {
    if (ws !== sock) return;
    ws = null;
    setStatus("offline");
    if (stopped) return;
    // Closed because the session ended / permissions changed? Ask the server.
    refreshMe()
      .then((m) => {
        if (m && !stopped) scheduleReconnect();
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
  }
});
window.addEventListener("online", () => {
  if (me()) connect();
});

export function send(cmd: DeepPartial<Command>): boolean {
  if (!ws || ws.readyState !== WebSocket.OPEN) return false;
  ws.send(Command.encode(Command.fromPartial(cmd)).finish());
  return true;
}
