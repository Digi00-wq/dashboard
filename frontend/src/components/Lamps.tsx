// Lights card: one tile per lamp (from HA_LIGHT_ENTITIES via the backend) with on/off,
// brightness and color. Sends Command.light / Command.allLights. Lamp names = HA friendly_name.

import { For, Show } from "solid-js";
import type { Lamp } from "../proto/dashboard";
import { send } from "../connection";
import { fromHex, hex } from "../format";
import { Card, Empty } from "./Card";
import { Icon } from "./Icon";
import { t } from "../i18n";

// Card with master switch (any on -> all off, else all on) and lamp tiles.
export function Lamps(props: { lamps: Lamp[] }) {
  const onCount = () => props.lamps.filter((l) => l.isOn).length;
  return (
    <Card
      class="lights-card"
      title={t("lights")}
      trailing={
        <Show when={props.lamps.length > 0}>
          <div class="master">
            <span class="label-large variant">
              {onCount() === 0 ? t("allOff") : onCount() === props.lamps.length ? t("allOn") : t("someOn")(onCount(), props.lamps.length)}
            </span>
            {/* Master switch: any lamp on -> turn all off, otherwise all on */}
            <button
              role="switch"
              class="switch"
              aria-checked={onCount() > 0}
              aria-label={t("switchAll")}
              disabled={props.lamps.every((l) => !l.available)}
              onClick={() => send({ allLights: onCount() === 0 })}
            >
              <span class="switch-handle">
                <Show when={onCount() > 0}>
                  <Icon name="check" size={16} />
                </Show>
              </span>
            </button>
          </div>
        </Show>
      }
    >
      <Show when={props.lamps.length > 0} fallback={<Empty>{t("noLights")}</Empty>}>
        <div class="lamps">
          <For each={props.lamps}>{(lamp) => <LampTile lamp={lamp} />}</For>
        </div>
      </Show>
    </Card>
  );
}

// One lamp: tap toggles, slider sets brightness (0-255), color picker sets RGB.
function LampTile(props: { lamp: Lamp }) {
  const color = () => (props.lamp.rgb !== undefined ? hex(props.lamp.rgb) : undefined);
  const pct = () => Math.max(1, Math.round(((props.lamp.brightness ?? 0) / 255) * 100));
  const status = () =>
    !props.lamp.available ? t("unavailable") : props.lamp.isOn ? (props.lamp.supportsBrightness ? `${pct()} %` : t("on")) : t("off");
  const set = (cmd: { on?: boolean; brightness?: number; rgb?: number }) =>
    send({ light: { entityId: props.lamp.entityId, ...cmd } });

  return (
    <div
      class="lamp"
      classList={{ on: props.lamp.isOn, disabled: !props.lamp.available }}
      style={{ "--lamp-color": props.lamp.isOn && color() ? color() : undefined }}
    >
      <div class="lamp-row">
        <span class="lamp-icon">
          <Icon name={props.lamp.isOn ? "lightbulb_fill" : "lightbulb"} />
        </span>
        <div class="lamp-text">
          <div class="title-medium ellipsis">{props.lamp.name}</div>
          <div class="body-medium variant">{status()}</div>
        </div>
        <Show when={props.lamp.supportsColor && props.lamp.available}>
          <label class="color-button" title={t("pickColor")}>
            <span class="color-swatch" style={{ background: color() ?? "var(--md-surface-container-highest)" }} />
            <input type="color" value={color() ?? "#ffffff"} onChange={(e) => set({ rgb: fromHex(e.currentTarget.value) })} />
          </label>
        </Show>
        <button
          role="switch"
          class="switch"
          aria-checked={props.lamp.isOn}
          aria-label={t("switchLamp")(props.lamp.name)}
          disabled={!props.lamp.available}
          onClick={() => set({ on: !props.lamp.isOn })}
        >
          <span class="switch-handle">
            <Show when={props.lamp.isOn}>
              <Icon name="check" size={16} />
            </Show>
          </span>
        </button>
      </div>
      <Show when={props.lamp.supportsBrightness && props.lamp.available && props.lamp.isOn}>
        <input
          class="slider"
          type="range"
          min="1"
          max="255"
          value={props.lamp.brightness ?? 255}
          style={{ "--pct": `${(((props.lamp.brightness ?? 255) - 1) / 254) * 100}%` }}
          onInput={(e) => e.currentTarget.style.setProperty("--pct", `${((+e.currentTarget.value - 1) / 254) * 100}%`)}
          onChange={(e) => set({ brightness: +e.currentTarget.value })}
          aria-label={t("brightness")}
        />
      </Show>
    </div>
  );
}
