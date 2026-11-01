import { For, Show } from "solid-js";
import type { Phone, Sleep } from "../proto/dashboard";
import { Card, Empty } from "./Card";
import { Icon, type IconName } from "./Icon";
import { hm, number } from "../format";
import { t } from "../i18n";


function Metric(props: { icon: IconName; value: string; label: string }) {
  return (
    <div class="metric">
      <Icon name={props.icon} size={20} class="metric-icon" />
      <div class="headline-small">{props.value}</div>
      <div class="label-medium variant">{props.label}</div>
    </div>
  );
}

function SleepSection(props: { sleep: Sleep }) {
  const phases = () => [
    { cls: "deep", label: t("deep"), v: props.sleep.deep },
    { cls: "core", label: t("core"), v: props.sleep.core },
    { cls: "rem", label: t("rem"), v: props.sleep.rem },
    { cls: "awake", label: t("awake"), v: props.sleep.awake },
  ].filter((p) => p.v !== undefined && p.v > 0);

  return (
    <div class="sleep">
      <div class="sleep-head">
        <Icon name="bedtime" size={20} class="metric-icon" />
        <span class="label-large variant">{t("sleepLastNight")}</span>
        <Show when={props.sleep.total !== undefined}>
          <span class="headline-small sleep-total">{hm(props.sleep.total!)}</span>
        </Show>
      </div>
      <Show when={phases().length > 0}>
        <div class="sleep-track" role="img" aria-label={t("sleepPhases")}>
          <For each={phases()}>{(p) => <span class={p.cls} style={{ flex: p.v }} />}</For>
        </div>
        <div class="sleep-legend">
          <For each={phases()}>
            {(p) => (
              <span class="body-medium">
                <span class={`legend-dot ${p.cls}`} />
                {p.label} <span class="variant">{hm(p.v!)}</span>
              </span>
            )}
          </For>
        </div>
      </Show>
    </div>
  );
}

export function PhoneCard(props: { phone?: Phone; sleep?: Sleep }) {
  return (
    <Card title={t("health")} class="wide health-card">
      <Show when={props.phone || props.sleep} fallback={<Empty>{t("noPhone")}</Empty>}>
        <Show when={props.phone}>
          {(p) => (
            <div class="metrics">
              <Show when={p().steps !== undefined}>
                <Metric icon="directions_walk" value={number(p().steps!)} label={t("steps")} />
              </Show>
              <Show when={p().heartRate !== undefined}>
                <Metric icon="favorite" value={`${p().heartRate}`} label={t("heartRate")} />
              </Show>
              <Show when={p().battery !== undefined}>
                <Metric icon="battery_full" value={`${p().battery} %`} label={t("battery")} />
              </Show>
            </div>
          )}
        </Show>
        <Show when={props.sleep}>{(s) => <SleepSection sleep={s()} />}</Show>
      </Show>
    </Card>
  );
}
