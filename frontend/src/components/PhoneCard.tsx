import { For, Show } from "solid-js";
import type { Activity, Phone, Sleep } from "../proto/dashboard";
import { Card, Empty } from "./Card";
import { Icon, type IconName } from "./Icon";
import { hm, number } from "../format";
import { lang, t } from "../i18n";


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

const SPORTS: Record<string, [string, string]> = {
  basketball: ["Basketball", "Basketball"],
  football: ["Fussball", "Football"],
  americanFootball: ["Football", "American football"],
  running: ["Laufen", "Running"],
  walking: ["Gehen", "Walking"],
  hiking: ["Wandern", "Hiking"],
  cycling: ["Velo", "Cycling"],
  swimming: ["Schwimmen", "Swimming"],
  strength: ["Krafttraining", "Strength"],
  functionalStrength: ["Krafttraining", "Strength"],
  hiit: ["HIIT", "HIIT"],
  core: ["Core", "Core"],
  yoga: ["Yoga", "Yoga"],
};

// One quiet line with what only the HomeCollect app delivers.
function ActivityLine(props: { a: Activity }) {
  const parts = () => {
    const a = props.a;
    const out: string[] = [];
    if (a.restingHr !== undefined) out.push(`${t("restingHr")} ${a.restingHr}`);
    if (a.hrvMs !== undefined) out.push(`HRV ${Math.round(a.hrvMs)} ms`);
    if (a.activeKcal !== undefined) out.push(`${number(Math.round(a.activeKcal))} kcal`);
    const w = a.lastWorkout;
    if (w) {
      const name = SPORTS[w.activity]?.[lang() === "de" ? 0 : 1] ?? t("workout");
      out.push(`${name} ${Math.round(w.durationS / 60)} min`);
    }
    return out;
  };
  return (
    <Show when={parts().length > 0}>
      <div class="activity-line body-medium variant">{parts().join(" · ")}</div>
    </Show>
  );
}

export function PhoneCard(props: { phone?: Phone; sleep?: Sleep; activity?: Activity }) {
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
        <Show when={props.activity}>{(a) => <ActivityLine a={a()} />}</Show>
        <Show when={props.sleep}>{(s) => <SleepSection sleep={s()} />}</Show>
      </Show>
    </Card>
  );
}
