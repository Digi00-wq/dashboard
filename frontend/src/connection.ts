import { createSignal } from "solid-js";
import { Command, Dashboard, type DeepPartial } from "./proto/dashboard";

export type Status = "connecting" | "online" | "offline";

const [dashboard, setDashboard] = createSignal<Dashboard | null>(null);
const [status, setStatus] = createSignal<Status>("connecting");
export { dashboard, status };

let ws: WebSocket | null = null;
let retry = 0;
let timer: number | undefined;

function wsUrl(): string {
  const override = import.meta.env.VITE_WS_URL as string | undefined;
  if (override) return override;
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}/ws`;
}

export function connect() {
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
    // Exponential backoff: 1s, 2s, 4s ... max 15s
    const delay = Math.min(15000, 1000 * 2 ** retry++);
    timer = window.setTimeout(connect, delay);
  };
  sock.onerror = () => sock.close();
}

// Phones freeze background tabs and silently drop sockets: reconnect on return.
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible") {
    retry = 0;
    connect();
  }
});
window.addEventListener("online", () => connect());

export function send(cmd: DeepPartial<Command>): boolean {
  if (!ws || ws.readyState !== WebSocket.OPEN) return false;
  ws.send(Command.encode(Command.fromPartial(cmd)).finish());
  return true;
}
