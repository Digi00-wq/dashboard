# Dashboard frontend

SolidJS + Vite. See the root README for setup.

- `npm run dev` – dev server with hot reload, proxies `/ws` and `/api` to `BACKEND_URL` (default `http://127.0.0.1:8080`)
- `npm run build` – production build into `dist/` (served by the backend via `STATIC_DIR`)
- `npm run proto` – regenerate `src/proto/dashboard.ts` from `../proto/dashboard.proto`
