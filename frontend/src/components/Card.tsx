import type { JSX } from "solid-js";
import { Show } from "solid-js";
import { Icon, type IconName } from "./Icon";

export function Card(props: { title?: string; trailing?: JSX.Element; class?: string; children: JSX.Element }) {
  return (
    <section class={`card ${props.class ?? ""}`}>
      <Show when={props.title}>
        <header class="card-header">
          <h2 class="title-medium">{props.title}</h2>
          {props.trailing}
        </header>
      </Show>
      {props.children}
    </section>
  );
}

/** M3 linear progress indicator with label row. */
export function Progress(props: { label: string; value: number; max: number; text: string }) {
  const pct = () => (props.max > 0 ? Math.min(100, Math.max(0, (props.value / props.max) * 100)) : 0);
  const level = () => (pct() > 90 ? "error" : pct() > 75 ? "warn" : "");
  return (
    <div class={`progress ${level()}`}>
      <div class="progress-label">
        <span class="label-large">{props.label}</span>
        <span class="body-medium variant">{props.text}</span>
      </div>
      <div class="progress-track" role="progressbar" aria-valuenow={Math.round(pct())} aria-valuemin="0" aria-valuemax="100">
        <div class="progress-active" style={{ width: `${pct()}%` }} />
        <div class="progress-inactive" />
      </div>
    </div>
  );
}

/** Icon in a tonal circle + headline + supporting text. */
export function Tile(props: { icon: IconName; value: JSX.Element; label: JSX.Element; tone?: "primary" | "tertiary" | "neutral" }) {
  return (
    <div class="tile">
      <span class={`tile-icon ${props.tone ?? "primary"}`}>
        <Icon name={props.icon} />
      </span>
      <div class="tile-text">
        <div class="title-large">{props.value}</div>
        <div class="body-medium variant">{props.label}</div>
      </div>
    </div>
  );
}

export function Empty(props: { children: JSX.Element }) {
  return <p class="empty body-medium">{props.children}</p>;
}
