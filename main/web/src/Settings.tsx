import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { Button } from "./ui/Button";
import { Modal } from "./ui/Modal";

type DigitalInputMode =
  | "none"
  | "push_to_exit"
  | "door_close_sensor"
  | "door_lock_sensor"
  | "generic";

interface DigitalInputConfig {
  mode: DigitalInputMode;
  inverted: boolean;
  debounce_ms: number;
}

interface MqttConfig {
  enabled: boolean;
  broker: string;
  port: number;
  username: string;
  password: string;
  mqtt_prefix: string;
}

export interface SettingsData {
  hostname: string;
  auth_proxy_base_url: string;
  auth_proxy_timeout_ms: number;
  auth_proxy_healthcheck_interval_ms: number;
  auth_proxy_cache_enabled: boolean;
  auth_proxy_cache_refresh_ms: number;
  relay_open_ms: number;
  mqtt: MqttConfig;
  web_password_enabled: boolean;
  web_password: string;
  remote_open_password: string;
  input_inp1: DigitalInputConfig;
  input_inp2: DigitalInputConfig;
  input_inp3: DigitalInputConfig;
}

type DigitalInputKey = "input_inp1" | "input_inp2" | "input_inp3";

const DIGITAL_INPUTS: { key: DigitalInputKey; label: string; pin: string }[] = [
  { key: "input_inp1", label: "INP1", pin: "P0.5" },
  { key: "input_inp2", label: "INP2", pin: "P0.6" },
  { key: "input_inp3", label: "INP3", pin: "P0.7" },
];

function DigitalInput({
  label,
  pin,
  value,
  onChange,
}: {
  label: string;
  pin: string;
  value: DigitalInputConfig;
  onChange: (next: DigitalInputConfig) => void;
}) {
  return (
    <div class="mb-4 last:mb-0">
      <p class="text-zinc-500 text-xs mb-3">
        {label} ({pin})
      </p>

      <div class="flex gap-4 flex-wrap mb-3">
        <div>
          <label class="block mb-1 text-xs text-zinc-400">Mode</label>
          <select
            value={value.mode}
            onChange={(e) =>
              onChange({ ...value, mode: (e.target as HTMLSelectElement).value as DigitalInputMode })
            }
            class="bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
          >
            <option value="none">None</option>
            <option value="push_to_exit">Push to Exit</option>
            <option value="door_close_sensor">Door Close Sensor</option>
            <option value="door_lock_sensor">Door Lock Sensor</option>
            <option value="generic">Generic (report only)</option>
          </select>
        </div>

        <div>
          <label class="block mb-1 text-xs text-zinc-400">Debounce</label>
          <div class="flex items-center gap-1">
            <input
              type="number"
              value={value.debounce_ms}
              min={0}
              max={5000}
              onInput={(e) =>
                onChange({
                  ...value,
                  debounce_ms: Math.min(5000, Math.max(0, parseInt((e.target as HTMLInputElement).value) || 300)),
                })
              }
              class="w-24 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
            />
            <span class="text-zinc-500 text-xs">ms</span>
          </div>
        </div>
      </div>

      <label class="flex items-center gap-2 cursor-pointer select-none">
        <input
          type="checkbox"
          checked={value.inverted}
          onChange={(e) => onChange({ ...value, inverted: (e.target as HTMLInputElement).checked })}
          class="w-4 h-4 accent-blue-500"
        />
        <span class="text-xs text-zinc-300">Inverted (active-low signal)</span>
      </label>
    </div>
  );
}

type SaveStatus = "idle" | "loading" | "saving" | "saved" | "rebooting" | "error";

export function Settings() {
  const { password } = useContext(AuthContext);
  const [draft, setDraft] = useState<SettingsData | null>(null);
  const [saveStatus, setSaveStatus] = useState<SaveStatus>("loading");
  const fileInputRef = useRef<HTMLInputElement>(null);
  const [showRestoreConfirm, setShowRestoreConfirm] = useState(false);
  const [restoreFileContent, setRestoreFileContent] = useState<string | null>(null);
  const [restoreFileName, setRestoreFileName] = useState<string>("");

  const authHeaders: Record<string, string> = password ? { "X-Auth": password } : {};

  useEffect(() => {
    fetch("/api/settings", { headers: authHeaders })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json() as Promise<SettingsData>;
      })
      .then((data) => {
        if (data.web_password_enabled && !data.web_password) {
          data.web_password = "***UNCHANGED***";
        }
        setDraft(data);
        setSaveStatus("idle");
      })
      .catch(() => setSaveStatus("error"));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const downloadBackup = () => {
    const now = new Date();
    const dateStr = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, "0")}-${String(now.getDate()).padStart(2, "0")}`;
    const timeStr = `${String(now.getHours()).padStart(2, "0")}${String(now.getMinutes()).padStart(2, "0")}`;
    const hostname = draft?.hostname ?? "zamkonator";
    const filename = `${hostname}-backup-${dateStr}-${timeStr}.json`;

    fetch("/api/backup", { headers: authHeaders })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.blob();
      })
      .then((blob) => {
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = filename;
        a.click();
        URL.revokeObjectURL(url);
      })
      .catch(() => alert("Backup failed — check connection"));
  };

  const handleRestoreFileChange = (e: Event) => {
    const file = (e.target as HTMLInputElement).files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.addEventListener("load", () => {
      setRestoreFileName(file.name);
      setRestoreFileContent(reader.result as string);
      setShowRestoreConfirm(true);
    });
    reader.readAsText(file);
    (e.target as HTMLInputElement).value = "";
  };

  const confirmRestore = () => {
    if (!restoreFileContent) return;
    setShowRestoreConfirm(false);
    setSaveStatus("saving");
    fetch("/api/restore", {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: restoreFileContent,
    })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        setSaveStatus("rebooting");
      })
      .catch(() => setSaveStatus("error"))
      .finally(() => {
        setRestoreFileContent(null);
        setRestoreFileName("");
      });
  };

  const save = () => {
    if (!draft) return;
    setSaveStatus("saving");
    fetch("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: JSON.stringify(draft),
    })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json() as Promise<SettingsData>;
      })
      .then((data) => {
        if (data.web_password_enabled && !data.web_password) {
          data.web_password = "***UNCHANGED***";
        }
        setDraft(data);
        setSaveStatus("saved");
        setTimeout(() => setSaveStatus("idle"), 2500);
      })
      .catch(() => setSaveStatus("error"));
  };

  const updateMqtt = <K extends keyof MqttConfig>(key: K, value: MqttConfig[K]) => {
    if (!draft) return;
    setDraft({ ...draft, mqtt: { ...draft.mqtt, [key]: value } });
  };

  if (saveStatus === "loading" || !draft) {
    return (
      <div class="h-full flex items-center justify-center text-zinc-500 text-xs">
        {saveStatus === "error" ? "Failed to load settings." : "Loading settings…"}
      </div>
    );
  }

  const mqttDisabled = !draft.mqtt.enabled;

  return (
    <>
      <div class="p-4 overflow-y-auto h-full">
        <div class="max-w-lg mx-auto">

          {/* ── Network ──────────────────────────────────────────────── */}
          <section class="mb-6">
            <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
              Network
            </h3>
            <label class="block mb-1 text-xs text-zinc-400">Hostname</label>
            <input
              type="text"
              value={draft.hostname}
              maxLength={63}
              onInput={(e) =>
                setDraft({ ...draft, hostname: (e.target as HTMLInputElement).value })
              }
              class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-1"
            />
            <p class="text-zinc-600 text-xs">
              Reachable via mDNS as{" "}
              <span class="text-zinc-400 font-mono">{draft.hostname}.local</span>
            </p>
          </section>

          {/* ── Access Control ───────────────────────────────────────── */}
          <section class="mb-6">
            <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
              Access Control
            </h3>

            <label class="block mb-1 text-xs text-zinc-400">Auth proxy URL</label>
            <input
              type="text"
              value={draft.auth_proxy_base_url}
              placeholder="http://auth-proxy.local:8000"
              onInput={(e) =>
                setDraft({ ...draft, auth_proxy_base_url: (e.target as HTMLInputElement).value })
              }
              class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-1"
            />
            <p class="text-zinc-600 text-xs mb-3">
              Leave blank to disable card authentication.
            </p>

            <div class="flex gap-4 flex-wrap">
              <div>
                <label class="block mb-1 text-xs text-zinc-400">Auth proxy timeout</label>
                <div class="flex items-center gap-1">
                  <input
                    type="number"
                    value={draft.auth_proxy_timeout_ms}
                    min={100}
                    max={60000}
                    onInput={(e) =>
                      setDraft({
                        ...draft,
                        auth_proxy_timeout_ms: Math.min(60000, Math.max(100, parseInt((e.target as HTMLInputElement).value) || 10000)),
                      })
                    }
                    class="w-28 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                  />
                  <span class="text-zinc-500 text-xs">ms</span>
                </div>
              </div>
              <div>
                <label class="block mb-1 text-xs text-zinc-400">Health check interval</label>
                <div class="flex items-center gap-1">
                  <input
                    type="number"
                    value={draft.auth_proxy_healthcheck_interval_ms}
                    min={1000}
                    max={300000}
                    onInput={(e) =>
                      setDraft({
                        ...draft,
                        auth_proxy_healthcheck_interval_ms: Math.min(300000, Math.max(1000, parseInt((e.target as HTMLInputElement).value) || 30000)),
                      })
                    }
                    class="w-28 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                  />
                  <span class="text-zinc-500 text-xs">ms</span>
                </div>
              </div>
              <div>
                <label class="block mb-1 text-xs text-zinc-400">Relay open time</label>
                <div class="flex items-center gap-1">
                  <input
                    type="number"
                    value={draft.relay_open_ms}
                    min={100}
                    max={60000}
                    onInput={(e) =>
                      setDraft({
                        ...draft,
                        relay_open_ms: Math.min(60000, Math.max(100, parseInt((e.target as HTMLInputElement).value) || 8000)),
                      })
                    }
                    class="w-28 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                  />
                  <span class="text-zinc-500 text-xs">ms</span>
                </div>
              </div>
            </div>

            {/* ── Offline cache ──────────────────────────────────────── */}
            <label class="flex items-center gap-2 mt-4 mb-1 cursor-pointer select-none">
              <input
                type="checkbox"
                checked={draft.auth_proxy_cache_enabled}
                onChange={(e) =>
                  setDraft({
                    ...draft,
                    auth_proxy_cache_enabled: (e.target as HTMLInputElement).checked,
                  })
                }
                class="w-4 h-4 accent-blue-500"
              />
              <span class="text-xs text-zinc-300">Use cached user database when proxy is down</span>
            </label>
            <p class="text-zinc-600 text-xs mb-3">
              Downloads the user list to{" "}
              <span class="text-zinc-400 font-mono">/zamkonator_users.dat</span> on the SD card
              and falls back to it when the proxy is unreachable. Cards that the proxy actively
              denies are never re-checked against the cache.
            </p>

            <div class={draft.auth_proxy_cache_enabled ? "" : "opacity-40 pointer-events-none"}>
              <label class="block mb-1 text-xs text-zinc-400">Cache refresh interval</label>
              <div class="flex items-center gap-1">
                <input
                  type="number"
                  value={draft.auth_proxy_cache_refresh_ms}
                  min={60000}
                  max={86400000}
                  onInput={(e) =>
                    setDraft({
                      ...draft,
                      auth_proxy_cache_refresh_ms: Math.min(
                        86400000,
                        Math.max(60000, parseInt((e.target as HTMLInputElement).value) || 600000),
                      ),
                    })
                  }
                  class="w-32 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                />
                <span class="text-zinc-500 text-xs">ms</span>
              </div>
            </div>
          </section>

          {/* ── Digital Inputs ───────────────────────────────────────── */}
          <section class="mb-6">
            <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
              Digital Inputs
            </h3>

            {DIGITAL_INPUTS.map(({ key, label, pin }) => (
              <DigitalInput
                key={key}
                label={label}
                pin={pin}
                value={draft[key]}
                onChange={(next) => setDraft({ ...draft, [key]: next })}
              />
            ))}
          </section>

          {/* ── MQTT ─────────────────────────────────────────────────── */}
          <section class="mb-6">
            <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">MQTT</h3>

            <label class="flex items-center gap-2 mb-4 cursor-pointer select-none">
              <input
                type="checkbox"
                checked={draft.mqtt.enabled}
                onChange={(e) => updateMqtt("enabled", (e.target as HTMLInputElement).checked)}
                class="w-4 h-4 accent-blue-500"
              />
              <span class="text-xs text-zinc-300">Enable MQTT</span>
            </label>

            <div class={mqttDisabled ? "opacity-40 pointer-events-none" : ""}>
              <label class="block mb-1 text-xs text-zinc-400">Broker host</label>
              <input
                type="text"
                value={draft.mqtt.broker}
                placeholder="192.168.1.100 or mqtt.example.com"
                onInput={(e) => updateMqtt("broker", (e.target as HTMLInputElement).value)}
                class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
              />

              <label class="block mb-1 text-xs text-zinc-400">Port</label>
              <input
                type="number"
                value={draft.mqtt.port}
                min={1}
                max={65535}
                onInput={(e) =>
                  updateMqtt(
                    "port",
                    Math.min(65535, Math.max(1, parseInt((e.target as HTMLInputElement).value) || 1883)),
                  )
                }
                class="w-32 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
              />

              <label class="block mb-1 text-xs text-zinc-400">Username</label>
              <input
                type="text"
                value={draft.mqtt.username}
                onInput={(e) => updateMqtt("username", (e.target as HTMLInputElement).value)}
                class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
              />

              <label class="block mb-1 text-xs text-zinc-400">Password</label>
              <input
                type="password"
                value={draft.mqtt.password}
                onInput={(e) => updateMqtt("password", (e.target as HTMLInputElement).value)}
                class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
              />

              <label class="block mb-1 text-xs text-zinc-400">MQTT prefix</label>
              <input
                type="text"
                value={draft.mqtt.mqtt_prefix}
                placeholder="zamkonator"
                onInput={(e) => updateMqtt("mqtt_prefix", (e.target as HTMLInputElement).value)}
                class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
              />
            </div>
          </section>

          {/* ── Security ─────────────────────────────────────────────── */}
          <section class="mb-6">
            <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
              Security
            </h3>
            <label class="flex items-center gap-2 mb-4 cursor-pointer select-none">
              <input
                type="checkbox"
                checked={draft.web_password_enabled}
                onChange={(e) => {
                  const enabled = (e.target as HTMLInputElement).checked;
                  setDraft({
                    ...draft,
                    web_password_enabled: enabled,
                    web_password: enabled ? "***UNCHANGED***" : "",
                  });
                }}
                class="w-4 h-4 accent-blue-500"
              />
              <span class="text-xs text-zinc-300">Enable web UI password</span>
            </label>
            {draft.web_password_enabled && (
              <div>
                <label class="block mb-1 text-xs text-zinc-400">Password</label>
                <input
                  type="password"
                  value={draft.web_password}
                  onInput={(e) =>
                    setDraft({ ...draft, web_password: (e.target as HTMLInputElement).value })
                  }
                  class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-1"
                />
                <p class="text-zinc-600 text-xs">
                  Leave unchanged to keep the current password. Clear and type a new value to change it.
                </p>
              </div>
            )}

            <div class="mt-4">
              <label class="block mb-1 text-xs text-zinc-400">Remote open password</label>
              <input
                type="password"
                value={draft.remote_open_password}
                onInput={(e) =>
                  setDraft({ ...draft, remote_open_password: (e.target as HTMLInputElement).value })
                }
                class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-1"
              />
              <p class="text-zinc-600 text-xs">
                Password for <span class="text-zinc-400 font-mono">POST /api/open</span>. Leave blank to disable remote relay opening.
              </p>
            </div>
          </section>

          {/* ── Save ─────────────────────────────────────────────────── */}
          <div class="flex items-center gap-3 flex-wrap">
            <Button
              variant="primary"
              onClick={save}
              disabled={saveStatus === "saving" || saveStatus === "rebooting"}
            >
              {saveStatus === "saving" ? "Saving…" : "Save settings"}
            </Button>
            {saveStatus === "saved" && <span class="text-green-400 text-xs">Saved!</span>}
            {saveStatus === "rebooting" && (
              <span class="text-amber-400 text-xs">Rebooting… reconnecting shortly</span>
            )}
            {saveStatus === "error" && (
              <span class="text-red-400 text-xs">Error — check connection</span>
            )}
          </div>

          {/* ── Backup / Restore ─────────────────────────────────────── */}
          <section class="mt-8 mb-4">
            <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
              Backup &amp; Restore
            </h3>
            <p class="text-zinc-500 text-xs mb-3">
              Export settings as a JSON file, or restore from a previous backup.
            </p>
            <div class="flex gap-2 flex-wrap">
              <Button onClick={downloadBackup}>Export backup</Button>
              <Button onClick={() => fileInputRef.current?.click()}>Import backup…</Button>
            </div>
            <input
              ref={fileInputRef}
              type="file"
              accept=".json,application/json"
              class="hidden"
              onChange={handleRestoreFileChange}
            />
          </section>

        </div>
      </div>

      {showRestoreConfirm && (
        <Modal
          title="Restore backup?"
          okLabel="Restore &amp; Reboot"
          onOk={confirmRestore}
          onCancel={() => {
            setShowRestoreConfirm(false);
            setRestoreFileContent(null);
            setRestoreFileName("");
          }}
        >
          <p class="text-xs text-zinc-500 mb-3 font-mono break-all">{restoreFileName}</p>
          <p class="text-sm text-zinc-300 mb-2">
            All current settings will be <strong>overwritten</strong> with the data from this
            backup. The device will reboot to apply the new configuration.
          </p>
          <p class="text-xs text-zinc-500">This action cannot be undone.</p>
        </Modal>
      )}
    </>
  );
}
