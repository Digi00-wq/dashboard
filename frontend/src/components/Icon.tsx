// Material Symbols (Rounded, weight 400) bundled at build time — works offline.
import sunny from "@material-symbols/svg-400/rounded/sunny.svg?raw";
import bedtime from "@material-symbols/svg-400/rounded/bedtime.svg?raw";
import partly_cloudy_day from "@material-symbols/svg-400/rounded/partly_cloudy_day.svg?raw";
import cloud from "@material-symbols/svg-400/rounded/cloud.svg?raw";
import foggy from "@material-symbols/svg-400/rounded/foggy.svg?raw";
import weather_hail from "@material-symbols/svg-400/rounded/weather_hail.svg?raw";
import thunderstorm from "@material-symbols/svg-400/rounded/thunderstorm.svg?raw";
import rainy from "@material-symbols/svg-400/rounded/rainy.svg?raw";
import rainy_heavy from "@material-symbols/svg-400/rounded/rainy_heavy.svg?raw";
import weather_snowy from "@material-symbols/svg-400/rounded/weather_snowy.svg?raw";
import weather_mix from "@material-symbols/svg-400/rounded/weather_mix.svg?raw";
import air from "@material-symbols/svg-400/rounded/air.svg?raw";
import warning from "@material-symbols/svg-400/rounded/warning.svg?raw";
import water_drop from "@material-symbols/svg-400/rounded/water_drop.svg?raw";
import person from "@material-symbols/svg-400/rounded/person.svg?raw";
import person_off from "@material-symbols/svg-400/rounded/person_off.svg?raw";
import light_mode from "@material-symbols/svg-400/rounded/light_mode.svg?raw";
import dark_mode from "@material-symbols/svg-400/rounded/dark_mode.svg?raw";
import lightbulb from "@material-symbols/svg-400/rounded/lightbulb.svg?raw";
import lightbulb_fill from "@material-symbols/svg-400/rounded/lightbulb-fill.svg?raw";
import directions_walk from "@material-symbols/svg-400/rounded/directions_walk.svg?raw";
import favorite from "@material-symbols/svg-400/rounded/favorite.svg?raw";
import battery_full from "@material-symbols/svg-400/rounded/battery_full.svg?raw";
import check from "@material-symbols/svg-400/rounded/check.svg?raw";
import wifi from "@material-symbols/svg-400/rounded/wifi.svg?raw";
import wifi_off from "@material-symbols/svg-400/rounded/wifi_off.svg?raw";
import sync from "@material-symbols/svg-400/rounded/sync.svg?raw";
import schedule from "@material-symbols/svg-400/rounded/schedule.svg?raw";
import checkroom from "@material-symbols/svg-400/rounded/checkroom.svg?raw";
import umbrella from "@material-symbols/svg-400/rounded/umbrella.svg?raw";
import device_thermostat from "@material-symbols/svg-400/rounded/device_thermostat.svg?raw";
import error from "@material-symbols/svg-400/rounded/error.svg?raw";
import keyboard_arrow_down from "@material-symbols/svg-400/rounded/keyboard_arrow_down.svg?raw";
import task_alt from "@material-symbols/svg-400/rounded/task_alt.svg?raw";
import alarm from "@material-symbols/svg-400/rounded/alarm.svg?raw";
import router from "@material-symbols/svg-400/rounded/router.svg?raw";
import arrow_downward from "@material-symbols/svg-400/rounded/arrow_downward.svg?raw";
import arrow_upward from "@material-symbols/svg-400/rounded/arrow_upward.svg?raw";
import music_note from "@material-symbols/svg-400/rounded/music_note.svg?raw";
import play_arrow from "@material-symbols/svg-400/rounded/play_arrow.svg?raw";
import pause from "@material-symbols/svg-400/rounded/pause.svg?raw";
import skip_next from "@material-symbols/svg-400/rounded/skip_next.svg?raw";
import skip_previous from "@material-symbols/svg-400/rounded/skip_previous.svg?raw";
import speaker from "@material-symbols/svg-400/rounded/speaker.svg?raw";
import cloud_off from "@material-symbols/svg-400/rounded/cloud_off.svg?raw";
import shuffle from "@material-symbols/svg-400/rounded/shuffle.svg?raw";
import open_in_full from "@material-symbols/svg-400/rounded/open_in_full.svg?raw";
import close_fullscreen from "@material-symbols/svg-400/rounded/close_fullscreen.svg?raw";
import volume_up from "@material-symbols/svg-400/rounded/volume_up.svg?raw";
import volume_off from "@material-symbols/svg-400/rounded/volume_off.svg?raw";
import speaker_group from "@material-symbols/svg-400/rounded/speaker_group.svg?raw";
import account_circle from "@material-symbols/svg-400/rounded/account_circle.svg?raw";
import logout from "@material-symbols/svg-400/rounded/logout.svg?raw";
import key from "@material-symbols/svg-400/rounded/key.svg?raw";
import manage_accounts from "@material-symbols/svg-400/rounded/manage_accounts.svg?raw";
import arrow_back from "@material-symbols/svg-400/rounded/arrow_back.svg?raw";
import delete_icon from "@material-symbols/svg-400/rounded/delete.svg?raw";
import person_add from "@material-symbols/svg-400/rounded/person_add.svg?raw";
import admin_panel_settings from "@material-symbols/svg-400/rounded/admin_panel_settings.svg?raw";
import lock from "@material-symbols/svg-400/rounded/lock.svg?raw";
import visibility from "@material-symbols/svg-400/rounded/visibility.svg?raw";
import visibility_off from "@material-symbols/svg-400/rounded/visibility_off.svg?raw";
import campaign from "@material-symbols/svg-400/rounded/campaign.svg?raw";
import send from "@material-symbols/svg-400/rounded/send.svg?raw";

const icons = {
  campaign,
  send,
  account_circle,
  logout,
  key,
  manage_accounts,
  arrow_back,
  delete: delete_icon,
  person_add,
  admin_panel_settings,
  lock,
  visibility,
  visibility_off,
  shuffle,
  open_in_full,
  close_fullscreen,
  volume_up,
  volume_off,
  speaker_group,
  alarm,
  router,
  arrow_downward,
  arrow_upward,
  music_note,
  play_arrow,
  pause,
  skip_next,
  skip_previous,
  speaker,
  cloud_off,
  sunny,
  bedtime,
  partly_cloudy_day,
  cloud,
  foggy,
  weather_hail,
  thunderstorm,
  rainy,
  rainy_heavy,
  weather_snowy,
  weather_mix,
  air,
  warning,
  water_drop,
  person,
  person_off,
  light_mode,
  dark_mode,
  lightbulb,
  lightbulb_fill,
  directions_walk,
  favorite,
  battery_full,
  check,
  wifi,
  wifi_off,
  sync,
  schedule,
  checkroom,
  umbrella,
  device_thermostat,
  error,
  keyboard_arrow_down,
  task_alt,
};
export type IconName = keyof typeof icons;

export function Icon(props: { name: IconName; size?: number; class?: string }) {
  return (
    <span
      class={`icon ${props.class ?? ""}`}
      style={{ "--icon-size": `${props.size ?? 24}px` }}
      aria-hidden="true"
      innerHTML={icons[props.name]}
    />
  );
}
