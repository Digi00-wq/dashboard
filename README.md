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

**Verlauf, Einstellungen, Anwesenheit** (Bearer oder Cookie, nur eigene Daten; Verlauf, Anwesenheit und Heimzone brauchen *iPhone-Daten*). „Tag“ = lokaler Kalendertag des Servers, Zeiten in Unix-ms:

- `GET /api/history/daily?days=1..365` (Standard 30) – pro Tag Schritte, Kalorien, Distanz, Training, Stockwerke, Ruhepuls, HRV, Puls min/max/Ø, Schlaf + Phasen (Nacht zählt zum Tag, an dem sie endet), Anzahl Workouts. iPhone + Watch werden wie in der Zusammenfassung nicht doppelt gezählt.
- `GET /api/history/heartrate?date=YYYY-MM-DD` (5-Minuten-Mittel), `/api/history/sleep?date=…` (Phasen der Nacht, beste Quelle), `/api/history/workouts?days=…`, `/api/history/locations?date=…` (max. 5000 Punkte + Besuche)
- `GET`/`PUT /api/prefs` (Benachrichtigungen, Automationen), `POST /api/presence` `{"event":"arrive"|"leave","t":ms}`, `GET /api/home`, `GET /api/briefing`, `GET /api/sleep-pattern`

## Automationen & Benachrichtigungen

Das Backend schickt Pushs über die Home-Assistant-Companion-App (`notify.<HA_NOTIFY_TARGET>`, leer = keine Pushs) und schaltet ein paar Dinge selbst. Was davon aktiv ist, stellt jeder in der iPhone-App unter *Einstellungen* ein (`/api/prefs`, Sprache Deutsch/Englisch); die Hausautomationen folgen den Einstellungen und Daten von `OWNER_USER` (leer = `COLLECT_DASHBOARD_USER`, sonst erster Admin).

| Was | Wann | Wie oft |
|---|---|---|
| „Licht brennt noch“ + Knopf **Licht aus** | Lampe an und `lights_idle_min` Minuten (Standard 30) keine Bewegung im Zimmer | einmal pro Fall, max. 1×/h |
| Server-Probleme | Festplatte > 90 %, RAM > 92 % (5 min), CPU > 80 °C (5 min), Internet weg (3 min), HA-Datenfehler (10 min) | Push beim Start + „wieder OK“, max. 1× pro Problem in 6 h |
| Morgen-Briefing (Tippen öffnet die App) | 04–12 Uhr, wenn du aufstehst: Schlaf-Fokus (`HA_FOCUS_ENTITY`) geht aus; sonst Präsenz im Zimmer ≥ 5 min nach dem Echo-Wecker bzw. ohne Wecker nach 05:30 | 1× pro Tag |
| Ankommen (iPhone-Region) | nach Sonnenuntergang (`sun.sun`) und alles aus → Licht 80 % | – |
| Weggehen | alle Lampen aus (nicht, wenn der Zimmersensor noch jemanden sieht), laufende Musik pausieren (nicht, wenn sie auf dem iPhone läuft) | nach 2 min ohne Rückkehr |
| *Experiment:* Aufwachlicht | 15 min vor deiner üblichen Aufwachzeit von 10 % hochdimmen – nur ohne Echo-Wecker in den nächsten 3 h, wenn jemand zuhause ist und das Licht aus ist | 1× pro Tag |
| *Experiment:* Schlafenszeit | 30 min vor deiner üblichen Schlafenszeit Push, Lampen die an sind auf 30 % | 1× pro Nacht |

- Der Knopf **Licht aus** kommt als HA-Event `mobile_app_notification_action` (Aktion `DASH_LIGHTS_OFF`) über die WebSocket-Verbindung zurück – keine HA-Automation nötig.
- Das Briefing (Schlaf, Wetter mit Hoch/Tief und Regenrisiko, fällige Aufgaben, Wecker, Server, Schritte/Ruhepuls/HRV, Tipps) wird alle 5 min neu berechnet: `GET /api/briefing`.
- **Schlafmuster:** aus den Schlafdaten der letzten 28 Nächte (HomeCollect, beste Quelle pro Nacht, Nächte < 3 h zählen nicht) der Median von Einschlaf- und Aufwachzeit, getrennt für Werktage (Mo–Fr morgens) und Wochenende, stündlich neu (`GET /api/sleep-pattern`, ab 3 Nächten). Prüfen ohne dass etwas passiert:
  ```bash
  docker compose exec dashboard dashboard_backend --sleep-pattern [user]
  ```
  zeigt das Muster und was Aufwachlicht, Schlafenszeit-Erinnerung und Briefing heute tun würden.
- Sicherheit: Ist Home Assistant nicht erreichbar, passiert nichts. „Schon gesendet“ steht in `DATA_DIR/automations_state.json`, ein Neustart schickt nichts doppelt. Jede Aktion steht im Log als `[Auto] …`.

## Backups

Der Server sichert jeden Tag um `BACKUP_HOUR` Uhr (Standard 3, lokale Zeit; nach dem Start zusätzlich einmal, falls es für heute noch keine Sicherung gibt) nach `BACKUP_DIR/YYYY-MM-DD/` (Standard `DATA_DIR/backups`, Ordner 0700, Dateien 0600):
`collect.db` (konsistente Kopie über die SQLite-Backup-API, auch während der Server schreibt), `users.json`, `prefs.json`. Behalten werden die neuesten `BACKUP_KEEP` Tage (Standard 14); gelöscht werden nur Ordner im Datumsformat. Sofort sichern: `dashboard_backend --backup-now` (Docker: `docker compose exec dashboard dashboard_backend --backup-now`).

**Wichtig:** Standardmässig liegen die Backups im selben Docker-Volume wie die Daten – gegen einen Plattendefekt hilft das nicht. Den Backup-Ordner regelmässig wegkopieren (zweite Platte, NAS, z.B. per `rsync`/Cron) oder in `docker-compose.yml` ein eigenes Volume/Bind-Mount für `BACKUP_DIR` eintragen (z.B. `- /mnt/nas/dashboard-backups:/backups` und `BACKUP_DIR=/backups`).

Wiederherstellen:

```bash
docker compose stop dashboard
# im Daten-Volume (DATA_DIR): alte DB inkl. WAL-Dateien weg, Sicherung zurück
rm -f collect.db collect.db-wal collect.db-shm
cp backups/2026-01-31/collect.db backups/2026-01-31/users.json backups/2026-01-31/prefs.json .
chmod 600 collect.db users.json prefs.json
docker compose start dashboard
```

Ohne `sessions.json` müssen sich alle im Web neu anmelden; die iPhone-App bleibt angemeldet (Geräte-Schlüssel sind in `collect.db`).

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
