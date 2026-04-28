import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { Button } from "./Button";
import { Modal } from "./Modal";

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
  mqtt: MqttConfig;
  web_password_enabled: boolean;
  web_password: string;
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
