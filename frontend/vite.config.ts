import { defineConfig } from "vite";
import solid from "vite-plugin-solid";

// In dev the backend runs separately on :8080; proxy WebSocket + API to it.
const backend = process.env.BACKEND_URL ?? "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [solid()],
  server: {
    host: true, // reachable from your phone on the LAN while developing
    proxy: {
      "/ws": { target: backend, ws: true },
      "/api": { target: backend },
    },
  },
});
