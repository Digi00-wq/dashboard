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
