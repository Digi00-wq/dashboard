// Media player card (Spotify etc. from HA_MEDIA_ENTITIES): cover, title, progress,
// play/pause/next/previous via Command.media. Covers come from /api/cover/<entity>.

import { For, Show } from "solid-js";
import type { Media } from "../proto/dashboard";
import { MediaCommand_Action } from "../proto/dashboard";
import { send } from "../connection";
import { t } from "../i18n";
import { setMediaMinimized } from "../ui";
import { Icon } from "./Icon";

// Seconds -> m:ss.
const mmss = (s: number) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

// Player is worth showing (playing or paused).
export const isActive = (m: Media) => m.state === "playing" || m.state === "paused";

/** Full player widget (Spotify etc.). Can be minimised into the glance pill. */
export function MediaCard(props: { media: Media; now: number }) {
  const m = () => props.media;
  const cmd = (action: MediaCommand_Action, extra: { volume?: number; source?: string; shuffle?: boolean } = {}) =>
    send({ media: { entityId: m().entityId, action, ...extra } });

  // HA only reports the position when something changes: extrapolate locally.
  const position = () => {
    const pos = m().position ?? 0;
    if (m().state !== "playing" || !m().positionUpdatedAt) return pos;
    return Math.min(m().duration ?? Infinity, pos + (props.now - m().positionUpdatedAt!) / 1000);
  };
  const pct = () => (m().duration ? Math.min(100, (position() / m().duration!) * 100) : 0);
  const volumePct = () => Math.round((m().volume ?? 0) * 100);

  return (
    <section tabindex="0" class="card media-card" classList={{ playing: m().state === "playing", "has-cover": isActive(m()) && !!m().cover }}>
      {/* Desktop: cover as full-card background (hidden on phones via CSS) */}
      <Show when={isActive(m()) && m().cover}>
        <div class="media-bg" aria-hidden="true">
          <img src={m().cover} alt="" />
          <div class="media-scrim" />
        </div>
      </Show>
      <header class="card-header">
        <h2 class="title-medium">{m().name}</h2>
        <button class="icon-button" aria-label={t("minimize")} title={t("minimize")} onClick={() => setMediaMinimized(true)}>
          <Icon name="close_fullscreen" size={20} />
        </button>
      </header>

      <Show
        when={isActive(m())}
        fallback={
          <div class="media-idle body-large variant">
            <Icon name="music_note" size={24} />
            {t("nothingPlaying")}
          </div>
        }
      >
        <div class="media-main">
          <div class="media-cover">
            <Show when={m().cover} fallback={<Icon name="music_note" size={40} />}>
              <img src={m().cover} alt="" />
            </Show>
          </div>
          <div class="media-info">
            <div class="title-large ellipsis" title={m().title}>
              {m().title}
            </div>
            <div class="body-medium variant ellipsis">{[m().artist, m().album].filter(Boolean).join(" · ")}</div>

            <Show when={m().duration}>
              <div class="media-progress">
                <div class="progress-track">
                  <div class="progress-active" style={{ width: `${pct()}%`, transition: "none" }} />
                  <div class="progress-inactive" />
                </div>
                <div class="media-times label-small variant">
                  <span>{mmss(position())}</span>
                  <span>{mmss(m().duration!)}</span>
                </div>
              </div>
            </Show>
          </div>
        </div>

        <div class="media-controls-row">
          <Show when={m().shuffle !== undefined} fallback={<span class="icon-button-spacer" />}>
            <button
              class="icon-button toggle"
              classList={{ on: m().shuffle }}
              aria-label={t("shuffle")}
              aria-pressed={m().shuffle}
              title={t("shuffle")}
              onClick={() => cmd(MediaCommand_Action.SET_SHUFFLE, { shuffle: !m().shuffle })}
            >
              <Icon name="shuffle" size={22} />
            </button>
          </Show>
          <button class="icon-button large" aria-label={t("previous")} onClick={() => cmd(MediaCommand_Action.PREVIOUS)}>
            <Icon name="skip_previous" size={28} />
          </button>
          <button
            class="play-fab"
            aria-label={m().state === "playing" ? t("pause") : t("play")}
            onClick={() => cmd(MediaCommand_Action.PLAY_PAUSE)}
          >
            <Icon name={m().state === "playing" ? "pause" : "play_arrow"} size={32} />
          </button>
          <button class="icon-button large" aria-label={t("next")} onClick={() => cmd(MediaCommand_Action.NEXT)}>
            <Icon name="skip_next" size={28} />
          </button>
          <span class="icon-button-spacer" />
        </div>

        <div class="media-bottom">
          <Show when={m().volume !== undefined}>
            <div class="media-volume">
              <Icon name={volumePct() === 0 ? "volume_off" : "volume_up"} size={20} />
              <input
                class="slider"
                type="range"
                min="0"
                max="100"
                value={volumePct()}
                style={{ "--pct": `${volumePct()}%` }}
                onInput={(e) => e.currentTarget.style.setProperty("--pct", `${e.currentTarget.value}%`)}
                onChange={(e) => cmd(MediaCommand_Action.SET_VOLUME, { volume: +e.currentTarget.value / 100 })}
                aria-label={t("volume")}
              />
            </div>
          </Show>
          <Show when={m().sources.length > 0}>
            <label class="device-chip label-large" title={t("playbackDevice")}>
              <Icon name="speaker_group" size={18} />
              <select
                aria-label={t("playbackDevice")}
                onChange={(e) => cmd(MediaCommand_Action.SELECT_SOURCE, { source: e.currentTarget.value })}
              >
                <Show when={!m().sources.includes(m().source)}>
                  <option value={m().source} selected>{m().source || "—"}</option>
                </Show>
                <For each={m().sources}>{(s) => (
                    <option value={s} selected={s === m().source}>
                      {s}
                    </option>
                  )}</For>
              </select>
            </label>
          </Show>
        </div>
      </Show>
    </section>
  );
}
