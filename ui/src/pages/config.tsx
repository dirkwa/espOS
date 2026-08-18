// SPDX-License-Identifier: Apache-2.0
// Generic settings editor rendered from the JSON Schema (GET /config/schema).
import { useEffect, useMemo, useState } from "preact/hooks";
import { get, put, post, useStore, configChangeStore, errText, type ConfigDoc, type ConfigSchema, type JsonSchemaProp, type PutResult } from "../api";
import { Msg, useAsync } from "../app";

const SENTINEL = "********";

export function ConfigPage() {
  const schema = useAsync(() => get<ConfigSchema>("/config/schema"));
  const [cfg, setCfg] = useState<ConfigDoc | null>(null);
  const [edits, setEdits] = useState<ConfigDoc>({});
  const [msg, setMsg] = useState("");
  const [ok, setOk] = useState("");
  const [errPath, setErrPath] = useState("");
  const [restart, setRestart] = useState(false);
  const [ns, setNs] = useState<string>(location.hash.slice(1));
  const remote = useStore(configChangeStore);

  const load = () => get<ConfigDoc>("/config").then(setCfg, (e: unknown) => setMsg(errText(e)));
  useEffect(() => { void load(); }, []);
  useEffect(() => { if (remote) void load(); }, [remote?.n]);       // someone else changed something → refresh (edits kept)

  const nsList = useMemo(() => Object.keys(schema.data?.properties ?? {}), [schema.data]);
  useEffect(() => { if (!ns && nsList[0]) setNs(nsList[0]); }, [nsList, ns]);
  const dirtyCount = Object.values(edits).reduce((a, o) => a + Object.keys(o).length, 0);

  function edit(n: string, k: string, v: unknown) {
    setEdits((e) => {
      const cur = { ...(e[n] ?? {}) };
      const orig = cfg?.[n]?.[k];
      if (JSON.stringify(v) === JSON.stringify(orig)) delete cur[k]; else cur[k] = v;
      const out = { ...e, [n]: cur };
      if (!Object.keys(cur).length) delete out[n];
      return out;
    });
  }
  async function save() {
    setMsg(""); setOk(""); setErrPath("");
    try {
      const r = await put<PutResult>("/config", edits);
      setEdits({});
      await load();
      setOk(r.changed.length ? `Saved ${r.changed.length} setting${r.changed.length > 1 ? "s" : ""}: ${r.changed.join(", ")}` : "Nothing changed.");
      if (r.restart_required) setRestart(true);
    } catch (e) {
      setMsg(errText(e));
      const p = (e as { body?: { path?: string } }).body?.path;
      if (p) { setErrPath(p); const n = p.split(".")[0]; if (n && nsList.includes(n)) setNs(n); }
    }
  }
  async function reboot() {
    try { await post("/system/reboot"); setRestart(false); setOk("Rebooting…"); } catch (e) { setMsg(errText(e)); }
  }
  async function resetNs() {
    if (!confirm(`Reset every setting in "${schema.data?.properties[ns]?.title ?? ns}" to its default?`)) return;
    const patch: Record<string, null> = {};
    for (const k of Object.keys(schema.data?.properties[ns]?.properties ?? {})) patch[k] = null;
    try { const r = await put<PutResult>("/config", { [ns]: patch }); setEdits((e) => { const o = { ...e }; delete o[ns]; return o; }); await load(); setOk(`Reset ${r.changed.length} setting(s).`); if (r.restart_required) setRestart(true); } catch (e) { setMsg(errText(e)); }
  }
  function exportJson() {
    const blob = new Blob([JSON.stringify(cfg, null, 2)], { type: "application/json" });
    const a = document.createElement("a"); a.href = URL.createObjectURL(blob); a.download = "espos-config.json"; a.click();
  }
  async function importJson(e: Event) {
    const f = (e.target as HTMLInputElement).files?.[0];
    if (!f) return;
    try {
      const doc = JSON.parse(await f.text()) as ConfigDoc;
      const r = await put<PutResult>("/config", doc);
      await load(); setOk(`Imported: ${r.changed.length} change(s).`); if (r.restart_required) setRestart(true);
    } catch (err) { setMsg(errText(err)); }
    (e.target as HTMLInputElement).value = "";
  }

  const nsSchema = schema.data?.properties[ns];
  return (
    <>
      <h1>Config</h1>
      {schema.error && <Msg text={schema.error} />}
      <div class="row spread">
        <nav class="row">
          {nsList.map((n) => (
            <a key={n} href={`#${n}`} class={`badge ${n === ns ? "ok" : "muted"}`} style="padding:.3rem .8rem" onClick={(e) => { e.preventDefault(); setNs(n); history.replaceState(null, "", `#${n}`); }}>
              {schema.data?.properties[n]?.title ?? n}{edits[n] ? " •" : ""}
            </a>
          ))}
        </nav>
        <span class="row small">
          <button onClick={exportJson} disabled={!cfg}>Export</button>
          <label class="badge" style="padding:.35rem .8rem;cursor:pointer">Import… <input type="file" accept="application/json" style="display:none" onChange={importJson} /></label>
        </span>
      </div>
      <Msg text={msg} />
      <Msg text={ok} kind="ok" />
      {restart && <div class="msg warn row spread"><span>A changed setting takes effect after a reboot.</span><button onClick={reboot}>Reboot now</button></div>}
      {nsSchema && cfg && (
        <section class="card">
          <h2>{nsSchema.title ?? ns} <span class="muted small" style="font-weight:400">v{nsSchema["x-espos-version"] ?? 1}</span></h2>
          {nsSchema.description && <p class="muted small">{nsSchema.description}</p>}
          {Object.entries(nsSchema.properties).map(([k, p]) => (
            <Field key={k} ns={ns} k={k} p={p} value={edits[ns]?.[k] !== undefined ? edits[ns]![k] : cfg[ns]?.[k]} dirty={edits[ns]?.[k] !== undefined}
              error={errPath === `${ns}.${k}` ? msg : ""} onChange={(v) => edit(ns, k, v)} />
          ))}
          <div class="sticky-actions">
            <button class="primary" disabled={!dirtyCount} onClick={save}>Save{dirtyCount ? ` (${dirtyCount})` : ""}</button>
            <button disabled={!dirtyCount} onClick={() => setEdits({})}>Discard</button>
            <span style="flex:1" />
            <button class="danger" onClick={resetNs}>Reset section to defaults</button>
          </div>
        </section>
      )}
    </>
  );
}

function Field({ ns, k, p, value, dirty, error, onChange }: { ns: string; k: string; p: JsonSchemaProp; value: unknown; dirty: boolean; error: string; onChange: (v: unknown) => void }) {
  const id = `${ns}-${k}`;
  const secret = !!p["x-espos-secret"];
  const blob = p["x-espos-type"] === "blob";
  const isDefault = !dirty && JSON.stringify(value) === JSON.stringify(p.default);
  let ctl;
  if (p.type === "boolean") {
    ctl = <input id={id} type="checkbox" checked={!!value} onChange={(e) => onChange((e.target as HTMLInputElement).checked)} />;
  } else if (p.enum) {
    ctl = <select id={id} value={String(value ?? "")} onChange={(e) => onChange((e.target as HTMLSelectElement).value)}>{p.enum.map((o) => <option key={o} value={o}>{o}</option>)}</select>;
  } else if (p.type === "integer" || p.type === "number") {
    ctl = <input id={id} type="number" value={value === null || value === undefined ? "" : String(value)} min={p.minimum} max={p.maximum} step={p.type === "integer" ? 1 : "any"}
      onInput={(e) => { const s = (e.target as HTMLInputElement).value; onChange(s === "" ? null : Number(s)); }} />;
  } else if (secret) {
    ctl = <SecretInput id={id} set={value === SENTINEL} onChange={onChange} />;
  } else {
    ctl = <input id={id} type="text" value={String(value ?? "")} maxLength={p.maxLength} pattern={p.pattern} placeholder={blob ? "base64" : ""} onInput={(e) => onChange((e.target as HTMLInputElement).value)} />;
  }
  const limits: string[] = [];
  if (p.minimum !== undefined || p.maximum !== undefined) limits.push(`${p.minimum ?? "…"} – ${p.maximum ?? "…"}`);
  if (p.maxLength !== undefined && p.type === "string" && !p.enum) limits.push(`≤ ${p.maxLength} chars`);
  if (blob && p["x-espos-maxBytes"]) limits.push(`≤ ${p["x-espos-maxBytes"]} bytes`);
  return (
    <div class={`field${dirty ? " dirty" : ""}`}>
      <label for={id}>{p.title ?? k}{p["x-espos-restartRequired"] && <span class="muted" title="takes effect after reboot"> ↻</span>}</label>
      <div class="ctl">
        {ctl}
        {p["x-espos-unit"] && <span class="unit">{p["x-espos-unit"]}</span>}
        {!isDefault && !secret && p.default !== undefined && <button class="small" title={`default: ${JSON.stringify(p.default)}`} onClick={() => onChange(p.default)}>default</button>}
      </div>
      <div class="help">
        {p.description}{p.description ? " " : ""}
        <span class="mono muted">{ns}.{k}</span>{limits.length ? <span class="muted"> · {limits.join(" · ")}</span> : null}
      </div>
      {error && <div class="err">{error}</div>}
    </div>
  );
}

function SecretInput({ id, set, onChange }: { id: string; set: boolean; onChange: (v: unknown) => void }) {
  const [editing, setEditing] = useState(false);
  const [v, setV] = useState("");
  if (!editing) {
    return (
      <>
        <span class="muted">{set ? "•••••••• (set)" : "not set"}</span>
        <button onClick={() => setEditing(true)}>{set ? "Change" : "Set"}</button>
        {set && <button onClick={() => onChange("")}>Clear</button>}
      </>
    );
  }
  return (
    <>
      <input id={id} type="password" value={v} autofocus onInput={(e) => { const s = (e.target as HTMLInputElement).value; setV(s); onChange(s); }} />
      <button onClick={() => { setEditing(false); setV(""); onChange(SENTINEL); }}>Cancel</button>
    </>
  );
}
