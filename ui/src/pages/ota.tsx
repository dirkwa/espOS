// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
import { useEffect, useState } from "preact/hooks";
import { get, put, post, useStore, otaStore, fmtBytes, fmtDuration, errText, type OtaStatus, type ConfigDoc, type PutResult } from "../api";
import { Badge, Row, Msg, navigate } from "../app";

const STATE_KIND: Record<OtaStatus["state"], "ok" | "warn" | "bad" | "muted"> = {
  idle: "muted", checking: "warn", available: "ok", downloading: "warn", verifying: "warn", ready: "ok", failed: "bad",
};

export function OtaPage() {
  const ota = useStore(otaStore);
  const [cfg, setCfg] = useState<Record<string, unknown> | null>(null);
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState("");
  const [url, setUrl] = useState("");
  const [manifest, setManifest] = useState({ manifest_url: "", channel: "stable" });

  const load = () => get<ConfigDoc>("/config?ns=ota").then((d) => {
    const c = d["ota"] ?? {};
    setCfg(c);
    setManifest({ manifest_url: (c["manifest_url"] as string) || "", channel: (c["channel"] as string) || "stable" });
  }, (e: unknown) => setMsg(errText(e)));
  useEffect(() => { void load(); if (!otaStore.value) void get<OtaStatus>("/ota/status").then((d) => otaStore.set(d), () => undefined); }, []);

  async function act(fn: () => Promise<unknown>, done: string) {
    setMsg(""); setOk("");
    try { await fn(); setOk(done); } catch (e) { setMsg(errText(e)); }
  }
  const save = (patch: Record<string, unknown>, done: string) => act(async () => { const r = await put<PutResult>("/config", { ota: patch }); if (!r.changed.length) done = "No change."; await load(); }, done);

  const r = ota?.running;
  const busy = ota?.state === "checking" || ota?.state === "downloading" || ota?.state === "verifying" || ota?.state === "ready";
  const pct = ota && ota.progress.total > 0 ? Math.round((100 * ota.progress.received) / ota.progress.total) : null;

  return (
    <>
      <h1>OTA</h1>
      <Msg text={msg} />
      <Msg text={ok} kind="ok" />
      <div class="grid">
        <section class="card">
          <h2>Running firmware {r && (r.pending_verify ? <Badge kind="warn">pending verify</Badge> : r.rolled_back ? <Badge kind="bad">rolled back</Badge> : <Badge kind="ok">{r.image_state}</Badge>)}</h2>
          {r && (
            <>
              <Row k="Version">{r.project} {r.version} <span class="muted">· {r.target}</span></Row>
              <Row k="Built">{r.built} <span class="muted">· ESP-IDF {r.idf}</span></Row>
              <Row k="Slot">{r.slot} <span class="muted">· other: {r.other_slot} {r.other_version ? `(${r.other_version})` : "(empty)"}</span></Row>
              {r.pending_verify && <Msg kind="warn" text="This image is not confirmed yet: it confirms itself once the network is up, or rolls back to the previous firmware after the rollback timeout." />}
              {r.rolled_back && <Msg kind="warn" text={`The last update (${r.other_version || "unknown version"}) failed to boot or to confirm and was rolled back.`} />}
              <div class="row" style="margin-top:.5rem">
                {r.pending_verify && <button class="primary" onClick={() => act(() => post("/ota/confirm"), "Image confirmed.")}>Confirm now</button>}
                {r.other_version && !r.rolled_back && <button class="danger" onClick={() => { if (confirm(`Reboot into the previous firmware (${r.other_version}) and mark this one invalid?`)) void act(() => post("/ota/rollback"), "Rolling back…"); }}>Roll back to {r.other_version}</button>}
              </div>
            </>
          )}
        </section>

        <section class="card">
          <h2>Update {ota && <Badge kind={STATE_KIND[ota.state]}>{ota.state}</Badge>}</h2>
          {ota?.last_error && <Msg text={ota.last_error} />}
          {ota && (ota.state === "downloading" || ota.state === "verifying" || ota.state === "ready") && (
            <>
              <div class="bar"><span style={`width:${pct ?? 5}%`} /></div>
              <p class="small muted">{ota.state === "ready" ? "Installed — rebooting into the new firmware…" : `${fmtBytes(ota.progress.received)}${ota.progress.total ? ` of ${fmtBytes(ota.progress.total)} (${pct}%)` : ""}`}</p>
            </>
          )}
          {ota?.available ? (
            <>
              <Row k="Available">{ota.available.version} {ota.available.newer ? <Badge kind="ok">newer</Badge> : <Badge kind="muted">not newer</Badge>}</Row>
              {ota.available.notes && <Row k="Notes">{ota.available.notes}</Row>}
              <Row k="Image"><span class="mono small">{ota.available.url}</span>{ota.available.size ? <span class="muted"> · {fmtBytes(ota.available.size)}</span> : null}</Row>
              <div class="row" style="margin-top:.5rem">
                <button class="primary" disabled={busy} onClick={() => { if (confirm(`Install ${ota.available!.version} now? The device reboots afterwards.`)) void act(() => post("/ota", {}), "Installing…"); }}>Install {ota.available.version}</button>
                <button disabled={busy} onClick={() => act(() => post("/ota/check"), "Checking…")}>Check again</button>
              </div>
            </>
          ) : (
            <div class="row">
              <button disabled={busy || !manifest.manifest_url} onClick={() => act(() => post("/ota/check"), "Checking…")}>Check for updates</button>
              {ota?.manifest.last_check_s !== null && ota?.manifest.last_check_s !== undefined && <span class="muted small">last check {fmtDuration(ota.manifest.last_check_s)} ago{ota.manifest.next_check_s !== null ? `, next in ${fmtDuration(ota.manifest.next_check_s)}` : ""}</span>}
            </div>
          )}
          <h3>From a URL</h3>
          <form class="row" onSubmit={(e) => { e.preventDefault(); if (url && confirm(`Install the image at ${url}? Only images signed for this device are accepted; the device reboots afterwards.`)) void act(() => post("/ota", { url }), "Installing…"); }}>
            <input type="url" placeholder="http(s)://…/espos.bin" style="min-width:20rem" value={url} onInput={(e) => setUrl((e.target as HTMLInputElement).value)} />
            <button type="submit" disabled={busy || !url}>Install</button>
          </form>
        </section>

        <section class="card">
          <h2>Update source</h2>
          <form onSubmit={(e) => { e.preventDefault(); void save({ manifest_url: manifest.manifest_url || null, channel: manifest.channel || null }, "Update source saved."); }}>
            <div class="row"><input type="url" placeholder="https://…/manifest.json" style="min-width:20rem;flex:1" value={manifest.manifest_url} onInput={(e) => setManifest({ ...manifest, manifest_url: (e.target as HTMLInputElement).value })} /></div>
            <div class="row">
              <label>channel <input type="text" style="width:8rem" value={manifest.channel} onInput={(e) => setManifest({ ...manifest, channel: (e.target as HTMLInputElement).value })} /></label>
              <button type="submit">Save</button>
            </div>
          </form>
          <div class="row" style="margin-top:.5rem">
            <label><input type="checkbox" checked={!!cfg?.["auto_check"]} onChange={(e) => void save({ auto_check: (e.target as HTMLInputElement).checked }, "Saved.")} /> check automatically</label>
            <label><input type="checkbox" checked={!!cfg?.["auto_install"]} onChange={(e) => void save({ auto_install: (e.target as HTMLInputElement).checked }, "Saved.")} /> install automatically</label>
            <label><input type="checkbox" checked={!!cfg?.["allow_insecure"]} onChange={(e) => void save({ allow_insecure: (e.target as HTMLInputElement).checked }, "Saved.")} /> skip TLS certificate check</label>
          </div>
          <p class="muted small">Manifest format and signing: docs/ota.md. Check interval and rollback timeout are under <a href="/config" onClick={(e) => { e.preventDefault(); navigate("/config"); }}>Config → Firmware updates</a>.</p>
        </section>
      </div>
    </>
  );
}
