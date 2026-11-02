import { createEffect, createSignal, For, onCleanup, onMount, Show } from "solid-js";
import { can, me, refreshMe } from "./auth";
import { connect, dashboard, disconnect, status } from "./connection";
import { clock, greeting, longDate } from "./format";
import { t } from "./i18n";
import { AccountMenu } from "./components/AccountMenu";
import { AnnounceButton } from "./components/Announce";
import { Admin } from "./components/Admin";
import { LangToggle } from "./components/Controls";
import { Glance } from "./components/Glance";
import { Icon } from "./components/Icon";
import { Lamps } from "./components/Lamps";
import { Login } from "./components/Login";
import { MediaCard } from "./components/MediaCard";
import { mediaMinimized } from "./ui";
import { PhoneCard } from "./components/PhoneCard";
import { Room } from "./components/Room";
import { ServerCard } from "./components/ServerCard";
import { Tasks } from "./components/Tasks";
import { WeatherCard } from "./components/WeatherCard";
import "./App.css";

const ALL_PERMS = ["weather", "lights", "room", "tasks", "media", "health", "server"];

export default function App() {
  onMount(() => {
    refreshMe().catch(() => setTimeout(() => refreshMe().catch(() => {}), 3000));
  });

  // Live data only while signed in.
  createEffect(() => (me() ? connect() : disconnect()));

  return (
    <Show
      when={me() !== undefined}
      fallback={
        <div class="loading">
          <div class="spinner" />
        </div>
      }
    >
      <Show when={me()} fallback={<Login />}>
        <Home />
      </Show>
    </Show>
  );
}

function Home() {
  const [now, setNow] = createSignal(Date.now());
  const [view, setView] = createSignal<"dashboard" | "admin">("dashboard");

  onMount(() => {
    const timer = setInterval(() => setNow(Date.now()), 1000);
    onCleanup(() => clearInterval(timer));
  });

  const stale = () => {
    const d = dashboard();
    return d ? now() - d.updatedAt > 60_000 : false;
  };
  const conn = () =>
    status() === "online"
      ? stale()
        ? { icon: "sync" as const, text: t("stale"), cls: "warn" }
        : { icon: "wifi" as const, text: t("connected"), cls: "ok" }
      : status() === "connecting"
        ? { icon: "sync" as const, text: t("connecting"), cls: "" }
        : { icon: "wifi_off" as const, text: t("offline"), cls: "error" };
  // The fixed 3x3 desktop layout assumes every card; others use auto flow.
  const full = () => ALL_PERMS.every(can);

  return (
    <div class="app">
      <header class="top-bar">
        <div>
          <div class="body-large variant">
            {greeting(now())}, {me()?.name}
          </div>
          <h1 class="display-medium clock">{clock(now())}</h1>
          <div class="title-medium variant">{longDate(now())}</div>
        </div>
        <div class="top-actions">
          <span class={`status-chip label-large ${conn().cls}`} title={conn().text}>
            <Icon name={conn().icon} size={18} />
            <span class="status-text">{conn().text}</span>
          </span>
          <LangToggle />
          <Show when={can("announce") && dashboard()?.voiceAvailable}>
            <AnnounceButton />
          </Show>
          <AccountMenu onManageUsers={() => setView("admin")} />
        </div>
      </header>

      <Show when={view() === "admin" && me()?.admin} fallback={<DashboardView now={now()} full={full()} />}>
        <Admin onBack={() => setView("dashboard")} />
      </Show>
    </div>
  );
}

function DashboardView(props: { now: number; full: boolean }) {
  return (
    <>
      <Glance glance={dashboard()?.glance} now={props.now} />
      <Show
        when={dashboard()}
        fallback={
          <div class="loading">
            <div class="spinner" />
            <span class="body-large variant">{t("waiting")}</span>
          </div>
        }
      >
        {(d) => (
          <>
            <Show when={d().errors.length > 0}>
              <details class="banner">
                <summary>
                  <Icon name="error" />
                  <span class="body-medium">{t("sourceErrors")(d().errors.length)}</span>
                  <Icon name="keyboard_arrow_down" class="banner-chevron" />
                </summary>
                <ul class="body-small">
                  <For each={d().errors}>{(e) => <li>{e}</li>}</For>
                </ul>
              </details>
            </Show>
            <main class="grid" classList={{ full: props.full }}>
              <Show when={can("weather")}>
                <WeatherCard weather={d().weather} />
              </Show>
              <Show when={can("tasks")}>
                <Tasks todo={d().todoist} now={props.now} />
              </Show>
              <Show when={can("media") && !mediaMinimized()}>
                <For each={d().glance?.media ?? []}>{(m) => <MediaCard media={m} now={props.now} />}</For>
              </Show>
              <Show when={can("lights")}>
                <Lamps lamps={d().lamps} />
              </Show>
              <Show when={can("room")}>
                <Room presence={d().presence} light={d().light} now={props.now} />
              </Show>
              <Show when={can("health")}>
                <PhoneCard phone={d().phone} sleep={d().sleep} />
              </Show>
              <Show when={can("server")}>
                <ServerCard server={d().server} />
              </Show>
            </main>
          </>
        )}
      </Show>
    </>
  );
}
