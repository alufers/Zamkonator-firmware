export interface InfoResponse {
  web_password_enabled: boolean;
  language: string;
  web_password_valid?: boolean | null;
}

export type MqttStatus = "unconfigured" | "connecting" | "connected" | "disconnected";

export interface StatusPayload {
  uptime: number;
  time: number;
  mqtt_status: MqttStatus;
  sd_card_mounted: boolean;
  auth_proxy_healthy: boolean;
  door_close_sensor_closed?: boolean | null;
  door_lock_sensor_locked?: boolean | null;
}
