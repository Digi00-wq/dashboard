import { createSignal } from "solid-js";

export type Lang = "de" | "en";
const STORAGE_KEY = "dashboard.lang";

function initialLang(): Lang {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved === "de" || saved === "en") return saved;
  } catch {
    /* storage unavailable */
  }
  return navigator.language?.toLowerCase().startsWith("de") ? "de" : "en";
}

const [lang, setLangSignal] = createSignal<Lang>(initialLang());
document.documentElement.lang = lang();
export { lang };

export function setLang(l: Lang) {
  setLangSignal(l);
  document.documentElement.lang = l;
  try {
    localStorage.setItem(STORAGE_KEY, l);
  } catch {
    /* selection just isn't remembered */
  }
}

/** BCP-47 locale for dates/numbers. en-GB keeps the 24 h clock. */
export const locale = () => (lang() === "de" ? "de-DE" : "en-GB");

// Every entry has the same shape in both languages (checked by `satisfies`).
const dict = {
  // header
  greetingNight: { de: "Gute Nacht", en: "Good night" },
  greetingMorning: { de: "Guten Morgen", en: "Good morning" },
  greetingDay: { de: "Guten Tag", en: "Good afternoon" },
  greetingEvening: { de: "Guten Abend", en: "Good evening" },
  connected: { de: "Verbunden", en: "Connected" },
  connecting: { de: "Verbinde", en: "Connecting" },
  offline: { de: "Offline", en: "Offline" },
  stale: { de: "Veraltet", en: "Stale" },
  waiting: { de: "Warte auf Daten vom Server", en: "Waiting for data from the server" },
  language: { de: "Sprache", en: "Language" },
  sourceErrors: {
    de: (n: number) => (n === 1 ? "1 Datenquelle meldet einen Fehler" : `${n} Datenquellen melden Fehler`),
    en: (n: number) => (n === 1 ? "1 data source reports an error" : `${n} data sources report errors`),
  },

  // glance
  nextAlarm: { de: "Nächster Wecker", en: "Next alarm" },
  internet: { de: "Internet", en: "Internet" },
  internetOffline: { de: "Internet offline", en: "Internet offline" },
  online: { de: "Online", en: "Online" },
  previous: { de: "Zurück", en: "Previous" },
  next: { de: "Weiter", en: "Next" },
  play: { de: "Abspielen", en: "Play" },
  pause: { de: "Pause", en: "Pause" },
  mediaOff: { de: "Aus", en: "Off" },
  mediaReady: { de: "Bereit", en: "Ready" },
  nowPlaying: { de: "Läuft gerade", en: "Now playing" },
  nothingPlaying: { de: "Nichts wird abgespielt", en: "Nothing playing" },
  minimize: { de: "Minimieren", en: "Minimize" },
  expand: { de: "Aufklappen", en: "Expand" },
  volume: { de: "Lautstärke", en: "Volume" },
  shuffle: { de: "Zufallswiedergabe", en: "Shuffle" },
  playbackDevice: { de: "Abspielgerät", en: "Playback device" },

  // weather
  weather: { de: "Wetter", en: "Weather" },
  noWeather: { de: "Keine Wetter-Entität konfiguriert", en: "No weather entity configured" },
  now: { de: "Jetzt", en: "Now" },
  today: { de: "Heute", en: "Today" },
  tomorrow: { de: "Morgen", en: "Tomorrow" },
  yesterday: { de: "Gestern", en: "Yesterday" },
  umbrella: { de: "Regenschirm", en: "Umbrella" },
  outfitFreezing: { de: "Winterjacke, Mütze und Handschuhe", en: "Winter coat, hat and gloves" },
  outfitCold: { de: "Warme Jacke", en: "Warm jacket" },
  outfitCool: { de: "Leichte Jacke oder Pullover", en: "Light jacket or sweater" },
  outfitMild: { de: "Langarmshirt oder T-Shirt", en: "Long sleeves or T-shirt" },
  outfitWarm: { de: "T-Shirt und kurze Hose", en: "T-shirt and shorts" },
  "w:clear-night": { de: "Klar", en: "Clear" },
  "w:cloudy": { de: "Bewölkt", en: "Cloudy" },
  "w:exceptional": { de: "Unwetter", en: "Severe weather" },
  "w:fog": { de: "Nebel", en: "Fog" },
  "w:hail": { de: "Hagel", en: "Hail" },
  "w:lightning": { de: "Gewitter", en: "Thunderstorm" },
  "w:lightning-rainy": { de: "Gewitter mit Regen", en: "Thunderstorm with rain" },
  "w:partlycloudy": { de: "Teilweise bewölkt", en: "Partly cloudy" },
  "w:pouring": { de: "Starkregen", en: "Heavy rain" },
  "w:rainy": { de: "Regen", en: "Rain" },
  "w:snowy": { de: "Schnee", en: "Snow" },
  "w:snowy-rainy": { de: "Schneeregen", en: "Sleet" },
  "w:sunny": { de: "Sonnig", en: "Sunny" },
  "w:windy": { de: "Windig", en: "Windy" },
  "w:windy-variant": { de: "Windig", en: "Windy" },

  // tasks
  tasks: { de: "Aufgaben", en: "Tasks" },
  noTodo: { de: "Keine To-do-Liste konfiguriert", en: "No to-do list configured" },
  todoList: { de: "To-do-Liste", en: "To-do list" },
  allDone: { de: "Alles erledigt", en: "All done" },
  completeTask: { de: (s: string) => `${s} erledigen`, en: (s: string) => `Complete ${s}` },

  // lights
  lights: { de: "Licht", en: "Lights" },
  noLights: { de: "Keine Lampen konfiguriert", en: "No lights configured" },
  allOff: { de: "Alle aus", en: "All off" },
  allOn: { de: "Alle an", en: "All on" },
  someOn: {
    de: (on: number, total: number) => `${on} von ${total} an`,
    en: (on: number, total: number) => `${on} of ${total} on`,
  },
  switchAll: { de: "Alle Lampen schalten", en: "Switch all lights" },
  switchLamp: { de: (n: string) => `${n} schalten`, en: (n: string) => `Switch ${n}` },
  unavailable: { de: "Nicht erreichbar", en: "Unavailable" },
  on: { de: "An", en: "On" },
  off: { de: "Aus", en: "Off" },
  pickColor: { de: "Farbe wählen", en: "Pick colour" },
  brightness: { de: "Helligkeit", en: "Brightness" },

  // room
  room: { de: "Zimmer", en: "Room" },
  noSensors: { de: "Keine Sensoren konfiguriert", en: "No sensors configured" },
  present: { de: "Anwesend", en: "Present" },
  away: { de: "Abwesend", en: "Away" },
  someoneHere: { de: "Jemand ist im Raum", en: "Someone is in the room" },
  lastSeen: { de: (ago: string) => `Zuletzt gesehen ${ago}`, en: (ago: string) => `Last seen ${ago}` },
  lastSeenUnknown: { de: "Zuletzt gesehen unbekannt", en: "Last seen unknown" },
  bright: { de: "Hell", en: "Bright" },
  dark: { de: "Dunkel", en: "Dark" },
  lightSensor: { de: "Lichtsensor", en: "Light sensor" },
  justNow: { de: "gerade eben", en: "just now" },
  minutesAgo: { de: (n: number) => `vor ${n} min`, en: (n: number) => `${n} min ago` },
  hoursAgo: { de: (n: number) => `vor ${n} h`, en: (n: number) => `${n} h ago` },
  daysAgo: {
    de: (n: number) => (n === 1 ? "vor 1 Tag" : `vor ${n} Tagen`),
    en: (n: number) => (n === 1 ? "1 day ago" : `${n} days ago`),
  },

  // health
  health: { de: "Gesundheit & Handy", en: "Health & phone" },
  noPhone: { de: "Keine Handy-Sensoren konfiguriert", en: "No phone sensors configured" },
  steps: { de: "Schritte", en: "Steps" },
  heartRate: { de: "Puls (bpm)", en: "Heart rate (bpm)" },
  battery: { de: "Akku", en: "Battery" },
  sleepLastNight: { de: "Schlaf letzte Nacht", en: "Sleep last night" },
  sleepPhases: { de: "Schlafphasen", en: "Sleep phases" },
  deep: { de: "Tief", en: "Deep" },
  core: { de: "Kern", en: "Core" },
  rem: { de: "REM", en: "REM" },
  awake: { de: "Wach", en: "Awake" },

  // server
  server: { de: "Server", en: "Server" },
  noData: { de: "Keine Daten", en: "No data" },
  cpu: { de: "CPU", en: "CPU" },
  memory: { de: "Arbeitsspeicher", en: "Memory" },
  disk: { de: "Speicher", en: "Disk" },
  of: { de: "von", en: "of" },
  dayShort: { de: "T", en: "d" },

  // login & account
  appName: { de: "Dashboard", en: "Dashboard" },
  signInTitle: { de: "Anmelden", en: "Sign in" },
  username: { de: "Benutzername", en: "Username" },
  password: { de: "Passwort", en: "Password" },
  signIn: { de: "Anmelden", en: "Sign in" },
  showPassword: { de: "Passwort anzeigen", en: "Show password" },
  account: { de: "Konto", en: "Account" },
  logout: { de: "Abmelden", en: "Sign out" },
  changePassword: { de: "Passwort ändern", en: "Change password" },
  currentPassword: { de: "Aktuelles Passwort", en: "Current password" },
  newPassword: { de: "Neues Passwort", en: "New password" },
  repeatPassword: { de: "Neues Passwort wiederholen", en: "Repeat new password" },
  passwordHint: { de: "Mindestens 8 Zeichen", en: "At least 8 characters" },
  save: { de: "Speichern", en: "Save" },
  cancel: { de: "Abbrechen", en: "Cancel" },
  setupTitle: { de: "Noch kein Konto eingerichtet", en: "No account set up yet" },
  setupText: {
    de: "Erstelle zuerst dein Admin-Konto auf dem Server:",
    en: "First create your admin account on the server:",
  },

  // user management
  manageUsers: { de: "Benutzer verwalten", en: "Manage users" },
  back: { de: "Zurück", en: "Back" },
  newUser: { de: "Neuer Benutzer", en: "New user" },
  create: { de: "Erstellen", en: "Create" },
  admin: { de: "Admin", en: "Admin" },
  makeAdmin: { de: "Zum Admin machen", en: "Make admin" },
  removeAdmin: { de: "Admin-Recht entfernen", en: "Remove admin" },
  adminHint: { de: "Darf alles sehen und Benutzer verwalten", en: "Can see everything and manage users" },
  you: { de: "Du", en: "You" },
  active: { de: "Aktiv", en: "Active" },
  disabledUser: { de: "Deaktiviert", en: "Disabled" },
  access: { de: "Zugriff", en: "Access" },
  allAccess: { de: "Voller Zugriff", en: "Full access" },
  resetPassword: { de: "Passwort zurücksetzen", en: "Reset password" },
  resetPasswordFor: { de: (n: string) => `Neues Passwort für ${n}`, en: (n: string) => `New password for ${n}` },
  deleteUser: { de: "Benutzer löschen", en: "Delete user" },
  confirmDelete: {
    de: (n: string) => `${n} wirklich löschen? Das kann nicht rückgängig gemacht werden.`,
    en: (n: string) => `Really delete ${n}? This cannot be undone.`,
  },
  delete: { de: "Löschen", en: "Delete" },
  saved: { de: "Gespeichert", en: "Saved" },
  "perm:weather": { de: "Wetter", en: "Weather" },
  "perm:lights": { de: "Licht", en: "Lights" },
  "perm:room": { de: "Zimmer", en: "Room" },
  "perm:tasks": { de: "Aufgaben", en: "Tasks" },
  "perm:media": { de: "Spotify", en: "Spotify" },
  "perm:health": { de: "Gesundheit", en: "Health" },
  "perm:server": { de: "Server", en: "Server" },

  // API errors
  "err:invalid_credentials": { de: "Benutzername oder Passwort falsch", en: "Wrong username or password" },
  "err:too_many_attempts": {
    de: "Zu viele Fehlversuche. Bitte später erneut versuchen.",
    en: "Too many failed attempts. Please try again later.",
  },
  "err:weak_password": { de: "Passwort zu kurz (mindestens 8 Zeichen)", en: "Password too short (at least 8 characters)" },
  "err:wrong_password": { de: "Aktuelles Passwort ist falsch", en: "Current password is wrong" },
  "err:mismatch": { de: "Passwörter stimmen nicht überein", en: "Passwords do not match" },
  "err:user_exists": { de: "Diesen Benutzer gibt es schon", en: "This user already exists" },
  "err:invalid_username": {
    de: "Nur Kleinbuchstaben, Ziffern und _ . - (max. 32)",
    en: "Only lowercase letters, digits and _ . - (max 32)",
  },
  "err:cannot_lock_out_self": {
    de: "Du kannst dich nicht selbst deaktivieren oder dein Admin-Recht entfernen",
    en: "You can't disable yourself or remove your own admin rights",
  },
  "err:cannot_delete_self": { de: "Du kannst dich nicht selbst löschen", en: "You can't delete yourself" },
  "err:last_admin": { de: "Es muss mindestens einen Admin geben", en: "There must be at least one admin" },
  "err:busy": { de: "Server ist beschäftigt, bitte erneut versuchen", en: "Server is busy, please try again" },
  "err:network": { de: "Server nicht erreichbar", en: "Server not reachable" },
  "err:generic": { de: "Etwas ist schiefgelaufen", en: "Something went wrong" },
} satisfies Record<string, { de: unknown; en: unknown }>;

type Dict = typeof dict;
export type Key = keyof Dict;

/** Translation for the current language (reactive: re-renders on switch). */
export function t<K extends Key>(key: K): Dict[K]["de"] {
  return dict[key][lang()] as Dict[K]["de"];
}

/** Translate a HA weather condition, falling back to the raw value. */
export function weatherText(condition: string): string {
  const key = `w:${condition}` as Key;
  return key in dict ? (t(key) as string) : condition;
}

/** Human readable text for an API error code. */
export function errorText(code: string): string {
  const key = `err:${code}` as Key;
  return key in dict ? (t(key) as string) : (t("err:generic") as string);
}

export const permText = (perm: string): string => {
  const key = `perm:${perm}` as Key;
  return key in dict ? (t(key) as string) : perm;
};
