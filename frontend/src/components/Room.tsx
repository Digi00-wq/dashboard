import { Show } from "solid-js";
import type { Light, Presence } from "../proto/dashboard";
import { ago } from "../format";
import { Card, Empty, Tile } from "./Card";
import { t } from "../i18n";

export function Room(props: { presence?: Presence; light?: Light; now: number }) {
  return (
    <Card title={t("room")} class="room-card">
      <Show when={props.presence || props.light} fallback={<Empty>{t("noSensors")}</Empty>}>
        <div class="tiles">
          <Show when={props.presence}>
            {(p) => (
              <Tile
                icon={p().isNow ? "person" : "person_off"}
                tone={p().isNow ? "primary" : "neutral"}
                value={p().isNow ? t("present") : t("away")}
                label={
                  p().isNow
                    ? t("someoneHere")
                    : p().lastSeen
                      ? t("lastSeen")(ago(p().lastSeen!, props.now))
                      : t("lastSeenUnknown")
                }
              />
            )}
          </Show>
          <Show when={props.light}>
            {(l) => (
              <Tile
                icon={l().isLight ? "light_mode" : "dark_mode"}
                tone={l().isLight ? "tertiary" : "neutral"}
                value={l().isLight ? t("bright") : t("dark")}
                label={l().luxValue !== undefined ? `${l().luxValue} lx` : t("lightSensor")}
              />
            )}
          </Show>
        </div>
      </Show>
    </Card>
  );
}
