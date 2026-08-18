// SPDX-License-Identifier: Apache-2.0
import { useEffect, useState } from "preact/hooks";
import { get, put, post, useStore, wifiStore, scanStore, errText, type ScanDoc, type ConfigDoc, type PutResult } from "../api";
import { Badge, Row, Msg, navigate } from "../app";

const SLOTS = [0, 1, 2, 3] as const;

export function WifiPage() {
  const wifi = useStore(wifiStore);
  const scan = useStore(scanStore);
  const [cfg, setCfg] = useState<Record<string, unknown> | null>(null);
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState("");
  const [scanning, setScanning] = useState(false);
  const [join, setJoin] = useState({ ssid: "", psk: "", slot: 0 });

  const load = () => get<ConfigDoc>("/config?ns=wifi").then((d) => setCfg(d["wifi"] ?? {}), (e: unknown) => setMsg(errText(e)));
  useEffect(() => { void load(); void get<ScanDoc>("/wifi/scan").then((d) => scanStore.set(d), () => undefined); }, []);
  useEffect(() => { if (scan && !scan.scanning) setScanning(false); }, [scan]);

  async function doScan() {
    setScanning(true); setMsg("");
    try { await post("/wifi/scan"); } catch (e) { setScanning(false); setMsg(errText(e)); }
  }
  async function save(patch: Record<string, unknown>, done: string) {
    setMsg(""); setOk("");
    try {
      const r = await put<PutResult>("/config", { wifi: patch });
      setOk(r.changed.length ? done : "No change.");
      await load();
    } catch (e) { setMsg(errText(e)); }
  }
  function submitJoin(e: Event) {
    e.preventDefault();
    if (!join.ssid) return;
    void save({ [`ssid${join.slot}`]: join.ssid, [`psk${join.slot}`]: join.psk }, `Saved ${join.ssid} in slot ${join.slot + 1}; connecting…`);
    setJoin({ ssid: "", psk: "", slot: join.slot });
  }

  const nets = SLOTS.map((i) => ({ i, ssid: (cfg?.[`ssid${i}`] as string) || "", psk: (cfg?.[`psk${i}`] as string) || "", bssid: (cfg?.[`bssid${i}`] as string) || "" }));
  const kind = wifi?.state === "connected" ? "ok" : wifi?.state === "backoff" || (wifi?.reason.code ?? 0) ? "bad" : "warn";

  return (
    <>
      <h1>WiFi</h1>
      <div class="grid">
        <section class="card">
          <h2>Connection <Badge kind={kind}>{wifi?.state ?? "…"}</Badge></h2>
          {wifi && (
            <>
              {wifi.ssid && <Row k="Network">{wifi.ssid} <span class="muted">(slot {(wifi.network_index ?? 0) + 1})</span></Row>}
              {wifi.bssid && <Row k="BSSID"><span class="mono">{wifi.bssid}</span> <span class="muted">ch {wifi.channel} · {wifi.rssi} dBm</span></Row>}
              {wifi.ip && <Row k="IP">{wifi.ip} <span class="muted">/ {wifi.netmask} via {wifi.gateway}</span></Row>}
              <Row k="Hostname">{wifi.hostname}</Row>
              <Row k="Last reason">{wifi.reason.code ? <span class="mono">{wifi.reason.text} [{wifi.reason.code}]</span> : <span class="muted">–</span>}</Row>
              {wifi.backoff_ms !== undefined && <Row k="Retry in">{Math.ceil(wifi.backoff_ms / 1000)} s <span class="muted">(attempt {wifi.attempt})</span></Row>}
              <Row k="Counters">{wifi.connect_count ?? 0} connects · {wifi.disconnect_count ?? 0} disconnects</Row>
              <Row k="Setup portal">{wifi.portal.active ? <><Badge kind="warn">on</Badge> {wifi.portal.ssid}{wifi.portal.ip ? ` · ${wifi.portal.ip}` : ""}{wifi.portal.clients ? ` · ${wifi.portal.clients} client(s)` : ""}</> : <span class="muted">off</span>}</Row>
              <div class="row" style="margin-top:.5rem">
                <label><input type="checkbox" checked={wifi.sta_enabled} onChange={(e) => void save({ sta_enabled: (e.target as HTMLInputElement).checked }, "Station mode updated.")} /> station enabled</label>
              </div>
            </>
          )}
        </section>

        <section class="card">
          <h2>Join a network</h2>
          <form onSubmit={submitJoin} autocomplete="off">
            <div class="row">
              <input type="text" list="ssids" placeholder="Network (SSID)" required value={join.ssid} onInput={(e) => setJoin({ ...join, ssid: (e.target as HTMLInputElement).value })} />
              <datalist id="ssids">{scan?.results.map((r) => <option key={r.bssid} value={r.ssid} />)}</datalist>
              <input type="password" placeholder="Password (empty = open)" value={join.psk} onInput={(e) => setJoin({ ...join, psk: (e.target as HTMLInputElement).value })} />
            </div>
            <div class="row">
              <label>slot <select value={join.slot} onChange={(e) => setJoin({ ...join, slot: Number((e.target as HTMLSelectElement).value) })}>{SLOTS.map((i) => <option key={i} value={i}>{i + 1}{nets[i]?.ssid ? ` (${nets[i]!.ssid})` : " (free)"}</option>)}</select></label>
              <button class="primary" type="submit">Save &amp; connect</button>
              <button type="button" onClick={doScan} disabled={scanning}>{scanning ? "Scanning…" : "Scan"}</button>
            </div>
          </form>
          <Msg text={msg} />
          <Msg text={ok} kind="ok" />
          {scan && scan.results.length > 0 && (
            <>
              <h3>Nearby {scan.age_s !== null && <span class="muted" style="text-transform:none;letter-spacing:0">· {scan.age_s}s ago</span>}</h3>
              <table>
                <thead><tr><th>SSID</th><th>Signal</th><th>Ch</th><th>Security</th></tr></thead>
                <tbody>
                  {[...scan.results].sort((a, b) => b.rssi - a.rssi).map((r) => (
                    <tr key={r.bssid} class="click" onClick={() => setJoin({ ...join, ssid: r.ssid })} title={r.bssid}>
                      <td>{r.ssid || <i class="muted">(hidden)</i>}</td><td>{r.rssi} dBm</td><td>{r.channel}</td><td>{r.auth}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </>
          )}
        </section>

        <section class="card">
          <h2>Saved networks</h2>
          <p class="muted small">Tried in order; the device falls back to the next slot when one fails.</p>
          <table>
            <thead><tr><th>#</th><th>SSID</th><th>Password</th><th></th></tr></thead>
            <tbody>
              {nets.map((n) => (
                <tr key={n.i} class={wifi?.network_index === n.i && wifi.state === "connected" ? "sel" : ""}>
                  <td>{n.i + 1}</td>
                  <td>{n.ssid || <span class="muted">–</span>}{n.bssid && <div class="mono small muted">{n.bssid}</div>}</td>
                  <td>{n.ssid ? (n.psk ? "••••••••" : <span class="muted">open</span>) : ""}</td>
                  <td>{n.ssid && <button class="danger" onClick={() => { if (confirm(`Forget ${n.ssid}?`)) void save({ [`ssid${n.i}`]: null, [`psk${n.i}`]: null, [`bssid${n.i}`]: null }, `Forgot ${n.ssid}.`); }}>Forget</button>}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p class="muted small">Backoff, timeouts and the setup portal are under <a href="/config" onClick={(e) => { e.preventDefault(); navigate("/config"); }}>Config → WiFi</a>.</p>
        </section>
      </div>
    </>
  );
}
