import { useCallback, useContext, useEffect, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { AuthGuard } from "./AuthGuard";
import { Files } from "./Files";
import { Settings } from "./Settings";
import { TopBar } from "./TopBar";
import { Tabs } from "./ui/Tabs";
import { StatusPayload, MqttStatus } from "./wsTypes";

export function App() {
  return (
    <AuthGuard>
      <AppInner />
    </AuthGuard>
  );
}

function AppInner() {
  const { password, onLogout } = useContext(AuthContext);
  const [status, setStatus] = useState<StatusPayload | null>(null);

  const fetchStatus = useCallback(() => {
    const headers: Record<string, string> = password ? { "X-Auth": password } : {};
    fetch("/api/status", { headers })
      .then((r) => (r.ok ? (r.json() as Promise<StatusPayload>) : null))
      .then((data) => { if (data) setStatus(data); })
      .catch(() => {});
  }, [password]);

  useEffect(() => {
    fetchStatus();
    const interval = setInterval(fetchStatus, 10_000);
    return () => clearInterval(interval);
  }, [fetchStatus]);

  const mqttStatus: MqttStatus | null = status?.mqtt_status ?? null;

  const [activeTab, setActiveTab] = useState<"settings" | "files">("settings");
  const TABS = [
    { id: "settings", label: "Settings" },
    { id: "files", label: "Files" },
  ];

  return (
    <div class="flex flex-col h-full bg-zinc-950 text-zinc-100 font-mono">
      <TopBar
        status={status}
        mqttStatus={mqttStatus}
        onLogout={password !== null ? onLogout : undefined}
      />
      <div class="flex flex-col flex-1 overflow-hidden">
        <Tabs
          tabs={TABS}
          active={activeTab}
          onChange={(id) => setActiveTab(id as typeof activeTab)}
        />
        <div class="flex-1 overflow-hidden">
          {activeTab === "settings" ? <Settings /> : <Files status={status} />}
        </div>
      </div>
    </div>
  );
}
