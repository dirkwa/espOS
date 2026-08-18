// SPDX-License-Identifier: Apache-2.0
// Live log: the ring is fetched by sequence number; the `logs` SSE event
// says when there is more, so the page never polls.
import { useEffect, useRef, useState } from "preact/hooks";
import { get, put, useStore, logsSeqStore, errText, fmtBytes, type LogsDoc } from "../api";
import { Msg } from "../app";

const LEVELS = ["none", "error", "warn", "info", "debug", "verbose"];

export function LogsPage() {
  const [lines, setLines] = useState<string[]>([]);
  const [next, setNext] = useState(0);
  const [stats, setStats] = useState<Pick<LogsDoc, "first" | "dropped" | "size" | "used"> | null>(null);
  const [filter, setFilter] = useState("");
  const [follow, setFollow] = useState(true);
  const [paused, setPaused] = useState(false);
  const [msg, setMsg] = useState("");
  const [lvl, setLvl] = useState({ tag: "*", level: "info" });
  const seq = useStore(logsSeqStore);
  const pre = useRef<HTMLPreElement>(null);
  const nextRef = useRef(0);

  async function fetchMore(reset = false) {
    try {
      const d = await get<LogsDoc>(`/logs?after=${reset ? 0 : nextRef.current - 1}&limit=1000`);
      setStats({ first: d.first, dropped: d.dropped, size: d.size, used: d.used });
      if (reset) setLines(d.lines);
      else if (d.lines.length) setLines((l) => [...l, ...d.lines].slice(-3000));
      if (d.gap && !reset) setLines((l) => [...l, `-- ${d.from - nextRef.current} line(s) missed (ring overwritten) --`]);
      nextRef.current = d.next;
      setNext(d.next);
    } catch (e) { setMsg(errText(e)); }
  }
  useEffect(() => { void fetchMore(true); }, []);
  useEffect(() => { if (!paused && seq !== undefined && seq > nextRef.current) void fetchMore(); }, [seq, paused]);
  useEffect(() => { if (follow && pre.current) pre.current.scrollTop = pre.current.scrollHeight; }, [lines, follow]);

  const shown = filter ? lines.filter((l) => l.toLowerCase().includes(filter.toLowerCase())) : lines;
  async function applyLevel(e: Event) {
    e.preventDefault();
    try { await put("/logs/level", lvl); setMsg(""); } catch (err) { setMsg(errText(err)); }
  }
  function download() {
    const blob = new Blob([lines.join("\n") + "\n"], { type: "text/plain" });
    const a = document.createElement("a"); a.href = URL.createObjectURL(blob); a.download = "espos-log.txt"; a.click();
  }

  return (
    <>
      <h1>Logs</h1>
      <div class="row spread">
        <div class="row">
          <input type="search" placeholder="filter" value={filter} onInput={(e) => setFilter((e.target as HTMLInputElement).value)} />
          <label><input type="checkbox" checked={follow} onChange={(e) => setFollow((e.target as HTMLInputElement).checked)} /> follow</label>
          <button onClick={() => setPaused(!paused)}>{paused ? "Resume" : "Pause"}</button>
          <button onClick={() => setLines([])}>Clear</button>
          <button onClick={download}>Download</button>
        </div>
        <form class="row" onSubmit={applyLevel}>
          <input type="text" style="width:9rem" placeholder="tag or *" value={lvl.tag} onInput={(e) => setLvl({ ...lvl, tag: (e.target as HTMLInputElement).value })} />
          <select value={lvl.level} onChange={(e) => setLvl({ ...lvl, level: (e.target as HTMLSelectElement).value })}>{LEVELS.map((l) => <option key={l}>{l}</option>)}</select>
          <button type="submit">Set level</button>
        </form>
      </div>
      <Msg text={msg} />
      <pre class="log" ref={pre} onScroll={() => { const p = pre.current!; if (p.scrollTop + p.clientHeight < p.scrollHeight - 20 && follow) setFollow(false); }}>
        {shown.map((l, i) => <span key={i} class={l[0] ?? ""}>{l}{"\n"}</span>)}
        {!shown.length && <span class="muted">{filter ? "nothing matches" : "no lines yet"}</span>}
      </pre>
      <p class="muted small">
        {shown.length} line{shown.length === 1 ? "" : "s"} shown · seq {next}
        {stats && <> · ring {fmtBytes(stats.used)} of {fmtBytes(stats.size)} · {stats.dropped} overwritten</>}
        {paused && " · paused"}
      </p>
    </>
  );
}
