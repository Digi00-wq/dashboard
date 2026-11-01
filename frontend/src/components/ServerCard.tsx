import { Show } from "solid-js";
import type { Server } from "../proto/dashboard";
import { bytes, duration } from "../format";
import { Card, Empty, Progress } from "./Card";
import { Icon } from "./Icon";
import { t } from "../i18n";

export function ServerCard(props: { server?: Server }) {
  return (
    <Card title={props.server?.hostname || t("server")} class="server-card">
      <Show when={props.server} fallback={<Empty>{t("noData")}</Empty>}>
        {(s) => (
          <>
            <div class="chips server-chips">
              <span class="chip">
                <Icon name="schedule" size={18} />
                {duration(s().uptimeSeconds)}
              </span>
              <span class="chip">Load {s().load1.toFixed(2)}</span>
              <Show when={s().temperature !== undefined}>
                <span class="chip">
                  <Icon name="device_thermostat" size={18} />
                  {Math.round(s().temperature!)} °C
                </span>
              </Show>
            </div>
            <div class="progress-list">
              <Progress label={t("cpu")} value={s().cpuPercent} max={100} text={`${Math.round(s().cpuPercent)} %`} />
              <Progress label={t("memory")} value={s().memUsed} max={s().memTotal} text={`${bytes(s().memUsed)} ${t("of")} ${bytes(s().memTotal)}`} />
              <Progress label={t("disk")} value={s().diskUsed} max={s().diskTotal} text={`${bytes(s().diskUsed)} ${t("of")} ${bytes(s().diskTotal)}`} />
            </div>
          </>
        )}
      </Show>
    </Card>
  );
}
