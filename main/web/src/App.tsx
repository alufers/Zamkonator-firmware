import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { AuthGuard } from "./AuthGuard";
import { Settings } from "./Settings";
import { TopBar } from "./TopBar";
import { useJsonWebsocket, ReadyState } from "./useWebsocket";
import { WsMessage, StatusPayload, MqttStatus } from "./wsTypes";

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
  const lastStatusTimeRef = useRef<number>(0);

  const wsUrl = password
    ? `ws://${location.host}/ws?auth=${encodeURIComponent(password)}`
    : `ws://${location.host}/ws`;
  const { lastJsonMessage, readyState, forceReconnect } =
    useJsonWebsocket<WsMessage>(wsUrl);

  useEffect(() => {
    if (!lastJsonMessage) return;
    if (lastJsonMessage.cmd === "status") {
      lastStatusTimeRef.current = Date.now();
      setStatus(lastJsonMessage.payload);
    }
  }, [lastJsonMessage]);

  useEffect(() => {
    if (readyState === ReadyState.OPEN) {
      setStatus(null);
      lastStatusTimeRef.current = 0;
    } else if (readyState === ReadyState.CLOSED) {
      setStatus(null);
    }
  }, [readyState]);

  useEffect(() => {
    if (readyState !== ReadyState.OPEN) return;
    const interval = setInterval(() => {
      if (lastStatusTimeRef.current > 0 && Date.now() - lastStatusTimeRef.current > 20_000) {
        forceReconnect();
      }
    }, 5000);
    return () => clearInterval(interval);
  }, [readyState, forceReconnect]);

  const mqttStatus: MqttStatus | null = status?.mqtt_status ?? null;
  const connected = readyState === ReadyState.OPEN;
  const connecting = readyState === ReadyState.CONNECTING;

  return (
    <div class="flex flex-col h-full bg-zinc-950 text-zinc-100 font-mono">
      <TopBar
        status={status}
        connected={connected}
        connecting={connecting}
        mqttStatus={mqttStatus}
        onLogout={password !== null ? onLogout : undefined}
      />
      <div class="flex-1 overflow-hidden">
        <Settings />
      </div>
    </div>
  );
}
