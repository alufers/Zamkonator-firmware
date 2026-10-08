import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { ComponentChildren } from "preact";
import {
  CircleAlert,
  CreditCard,
  DoorClosed,
  DoorOpen,
  LogIn,
  Lock,
  LockOpen,
  Power,
  Radio,
  Server,
  Unlock,
} from "lucide-preact";
import { AuthContext } from "./AuthContext";
import { DeviceEvent, EventPayload } from "./wsTypes";

interface LogProps {
  /** Live events pushed over the WebSocket, oldest first. */
  liveEvents: DeviceEvent[];
}

/* ── Presentation per event type ────────────────────────────────────────── */

interface Presentation {
  icon: ComponentChildren;
  label: string;
  /** Border/text colour classes for the type badge. */
  cls: string;
  detail: ComponentChildren;
}

const CARD_RESULTS: Record<string, { label: string; cls: string }> = {
  granted: { label: "GRANTED", cls: "text-green-400 border-green-900" },
  denied_not_found: { label: "NOT FOUND", cls: "text-red-400 border-red-900" },
  denied_expired: { label: "EXPIRED", cls: "text-amber-400 border-amber-900" },
  error_comm: { label: "COMM ERROR", cls: "text-red-400 border-red-900" },
  unconfigured: { label: "UNCONFIGURED", cls: "text-zinc-400 border-zinc-700" },
  unknown_format: { label: "UNKNOWN FORMAT", cls: "text-zinc-400 border-zinc-700" },
};

function Badge({ text, cls }: { text: string; cls: string }) {
  return (
    <span class={`px-1.5 py-0.5 rounded border text-[10px] font-semibold ${cls}`}>{text}</span>
  );
}

function Field({ label, value }: { label: string; value: ComponentChildren }) {
  return (
    <span class="text-zinc-500">
      {label}=<span class="text-zinc-300">{value}</span>
    </span>
  );
}

function present(p: EventPayload): Presentation {
  switch (p.type) {
    case "booted":
      return {
        icon: <Power size={13} />,
        label: "booted",
        cls: "text-blue-400 border-blue-900",
        detail: (
          <>
            <Field label="version" value={p.firmware_version} />
            <Field label="reset" value={p.reset_reason} />
          </>
        ),
      };

    case "coredump_saved":
      return {
        icon: <CircleAlert size={13} />,
        label: "coredump",
        cls: "text-red-400 border-red-900",
        detail: (
          <>
            <Field label="path" value={p.path} />
            <Field label="size" value={`${p.size} B`} />
          </>
        ),
      };

    case "card_scanned": {
      const res = CARD_RESULTS[p.result] ?? {
        label: p.result,
        cls: "text-zinc-400 border-zinc-700",
      };
      return {
        icon: <CreditCard size={13} />,
        label: "card",
        cls:
          p.result === "granted"
            ? "text-green-400 border-green-900"
            : "text-amber-400 border-amber-900",
        detail: (
          <>
            <Badge text={res.label} cls={res.cls} />
            <Field label="card" value={p.card_id} />
            {p.username && <Field label="user" value={p.username} />}
            {p.strategy && (
              <Field label="via" value={p.strategy === "auth_proxy_cached" ? "cache" : "online"} />
            )}
          </>
        ),
      };
    }

    case "door_state_changed":
      return {
        icon: p.closed ? <DoorClosed size={13} /> : <DoorOpen size={13} />,
        label: "door",
        cls: p.closed ? "text-green-400 border-green-900" : "text-amber-400 border-amber-900",
        detail: (
          <Badge
            text={p.closed ? "CLOSED" : "OPEN"}
            cls={p.closed ? "text-green-400 border-green-900" : "text-amber-400 border-amber-900"}
          />
        ),
      };

    case "lock_state_changed":
      return {
        icon: p.locked ? <Lock size={13} /> : <LockOpen size={13} />,
        label: "lock",
        cls: p.locked ? "text-green-400 border-green-900" : "text-amber-400 border-amber-900",
        detail: (
          <Badge
            text={p.locked ? "LOCKED" : "UNLOCKED"}
            cls={p.locked ? "text-green-400 border-green-900" : "text-amber-400 border-amber-900"}
          />
        ),
      };

    case "push_to_exit":
      return {
        icon: <LogIn size={13} />,
        label: "push to exit",
        cls: "text-blue-400 border-blue-900",
        detail: <Field label="input" value={`INP${p.input}`} />,
      };

    case "remote_open":
      return {
        icon: <Unlock size={13} />,
        label: "remote open",
        cls: "text-blue-400 border-blue-900",
        detail: (
          <>
            {p.username && <Field label="user" value={p.username} />}
            {p.reason && <Field label="reason" value={p.reason} />}
            <Field label="for" value={`${p.open_time_ms} ms`} />
          </>
        ),
      };

    case "auth_proxy_state_changed":
      return {
        icon: <Server size={13} />,
        label: "auth proxy",
        cls: p.available ? "text-green-400 border-green-900" : "text-red-400 border-red-900",
        detail: (
          <Badge
            text={p.available ? "AVAILABLE" : "UNAVAILABLE"}
            cls={p.available ? "text-green-400 border-green-900" : "text-red-400 border-red-900"}
          />
        ),
      };

    default:
      return {
        icon: <Radio size={13} />,
        label: (p as { type: string }).type,
        cls: "text-zinc-400 border-zinc-700",
        detail: null,
      };
  }
}

/* ── Timestamp ──────────────────────────────────────────────────────────── */

function formatUptime(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  if (h > 0) return `${h}h${String(m).padStart(2, "0")}m`;
  if (m > 0) return `${m}m${String(s).padStart(2, "0")}s`;
  return `${s}s`;
}

function EventRow({ ev }: { ev: DeviceEvent }) {
  const { icon, label, cls, detail } = present(ev.payload);
  const date = ev.time ? new Date(ev.time * 1000) : null;

  return (
    <div class="flex items-start gap-2 px-3 py-1.5 border-b border-zinc-900 hover:bg-zinc-900/50">
      <span
        class="text-zinc-500 text-xs shrink-0 w-20 pt-0.5 tabular-nums"
        title={date ? date.toLocaleString() : `${ev.uptime}s since boot`}
      >
        {date ? date.toLocaleTimeString() : `+${formatUptime(ev.uptime)}`}
      </span>

      <span
        class={`inline-flex items-center gap-1 px-1.5 py-0.5 rounded border bg-zinc-900 text-xs shrink-0 ${cls}`}
      >
        {icon}
        {label}
      </span>

      <span class="flex flex-wrap items-center gap-x-3 gap-y-1 text-xs pt-0.5 min-w-0 break-all">
        {detail}
      </span>
    </div>
  );
}

/* ── Tab ────────────────────────────────────────────────────────────────── */

type LoadState = "loading" | "loaded" | "error";

export function Log({ liveEvents }: LogProps) {
  const { password } = useContext(AuthContext);
  const [history, setHistory] = useState<DeviceEvent[]>([]);
  const [loadState, setLoadState] = useState<LoadState>("loading");
  const scrollRef = useRef<HTMLDivElement>(null);
  const pinnedRef = useRef(true);

  /* Backfill from the device's in-memory ring. */
  useEffect(() => {
    const headers: Record<string, string> = password ? { "X-Auth": password } : {};
    fetch("/api/events", { headers })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json() as Promise<DeviceEvent[]>;
      })
      .then((data) => {
        setHistory(data);
        setLoadState("loaded");
      })
      .catch(() => setLoadState("error"));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  /* The ring and the live stream overlap around the moment the socket opened,
   * so seq is the join key. */
  const seen = new Set<number>();
  const events: DeviceEvent[] = [];
  for (const ev of [...history, ...liveEvents]) {
    if (seen.has(ev.seq)) continue;
    seen.add(ev.seq);
    events.push(ev);
  }
  events.sort((a, b) => a.seq - b.seq);

  /* Stay pinned to the newest entry unless the user has scrolled up to read. */
  useEffect(() => {
    const el = scrollRef.current;
    if (el && pinnedRef.current) el.scrollTop = el.scrollHeight;
  }, [events.length]);

  const onScroll = () => {
    const el = scrollRef.current;
    if (!el) return;
    pinnedRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
  };

  if (loadState === "loading") {
    return (
      <div class="h-full flex items-center justify-center text-zinc-500 text-xs">Loading log…</div>
    );
  }

  if (events.length === 0) {
    return (
      <div class="h-full flex items-center justify-center text-zinc-600 text-xs">
        {loadState === "error"
          ? "Could not load history — waiting for live events…"
          : "No events recorded yet."}
      </div>
    );
  }

  return (
    <div ref={scrollRef} onScroll={onScroll} class="h-full overflow-y-auto font-mono">
      {loadState === "error" && (
        <div class="px-3 py-1.5 text-xs text-amber-400 border-b border-zinc-900">
          Could not load history — showing live events only.
        </div>
      )}
      {events.map((ev) => (
        <EventRow key={ev.seq} ev={ev} />
      ))}
    </div>
  );
}
