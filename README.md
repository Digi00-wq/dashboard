# Dashboard

## features

- infos darstellen
  - Todoist tasks
  - Homelab/assitant infos
    - Wetter
    - Lampen
  - Kalender

Später mehr finden

## Genaue Daten

- Presense detector
  - If somebody is here and if not then the last time seen
- Light sensor
  - Is light in my room
- Lights
  - Current status and color
  - Maybe colorwheel to choose new color
- Weather
  - Current degrees
  - Wheater for the next days
  - In far future clorthings sugestions
- Phone
  - Steps
  - Heartrate
- Todoist
  - Today/next tasks

## UI

### Mind dump

- Dashboard
- grupierte aber viele infos
- keine oder nicht viel menu navigation

## Sprachen

### Backend

#### optionen

| Name | Forteil                        | Nachteil                                   |
| ---- | ------------------------------ | ------------------------------------------ |
| cpp  | rpc, schnell, hilfe für Google | Cpp lernen, libraries instalieren          |
| java | gelernt, einfacht, REST api    | Langsam, nicht flexibel, viel boilderplate |

#### Auswahl

C++ mit uWebSockets

Holt externe daten von andernen servisen

### Frontend

#### optionen

| Name    | Forteil                     | Nachteil                     |
| ------- | --------------------------- | ---------------------------- |
| React   | gelernt, einfach fürs setup | langsam, React/mui look, tsx |
| SolidJS | Schnell, einfach            | ...                          |

#### Auswahl

SolidJS framework

### Datatransfer Methoden

#### Optionen

| Name      | vorteil                                              | nachteil                                            |
| --------- | ---------------------------------------------------- | --------------------------------------------------- |
| JSON      | einfach, Brauch nur sich selber um zu rekonstruieren | Gross, nicht so schnell                             |
| Protobufs | sehr klein, schnell                                  | frontend braucht .proto files zur wiederherstellung |

#### Auswahl

Protobufs

### Datatransfer Transfehr art

#### Optionen

| Name              | vorteil                            | nachteil                       |
| ----------------- | ---------------------------------- | ------------------------------ |
| Websockets        | beidseitig, schell, binary support | Muss selber programiert werden |
| Server side event | Browser native, schnell, einfach   | Nur from server aus, nur UTF-8 |

#### Auswahl

Websockets

## Architektur

```
Home Assistant (REST) ─┐
                       ├─> C++ Backend (uWebSockets) ──protobuf/WebSocket──> SolidJS Frontend
/proc, /sys (Server) ──┘        └─ liefert auch das gebaute Frontend aus (ein Port)
```

- Backend pollt HA alle `POLL_INTERVAL` s, Server-Werte alle `SERVER_POLL_INTERVAL` s
  und schickt jedes Snapshot (`Dashboard`) an alle Clients.
- Frontend schickt `Command`s zurück: Lampe an/aus, Helligkeit, Farbe, Aufgabe erledigen.
  Nur Lampen aus `HA_LIGHT_ENTITIES` dürfen geschaltet werden.
- Todos kommen aus einer beliebigen HA To-do-Liste (z.B. Todoist-Integration).
- `GET /api/dashboard` zeigt das aktuelle Snapshot als JSON (Debug), `GET /healthz` für Healthchecks.

## Konten & Sicherheit

- Login ist Pflicht. Admins verwalten Benutzer im Dashboard (Kontomenü → *Benutzer verwalten*) und legen pro Person fest, was sie sehen/bedienen darf: Wetter, Licht, Zimmer, Aufgaben, Spotify, Gesundheit, Server.
- Das Backend schickt jedem nur die erlaubten Daten und lehnt Befehle ohne Berechtigung ab – die Prüfung passiert auf dem Server, nicht nur im UI.
- Ersten Admin anlegen bzw. Passwort zurücksetzen (Passwort wird verdeckt abgefragt):
  ```bash
  docker compose exec -it dashboard dashboard_backend --create-admin <name>
  docker compose exec -it dashboard dashboard_backend --set-password <name>
  # lokal: ./backend/build/dashboard_backend --create-admin <name>
  ```
- Gespeichert in `DATA_DIR` (Docker-Volume `dashboard-data`, lokal `./data`): `users.json` und `sessions.json`, beide mit Rechten `0600`.

| Schutz | Umsetzung |
|---|---|
| Passwörter | Argon2id (libsodium, 64 MiB), nie im Klartext gespeichert |
| Sessions | 256-Bit-Zufallstoken im `HttpOnly`-, `SameSite=Strict`-Cookie; serverseitig nur als SHA-256 gespeichert |
| Brute Force | Sperre pro IP und pro Konto nach 5 Fehlversuchen (1 → 60 min); gleiche Antwortzeit für unbekannte Benutzer |
| CSRF / WebSocket-Hijacking | Origin-Prüfung, nur `application/json`-Requests, SameSite-Cookie |
| XSS | Strikte Content-Security-Policy, keine Nutzer-HTML-Ausgabe, Cookie für JavaScript unsichtbar |
| Clickjacking | `frame-ancestors 'none'`, `X-Frame-Options: DENY` |
| SQL-Injection | Keine Datenbank/kein SQL – Speicherung als JSON |
| Sonstiges | Größenlimit für Requests, Path-Traversal-Schutz, Admin kann sich nicht selbst aussperren, Deaktivieren/Passwort-Reset beendet alle Sessions sofort |

Hinter einem HTTPS-Reverse-Proxy `COOKIE_SECURE=1` und `TRUST_PROXY=1` setzen.

## Konfiguration

`cp .env.example .env` und ausfüllen. Token: HA → Profil → Sicherheit → Langlebige Zugangstoken.
Entity-IDs findest du in HA unter Einstellungen → Geräte & Dienste → Entitäten.
Fehler (falsche Entity, falscher Token, …) erscheinen oben im Dashboard als gelbe Box.

## Auf dem Server laufen lassen

### Docker (empfohlen)

```bash
cp .env.example .env   # ausfüllen
docker compose up -d --build
```

Danach `http://<server-ip>:8080` auf Handy und Desktop öffnen.
Auf dem Handy "Zum Startbildschirm hinzufügen" → läuft wie eine App (Vollbild).

Von unterwegs: am besten über VPN (z.B. Tailscale/WireGuard) statt den Port ins Internet
zu öffnen – das Dashboard hat keinen Login und kann Lampen schalten.
Hinter einem Reverse-Proxy mit HTTPS funktioniert es automatisch (`wss://`).

### Ohne Docker (systemd)

Siehe Anleitung in `deploy/dashboard.service`.

## Entwicklung

Abhängigkeiten (Arch): `cmake protobuf curl openssl zlib nodejs npm`

```bash
# Backend bauen, starten und bei Änderungen automatisch neu bauen + neu starten
./scripts/dev-backend.sh

# Frontend mit Hot-Reload (proxied /ws und /api an localhost:8080)
cd frontend && npm install && npm run dev
```

Nach Änderungen an `proto/dashboard.proto`:

```bash
cd frontend && npm run proto   # TypeScript neu generieren
```

(Das Backend generiert seinen C++-Code beim Bauen selbst.)
