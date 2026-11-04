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

## Sprachsteuerung mit Alexa

Läuft über die *Alexa Media Player*-Integration: das Backend liest, was zum Echo gesagt wurde (`last_called_summary`), führt den Befehl aus und antwortet mit einer Amazon-Stimme (Standard: „Hans“).

| Sag „Alexa, …“ | Passiert |
|---|---|
| Licht an / Licht aus / Licht dimmen / Licht hell | alle Lampen |
| Serverstatus | Uptime, CPU, Temperatur, RAM, Disk, Internet |
| Guten Morgen | Morgen-Briefing |
| Was steht an | heute fällige Aufgaben |
| Wie habe ich geschlafen | Schlafdauer + Tiefschlaf |
| Gute Nacht | Licht aus, Musik pausieren, Wecker für morgen |

Das **Morgen-Briefing** kommt automatisch `BRIEFING_DELAY_MIN` Minuten nach dem Echo-Wecker (egal welche Uhrzeit), wenn der Präsenzsensor jemanden erkennt. Optional dimmt `WAKE_LIGHT_MIN` das Licht vor dem Wecker hoch.

**Einrichtung:** In der Alexa-App für jeden Satz eine Routine anlegen (*Wenn du sagst* → z.B. „Licht an“, *Aktion* → *Alexa sagt* → „Okay“ oder *Warten*). Sonst antwortet Alexa selbst mit „Ich kenne kein Gerät…“. Vorschau aller Antworten ohne Sprachausgabe: `dashboard_backend --voice-preview`. Nur auf **einer** Instanz aktivieren.

## iPhone-App (HomeCollect)

`ios/` enthält eine eigene iOS-App, die deine Daten dauerhaft auf den Server kopiert (Home Assistant behält nur ~10 Tage):

- **Health**: alle HealthKit-Samples inkl. kompletter Vergangenheit (Schritte, Puls, HRV, Schlafphasen, Workouts, VO2max, …), danach nur noch Neues, Gelöschtes wird auch gelöscht
- **Standortverlauf**: besuchte Orte + grössere Bewegungen, optional „Detailliertes Tracking“ (~alle 100 m)
- **Handy**: Akku, Speicher, Netzwerk (Bildschirmzeit gibt iOS Apps nicht frei)

Gespeichert wird in `DATA_DIR/collect.db` (SQLite, nur Prepared Statements, Datei 0600). Die App meldet sich einmal mit Benutzername + Passwort an und bekommt einen Geräte-Schlüssel (Keychain, Server speichert nur den Hash). Benutzer brauchen die Berechtigung **iPhone-Daten** (Admins haben sie immer). Geräte anzeigen/sperren: `dashboard_backend --list-devices` / `--revoke-device <id>`. Benutzer deaktivieren oder Berechtigung entziehen sperrt die App sofort.

Bauen (Mac mit Xcode):

```bash
brew install xcodegen
cd ios
# in project.yml PRODUCT_BUNDLE_IDENTIFIER auf etwas Eigenes setzen, z.B. ch.deinname.homecollect
xcodegen generate
open HomeCollect.xcodeproj
```

In Xcode: Target → *Signing & Capabilities* → Team = deine Apple-ID, iPhone per Kabel anschliessen, ▶ Run. Auf dem iPhone *Einstellungen → Datenschutz → Entwicklermodus* einschalten. Mit einer kostenlosen Apple-ID läuft die App 7 Tage, dann in Xcode neu installieren (bezahlter Account: 1 Jahr).

**Dashboard in der App:** Tab *Home* zeigt das Dashboard nativ (SwiftUI, iOS-Stil) mit Live-Updates: Licht (inkl. Helligkeit/Farbe), Spotify, Wetter, Gesundheit, Aufgaben, Zimmer, Server und Durchsagen. Es gelten dieselben Berechtigungen wie im Web; Familienmitglieder ohne *iPhone-Daten* sehen nur das Dashboard. Auf dem eigenen iPhone kommen Schritte, Puls und Kalorien direkt aus HealthKit.

**Weiter an Home Assistant:** Nach jedem Upload schreibt der Server eine Zusammenfassung nach HA (`HA_COLLECT_PUBLISH=1`):
`sensor.homecollect_<user>_steps`, `_active_energy`, `_distance`, `_exercise`, `_flights`, `_heart_rate`, `_resting_heart_rate`, `_hrv`, `_vo2_max`, `_sleep` (Phasen als Attribute), `_last_workout`, `_battery` und `device_tracker.homecollect_<user>` (home/not_home über `zone.home`). Damit gehen Automationen in HA. Diese Zustände kommen über die REST-API: nach einem HA-Neustart fehlen sie bis zum nächsten Upload. Das Web-Dashboard nimmt die Werte direkt (ohne Umweg über HA) und zeigt zusätzlich Ruhepuls, HRV, Kalorien und das letzte Training. Welche Person: `COLLECT_DASHBOARD_USER` (leer = letzter Upload).

API: `POST /api/devices/login`, `POST /api/collect` (Bearer, JSON, max. 8 MB), `GET /api/collect/status`, `POST /api/devices/logout`. Der Geräte-Schlüssel funktioniert auch für `/api/me`, `/api/snapshot?format=json`, `/api/events?format=json` und `POST /api/command` (JSON).

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

### Von überall erreichbar: `https://dashboard.task-list.ch`

Über einen **Cloudflare Tunnel**: keine Portfreigabe am Router, automatisches HTTPS, die Heim-IP bleibt verborgen.

1. Cloudflare → *Zero Trust* → *Networks* → *Tunnels* → **Create a tunnel** (Typ *Cloudflared*), Name z.B. `homeserver`, den **Token** kopieren.
2. Im Tunnel unter *Public hostnames* → **Add**: Subdomain `dashboard`, Domain `task-list.ch`, Service `HTTP` → `localhost:8090`.
3. In der `.env` auf dem Server:
   ```env
   TUNNEL_TOKEN=<token>
   BIND_ADDRESS=127.0.0.1   # nur noch über den Tunnel erreichbar
   COOKIE_SECURE=1          # Login-Cookie nur über HTTPS
   TRUST_PROXY=1            # echte Besucher-IPs für die Login-Sperre
   ```
4. `docker compose --profile tunnel up -d --build`

Danach ist das Dashboard nur noch über `https://dashboard.task-list.ch` erreichbar – auch zuhause.

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
