import { ComponentChildren } from "preact";
import { useContext, useEffect, useState } from "preact/hooks";
import { AuthContext } from "./AuthContext";
import { formatUptime } from "./TopBar";
import { Button } from "./ui/Button";
import { StatusPayload } from "./wsTypes";

const POLL_MS = 5000;

function Row({ label, children }: { label: string; children: ComponentChildren }) {
  return (
    <div class="flex gap-4 py-1 border-b border-zinc-900 last:border-b-0">
      <div class="w-36 shrink-0 text-xs text-zinc-500">{label}</div>
      <div class="text-xs text-zinc-200 font-mono break-all select-text">{children}</div>
    </div>
  );
}

function Dim({ children }: { children: ComponentChildren }) {
  return <span class="text-zinc-600">{children}</span>;
}

/** "since boot" seconds → "3h 2m ago" relative to the current uptime. */
function ago(at: number | null, uptime: number) {
  if (at === null) return <Dim>—</Dim>;
  return `${formatUptime(Math.max(0, uptime - at))} ago`;
}

export function Status() {
  const { password } = useContext(AuthContext);
  const [data, setData] = useState<StatusPayload | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = async () => {
    const headers: Record<string, string> = password ? { "X-Auth": password } : {};
    try {
      const res = await fetch("/api/status", { headers });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      setData((await res.json()) as StatusPayload);
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Network error");
    }
  };

  useEffect(() => {
    void load();
    const id = setInterval(() => void load(), POLL_MS);
    return () => clearInterval(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [password]);

  const net = data?.network;

  return (
    <div class="p-4 overflow-y-auto h-full">
      <div class="max-w-lg mx-auto">
        <div class="flex items-center gap-3 mb-4">
          <Button onClick={() => void load()}>Refresh</Button>
          {error && <span class="text-xs text-red-400">{error}</span>}
        </div>

        {!data ? (
          <div class="text-zinc-500 text-xs">{error ? "Failed to load status." : "Loading…"}</div>
        ) : (
          <>
            <section class="mb-6">
              <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
                Ethernet
              </h3>
              {net ? (
                <>
                  <Row label="Link">
                    {net.link_up ? (
                      <span class="text-green-400">up</span>
                    ) : (
                      <span class="text-red-400">down</span>
                    )}
                    {net.link_up && net.speed_mbps !== null && (
                      <span class="text-zinc-400">
                        {" "}
                        · {net.speed_mbps} Mbps
                        {net.full_duplex !== null &&
                          (net.full_duplex ? " full duplex" : " half duplex")}
                      </span>
                    )}
                  </Row>
                  <Row label="Link up since">{ago(net.link_up_since, data.uptime)}</Row>
                  <Row label="Link drops">{net.link_down_count}</Row>
                  <Row label="MAC address">{net.mac || <Dim>—</Dim>}</Row>
                  <Row label="Hostname">{net.hostname || <Dim>—</Dim>}</Row>
                </>
              ) : (
                <div class="text-zinc-500 text-xs">Not reported by firmware.</div>
              )}
            </section>

            {net && (
              <section class="mb-6">
                <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">IP</h3>
                <Row label="Mode">{net.dhcp_enabled ? "DHCP" : "Static"}</Row>
                <Row label="IPv4 address">
                  {net.got_ip ? net.ip : <span class="text-amber-400">no address</span>}
                </Row>
                <Row label="Netmask">{net.got_ip ? net.netmask : <Dim>—</Dim>}</Row>
                <Row label="Gateway">{net.got_ip ? net.gateway : <Dim>—</Dim>}</Row>
                <Row label="DNS servers">
                  {net.dns.length ? net.dns.map((d) => <div key={d}>{d}</div>) : <Dim>none</Dim>}
                </Row>
                <Row label="Address acquired">{ago(net.ip_acquired_at, data.uptime)}</Row>
                <Row label="IPv6 addresses">
                  {net.ipv6.length ? net.ipv6.map((a) => <div key={a}>{a}</div>) : <Dim>none</Dim>}
                </Row>
              </section>
            )}

            <section class="mb-6">
              <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
                Device
              </h3>
              <Row label="Uptime">{formatUptime(data.uptime)}</Row>
              <Row label="Device time">
                {data.time > 1_000_000_000 ? (
                  new Date(data.time * 1000).toLocaleString()
                ) : (
                  <span class="text-amber-400">not synced</span>
                )}
              </Row>
              <Row label="MQTT">{data.mqtt_status}</Row>
              <Row label="Auth proxy">
                {data.auth_proxy_healthy ? (
                  <span class="text-green-400">healthy</span>
                ) : (
                  <span class="text-red-400">unreachable</span>
                )}
              </Row>
              <Row label="SD card">{data.sd_card_mounted ? "mounted" : "not mounted"}</Row>
            </section>
          </>
        )}
      </div>
    </div>
  );
}
