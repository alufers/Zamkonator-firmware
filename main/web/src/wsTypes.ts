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
}
