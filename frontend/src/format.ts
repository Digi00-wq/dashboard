import type { IconName } from "./components/Icon";
import { locale, t } from "./i18n";

export { weatherText } from "./i18n";

const WEATHER_ICON: Record<string, IconName> = {
  "clear-night": "bedtime",
  cloudy: "cloud",
  exceptional: "warning",
  fog: "foggy",
  hail: "weather_hail",
  lightning: "thunderstorm",
  "lightning-rainy": "thunderstorm",
  partlycloudy: "partly_cloudy_day",
  pouring: "rainy_heavy",
  rainy: "rainy",
  snowy: "weather_snowy",
  "snowy-rainy": "weather_mix",
  sunny: "sunny",
  windy: "air",
  "windy-variant": "air",
};

export const weatherIcon = (c: string): IconName => WEATHER_ICON[c] ?? "device_thermostat";

export function bytes(n: number): string {
  const units = ["B", "KB", "MB", "GB", "TB"];
  let i = 0;
  while (n >= 1024 && i < units.length - 1) {
    n /= 1024;
    i++;
  }
  return `${n.toLocaleString(locale(), { maximumFractionDigits: n >= 100 || i === 0 ? 0 : 1 })} ${units[i]}`;
}

export function duration(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d} ${t("dayShort")} ${h} h`;
  if (h > 0) return `${h} h ${m} min`;
  return `${m} min`;
}

/** Minutes -> "7 h 32 min" */
export const hm = (min: number) =>
  min >= 60 ? `${Math.floor(min / 60)} h ${String(min % 60).padStart(2, "0")} min` : `${min} min`;

export function ago(ms: number, now: number): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 60) return t("justNow");
  if (s < 3600) return t("minutesAgo")(Math.floor(s / 60));
  if (s < 86400) return t("hoursAgo")(Math.floor(s / 3600));
  return t("daysAgo")(Math.floor(s / 86400));
}

export function greeting(now: number): string {
  const h = new Date(now).getHours();
  if (h < 5) return t("greetingNight");
  if (h < 11) return t("greetingMorning");
  if (h < 18) return t("greetingDay");
  return t("greetingEvening");
}

export const clock = (ms: number) => new Date(ms).toLocaleTimeString(locale(), { hour: "2-digit", minute: "2-digit" });
export const longDate = (ms: number) =>
  new Date(ms).toLocaleDateString(locale(), { weekday: "long", day: "numeric", month: "long" });
export const weekday = (ms: number) => new Date(ms).toLocaleDateString(locale(), { weekday: "short" }).replace(".", "");
export const number = (n: number) => n.toLocaleString(locale());

export const hex = (rgb: number) => "#" + rgb.toString(16).padStart(6, "0");
export const fromHex = (h: string) => parseInt(h.slice(1), 16);

/** "2026-11-01" or ISO datetime -> label + whether it is overdue/today. */
export function dueLabel(due: string, now: number): { text: string; state: "overdue" | "today" | "future" } {
  const hasTime = due.includes("T");
  const date = hasTime ? new Date(due) : new Date(due + "T00:00:00");
  const today = new Date(now);
  today.setHours(0, 0, 0, 0);
  const day = new Date(date);
  day.setHours(0, 0, 0, 0);
  const diffDays = Math.round((day.getTime() - today.getTime()) / 86400000);
  const time = hasTime ? ", " + clock(date.getTime()) : "";

  let text: string;
  if (diffDays === 0) text = t("today") + time;
  else if (diffDays === 1) text = t("tomorrow") + time;
  else if (diffDays === -1) text = t("yesterday") + time;
  else text = date.toLocaleDateString(locale(), { weekday: "short", day: "numeric", month: "short" }) + time;

  const overdue = hasTime ? date.getTime() < now : diffDays < 0;
  return { text, state: overdue ? "overdue" : diffDays === 0 ? "today" : "future" };
}

/** Very simple clothing hint from temperature and rain. */
export function clothing(temp: number | undefined, condition: string, rainChance?: number) {
  if (temp === undefined) return undefined;
  const outfit =
    temp < 0
      ? t("outfitFreezing")
      : temp < 8
        ? t("outfitCold")
        : temp < 15
          ? t("outfitCool")
          : temp < 22
            ? t("outfitMild")
            : t("outfitWarm");
  const wet = ["rainy", "pouring", "lightning-rainy", "snowy-rainy", "hail"].includes(condition);
  return { outfit, umbrella: wet || (rainChance ?? 0) >= 50 };
}
