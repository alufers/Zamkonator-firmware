import { useContext, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { Button } from "./ui/Button";
import { Modal } from "./ui/Modal";

type ControlAction = "open" | "beep" | "led";

interface ActionDef {
  id: ControlAction;
  title: string;
  description: string;
  button: string;
  variant: "danger" | "secondary";
  /** Empty means "let the device pick its default". */
  defaultMs: string;
  placeholder: string;
}

const ACTIONS: ActionDef[] = [
  {
    id: "open",
    title: "Lock",
    description: "",
    button: "Open lock",
    variant: "danger",
    defaultMs: "",
    placeholder: "relay_open_ms",
  },
  {
    id: "beep",
    title: "Beeper",
    description: "Sound the reader beeper.",
    button: "Beep",
    variant: "secondary",
    defaultMs: "200",
    placeholder: "200",
  },
  {
    id: "led",
    title: "Reader LED",
    description: "Turn on the reader LED.",
    button: "Turn on LED",
    variant: "secondary",
    defaultMs: "3000",
    placeholder: "3000",
  },
];

type Result = { ok: true } | { ok: false; error: string };

export function Control() {
  const { password } = useContext(AuthContext);
  const [durations, setDurations] = useState<Record<ControlAction, string>>(
    () =>
      Object.fromEntries(ACTIONS.map((a) => [a.id, a.defaultMs])) as Record<ControlAction, string>,
  );
  const [busy, setBusy] = useState<ControlAction | null>(null);
  const [results, setResults] = useState<Partial<Record<ControlAction, Result>>>({});
  const [confirmReboot, setConfirmReboot] = useState(false);
  const [reboot, setReboot] = useState<Result | null>(null);

  const send = async (action: ControlAction) => {
    const ms = durations[action].trim();
    const query = ms ? `?duration_ms=${encodeURIComponent(ms)}` : "";
    const headers: Record<string, string> = password ? { "X-Auth": password } : {};

    setBusy(action);
    let result: Result;
    try {
      const res = await fetch(`/api/control/${action}${query}`, { method: "POST", headers });
      result = res.ok ? { ok: true } : { ok: false, error: `${res.status}: ${await res.text()}` };
    } catch {
      result = { ok: false, error: "Network error" };
    }
    setResults((prev) => ({ ...prev, [action]: result }));
    setBusy(null);
  };

  const doReboot = async () => {
    setConfirmReboot(false);
    const headers: Record<string, string> = password ? { "X-Auth": password } : {};
    try {
      const res = await fetch("/api/control/reboot", { method: "POST", headers });
      setReboot(res.ok ? { ok: true } : { ok: false, error: `${res.status}: ${await res.text()}` });
    } catch {
      setReboot({ ok: false, error: "Network error" });
    }
  };

  return (
    <div class="flex flex-col h-full overflow-hidden">
      <div class="flex-1 overflow-y-auto p-4 flex flex-col gap-4 max-w-xl">
        {ACTIONS.map((a) => {
          const result = results[a.id];
          return (
            <div key={a.id} class="border border-zinc-800 rounded p-3 flex flex-col gap-2">
              <div class="text-sm text-zinc-200 font-medium">{a.title}</div>
              <p class="text-xs text-zinc-400">{a.description}</p>
              <div class="flex items-center gap-2">
                <input
                  type="number"
                  min={0}
                  value={durations[a.id]}
                  placeholder={a.placeholder}
                  onInput={(e) =>
                    setDurations({ ...durations, [a.id]: (e.target as HTMLInputElement).value })
                  }
                  class="w-28 bg-zinc-900 border border-zinc-700 rounded px-2 py-1 text-xs text-zinc-200"
                />
                <span class="text-xs text-zinc-500">ms</span>
                <Button
                  variant={a.variant}
                  disabled={busy !== null}
                  onClick={() => void send(a.id)}
                >
                  {a.button}
                </Button>
                {result &&
                  (result.ok ? (
                    <span class="text-xs text-green-400">ok</span>
                  ) : (
                    <span class="text-xs text-red-400">{result.error}</span>
                  ))}
              </div>
            </div>
          );
        })}
        <div class="border border-zinc-800 rounded p-3 flex flex-col gap-2">
          <div class="text-sm text-zinc-200 font-medium">Device</div>
          <div class="flex items-center gap-2">
            <Button variant="danger" onClick={() => setConfirmReboot(true)}>
              Reboot
            </Button>
            {reboot &&
              (reboot.ok ? (
                <span class="text-xs text-green-400">Rebooting....</span>
              ) : (
                <span class="text-xs text-red-400">{reboot.error}</span>
              ))}
          </div>
        </div>
      </div>

      {confirmReboot && (
        <Modal
          title="Reboot device?"
          okLabel="Reboot"
          onOk={() => void doReboot()}
          onCancel={() => setConfirmReboot(false)}
        >
          
        </Modal>
      )}
    </div>
  );
}
