// Weather card: current conditions, forecast days and a clothing/umbrella hint.

import { For, Show } from "solid-js";
import type { Weather } from "../proto/dashboard";
import { clothing, weatherIcon, weatherText, weekday } from "../format";
import { Card, Empty } from "./Card";
import { Icon } from "./Icon";
import { t } from "../i18n";

// Rounded number or "–".
const round = (n?: number) => (n === undefined ? "–" : Math.round(n).toString());

// Data from HA_WEATHER_ENTITY.
export function WeatherCard(props: { weather?: Weather }) {
  return (
    <Show
      when={props.weather}
      fallback={
        <Card title={t("weather")} class="weather-card">
          <Empty>{t("noWeather")}</Empty>
        </Card>
      }
    >
      {(w) => {
        const hint = () => clothing(w().temperature, w().condition, w().forecast[0]?.precipitationProbability);
        return (
          <section class="card weather weather-card">
            <div class="weather-now">
              <div>
                <div class="label-large weather-place">{t("now")}</div>
                <div class="display-large">
                  {round(w().temperature)}
                  <span class="weather-unit">{w().temperatureUnit || "°C"}</span>
                </div>
                <div class="title-medium">{weatherText(w().condition)}</div>
              </div>
              <Icon name={weatherIcon(w().condition)} size={88} class="weather-icon" />
            </div>

            <div class="chips">
              <Show when={w().humidity !== undefined}>
                <span class="chip">
                  <Icon name="water_drop" size={18} />
                  {round(w().humidity)} %
                </span>
              </Show>
              <Show when={w().windSpeed !== undefined}>
                <span class="chip">
                  <Icon name="air" size={18} />
                  {round(w().windSpeed)} {w().windSpeedUnit}
                </span>
              </Show>
              <Show when={hint()}>
                {(h) => (
                  <>
                    <span class="chip">
                      <Icon name="checkroom" size={18} />
                      {h().outfit}
                    </span>
                    <Show when={h().umbrella}>
                      <span class="chip">
                        <Icon name="umbrella" size={18} />
                        {t("umbrella")}
                      </span>
                    </Show>
                  </>
                )}
              </Show>
            </div>

            <Show when={w().forecast.length > 0}>
              <div class="forecast">
                <For each={w().forecast}>
                  {(f, i) => (
                    <div class="forecast-day">
                      <span class="label-medium">{i() === 0 ? t("today") : weekday(f.time)}</span>
                      <Icon name={weatherIcon(f.condition)} size={28} />
                      <span class="title-small">{round(f.temperature)}°</span>
                      <span class="body-small dim">{round(f.tempLow)}°</span>
                      <span class="label-small rain">
                        {f.precipitationProbability !== undefined ? `${f.precipitationProbability} %` : " "}
                      </span>
                    </div>
                  )}
                </For>
              </div>
            </Show>
          </section>
        );
      }}
    </Show>
  );
}
