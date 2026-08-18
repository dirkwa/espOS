// SPDX-License-Identifier: Apache-2.0
import { useEffect, useState } from "preact/hooks";
import { get, post, del, useStore, wifiStore, skStore, skWsStore, fmtDuration, fmtBytes, errText, type SystemInfo, type Coredump } from "../api";
import { Badge, Row, Msg, useAsync, navigate } from "../app";

export function StatusPage() {
  const wifi = useStore(wifiStore);
  const sk = useStore(skStore);
  const ws = useStore(skWsStore) ?? sk?.ws;
  const info = useAsync(() => get<SystemInfo>("/system/info"));
  const [msg, setMsg] = useState("");
  const [tick, setTick] = useState(0);
  useEffect(() => { const t = setInterval(() => { info.reload(); setTick((n) => n + 1); }, 10000); return () => clearInterval(t); }, []);
  void tick;
  const i = info.data;
  const wifiKind = wifi?.state === "connected" ? "ok" : wifi?.state === "backoff" || (wifi?.reason.code ?? 0) ? "bad" : "warn";
  const tokKind = sk?.token.state === "approved" ? "ok" : sk?.token.state === "denied" || sk?.token.state === "error" ? "bad" : "warn";

  async function reboot() {
    if (!confirm("Reboot the device now?")) return;
    try { await post("/system/reboot"); setMsg("Rebooting… the page reconnects by itself."); } catch (e) { setMsg(errText(e)); }
  }
  async function factoryReset() {
    if (!confirm("Erase ALL settings (WiFi, SignalK token, config) and reboot into the setup portal?")) return;
    try { await post("/system/factory-reset"); setMsg("Factory reset requested — the device reboots into the setup portal."); } catch (e) { setMsg(errText(e)); }
  }

  return (
    <>
      <h1>Status</h1>
      <Msg text={msg} kind="warn" />
      <div class="grid">
        <section class="card">
          <h2>WiFi <Badge kind={wifiKind}>{wifi?.state ?? "…"}</Badge></h2>
          {wifi && (
            <>
              {wifi.ssid && <Row k="Network">{wifi.ssid}{wifi.rssi !== undefined && <span class="muted"> · {wifi.rssi} dBm · ch {wifi.channel}</span>}</Row>}
              {wifi.ip && <Row k="IP">{wifi.ip} <span class="muted">via {wifi.gateway}</span></Row>}
              <Row k="Hostname">{wifi.hostname}</Row>
              {wifi.reason.code !== 0 && <Row k="Last reason"><span class="mono">{wifi.reason.text} [{wifi.reason.code}]</span></Row>}
              {wifi.backoff_ms !== undefined && <Row k="Retry in">{Math.ceil(wifi.backoff_ms / 1000)} s</Row>}
              {wifi.portal.active && <Row k="Setup portal"><Badge kind="warn">on</Badge> {wifi.portal.ssid} {wifi.portal.ip && <span class="muted">· {wifi.portal.ip}</span>}</Row>}
              <div class="row"><a href="/wifi" onClick={(e) => { e.preventDefault(); navigate("/wifi"); }}>WiFi setup →</a></div>
            </>
          )}
        </section>

        <section class="card">
          <h2>SignalK <Badge kind={tokKind}>{sk?.token.state ?? "…"}</Badge></h2>
          {sk && (
            <>
              <Row k="Server">{sk.server.source === "none" ? <span class="muted">none selected</span> : <>{sk.server.name || sk.server.host} <span class="muted">· {sk.server.host}:{sk.server.port} · {sk.server.source}</span></>}</Row>
              <Row k="Stream">
                {ws ? (
                  ws.connected ? <><Badge kind="ok">connected</Badge> <span class="muted">{fmtDuration(ws.connected_s)} · {ws.sent} sent</span></>
                    : ws.enabled ? <><Badge kind="warn">offline</Badge> <span class="muted">{ws.buffered} buffered{ws.next_retry_s ? ` · retry in ${ws.next_retry_s}s` : ""}</span></>
                      : <Badge kind="muted">disabled</Badge>
                ) : "–"}
              </Row>
              {ws?.last_error && <Row k="Stream error"><span class="mono">{ws.last_error}</span></Row>}
              {sk.token.last_error && <Row k="Token error"><span class="mono">{sk.token.last_error}</span></Row>}
              <div class="row"><a href="/signalk" onClick={(e) => { e.preventDefault(); navigate("/signalk"); }}>SignalK setup →</a></div>
            </>
          )}
        </section>

        <section class="card">
          <h2>Device {i && <Badge kind={i.reset_reason === "panic" || i.reset_reason.includes("wdt") ? "bad" : "muted"}>{i.reset_reason}</Badge>}</h2>
          {info.error && <Msg text={info.error} />}
          {i && (
            <>
              <Row k="Firmware">{i.app} {i.version}</Row>
              <Row k="ESP-IDF">{i.idf_version}</Row>
              <Row k="Chip">{i.chip} <span class="muted">rev {i.chip_revision} · {i.cores} core{i.cores > 1 ? "s" : ""}</span></Row>
              <Row k="Uptime">{fmtDuration(i.uptime_s)}</Row>
              <Row k="Free heap">{fmtBytes(i.free_heap)} <span class="muted">min {fmtBytes(i.min_free_heap)}</span></Row>
              {i.config_storage_reset && <Msg text="Config storage was reset at boot — all values are defaults." kind="warn" />}
              {i.ui_storage === false && <Msg text="UI storage partition not mounted; serving the embedded page." kind="warn" />}
              <div class="row" style="margin-top:.75rem">
                <button onClick={reboot}>Reboot</button>
                <button class="danger" onClick={factoryReset}>Factory reset…</button>
              </div>
            </>
          )}
        </section>

        <CoredumpCard />
      </div>
    </>
  );
}

function CoredumpCard() {
  const [dump, setDump] = useState<Coredump | null | undefined>(undefined);
  const [msg, setMsg] = useState("");
  const load = () => get<Coredump>("/system/coredump").then(setDump, (e: unknown) => {
    if (e instanceof Error && "status" in e && (e as { status: number }).status === 404) setDump(null); else setMsg(errText(e));
  });
  useEffect(() => { void load(); }, []);
  if (dump === undefined) return null;
  return (
    <section class="card">
      <h2>Last crash {dump ? <Badge kind="bad">core dump</Badge> : <Badge kind="ok">none</Badge>}</h2>
      <Msg text={msg} />
      {dump ? (
        <>
          <Row k="Task">{dump.task ?? "?"}</Row>
          <Row k="PC"><span class="mono">{dump.pc}</span></Row>
          {dump.exc_cause !== undefined && <Row k="Exception">cause {dump.exc_cause} <span class="mono muted">@ {dump.exc_vaddr}</span></Row>}
          {dump.mcause !== undefined && <Row k="Trap">mcause {dump.mcause} <span class="mono muted">mtval {dump.mtval} · ra {dump.ra}</span></Row>}
          {dump.backtrace && <Row k="Backtrace"><span class="mono small">{dump.backtrace.join(" ")}{dump.backtrace_corrupted ? " (corrupted)" : ""}</span></Row>}
          <Row k="Image">{fmtBytes(dump.size)} {dump.valid ? "" : "(checksum invalid)"} <span class="muted">· ELF {dump.app_elf_sha256}</span></Row>
          {dump.summary_error && <Row k="Summary"><span class="mono">{dump.summary_error}</span></Row>}
          <div class="row" style="margin-top:.75rem">
            <a class="mono small" href="/api/v1/system/coredump/raw" download="coredump.bin">download raw</a>
            <button onClick={async () => { try { await del("/system/coredump"); setDump(null); } catch (e) { setMsg(errText(e)); } }}>Erase</button>
          </div>
          <p class="muted small">Decode with <code>espcoredump.py info_corefile -c coredump.bin -t raw build/espos.elf</code> from the matching build.</p>
        </>
      ) : <p class="muted small">No core dump stored. After a panic the summary appears here.</p>}
    </section>
  );
}
