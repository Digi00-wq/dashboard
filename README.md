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
