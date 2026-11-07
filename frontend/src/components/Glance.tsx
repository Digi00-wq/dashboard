// "At a glance" strip below the clock: next alarm, internet status/speed, minimized media.

import { For, Show } from "solid-js";
import type { Glance as GlanceData, Media } from "../proto/dashboard";
import { MediaCommand_Action } from "../proto/dashboard";
import { send } from "../connection";
import { Icon } from "./Icon";
import { clock, weekday } from "../format";
import { locale, t } from "../i18n";
import { mediaMinimized, setMediaMinimized } from "../ui";


// "07:00" within 24 h, else "Mon 07:00".
function alarmLabel(ms: number, now: number): string {
  // Within the next 24 h the time is enough; otherwise add the weekday.
  if (ms - now < 24 * 3600_000) return clock(ms);
  return `${weekday(ms)} ${clock(ms)}`;
}

// KiB/s -> readable speed.
function speed(kib: number): string {
  if (kib >= 1024) return `${(kib / 1024).toLocaleString(locale(), { maximumFractionDigits: 1 })} MB/s`;
  return `${Math.round(kib)} KB/s`;
}

// Small player (title + play/pause) shown when the media card is minimized.
function MediaPill(props: { media: Media }) {
  const active = () => props.media.state === "playing" || props.media.state === "paused";
  const act = (action: MediaCommand_Action) => send({ media: { entityId: props.media.entityId, action } });
  const label = () =>
    active() && props.media.title
      ? [props.media.title, props.media.artist].filter(Boolean).join(" · ")
      : `${props.media.name} · ${props.media.state === "off" || props.media.state === "unavailable" ? t("mediaOff") : t("mediaReady")}`;

  return (
    <div class="glance-pill media" classList={{ playing: props.media.state === "playing" }}>
      <Icon name={active() ? "music_note" : "speaker"} size={20} />
      <span class="ellipsis media-label" title={label()}>
        {label()}
      </span>
      <span class="media-controls">
        <Show when={active()}>
          <button class="icon-button" aria-label={t("previous")} onClick={() => act(MediaCommand_Action.PREVIOUS)}>
            <Icon name="skip_previous" size={20} />
          </button>
          <button
            class="icon-button filled"
            aria-label={props.media.state === "playing" ? t("pause") : t("play")}
            onClick={() => act(MediaCommand_Action.PLAY_PAUSE)}
          >
            <Icon name={props.media.state === "playing" ? "pause" : "play_arrow"} size={20} />
          </button>
          <button class="icon-button" aria-label={t("next")} onClick={() => act(MediaCommand_Action.NEXT)}>
            <Icon name="skip_next" size={20} />
          </button>
        </Show>
        <button class="icon-button" aria-label={t("expand")} title={t("expand")} onClick={() => setMediaMinimized(false)}>
          <Icon name="open_in_full" size={18} />
        </button>
      </span>
    </div>
  );
}

/** Google "At a glance"-style strip next to the clock. */
export function Glance(props: { glance?: GlanceData; now: number }) {
  return (
    <Show when={props.glance}>
      {(g) => (
        <div class="glance">
          <Show when={g().nextAlarm}>
            {(a) => (
              <div class="glance-pill" title={t("nextAlarm")}>
                <Icon name="alarm" size={20} />
                <span>{alarmLabel(a(), props.now)}</span>
              </div>
            )}
          </Show>

          <Show when={g().network}>
            {(n) => (
              <Show
                when={n().online !== false}
                fallback={
                  <div class="glance-pill error">
                    <Icon name="cloud_off" size={20} />
                    <span>{t("internetOffline")}</span>
                  </div>
                }
              >
                <div class="glance-pill" title={t("internet")}>
                  <Icon name="router" size={20} />
                  <Show when={n().downloadKib !== undefined} fallback={<span>{t("online")}</span>}>
                    <span class="net">
                      <Icon name="arrow_downward" size={16} />
                      {speed(n().downloadKib!)}
                      <Icon name="arrow_upward" size={16} />
                      {speed(n().uploadKib ?? 0)}
                    </span>
                  </Show>
                </div>
              </Show>
            )}
          </Show>

          <Show when={mediaMinimized()}>
            <For each={g().media}>{(m) => <MediaPill media={m} />}</For>
          </Show>
        </div>
      )}
    </Show>
  );
}
