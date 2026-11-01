import { createSignal } from "solid-js";

// Per-device UI state that survives reloads.
const KEY = "dashboard.mediaMinimized";

function load(): boolean {
  try {
    return localStorage.getItem(KEY) === "1";
  } catch {
    return false;
  }
}

const [mediaMinimized, setMinimized] = createSignal(load());
export { mediaMinimized };

export function setMediaMinimized(v: boolean) {
  setMinimized(v);
  try {
    localStorage.setItem(KEY, v ? "1" : "0");
  } catch {
    /* not remembered */
  }
}
