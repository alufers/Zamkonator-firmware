import { useContext, useRef, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { Button } from "./ui/Button";

type OtaPhase = "idle" | "uploading" | "done" | "error";

export function OTA() {
  const { password } = useContext(AuthContext);
  const fileRef = useRef<HTMLInputElement>(null);
  const xhrRef = useRef<XMLHttpRequest | null>(null);

  const [phase, setPhase] = useState<OtaPhase>("idle");
  const [progress, setProgress] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [filename, setFilename] = useState<string>("");

  const upload = () => {
    const file = fileRef.current?.files?.[0];
    if (!file) return;

    setFilename(file.name);
    setPhase("uploading");
    setProgress(0);
    setError(null);

    const xhr = new XMLHttpRequest();
    xhrRef.current = xhr;
    xhr.open("POST", "/api/ota/upload");
    if (password) xhr.setRequestHeader("X-Auth", password);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");

    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        setProgress(Math.round((e.loaded / e.total) * 100));
      }
    };

    xhr.onload = () => {
      if (xhr.status === 200) {
        setPhase("done");
      } else {
        setPhase("error");
        setError(`Server error ${xhr.status}: ${xhr.responseText}`);
      }
    };

    xhr.onerror = () => {
      setPhase("error");
      setError("Network error or connection lost during upload");
    };

    xhr.send(file);
  };

  const reset = () => {
    xhrRef.current?.abort();
    xhrRef.current = null;
    setPhase("idle");
    setProgress(0);
    setError(null);
    if (fileRef.current) fileRef.current.value = "";
  };

  return (
    <div class="relative flex flex-col h-full overflow-hidden">
      <div class="flex-1 overflow-y-auto p-4 flex flex-col gap-4 max-w-xl">
        <p class="text-xs text-zinc-400">
          Upload a new firmware binary (.bin) to update the device over-the-air.
          The device will reboot automatically after a successful upload.
        </p>
        <p class="text-xs text-yellow-500/80">
          The device will roll back to the previous firmware if it fails to boot
          and obtain a network address within 20 seconds of starting.
        </p>

        <div class="flex flex-col gap-3">
          <input
            ref={fileRef}
            type="file"
            accept=".bin"
            disabled={phase === "uploading"}
            class="text-xs text-zinc-300 file:mr-3 file:py-1 file:px-3 file:rounded file:border-0 file:text-xs file:bg-zinc-700 file:text-zinc-200 file:cursor-pointer disabled:opacity-50"
          />
          <div class="flex">
            <Button onClick={upload} disabled={phase === "uploading"}>
              Upload Firmware
            </Button>
          </div>
        </div>
      </div>

      {phase !== "idle" && (
        <div class="absolute inset-0 flex items-center justify-center bg-black/60 z-10">
          <div class="bg-zinc-900 border border-zinc-700 rounded p-6 flex flex-col gap-3 min-w-56 max-w-xs w-full mx-4">
            <p
              class="text-sm text-zinc-200 font-medium truncate"
              title={filename}
            >
              {filename}
            </p>

            {phase === "error" ? (
              <p class="text-xs text-red-400">{error}</p>
            ) : phase === "done" ? (
              <p class="text-xs text-green-400">
                Update complete. Device is rebooting — reconnect in ~10 seconds.
              </p>
            ) : (
              <>
                <div class="bg-zinc-700 rounded-full h-1.5 overflow-hidden">
                  <div
                    class="bg-blue-500 h-1.5 rounded-full transition-all duration-200"
                    style={{ width: `${progress}%` }}
                  />
                </div>
                <p class="text-xs text-zinc-500">
                  {progress < 100 ? `Uploading… ${progress}%` : "Verifying…"}
                </p>
              </>
            )}

            {(phase === "done" || phase === "error") && (
              <Button onClick={reset}>Close</Button>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
