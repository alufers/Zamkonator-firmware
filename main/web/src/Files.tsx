import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { File, Folder, ArrowUp } from "lucide-preact";
import { AuthContext } from "./AuthContext";
import { StatusPayload } from "./wsTypes";
import { Button } from "./ui/Button";

type FileEntry = { name: string; size: number; is_dir: boolean };
type LoadState = "loading" | "loaded" | "error" | "not_mounted";
type DlState = { filename: string; received: number; total: number; error: string | null; done: boolean };

interface FilesProps {
  status: StatusPayload | null;
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export function Files({ status }: FilesProps) {
  const { password } = useContext(AuthContext);
  const authHeaders: Record<string, string> = password ? { "X-Auth": password } : {};

  const [currentPath, setCurrentPath] = useState("/");
  const [entries, setEntries] = useState<FileEntry[]>([]);
  const [loadState, setLoadState] = useState<LoadState>("loading");
  const [dlState, setDlState] = useState<DlState | null>(null);
  const abortRef = useRef<AbortController | null>(null);

  const fetchDir = (path: string) => {
    setLoadState("loading");
    fetch(`/api/files?path=${encodeURIComponent(path)}`, { headers: authHeaders })
      .then((r) => {
        if (r.status === 503) { setLoadState("not_mounted"); return null; }
        if (!r.ok) { setLoadState("error"); return null; }
        return r.json() as Promise<FileEntry[]>;
      })
      .then((data) => {
        if (!data) return;
        setEntries(data.sort((a, b) => {
          if (a.is_dir !== b.is_dir) return a.is_dir ? -1 : 1;
          return a.name.localeCompare(b.name);
        }));
        setLoadState("loaded");
      })
      .catch(() => setLoadState("error"));
  };

  useEffect(() => {
    fetchDir(currentPath);
  }, [currentPath]);

  /* Re-fetch when SD card becomes available */
  useEffect(() => {
    if (status?.sd_card_mounted) fetchDir(currentPath);
  }, [status?.sd_card_mounted]);

  const navigate = (dir: string) => {
    const path = currentPath === "/" ? `/${dir}` : `${currentPath}/${dir}`;
    setCurrentPath(path);
  };

  const navigateUp = () => {
    if (currentPath === "/") return;
    const parts = currentPath.split("/").filter(Boolean);
    parts.pop();
    setCurrentPath(parts.length === 0 ? "/" : "/" + parts.join("/"));
  };

  const downloadFile = async (name: string) => {
    const path = currentPath === "/" ? `/${name}` : `${currentPath}/${name}`;
    const ctrl = new AbortController();
    abortRef.current = ctrl;
    setDlState({ filename: name, received: 0, total: 0, error: null, done: false });

    try {
      const r = await fetch(`/api/files/download?path=${encodeURIComponent(path)}`, {
        headers: authHeaders,
        signal: ctrl.signal,
      });
      if (!r.ok) throw new Error("Server returned " + r.status);

      const total = parseInt(r.headers.get("Content-Length") ?? "0", 10);
      setDlState((s) => s && { ...s, total });

      const reader = r.body!.getReader();
      const chunks: Uint8Array[] = [];
      let received = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        received += value.length;
        setDlState((s) => s && { ...s, received });
      }

      const blob = new Blob(chunks);
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = name;
      a.click();
      URL.revokeObjectURL(url);
      setDlState((s) => s && { ...s, done: true });
      setTimeout(() => setDlState(null), 1500);
    } catch (err: unknown) {
      if (err instanceof DOMException && err.name === "AbortError") {
        setDlState(null);
        return;
      }
      setDlState((s) => s && { ...s, error: err instanceof Error ? err.message : "Download failed" });
    }
  };

  /* Breadcrumb */
  const parts = currentPath.split("/").filter(Boolean);
  const crumbs: { label: string; path: string }[] = [{ label: "SD card", path: "/" }];
  let acc = "";
  for (const part of parts) {
    acc += "/" + part;
    crumbs.push({ label: part, path: acc });
  }

  return (
    <div class="relative flex flex-col h-full overflow-hidden">
      {/* Breadcrumb + refresh */}
      <div class="flex items-center gap-1 px-4 py-2 border-b border-zinc-800 shrink-0 flex-wrap">
        {crumbs.map((crumb, i) => (
          <span key={crumb.path} class="flex items-center gap-1">
            {i > 0 && <span class="text-zinc-600">/</span>}
            <button
              onClick={() => setCurrentPath(crumb.path)}
              class={`text-xs bg-transparent border-0 cursor-pointer px-0 py-0 ${
                i === crumbs.length - 1
                  ? "text-zinc-200 cursor-default"
                  : "text-blue-400 hover:text-blue-300"
              }`}
              disabled={i === crumbs.length - 1}
            >
              {crumb.label}
            </button>
          </span>
        ))}
        <div class="ml-auto">
          <Button onClick={() => fetchDir(currentPath)}>Refresh</Button>
        </div>
      </div>

      {/* Content */}
      <div class="flex-1 overflow-y-auto">
        {loadState === "not_mounted" && (
          <div class="flex flex-col items-center justify-center h-full gap-2 text-zinc-500">
            <span class="text-sm">SD card not available</span>
            <Button onClick={() => fetchDir(currentPath)}>Retry</Button>
          </div>
        )}

        {loadState === "error" && (
          <div class="flex flex-col items-center justify-center h-full gap-2 text-zinc-500">
            <span class="text-sm">Failed to load directory</span>
            <Button onClick={() => fetchDir(currentPath)}>Retry</Button>
          </div>
        )}

        {loadState === "loading" && (
          <div class="flex items-center justify-center h-full text-zinc-500 text-sm">
            Loading…
          </div>
        )}

        {loadState === "loaded" && (
          <table class="w-full text-xs">
            <thead>
              <tr class="text-zinc-500 border-b border-zinc-800">
                <th class="text-left px-4 py-2 font-normal">Name</th>
                <th class="text-right px-4 py-2 font-normal">Size</th>
                <th class="px-4 py-2" />
              </tr>
            </thead>
            <tbody>
              {currentPath !== "/" && (
                <tr
                  class="border-b border-zinc-800/50 hover:bg-zinc-800/30 cursor-pointer"
                  onClick={navigateUp}
                >
                  <td class="px-4 py-2 text-zinc-400 flex items-center gap-2" colSpan={3}>
                    <ArrowUp size={14} class="shrink-0" />
                    <span>.. (up)</span>
                  </td>
                </tr>
              )}
              {entries.length === 0 && (
                <tr>
                  <td class="px-4 py-4 text-zinc-600 text-center" colSpan={3}>
                    Empty directory
                  </td>
                </tr>
              )}
              {entries.map((entry) => (
                <tr
                  key={entry.name}
                  class={`border-b border-zinc-800/50 ${entry.is_dir ? "hover:bg-zinc-800/30 cursor-pointer" : "hover:bg-zinc-800/20"}`}
                  onClick={entry.is_dir ? () => navigate(entry.name) : undefined}
                >
                  <td class="px-4 py-2 text-zinc-200">
                    <span class="inline-flex items-center gap-2">
                      {entry.is_dir
                        ? <Folder size={14} class="shrink-0 text-zinc-400" />
                        : <File size={14} class="shrink-0 text-zinc-500" />}
                      {entry.name}
                    </span>
                  </td>
                  <td class="px-4 py-2 text-right text-zinc-500">
                    {entry.is_dir ? "—" : formatSize(entry.size)}
                  </td>
                  <td class="px-4 py-2 text-right">
                    {!entry.is_dir && (
                      <Button
                        onClick={(e) => {
                          e.stopPropagation();
                          downloadFile(entry.name);
                        }}
                      >
                        Download
                      </Button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      {dlState && (
        <div class="absolute inset-0 flex items-center justify-center bg-black/60 z-10">
          <div class="bg-zinc-900 border border-zinc-700 rounded p-6 flex flex-col gap-3 min-w-56 max-w-xs w-full mx-4">
            <p class="text-sm text-zinc-200 font-medium truncate" title={dlState.filename}>
              {dlState.filename}
            </p>

            {dlState.error ? (
              <p class="text-xs text-red-400">{dlState.error}</p>
            ) : dlState.done ? (
              <p class="text-xs text-green-400">Done</p>
            ) : (
              <>
                {dlState.total > 0 ? (
                  <div class="bg-zinc-700 rounded-full h-1.5 overflow-hidden">
                    <div
                      class="bg-blue-500 h-1.5 rounded-full transition-all duration-200"
                      style={{ width: `${Math.min(100, (dlState.received / dlState.total) * 100)}%` }}
                    />
                  </div>
                ) : (
                  <div class="bg-zinc-700 rounded-full h-1.5 overflow-hidden">
                    <div class="bg-blue-500 h-1.5 w-full animate-pulse" />
                  </div>
                )}
                <p class="text-xs text-zinc-500">
                  {formatSize(dlState.received)}
                  {dlState.total > 0 ? ` / ${formatSize(dlState.total)}` : ""}
                </p>
              </>
            )}

            {dlState.done || dlState.error ? (
              <Button onClick={() => setDlState(null)}>Close</Button>
            ) : (
              <Button onClick={() => abortRef.current?.abort()}>Cancel</Button>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
