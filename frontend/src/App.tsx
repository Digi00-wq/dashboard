import { createSignal, For, onCleanup, onMount, Show } from "solid-js";
import { connect, dashboard, status } from "./connection";
import { clock, greeting, longDate } from "./format";
import { lang, setLang, t, type Lang } from "./i18n";
import { Glance } from "./components/Glance";
import { Icon } from "./components/Icon";
import { Lamps } from "./components/Lamps";
import { MediaCard } from "./components/MediaCard";
import { mediaMinimized } from "./ui";
import { PhoneCard } from "./components/PhoneCard";
import { Room } from "./components/Room";
import { ServerCard } from "./components/ServerCard";
import { Tasks } from "./components/Tasks";
import { WeatherCard } from "./components/WeatherCard";
import "./App.css";

export default function App() {
  const [now, setNow] = createSignal(Date.now());

  onMount(() => {
    connect();
    const t = setInterval(() => setNow(Date.now()), 1000);
    onCleanup(() => clearInterval(t));
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

  return (
    <div class="app">
      <header class="top-bar">
        <div>
          <div class="body-large variant">{greeting(now())}</div>
          <h1 class="display-medium clock">
            {clock(now())}
          </h1>
          <div class="title-medium variant">
            {longDate(now())}
          </div>
        </div>
        <div class="top-actions">
          <span class={`status-chip label-large ${conn().cls}`}>
            <Icon name={conn().icon} size={18} />
            {conn().text}
          </span>
          <div class="segmented label-large" role="radiogroup" aria-label={t("language")}>
            <For each={["de", "en"] as Lang[]}>
              {(l) => (
                <button role="radio" aria-checked={lang() === l} classList={{ selected: lang() === l }} onClick={() => setLang(l)}>
                  {l.toUpperCase()}
                </button>
              )}
            </For>
          </div>
        </div>
      </header>
      <Glance glance={dashboard()?.glance} now={now()} />

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
                  <span class="body-medium">
                    {t("sourceErrors")(d().errors.length)}
                  </span>
                  <Icon name="keyboard_arrow_down" class="banner-chevron" />
                </summary>
                <ul class="body-small">
                  <For each={d().errors}>{(e) => <li>{e}</li>}</For>
                </ul>
              </details>
            </Show>
            <main class="grid">
              <WeatherCard weather={d().weather} />
              <Tasks todo={d().todoist} now={now()} />
              <Show when={!mediaMinimized()}>
                <For each={d().glance?.media ?? []}>{(m) => <MediaCard media={m} now={now()} />}</For>
              </Show>
              <Lamps lamps={d().lamps} />
              <Room presence={d().presence} light={d().light} now={now()} />
              <PhoneCard phone={d().phone} sleep={d().sleep} />
              <ServerCard server={d().server} />
            </main>
          </>
        )}
      </Show>
    </div>
  );
}
