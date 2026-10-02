import { useContext, useEffect, useMemo, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { AuthGuard } from "./AuthGuard";
import { Files } from "./Files";
import { Log } from "./Log";
import { OTA } from "./OTA";
import { Settings } from "./Settings";
import { TopBar } from "./TopBar";
import { Tabs } from "./ui/Tabs";
import { useJsonWebsocket, ReadyState } from "./useWebsocket";
import { DeviceEvent, MqttStatus, ServerMessage, StatusPayload } from "./wsTypes";

export function App() {
  return (
    <AuthGuard>
      <AppInner />
    </AuthGuard>
  );
}

/** Cap on retained live events — the device ring is the source of truth for history. */
const MAX_LIVE_EVENTS = 500;

type TabId = "log" | "settings" | "files" | "ota";

const TABS = [
  { id: "log", label: "Log" },
  { id: "settings", label: "Settings" },
  { id: "files", label: "Files" },
  { id: "ota", label: "OTA" },
];

function AppInner() {
  const { password, onLogout } = useContext(AuthContext);
  const [status, setStatus] = useState<StatusPayload | null>(null);
  const [liveEvents, setLiveEvents] = useState<DeviceEvent[]>([]);
  const [activeTab, setActiveTab] = useState<TabId>("log");

  /* The password is only appended when one is set; ws_pre_handshake_cb on the
   * device skips the check entirely when web auth is disabled. */
  const wsUrl = useMemo(() => {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const auth = password ? `?auth=${encodeURIComponent(password)}` : "";
    return `${proto}://${location.host}/ws${auth}`;
  }, [password]);

  const { lastJsonMessage, readyState } = useJsonWebsocket<ServerMessage>(wsUrl);

  useEffect(() => {
    if (!lastJsonMessage) return;
    if (lastJsonMessage.cmd === "status") {
      setStatus(lastJsonMessage.payload);
    } else if (lastJsonMessage.cmd === "event") {
      const ev = lastJsonMessage.payload;
      setLiveEvents((prev) =>
        prev.length >= MAX_LIVE_EVENTS
          ? [...prev.slice(prev.length - MAX_LIVE_EVENTS + 1), ev]
          : [...prev, ev],
      );
    }
  }, [lastJsonMessage]);

  const mqttStatus: MqttStatus | null = status?.mqtt_status ?? null;

  return (
    <div class="flex flex-col h-full bg-zinc-950 text-zinc-100 font-mono">
      <TopBar
        status={status}
        mqttStatus={mqttStatus}
        wsConnected={readyState === ReadyState.OPEN}
        onLogout={password !== null ? onLogout : undefined}
      />
      <div class="flex flex-col flex-1 overflow-hidden">
        <Tabs tabs={TABS} active={activeTab} onChange={(id) => setActiveTab(id as TabId)} />
        <div class="flex-1 overflow-hidden">
          {activeTab === "log" ? (
            <Log liveEvents={liveEvents} />
          ) : activeTab === "settings" ? (
            <Settings />
          ) : activeTab === "files" ? (
            <Files status={status} />
          ) : (
            <OTA />
          )}
        </div>
      </div>
    </div>
  );
}
