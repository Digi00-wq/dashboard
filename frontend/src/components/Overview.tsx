// Admin page "System overview": everything the dashboard does in one place.
// Data: GET /api/admin/overview (refreshed every 15 s while open). Sections:
// status (home/away/work), notification delivery (live / fetched / HA push per
// notification + app connections, test button), dashboard automations with
// their switches (PUT /api/prefs, owner only) and last run, Home Assistant's own
// automations, history, configuration (.env, secrets hidden), devices, backups.
// Texts are inline per language (L(de, en)) because they only appear here.

import { createResource, createSignal, For, onCleanup, onMount, Show } from "solid-js";
import { api, ApiError } from "../api";
import { clock, number } from "../format";
import { errorText, lang, t } from "../i18n";
import { Switch } from "./Controls";
import { Icon } from "./Icon";

type Text = { de: string; en: string };
interface Automation {
  key: string;
  pref: string | null;
  group: string;
  name: Text;
  when: Text;
  does: Text;
  enabled: boolean | null;
  last?: { t: number; text: string };
}
interface Delivery {
  stream?: number; // live app connections it was sent to
  fetched?: number; // when the app picked it up (ms)
  ha?: string; // "ok" | "failed: ..." | "off" | ...
}
interface Notification {
  id: number;
  t: number;
  title: string;
  message: string;
  delivery?: Delivery;
}
interface StreamEvent {
  t: number;
  event: "connected" | "disconnected";
  ip?: string;
  presence?: string;
  minutes?: number;
}
interface Overview {
  owner: { user?: string; name?: string };
  canEdit: boolean;
  ha: { reachable?: boolean; voice?: boolean };
  presence: {
    state?: string;
    since?: number;
    work?: { atWork: boolean; since: number };
    home?: { lat: number; lon: number; radius: number };
    workZone?: { lat: number; lon: number; radius: number; hours: number; days: number };
  };
  automations: Automation[];
  haAutomations: { entityId: string; name: string; on: boolean; lastTriggered: string | null }[];
  history: { t: number; key: string; text: string }[];
  notifications: { liveConnections: number; recent: Notification[]; streams: StreamEvent[] };
  config: { group: string; items: { key: string; value: string }[] }[];
  devices: { id: string; user: string; name: string; lastSeen: number }[];
  backups: { day: string; bytes: number }[];
}

const L = (de: string, en: string) => (lang() === "de" ? de : en);
const tx = (x: Text) => (lang() === "de" ? x.de : x.en);

/** "20:09" today, "Mo 20:09" this week, else "3.11. 20:09". */
function when(ms: number, now: number): string {
  if (!ms) return "–";
  const d = new Date(ms);
  const today = new Date(now);
  if (d.toDateString() === today.toDateString()) return clock(ms);
  if (now - ms < 6 * 86400_000)
    return `${d.toLocaleDateString(lang() === "de" ? "de-DE" : "en-GB", { weekday: "short" })} ${clock(ms)}`;
  return `${d.getDate()}.${d.getMonth() + 1}. ${clock(ms)}`;
}

const mapLink = (lat: number, lon: number) =>
  `https://www.openstreetmap.org/?mlat=${lat}&mlon=${lon}#map=17/${lat}/${lon}`;

export function Overview(props: { onBack: () => void }) {
  const [data, { refetch, mutate }] = createResource(() => api<Overview>("GET", "/api/admin/overview"));
  const [now, setNow] = createSignal(Date.now());
  const [msg, setMsg] = createSignal<{ text: string; error: boolean } | null>(null);
  let msgTimer: number | undefined;
  const flash = (text: string, error = false) => {
    setMsg({ text, error });
    clearTimeout(msgTimer);
    msgTimer = window.setTimeout(() => setMsg(null), 4000);
  };

  onMount(() => {
    const timer = setInterval(() => {
      setNow(Date.now());
      void refetch();
    }, 15_000);
    onCleanup(() => clearInterval(timer));
  });

  // Automation switch = the owner's pref (same as in the iPhone app).
  const toggle = async (a: Automation, v: boolean) => {
    if (!a.pref) return;
    mutate((d) => d && { ...d, automations: d.automations.map((x) => (x.key === a.key ? { ...x, enabled: v } : x)) });
    try {
      await api("PUT", "/api/prefs", { [a.pref]: v });
      flash(t("saved"));
    } catch (err) {
      flash(err instanceof ApiError ? errorText(err.code) : errorText("network"), true);
      void refetch();
    }
  };

  const test = async () => {
    try {
      await api("POST", "/api/notifications/test", {});
      flash(L("Test gesendet – Zustellung erscheint unten", "Test sent - delivery shows below"));
      setTimeout(() => void refetch(), 3000);
      setTimeout(() => void refetch(), 10000);
    } catch (err) {
      flash(err instanceof ApiError ? errorText(err.code) : errorText("network"), true);
    }
  };

  return (
    <div class="admin overview">
      <header class="admin-header">
        <button class="icon-button large" aria-label={t("back")} onClick={props.onBack}>
          <Icon name="arrow_back" size={24} />
        </button>
        <h1 class="headline-small">{t("systemOverview")}</h1>
        <button class="icon-button" aria-label={L("Aktualisieren", "Refresh")} onClick={() => void refetch()}>
          <Icon name="sync" size={20} />
        </button>
      </header>

      <Show when={data()} fallback={<div class="spinner" />}>
        {(d) => (
          <>
            <StatusCard d={d()} now={now()} />
            <NotificationsCard d={d()} now={now()} onTest={test} />
            <AutomationsCard d={d()} now={now()} onToggle={toggle} />
            <HaAutomationsCard d={d()} />
            <HistoryCard d={d()} now={now()} />
            <ConfigCard d={d()} now={now()} />
          </>
        )}
      </Show>

      <Show when={msg()}>
        {(m) => (
          <div class="snackbar body-medium" classList={{ error: m().error }} role="status">
            {m().text}
          </div>
        )}
      </Show>
    </div>
  );
}

// Who is where, Home Assistant reachable, zones.
function StatusCard(props: { d: Overview; now: number }) {
  const p = () => props.d.presence;
  const state = () =>
    ({
      home: L("Zuhause", "Home"),
      away: L("Unterwegs", "Away"),
      leaving: L("Geht gerade (2-min-Wartezeit)", "Leaving (2 min grace)"),
    })[p().state ?? ""] ?? L("Unbekannt", "Unknown");
  return (
    <section class="card ov-card">
      <h2 class="title-large">{L("Status", "Status")}</h2>
      <div class="ov-facts">
        <Fact label={props.d.owner.name || props.d.owner.user || "–"} value={state()} sub={p().since ? L("seit ", "since ") + when(p().since!, props.now) : ""} />
        <Fact
          label={L("Arbeit", "Work")}
          value={p().work?.atWork ? L("Bei der Arbeit", "At work") : L("Nicht dort", "Not there")}
          sub={p().work?.since ? L("seit ", "since ") + when(p().work!.since, props.now) : ""}
        />
        <Fact label="Home Assistant" value={props.d.ha.reachable ? L("Erreichbar", "Reachable") : L("Nicht erreichbar", "Unreachable")} bad={!props.d.ha.reachable} />
        <Fact label="Echo" value={props.d.ha.voice ? L("Sprachausgabe an", "Voice on") : L("Nicht eingerichtet", "Not set up")} />
      </div>
      <div class="body-medium variant ov-zones">
        <Show when={p().home}>
          {(h) => (
            <a href={mapLink(h().lat, h().lon)} target="_blank" rel="noreferrer">
              {L("Heimzone", "Home zone")} ({Math.round(h().radius)} m)
            </a>
          )}
        </Show>
        <Show
          when={p().workZone}
          fallback={<span>{L("Arbeitsort: noch nicht erkannt (die App fragt stündlich)", "Work place: not detected yet (the app asks hourly)")}</span>}
        >
          {(w) => (
            <a href={mapLink(w().lat, w().lon)} target="_blank" rel="noreferrer">
              {L("Arbeitsort", "Work place")} ({w().hours} h / {w().days} {L("Tage", "days")})
            </a>
          )}
        </Show>
      </div>
    </section>
  );
}

function Fact(props: { label: string; value: string; sub?: string; bad?: boolean }) {
  return (
    <div class="ov-fact">
      <div class="label-medium variant">{props.label}</div>
      <div class="title-medium" classList={{ "ov-bad": props.bad }}>
        {props.value}
      </div>
      <Show when={props.sub}>
        <div class="body-small variant">{props.sub}</div>
      </Show>
    </div>
  );
}

// Per notification: did the app get it live, did it fetch it later, did the
// Home Assistant push go out? Plus when the app was connected.
function NotificationsCard(props: { d: Overview; now: number; onTest: () => void }) {
  const n = () => props.d.notifications;
  const haOk = (h?: string) => h === "ok";
  return (
    <section class="card ov-card">
      <div class="ov-card-head">
        <h2 class="title-large">{L("Mitteilungen", "Notifications")}</h2>
        <button class="tonal-button label-large" onClick={props.onTest}>
          <Icon name="send" size={18} />
          {L("Test senden", "Send test")}
        </button>
      </div>
      <p class="body-medium">
        <span class="chip" classList={{ "ov-ok": n().liveConnections > 0, "ov-badchip": n().liveConnections === 0 }}>
          {n().liveConnections > 0
            ? L(`App jetzt live verbunden (${n().liveConnections})`, `App live now (${n().liveConnections})`)
            : L("App jetzt nicht verbunden", "App not connected now")}
        </span>
      </p>
      <p class="body-small variant">
        {L(
          "Jede Mitteilung geht zwei Wege: HomeCollect-App (live oder beim nächsten Aufwachen abgeholt) und Home-Assistant-App (Push über Apple). „Live 0“ und nicht abgeholt = die App war nicht verbunden.",
          "Every notification takes two paths: the HomeCollect app (live, or fetched at its next wake-up) and the Home Assistant app (push via Apple). \"Live 0\" and not fetched = the app wasn't connected."
        )}
      </p>
      <div class="ov-table">
        <For each={n().recent} fallback={<div class="body-medium variant">{L("Noch keine", "None yet")}</div>}>
          {(x) => (
            <div class="ov-row">
              <div class="ov-time body-small variant">{when(x.t, props.now)}</div>
              <div class="ov-main">
                <div class="body-medium ellipsis">{x.title}</div>
                <div class="ov-chips">
                  <span class="chip small" classList={{ "ov-ok": (x.delivery?.stream ?? 0) > 0, "ov-badchip": x.delivery?.stream === 0 }}>
                    {L("App live", "App live")}: {x.delivery?.stream ?? "?"}
                  </span>
                  <span class="chip small" classList={{ "ov-ok": !!x.delivery?.fetched }}>
                    {x.delivery?.fetched ? L("abgeholt ", "fetched ") + when(x.delivery.fetched, props.now) : L("nicht abgeholt", "not fetched")}
                  </span>
                  <span class="chip small" classList={{ "ov-ok": haOk(x.delivery?.ha), "ov-badchip": !!x.delivery?.ha && !haOk(x.delivery?.ha) && x.delivery?.ha !== "off" }}>
                    HA: {x.delivery?.ha ?? "–"}
                  </span>
                </div>
              </div>
            </div>
          )}
        </For>
      </div>
      <details class="ov-details">
        <summary class="label-large">{L("App-Verbindungen", "App connections")}</summary>
        <For each={n().streams} fallback={<div class="body-small variant">{L("Seit dem Serverstart keine", "None since the server started")}</div>}>
          {(s) => (
            <div class="body-small ov-line">
              {when(s.t, props.now)} ·{" "}
              {s.event === "connected"
                ? L(`verbunden (${s.presence ?? "?"}) von ${s.ip ?? "?"}`, `connected (${s.presence ?? "?"}) from ${s.ip ?? "?"}`)
                : L(`getrennt nach ${s.minutes} min`, `closed after ${s.minutes} min`)}
            </div>
          )}
        </For>
      </details>
    </section>
  );
}

const GROUPS: Record<string, Text> = {
  home: { de: "Zuhause", en: "Home" },
  school: { de: "Schule", en: "School" },
  work: { de: "Arbeit", en: "Work" },
  day: { de: "Tagesablauf", en: "Daily" },
  system: { de: "System", en: "System" },
};

function AutomationsCard(props: { d: Overview; now: number; onToggle: (a: Automation, v: boolean) => void }) {
  const groups = () => Object.keys(GROUPS).filter((g) => props.d.automations.some((a) => a.group === g));
  return (
    <section class="card ov-card">
      <h2 class="title-large">{L("Automationen (Dashboard)", "Automations (dashboard)")}</h2>
      <Show when={!props.d.canEdit}>
        <p class="body-small variant">{L("Schalter ändern die Einstellungen des Besitzers – nur er kann sie hier umstellen.", "Switches are the owner's settings - only the owner can change them here.")}</p>
      </Show>
      <For each={groups()}>
        {(g) => (
          <>
            <h3 class="title-small ov-group">{tx(GROUPS[g])}</h3>
            <For each={props.d.automations.filter((a) => a.group === g)}>
              {(a) => (
                <div class="ov-auto">
                  <div class="ov-main">
                    <div class="title-medium">{tx(a.name)}</div>
                    <div class="body-small variant">
                      <b>{L("Wann", "When")}:</b> {tx(a.when)}
                    </div>
                    <div class="body-small variant">
                      <b>{L("Was", "Does")}:</b> {tx(a.does)}
                    </div>
                    <div class="body-small ov-last">
                      {a.last ? `${L("Zuletzt", "Last")}: ${when(a.last.t, props.now)} – ${a.last.text}` : L("Seit dem Serverstart nicht gelaufen", "Not run since the server started")}
                    </div>
                  </div>
                  <Show when={a.pref} fallback={<span class="chip small">{L("immer an", "always on")}</span>}>
                    <Switch checked={!!a.enabled} disabled={!props.d.canEdit} label={tx(a.name)} onChange={(v) => props.onToggle(a, v)} />
                  </Show>
                </div>
              )}
            </For>
          </>
        )}
      </For>
    </section>
  );
}

function HaAutomationsCard(props: { d: Overview }) {
  return (
    <section class="card ov-card">
      <h2 class="title-large">{L("Automationen (Home Assistant)", "Automations (Home Assistant)")}</h2>
      <p class="body-small variant">{L("Laufen in Home Assistant selbst; ändern dort unter Einstellungen → Automationen.", "Run inside Home Assistant; edit them there under Settings -> Automations.")}</p>
      <For each={props.d.haAutomations} fallback={<div class="body-medium variant">{L("Keine (oder noch nicht geladen)", "None (or not loaded yet)")}</div>}>
        {(a) => (
          <div class="ov-row">
            <div class="ov-main">
              <div class="body-large">{a.name}</div>
              <div class="body-small variant">
                {a.entityId} · {L("zuletzt ausgelöst", "last triggered")}: {a.lastTriggered ? new Date(a.lastTriggered).toLocaleString(lang() === "de" ? "de-CH" : "en-GB") : L("nie", "never")}
              </div>
            </div>
            <span class="chip small" classList={{ "ov-ok": a.on }}>
              {a.on ? L("an", "on") : L("aus", "off")}
            </span>
          </div>
        )}
      </For>
    </section>
  );
}

function HistoryCard(props: { d: Overview; now: number }) {
  const [filter, setFilter] = createSignal("");
  const names = () => Object.fromEntries(props.d.automations.map((a) => [a.key, tx(a.name)]));
  const label = (k: string) => names()[k] ?? ({ notify: L("Mitteilung", "Notification"), system: "System" } as Record<string, string>)[k] ?? k;
  const keys = () => [...new Set(props.d.history.map((h) => h.key))];
  return (
    <section class="card ov-card">
      <div class="ov-card-head">
        <h2 class="title-large">{L("Verlauf", "History")}</h2>
        <select class="ov-select body-medium" value={filter()} onChange={(e) => setFilter(e.currentTarget.value)}>
          <option value="">{L("Alle", "All")}</option>
          <For each={keys()}>{(k) => <option value={k}>{label(k)}</option>}</For>
        </select>
      </div>
      <div class="ov-history">
        <For
          each={props.d.history.filter((h) => !filter() || h.key === filter())}
          fallback={<div class="body-medium variant">{L("Seit dem Serverstart noch nichts", "Nothing since the server started")}</div>}
        >
          {(h) => (
            <div class="ov-line body-small">
              <span class="ov-time variant">{when(h.t, props.now)}</span>
              <span class="chip small">{label(h.key)}</span>
              <span>{h.text}</span>
            </div>
          )}
        </For>
      </div>
    </section>
  );
}

// .env values, iPhone devices, backups.
function ConfigCard(props: { d: Overview; now: number }) {
  const mb = (b: number) => `${number(Math.round(b / 1e5) / 10)} MB`;
  return (
    <section class="card ov-card">
      <h2 class="title-large">{L("Konfiguration", "Configuration")}</h2>
      <p class="body-small variant">{L("Aus der .env auf dem Server. Geheimnisse werden nur als „set“ angezeigt.", "From .env on the server. Secrets only show as \"set\".")}</p>
      <For each={props.d.config}>
        {(g) => (
          <>
            <h3 class="title-small ov-group">{g.group}</h3>
            <dl class="ov-config">
              <For each={g.items}>
                {(i) => (
                  <>
                    <dt class="body-small variant">{i.key}</dt>
                    <dd class="body-small">{i.value || "–"}</dd>
                  </>
                )}
              </For>
            </dl>
          </>
        )}
      </For>
      <h3 class="title-small ov-group">{L("iPhone-Geräte", "iPhone devices")}</h3>
      <For each={props.d.devices}>
        {(dv) => (
          <div class="ov-line body-small">
            <span class="ov-time variant">{dv.id}</span>
            <span>
              {dv.user} · {dv.name} · {L("zuletzt", "last seen")} {dv.lastSeen ? when(dv.lastSeen, props.now) : L("nie", "never")}
            </span>
          </div>
        )}
      </For>
      <h3 class="title-small ov-group">{L("Backups", "Backups")}</h3>
      <For each={props.d.backups} fallback={<div class="body-small variant">{L("Keine", "None")}</div>}>
        {(b) => (
          <div class="ov-line body-small">
            <span class="ov-time variant">{b.day}</span>
            <span>{mb(b.bytes)}</span>
          </div>
        )}
      </For>
    </section>
  );
}
