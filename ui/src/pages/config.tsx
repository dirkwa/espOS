// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
// Generic settings editor rendered from the JSON Schema (GET /config/schema).
import { useEffect, useMemo, useState } from "preact/hooks";
import { get, put, post, useStore, configChangeStore, errText, randomKey, toDisplay, fromDisplay, type ConfigDoc, type ConfigSchema, type JsonSchemaProp, type PutResult } from "../api";
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
  const [group, setGroup] = useState<string>("");
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
  // A descriptor may put its keys into named groups; the namespace's own tab
  // row then becomes a second level. Keys without a group share the first
  // ("General") tab, so a namespace that groups nothing renders as before.
  const groups = useMemo(() => {
    const seen: string[] = [];
    for (const p of Object.values(nsSchema?.properties ?? {})) {
      const g = p["x-espos-group"] ?? "";
      if (!seen.includes(g)) seen.push(g);
    }
    return seen.length > 1 ? seen : [];
  }, [nsSchema]);
  useEffect(() => { setGroup(""); }, [ns]);
  const activeGroup = groups.length && groups.includes(group) ? group : groups[0] ?? "";
  const fields = Object.entries(nsSchema?.properties ?? {})
    .filter(([, p]) => !groups.length || (p["x-espos-group"] ?? "") === activeGroup);
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
          <h2>{nsSchema.title ?? ns} <span class="muted small" style="font-weight:400">v{nsSchema["x-espos-version"] ?? 1}</span>
            {nsSchema["x-espos-runtime"] && <span class="badge muted" style="margin-left:.5rem;font-weight:400" title="registered by a running node, not compiled in">node</span>}</h2>
          {nsSchema.description && <p class="muted small">{nsSchema.description}</p>}
          {groups.length > 0 && (
            <nav class="row" style="margin-bottom:.5rem">
              {groups.map((g) => (
                <a key={g} href="#" class={`badge ${g === activeGroup ? "ok" : "muted"}`} style="padding:.25rem .7rem"
                  onClick={(e) => { e.preventDefault(); setGroup(g); }}>{g || "General"}</a>
              ))}
            </nav>
          )}
          {fields.map(([k, p]) => (
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
  const ro = !!p.readOnly;
  const isDefault = !dirty && JSON.stringify(value) === JSON.stringify(p.default);
  // Everything below the conversion works in display units; only onChange
  // crosses back, so a number never round-trips through the transform twice.
  const shown = toDisplay(p, value);
  const send = (v: unknown) => onChange(fromDisplay(p, v));
  let ctl;
  if (ro) {
    // Shown, never editable: the device refuses the write anyway, so offering
    // the control would only produce a save that fails.
    ctl = <span class="mono" id={id}>{shown === null || shown === undefined || shown === "" ? "—" : String(shown)}</span>;
  } else if (p["x-espos-format"] === "table") {
    ctl = <TableEditor id={id} p={p} value={value} onChange={onChange} />;
  } else if (p.type === "boolean") {
    ctl = <input id={id} type="checkbox" checked={!!value} onChange={(e) => onChange((e.target as HTMLInputElement).checked)} />;
  } else if (p.enum) {
    ctl = <select id={id} value={String(value ?? "")} onChange={(e) => onChange((e.target as HTMLSelectElement).value)}>{p.enum.map((o) => <option key={o} value={o}>{o}</option>)}</select>;
  } else if (p.type === "integer" || p.type === "number") {
    ctl = <input id={id} type="number" value={shown === null || shown === undefined ? "" : String(shown)}
      min={toDisplay(p, p.minimum) as number | undefined} max={toDisplay(p, p.maximum) as number | undefined} step="any"
      onInput={(e) => { const s = (e.target as HTMLInputElement).value; send(s === "" ? null : Number(s)); }} />;
  } else if (secret) {
    // The API key gets a Generate button: a key nobody typed is a key nobody
    // reused; it is shown once, in full, so it can be written down.
    ctl = <SecretInput id={id} set={value === SENTINEL} generate={ns === "httpd" && k === "api_key"} onChange={onChange} />;
  } else {
    ctl = <input id={id} type="text" value={String(value ?? "")} maxLength={p.maxLength} pattern={p.pattern} placeholder={blob ? "base64" : ""} onInput={(e) => onChange((e.target as HTMLInputElement).value)} />;
  }
  const limits: string[] = [];
  const round = (v: unknown) => (typeof v === "number" ? Math.round(v * 1e4) / 1e4 : v);
  // The range is stated in whatever unit the field is typed in, or a value
  // shown in degrees would carry a limit in radians.
  if (p.minimum !== undefined || p.maximum !== undefined) {
    limits.push(`${p.minimum === undefined ? "…" : round(toDisplay(p, p.minimum))} – ${p.maximum === undefined ? "…" : round(toDisplay(p, p.maximum))}`);
  }
  if (p.maxLength !== undefined && p.type === "string" && !p.enum && p["x-espos-format"] !== "table") limits.push(`≤ ${p.maxLength} chars`);
  if (p["x-espos-format"] === "table") limits.push(`≤ ${p.maxLength ?? 3999} bytes of JSON`);
  if (blob && p["x-espos-maxBytes"]) limits.push(`≤ ${p["x-espos-maxBytes"]} bytes`);
  if (ro) limits.push("read-only");
  return (
    <div class={`field${dirty ? " dirty" : ""}`}>
      <label for={id}>{p.title ?? k}{p["x-espos-restartRequired"] && <span class="muted" title="takes effect after reboot"> ↻</span>}</label>
      <div class="ctl">
        {ctl}
        {p["x-espos-unit"] && <span class="unit">{p["x-espos-unit"]}</span>}
        {!isDefault && !secret && !ro && p.default !== undefined && <button class="small" title={`default: ${JSON.stringify(p.default)}`} onClick={() => onChange(p.default)}>default</button>}
      </div>
      <div class="help">
        {p.description}{p.description ? " " : ""}
        <span class="mono muted">{ns}.{k}</span>{limits.length ? <span class="muted"> · {limits.join(" · ")}</span> : null}
      </div>
      {error && <div class="err">{error}</div>}
    </div>
  );
}

function SecretInput({ id, set, generate, onChange }: { id: string; set: boolean; generate?: boolean; onChange: (v: unknown) => void }) {
  const [editing, setEditing] = useState(false);
  const [v, setV] = useState("");
  const [shown, setShown] = useState("");   // a generated key, displayed until saved or edited
  function gen() {
    const k = randomKey(20);
    setV(k); setShown(k); setEditing(true); onChange(k);
  }
  if (!editing) {
    return (
      <>
        <span class="muted">{set ? "•••••••• (set)" : "not set"}</span>
        <button onClick={() => setEditing(true)}>{set ? "Change" : "Set"}</button>
        {generate && <button onClick={gen} title="20 random characters, shown once">Generate</button>}
        {set && <button onClick={() => onChange("")}>Clear</button>}
      </>
    );
  }
  return (
    <>
      <input id={id} type="password" value={v} autofocus onInput={(e) => { const s = (e.target as HTMLInputElement).value; setV(s); setShown(""); onChange(s); }} />
      {generate && <button onClick={gen}>Generate</button>}
      <button onClick={() => { setEditing(false); setV(""); setShown(""); onChange(SENTINEL); }}>Cancel</button>
      {shown && <div class="keybox">New key: <code>{shown}</code> — write it down; it is shown only now. After Save every browser, the designer and any script need it.</div>}
    </>
  );
}

/*
 * Rows of a table key. The stored value is a plain JSON string, not a blob,
 * so what the device holds is what an export shows and what this edits — no
 * base64 in the middle. A row is an array of cells in column order; a value
 * the device already holds in the older object-per-row shape is accepted on
 * read so an existing table is never silently emptied.
 */
function TableEditor({ id, p, value, onChange }: { id: string; p: JsonSchemaProp; value: unknown; onChange: (v: unknown) => void }) {
  const cols = p["x-espos-columns"] ?? [];
  const max = p.maxLength ?? 3999;
  const [raw, setRaw] = useState(false);
  const text = typeof value === "string" ? value : "";
  let rows: string[][] = [];
  let parseError = "";
  try {
    const parsed = text.trim() === "" ? [] : (JSON.parse(text) as unknown);
    if (!Array.isArray(parsed)) throw new Error("expected an array of rows");
    rows = parsed.map((r) =>
      Array.isArray(r) ? cols.map((_, i) => fmt(r[i]))
                       : cols.map((c) => fmt((r as Record<string, unknown>)?.[c])));
  } catch (e) {
    parseError = e instanceof Error ? e.message : String(e);
  }

  // Cells are typed as text and parsed back to numbers where they look like
  // numbers: a curve is numeric, but a label column must survive intact.
  function write(next: string[][]) {
    const doc = JSON.stringify(next.map((r) => r.map(cell)));
    onChange(doc);
  }
  const setCell = (ri: number, ci: number, v: string) => {
    const next = rows.map((r) => [...r]);
    next[ri]![ci] = v;
    write(next);
  };

  if (parseError || raw) {
    return (
      <>
        <textarea id={id} rows={4} style="min-width:22rem;font-family:var(--mono,monospace)" maxLength={max}
          value={text} onInput={(e) => onChange((e.target as HTMLTextAreaElement).value)} />
        {parseError
          ? <span class="err">not valid JSON ({parseError}) — edit it as text</span>
          : <button class="small" onClick={() => setRaw(false)}>rows</button>}
      </>
    );
  }
  return (
    <div style="flex-basis:100%">
      <table>
        <thead><tr>{cols.map((c) => <th key={c}>{c}</th>)}<th style="width:2rem" /></tr></thead>
        <tbody>
          {rows.map((r, ri) => (
            <tr key={ri}>
              {cols.map((c, ci) => (
                <td key={c}><input type="text" style="min-width:5rem" value={r[ci] ?? ""}
                  onInput={(e) => setCell(ri, ci, (e.target as HTMLInputElement).value)} /></td>
              ))}
              <td><button class="small" title="remove this row"
                onClick={() => write(rows.filter((_, i) => i !== ri))}>−</button></td>
            </tr>
          ))}
        </tbody>
      </table>
      <div class="row small">
        <button onClick={() => write([...rows, cols.map(() => "")])}>Add row</button>
        <button class="small" onClick={() => setRaw(true)}>edit as JSON</button>
        <span class="muted">{rows.length} row{rows.length === 1 ? "" : "s"} · {text.length}/{max} bytes</span>
      </div>
    </div>
  );
}

function fmt(v: unknown): string {
  if (v === null || v === undefined) return "";
  return typeof v === "string" ? v : JSON.stringify(v);
}

function cell(v: string): unknown {
  const t = v.trim();
  if (t === "") return "";
  // Number() accepts "" and whitespace as 0, hence the guard above; anything
  // that is not a finite number stays the string the user typed.
  const n = Number(t);
  return Number.isFinite(n) && t === String(n) ? n : v;
}
