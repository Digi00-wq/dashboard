import { createSignal } from "solid-js";
import { api, ApiError } from "./api";
import { disconnect } from "./connection";

export interface Me {
  name: string;
  displayName?: string;
  admin: boolean;
  permissions: string[];
}

// undefined = still checking, null = logged out
const [me, setMe] = createSignal<Me | null | undefined>(undefined);
const [setupNeeded, setSetupNeeded] = createSignal(false);
export { me, setupNeeded };

export const can = (perm: string) => !!me()?.permissions.includes(perm);

/** Re-reads the session. Throws only on network errors. */
export async function refreshMe(): Promise<Me | null> {
  try {
    const m = await api<Me>("GET", "/api/me");
    setMe(m);
    return m;
  } catch (e) {
    if (e instanceof ApiError && e.status === 401) {
      setSetupNeeded(e.data.setup === true);
      setMe(null);
      return null;
    }
    throw e;
  }
}

export async function login(username: string, password: string) {
  setMe(await api<Me>("POST", "/api/login", { username, password }));
}

export async function logout() {
  disconnect();
  try {
    await api("POST", "/api/logout");
  } finally {
    setMe(null);
  }
}
