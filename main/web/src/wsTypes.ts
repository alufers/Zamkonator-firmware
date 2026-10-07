export interface InfoResponse {
  web_password_enabled: boolean;
  language: string;
  web_password_valid?: boolean | null;
}

export type MqttStatus = "unconfigured" | "connecting" | "connected" | "disconnected";

export interface NetworkStatus {
  hostname: string;
  mac: string;
  link_up: boolean;
  speed_mbps: number | null;
  full_duplex: boolean | null;
  link_down_count: number;
  /** Seconds since boot of the last link-up, null while down. */
  link_up_since: number | null;
  dhcp_enabled: boolean;
  got_ip: boolean;
  /** Seconds since boot of the last GOT_IP event. */
  ip_acquired_at: number | null;
  ip: string;
  netmask: string;
  gateway: string;
  dns: string[];
  ipv6: string[];
}

export interface StatusPayload {
  uptime: number;
  time: number;
  mqtt_status: MqttStatus;
  sd_card_mounted: boolean;
  auth_proxy_healthy: boolean;
  door_close_sensor_closed?: boolean | null;
  door_lock_sensor_locked?: boolean | null;
  /** Only present in GET /api/status, never in the WS broadcast. */
  network?: NetworkStatus;
}

/* ── Events ─────────────────────────────────────────────────────────────── */

export type CardScanResult =
  | "granted"
  | "denied_not_found"
  | "denied_expired"
  | "error_comm"
  | "unconfigured"
  | "unknown_format";

export type AuthStrategyName = "auth_proxy_online" | "auth_proxy_cached";

export interface EvBooted {
  type: "booted";
  firmware_version: string;
  reset_reason: string;
}

export interface EvCoredumpSaved {
  type: "coredump_saved";
  path: string;
  size: number;
}

export interface EvCardScanned {
  type: "card_scanned";
  card_id: string;
  bit_count: number;
  result: CardScanResult;
  username?: string | null;
  strategy?: AuthStrategyName | null;
  membership_expiration?: number | null;
}

export interface EvDoorStateChanged {
  type: "door_state_changed";
  closed: boolean;
}

export interface EvLockStateChanged {
  type: "lock_state_changed";
  locked: boolean;
}

export interface EvPushToExit {
  type: "push_to_exit";
  input: number;
}

export interface EvRemoteOpen {
  type: "remote_open";
  reason?: string | null;
  open_time_ms: number;
}

export type EventPayload =
  | EvBooted
  | EvCoredumpSaved
  | EvCardScanned
  | EvDoorStateChanged
  | EvLockStateChanged
  | EvPushToExit
  | EvRemoteOpen;

export interface DeviceEvent {
  seq: number;
  /** Seconds since boot. */
  uptime: number;
  /** Unix timestamp, or null when the event predates NTP sync. */
  time: number | null;
  payload: EventPayload;
}

/* ── WebSocket server → client messages ─────────────────────────────────── */

export interface WsStatusMessage {
  cmd: "status";
  payload: StatusPayload;
}

export interface WsEventMessage {
  cmd: "event";
  payload: DeviceEvent;
}

export type ServerMessage = WsStatusMessage | WsEventMessage;
