# dashboard — LLM context

## RULES (owner's instructions — always follow)
1. Commit every change, authored as the owner only (git config user = Server diego <dj2010@proton.me>). NO Co-Authored-By / Claude / "Generated with" lines in commit messages.
2. If backend/, frontend/, proto/, Dockerfile, docker-compose.yml or .env changed: deploy on this server right after (`docker compose --profile tunnel up -d --build`, or `docker compose up -d` for .env only), then check `docker ps` health + `docker logs dashboard`.
3. ios/ changes: do NOT build/deploy. Tell the owner what changed so they rebuild in Xcode on their Mac.
4. Every source file starts with a header comment (what it does / how it connects); keep it accurate when changing a file.

Home dashboard + iOS app for one household. Home Assistant (HA) → C++ backend → SolidJS web + SwiftUI iOS app. Human docs: README.md (German, long — read only the section you need). UI language: German (en via i18n).

## Runtime (prod, this host)
- Docker container `dashboard` (network_mode host, user `nobody`, TZ Europe/Zurich) + `dashboard-tunnel` (cloudflared) → https://dashboard.task-list.ch → 127.0.0.1:8090.
- Deploy code changes: `docker compose --profile tunnel up -d --build` (or `/srv/projekte/update.sh dashboard` = git pull + that).
- `.env` change only: `docker compose up -d` (recreates; `restart` does NOT reload env). Backup .env first to `/srv/projekte/dashboard-backups/`.
- Data: volume `dashboard_dashboard-data` at `/data`: users.json, sessions.json, prefs.json, notifications.json, automations_state.json, collect.db (SQLite, big), backups/YYYY-MM-DD/{collect.db.gz,users.json,prefs.json} (7 days, ~120 MB each; restore: gunzip -c collect.db.gz > collect.db). Inspect: `docker exec dashboard ls /data`.
- Logs: `docker logs dashboard` (prefixes `[Config] [Auth] [HA-WS] [Auto] …`). Health: `GET /healthz`.
- CLI (in container: `docker compose exec dashboard dashboard_backend …`): `--create-admin|--set-password|--set-name <user> "<name>"|--list-users|--list-devices|--revoke-device <id>|--voice-preview|--backup-now|--sleep-pattern [user]`.
- Host service `claude-bridge` (systemd, root, 127.0.0.1:8091, `scripts/claude-bridge.py`) runs `claude -p` for the iOS "Claude" tab; reads same .env.
- Git: origin github.com:Digi00-wq/dashboard, branch main. `.env` is gitignored.

## Config = env vars only (`.env`, template `.env.example`, parsed in backend/src/config.cpp)
All HA entities are referenced ONLY here, never hardcoded in code. Names shown in UI come live from HA `friendly_name`.
- Lights: `HA_LIGHT_ENTITIES` (comma list; also the whitelist for switching). Current: light.bedroom_middle, light.bedroom_door, light.bedroom_pc.
- Others: HA_PRESENCE/LUX/WEATHER/STEPS/HEART_RATE/BATTERY/ALARM/WAN/DOWNLOAD/UPLOAD/FOCUS_ENTITY, HA_TODO_ENTITIES, HA_MEDIA_ENTITIES, HA_SLEEP_PREFIX, HA_ALEXA_ENTITY/NOTIFY, HA_NOTIFY_TARGET.
- If HA entity IDs are renamed: query `GET $HA_URL/api/states` (Bearer $HA_TOKEN from .env), update .env, `docker compose up -d`. Nothing else stores entity IDs.
- Never print/commit HA_TOKEN, TUNNEL_TOKEN, CLAUDE_BRIDGE_TOKEN.

## Architecture
```
HA REST + HA WebSocket(events) ─┐
/proc,/sys (host via /host ro) ─┼─> worker thread ──snapshot(proto Dashboard)──> WebServer (uWebSockets) ──> web / iOS
iOS HomeCollect uploads ────────┘     ↑ CommandQueue (Command from clients)          │ per-user filtered by perms
```
Threads (main.cpp): uWS loop (WebServer), worker (HA poll every POLL_INTERVAL, sys stats, automations, voice), HaEventStream (HA WS → refresh), backup thread. Hand-offs: CommandQueue, AutomationBus, CollectedState, CoverCache, PrefsStore (thread-safe). HomeAssistant class = worker thread only.
Transport fallback (web): WS `/ws` → SSE `/api/events` → poll `/api/snapshot`; commands via WS or `POST /api/command`.

## Code map (lines)
backend/src (C++17, CMake; deps: protobuf, curl, openssl, zlib, libsodium, sqlite3; FetchContent uWebSockets/uSockets, nlohmann_json):
- main.cpp 502 — CLI flags, thread startup, worker loop
- web.cpp 1554 / web.h — all HTTP routes, WS, auth checks, perm filtering
- collector.cpp 1131 — HomeCollect SQLite (health, locations, history APIs)
- automations.cpp 1107 — notifications, lights-idle, briefing, arrive/leave, sleep pattern, wake light
- home_assistant.cpp 712 — REST: fill snapshot, light/media/todo commands (rejects lights not in whitelist)
- auth.cpp 385 — users, Argon2id, sessions, rate limit; Perm bitmask in auth.h
- voice.cpp 336 — Alexa Media Player commands + briefing speech
- ha_events.cpp, backup.cpp, prefs.cpp, system_stats.cpp, config.cpp, http_client.cpp; notification_inbox.h, cover_cache.h, automation_bus.h
proto/dashboard.proto — Dashboard snapshot (Lamp{entity_id,name,…}, Weather, Glance, Media, Todoist, Server, Phone, Activity…) + Command (LightCommand, MediaCommand, CompleteTask, Announce, refresh). After edit: `cd frontend && npm run proto` (C++ generated at build).
frontend/src (SolidJS + Vite + TS): App.tsx, connection.ts (transport), auth.ts, api.ts, i18n.ts, format.ts; components/: Lamps, Room, WeatherCard, Tasks, MediaCard, PhoneCard, ServerCard, Glance, Controls, Announce, Admin (user mgmt), AccountMenu, Login. Generated: src/proto/dashboard.ts (don't edit).
ios/ (XcodeGen project.yml, iOS 17, app "HomeCollect", built on Mac): HomeCollect/{Dashboard (native live dashboard), History, Settings, Claude, Briefing, Notifications}, HealthSync, LocationTracker, SyncEngine, APIClient; HomeCollectWidgets (Control Center all-lights toggle); Shared/{LightsAPI, SharedStore, WidgetRefresher}. JSON APIs, Bearer device key.

## Auth / perms
Login required. Perms (auth.h): WEATHER LIGHTS ROOM TASKS MEDIA HEALTH SERVER ANNOUNCE COLLECT; admin = all + ADMIN_VIEW. Server filters snapshot + rejects commands per user. Cookie session (web) or device Bearer key (iOS, hash in collect.db). OWNER_USER drives home automations.

## API (web.cpp)
Auth: POST /api/login|logout|me/password, GET /api/me; users (admin): GET/POST /api/users, PUT/DELETE /api/users/:name.
Live: /ws, GET /api/events, /api/snapshot, /api/dashboard (debug JSON), POST /api/command, GET /api/cover/<entity>.
iOS: POST /api/devices/login|logout, POST /api/collect, GET /api/collect/status, GET /api/history/{daily,heartrate,sleep,workouts,locations,places,users}[?user= admin], GET/PUT /api/prefs, POST /api/presence, GET /api/home|briefing|sleep-pattern, notifications: GET /api/notifications[?after=], /stream (SSE), POST /action|/test. Claude: GET /api/claude/status, POST /api/claude/jobs, GET /api/claude/jobs/:id, POST …/cancel.

## Dev
`./scripts/dev-backend.sh` (build+run+watch, uses .env) · `cd frontend && npm run dev` (proxies /ws,/api to :8080) · `npm run build`. No test suite. Keep comments/README style: English code comments, German README.
