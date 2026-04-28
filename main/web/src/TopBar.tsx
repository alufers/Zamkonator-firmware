import { Clock, ClockArrowUp, HardDrive, LogOut, Server } from "lucide-preact";
import { Button } from "./ui/Button";
import { Chip } from "./ui/Chip";
import { StatusPayload, MqttStatus } from "./wsTypes";

function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m ${s}s`;
}

const MQTT_CHIP: Record<MqttStatus, { label: string; cls: string }> = {
  unconfigured: { label: "MQTT off",          cls: "text-zinc-500 border-zinc-700" },
  connecting:   { label: "MQTT connecting",   cls: "text-amber-400 border-amber-900" },
  connected:    { label: "MQTT",              cls: "text-green-400 border-green-900" },
  disconnected: { label: "MQTT disconnected", cls: "text-red-400 border-red-900" },
};

interface TopBarProps {
  status: StatusPayload | null;
  mqttStatus: MqttStatus | null;
  onLogout?: () => void;
}

export function TopBar({ status, mqttStatus, onLogout }: TopBarProps) {
  const mqttChip = mqttStatus ? MQTT_CHIP[mqttStatus] : null;
  const uptimeStr = status ? formatUptime(status.uptime) : null;
  const timeStr =
    status && status.time > 0 ? new Date(status.time * 1000).toLocaleTimeString() : null;

  return (
    <div class="flex items-center px-3 py-2.5 border-b border-zinc-800 shrink-0 gap-2">
      <span class="flex-1 font-bold tracking-wide text-sm">Zamkonator</span>

      {timeStr && (
        <Chip title="Local time">
          <Clock size={13} class="shrink-0" />
          {timeStr}
        </Chip>
      )}

      {uptimeStr && (
        <Chip title="Uptime since last boot">
          <ClockArrowUp size={13} class="shrink-0" />
          {uptimeStr}
        </Chip>
      )}

      {mqttChip && (
        <Chip class={mqttChip.cls} title={mqttChip.label}>
          <Server size={13} class="shrink-0" />
          {mqttChip.label}
        </Chip>
      )}

      {status && (
        <Chip
          class={
            status.sd_card_mounted
              ? "text-green-400 border-green-900"
              : "text-red-400 border-red-900"
          }
          title={status.sd_card_mounted ? "SD card mounted" : "SD card not mounted"}
        >
          <HardDrive size={13} class="shrink-0" />
          {status.sd_card_mounted ? "SD" : "No SD"}
        </Chip>
      )}

      {onLogout && (
        <>
          <div class="w-px h-5 bg-zinc-700 shrink-0" />
          <Button variant="ghost" onClick={onLogout} title="Log out" class="flex items-center gap-1.5">
            <LogOut size={13} class="shrink-0" />
            Log out
          </Button>
        </>
      )}
    </div>
  );
}
