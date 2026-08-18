// SPDX-License-Identifier: Apache-2.0
//
// espOS API mock for UI development: implements docs/api.md well enough to
// exercise every page without a device — config with schema validation of
// the basics, a simulated WiFi state machine, SignalK discovery/token flow,
// a log ring, SSE. Zero dependencies (node:http only).
//
//   node mock/server.mjs [port]      (vite dev starts it automatically)
//
// The schema is regenerated from the real descriptors when python3 is
// available (tools/espos_gen_config.py); otherwise mock/schema.json is used.
import http from "node:http";
import { spawnSync } from "node:child_process";
import { readFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..", "..");

function loadSchema() {
  try {
    const gen = path.join(root, "tools", "espos_gen_config.py");
    const descs = ["main/config/app.json", "components/espos_httpd/config/httpd.json",
      "components/espos_wifi/config/wifi.json", "components/espos_sk/config/sk.json"].map((p) => path.join(root, p));
    const out = mkdtempSync(path.join(tmpdir(), "espos-mock-"));
    const r = spawnSync("python3", [gen, "--schema-out", path.join(out, "s.json"), "--c-out", path.join(out, "c.c"),
      "--h-out", path.join(out, "h.h"), ...descs], { stdio: "ignore" });
    if (r.status === 0) return JSON.parse(readFileSync(path.join(out, "s.json"), "utf8"));
  } catch { /* fall through */ }
  return JSON.parse(readFileSync(path.join(here, "schema.json"), "utf8"));
}

export function startMock(port = 8484) {
  const schema = loadSchema();
  const etag = "mock" + Math.abs(hash(JSON.stringify(schema))).toString(16).slice(0, 12);

  // ---- state
  const defaults = {};
  for (const [ns, o] of Object.entries(schema.properties)) {
    defaults[ns] = {};
    for (const [k, p] of Object.entries(o.properties)) defaults[ns][k] = p.default ?? null;
  }
  const stored = { wifi: { ssid0: "Marina-Guest", psk0: "hunter22" } };
  const effective = () => {
    const out = {};
    for (const ns of Object.keys(defaults)) out[ns] = { ...defaults[ns], ...(stored[ns] ?? {}) };
    return out;
  };
  const isSecret = (ns, k) => !!schema.properties[ns]?.properties[k]?.["x-espos-secret"];
  const redacted = (ns) => {
    const doc = effective();
    const pick = ns ? { [ns]: doc[ns] } : doc;
    for (const [n, o] of Object.entries(pick)) for (const k of Object.keys(o)) if (isSecret(n, k)) o[k] = o[k] ? "********" : "";
    return pick;
  };

  const boot = Date.now();
  const logs = [];
  let logSeq = 1;
  const clients = new Set();
  const log = (lvl, tag, msg) => {
    const line = `${lvl} (${Date.now() - boot}) ${tag}: ${msg}`;
    logs.push({ seq: logSeq++, line });
    while (logs.length > 400) logs.shift();
    console.log("  [mock] " + line);
  };
  const emit = (event, data) => {
    const chunk = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
    for (const c of clients) c.write(chunk);
  };
  let logsDirty = false;
  const origLog = log;
  const logAndMark = (l, t, m) => { origLog(l, t, m); logsDirty = true; };
  setInterval(() => { if (logsDirty) { logsDirty = false; emit("logs", { next: logSeq }); } }, 500);

  const wifi = {
    state: "unconfigured", sta_enabled: true, hostname: "espos-1a2b", reason: { code: 0, text: "" },
    connect_count: 0, disconnect_count: 0, attempt: 0,
    portal: { active: true, ssid: "espOS-1a2b", ip: "192.168.4.1", clients: 0 },
  };
  let wifiTimer = null;
  const wifiEmit = () => emit("wifi", wifiStatus());
  const wifiStatus = () => ({ ...wifi });
  function wifiEval() {
    clearTimeout(wifiTimer);
    const cfg = effective().wifi;
    const nets = [0, 1, 2, 3].map((i) => cfg[`ssid${i}`]).filter(Boolean);
    if (!cfg.sta_enabled) {
      Object.assign(wifi, { state: "disabled", reason: { code: 1005, text: "station disabled by configuration" } });
      wifi.portal.active = true; delete wifi.ip; return wifiEmit();
    }
    if (!nets.length) {
      Object.assign(wifi, { state: "unconfigured", reason: { code: 0, text: "" } });
      wifi.portal.active = true; delete wifi.ip; return wifiEmit();
    }
    const idx = nets.findIndex((s) => scanResults.some((r) => r.ssid === s));
    Object.assign(wifi, { state: "connecting", ssid: nets[Math.max(idx, 0)], network_index: Math.max(idx, 0), attempt: wifi.attempt + 1 });
    delete wifi.ip; wifiEmit();
    logAndMark("I", "espos_wifi", `connecting to ${wifi.ssid}`);
    wifiTimer = setTimeout(() => {
      if (idx < 0) {
        Object.assign(wifi, { state: "backoff", reason: { code: 201, text: "no AP with that SSID found (NO_AP_FOUND)" }, backoff_ms: 8000, disconnect_count: wifi.disconnect_count + 1 });
        wifi.portal.active = true; wifiEmit();
        logAndMark("W", "espos_wifi", `disconnected: NO_AP_FOUND (201), retry in 8 s`);
        wifiTimer = setTimeout(wifiEval, 8000);
        return;
      }
      Object.assign(wifi, { state: "obtaining_ip" }); wifiEmit();
      wifiTimer = setTimeout(() => {
        const r = scanResults.find((x) => x.ssid === wifi.ssid);
        Object.assign(wifi, { state: "connected", ip: "192.168.1.42", gateway: "192.168.1.1", netmask: "255.255.255.0",
          rssi: r.rssi, channel: r.channel, bssid: r.bssid, connect_count: wifi.connect_count + 1, reason: { code: 0, text: "" } });
        delete wifi.backoff_ms; wifi.portal.active = false; wifiEmit();
        logAndMark("I", "espos_wifi", `connected: ${wifi.ssid} ip=${wifi.ip} rssi=${wifi.rssi}`);
        skDiscover();
      }, 1200);
    }, 1500);
  }
  const scanResults = [
    { ssid: "Marina-Guest", bssid: "de:ad:be:ef:00:02", rssi: -71, channel: 11, auth: "wpa2" },
    { ssid: "Boat", bssid: "de:ad:be:ef:00:01", rssi: -48, channel: 6, auth: "wpa2/wpa3" },
    { ssid: "Harbour Cafe", bssid: "de:ad:be:ef:00:07", rssi: -84, channel: 1, auth: "open" },
  ];
  let scan = { scanning: false, age_s: null, results: [] };
  let scanAt = 0;

  // ---- SignalK
  const servers = [
    { host: "192.168.1.10", port: 80, self: "urn:mrn:signalk:uuid:0e6d1a1a-1111-4111-8111-000000000099", name: "boat", roles: "master,main", swname: "signalk-server", swvers: "2.31.1" },
    { host: "192.168.1.11", port: 3000, self: "urn:mrn:signalk:uuid:0e6d1a1a-2222-4222-8222-000000000042", name: "nav-pc", roles: "master,main", swname: "signalk-server", swvers: "2.30.0" },
  ];
  const sk = {
    token: { state: "no_server", has_token: false, busy: false, last_http_status: 0, last_error: "", counts: { requests: 0, approved: 0, denied: 0, unauthorized: 0 } },
    server: { source: "none" }, client_id: "9cf791de-aa92-4830-a958-0388a42ef72b", description: "espOS espos-1a2b", permissions: "readwrite",
    discovery: { enabled: true, count: 0, last_s: null },
    ws: { enabled: true, connected: false, reconnects: 0, sent: 0, send_errors: 0, pending: 0, buffered: 0, buffered_bytes: 0, dropped: 0, last_error: "", meta: { declared: 3, reconciled: 0 } },
  };
  let discovered = [];
  let discoverAt = 0;
  let pendingAt = 0;
  const skEmit = () => emit("sk", skStatus());
  const skStatus = () => {
    const s = structuredClone(sk);
    s.discovery.last_s = discoverAt ? Math.round((Date.now() - discoverAt) / 1000) : null;
    if (s.token.state === "pending") s.token.pending_s = Math.round((Date.now() - pendingAt) / 1000);
    return s;
  };
  function skDiscover() {
    if (wifi.state !== "connected") return;
    discovered = servers.map((s) => ({ ...s, seen_s: 0 }));
    discoverAt = Date.now();
    sk.discovery.count = discovered.length;
    logAndMark("I", "espos_sk", `discovery: ${discovered.length} server(s)`);
    emit("sk_servers", serversDoc());
    if (sk.server.source === "none") {
      const cfg = effective().sk;
      const pick = cfg.server_host ? { host: cfg.server_host, port: cfg.server_port, self: cfg.server_self || "", source: "manual", name: cfg.server_host }
        : { ...discovered[0], source: "discovered" };
      sk.server = { host: pick.host, port: pick.port, self: pick.self, source: pick.source, name: pick.name, swname: pick.swname ?? "", swvers: pick.swvers ?? "" };
      skRequest();
    }
  }
  const serversDoc = () => ({ servers: discovered.map((s) => ({ ...s, selected: s.self === sk.server.self })), last_s: discoverAt ? Math.round((Date.now() - discoverAt) / 1000) : null });
  let approveTimer = null;
  function skRequest() {
    if (sk.token.has_token) return;
    Object.assign(sk.token, { state: "requesting", busy: true }); skEmit();
    setTimeout(() => {
      Object.assign(sk.token, { state: "pending", busy: false, last_http_status: 202, pending_href: "/signalk/v1/access/requests/" + sk.client_id });
      sk.token.counts.requests++; pendingAt = Date.now(); skEmit();
      logAndMark("I", "espos_sk", "access request PENDING — approve it in the SignalK admin UI");
      // the "admin" approves after a while unless the UI pastes a token first
      clearTimeout(approveTimer);
      approveTimer = setTimeout(() => {
        if (sk.token.state !== "pending") return;
        Object.assign(sk.token, { state: "approved", has_token: true, approved_s: 0, last_http_status: 200 });
        delete sk.token.pending_href; sk.token.counts.approved++; skEmit();
        logAndMark("I", "espos_sk", "access APPROVED, token stored");
        wsConnect();
      }, 12000);
    }, 800);
  }
  let wsTimer = null;
  function wsConnect() {
    clearTimeout(wsTimer);
    if (!sk.token.has_token || !effective().sk.ws_enabled || wifi.state !== "connected") {
      if (sk.ws.connected) { sk.ws.connected = false; emit("sk_ws", sk.ws); }
      return;
    }
    Object.assign(sk.ws, { connected: true, connected_s: 0, reconnects: sk.ws.reconnects + 1, last_error: "" });
    sk.ws.meta.reconciled = sk.ws.meta.declared;
    emit("sk_ws", sk.ws); skEmit();
    logAndMark("I", "espos_skws", `stream connected to ${sk.server.host}:${sk.server.port}`);
    wsTimer = setInterval(() => { sk.ws.sent += 2; sk.ws.connected_s += 5; }, 5000);
  }
  function skForget() {
    Object.assign(sk.token, { state: "no_server", has_token: false, busy: false, last_http_status: 0, last_error: "" });
    delete sk.token.approved_s; delete sk.token.pending_href;
    clearInterval(wsTimer); sk.ws.connected = false; emit("sk_ws", sk.ws);
    logAndMark("W", "espos_sk", "token forgotten");
    skEmit();
    if (sk.server.source !== "none") skRequest();
  }

  // ---- HTTP
  const json = (res, status, body, headers = {}) => {
    const data = JSON.stringify(body);
    res.writeHead(status, { "Content-Type": "application/json", "Cache-Control": "no-store", ...headers });
    res.end(data);
  };
  const err = (res, status, code, message, extra = {}) => json(res, status, { error: code, message, ...extra });
  const body = (req) => new Promise((resolve) => { let b = ""; req.on("data", (c) => (b += c)); req.on("end", () => resolve(b)); });
  const needJson = (req, res) => {
    if (!/^application\/json/.test(req.headers["content-type"] ?? "")) { err(res, 415, "unsupported_media_type", "Content-Type: application/json required"); return false; }
    return true;
  };

  function validate(ns, k, v) {
    const p = schema.properties[ns]?.properties[k];
    if (!p) return `unknown key ${ns}.${k}`;
    if (v === null) return null;
    if (p.type === "boolean" && typeof v !== "boolean") return "expected boolean";
    if ((p.type === "integer" || p.type === "number") && typeof v !== "number") return "expected number";
    if (p.type === "integer" && !Number.isInteger(v)) return "expected integer";
    if (p.type === "string" && typeof v !== "string") return "expected string";
    if (typeof v === "number" && ((p.minimum !== undefined && v < p.minimum) || (p.maximum !== undefined && v > p.maximum))) return `out of range [${p.minimum},${p.maximum}]`;
    if (typeof v === "string" && p.maxLength !== undefined && v.length > p.maxLength) return `longer than ${p.maxLength}`;
    if (p.enum && !p.enum.includes(v)) return `not one of ${p.enum.join(", ")}`;
    if (p.pattern && !new RegExp(p.pattern).test(v)) return `does not match ${p.pattern}`;
    return null;
  }

  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, "http://x");
    const p = url.pathname;
    const m = req.method;
    if (!p.startsWith("/api/v1/")) return err(res, 404, "not_found", "no such resource");
    const r = p.slice("/api/v1".length);
    try {
      // ---- system
      if (r === "/system/info" && m === "GET") {
        return json(res, 200, { app: "espos", version: "0.5.0-mock", idf_version: "v6.0.2", chip: "esp32c6", chip_revision: 1, cores: 1,
          uptime_s: Math.round((Date.now() - boot) / 1000), free_heap: 214000 + Math.round(Math.random() * 3000), min_free_heap: 190000,
          reset_reason: "software", config_storage_reset: false, schema_etag: etag, ui_storage: true });
      }
      if (r === "/system/reboot" && m === "POST") { if (!needJson(req, res)) return; logAndMark("W", "espos_httpd", "restarting"); return json(res, 202, { status: "rebooting" }); }
      if (r === "/system/factory-reset" && m === "POST") { if (!needJson(req, res)) return; for (const k of Object.keys(stored)) delete stored[k]; wifiEval(); return json(res, 202, { status: "factory_reset", rebooting: true }); }
      if (r === "/system/coredump" && m === "GET") {
        return json(res, 200, { present: true, size: 18344, valid: true, task: "espos_skws", pc: "0x4200a5c2", app_elf_sha256: "3f9c1e2b7d0a44f1", version: 1,
          mcause: 7, mtval: "0x00000010", ra: "0x42009f80", sp: "0x3fc9e2d0", stackdump_bytes: 1024 });
      }
      if (r === "/system/coredump" && m === "DELETE") return json(res, 200, { status: "erased" });
      // ---- config
      if (r === "/config/schema" && m === "GET") {
        if (req.headers["if-none-match"] === `"${etag}"`) { res.writeHead(304); return res.end(); }
        res.writeHead(200, { "Content-Type": "application/schema+json", ETag: `"${etag}"` }); return res.end(JSON.stringify(schema));
      }
      if (r === "/config" && m === "GET") {
        const ns = url.searchParams.get("ns");
        if (ns && !schema.properties[ns]) return err(res, 404, "unknown_namespace", `no namespace ${ns}`);
        return json(res, 200, redacted(ns));
      }
      if (r === "/config" && m === "PUT") {
        if (!needJson(req, res)) return;
        let doc; try { doc = JSON.parse(await body(req)); } catch { return err(res, 400, "validation", "malformed JSON", { path: "" }); }
        if (!doc || typeof doc !== "object" || Array.isArray(doc)) return err(res, 400, "validation", "expected object of namespaces", { path: "" });
        for (const [ns, o] of Object.entries(doc)) {
          if (!schema.properties[ns]) return err(res, 400, "validation", `unknown namespace ${ns}`, { path: ns });
          if (!o || typeof o !== "object") return err(res, 400, "validation", "expected object", { path: ns });
          for (const [k, v] of Object.entries(o)) { const e = validate(ns, k, v); if (e) return err(res, 400, "validation", e, { path: `${ns}.${k}` }); }
        }
        const before = effective();
        const changed = [];
        let restart = false;
        for (const [ns, o] of Object.entries(doc)) for (const [k, v] of Object.entries(o)) {
          if (isSecret(ns, k) && v === "********") continue;
          stored[ns] ??= {};
          if (v === null) delete stored[ns][k]; else stored[ns][k] = v;
          if (JSON.stringify(effective()[ns][k]) !== JSON.stringify(before[ns][k])) {
            changed.push(`${ns}.${k}`);
            if (schema.properties[ns].properties[k]["x-espos-restartRequired"]) restart = true;
            emit("config", { ns, key: k });
            logAndMark("I", "espos_config", `changed ${ns}.${k}`);
          }
        }
        if (changed.some((c) => c.startsWith("wifi."))) wifiEval();
        if (changed.some((c) => c.startsWith("sk."))) { if (!effective().sk.ws_enabled) wsConnect(); else if (sk.token.has_token) wsConnect(); }
        return json(res, 200, { changed, restart_required: restart });
      }
      // ---- wifi
      if (r === "/wifi/status" && m === "GET") return json(res, 200, wifiStatus());
      if (r === "/wifi/scan" && m === "POST") {
        if (!needJson(req, res)) return;
        if (scan.scanning) return err(res, 409, "busy", "scan in progress");
        scan = { ...scan, scanning: true };
        setTimeout(() => { scanAt = Date.now(); scan = { scanning: false, age_s: 0, results: scanResults }; emit("wifi_scan", scanDoc()); logAndMark("I", "espos_wifi", `scan done: ${scanResults.length} networks`); }, 1500);
        return json(res, 202, { status: "scanning" });
      }
      const scanDoc = () => ({ ...scan, age_s: scanAt ? Math.round((Date.now() - scanAt) / 1000) : null });
      if (r === "/wifi/scan" && m === "GET") return json(res, 200, scanDoc());
      // ---- signalk
      if (r === "/sk/status" && m === "GET") return json(res, 200, skStatus());
      if (r === "/sk/servers" && m === "GET") return json(res, 200, serversDoc());
      if (r === "/sk/discover" && m === "POST") { if (!needJson(req, res)) return; setTimeout(skDiscover, 600); return json(res, 202, { status: "discovering" }); }
      if (r === "/sk/request" && m === "POST") { if (!needJson(req, res)) return; skRequest(); return json(res, 202, { status: "requesting" }); }
      if (r === "/sk/forget" && m === "POST") { if (!needJson(req, res)) return; skForget(); return json(res, 202, { status: "forgotten" }); }
      if (r === "/sk/token" && m === "POST") {
        if (!needJson(req, res)) return;
        let doc; try { doc = JSON.parse(await body(req)); } catch { doc = null; }
        if (!doc || typeof doc.token !== "string" || !doc.token) return err(res, 400, "validation", "expected {\"token\": \"...\"}");
        Object.assign(sk.token, { state: "verifying", busy: true }); skEmit();
        setTimeout(() => { Object.assign(sk.token, { state: "approved", has_token: true, busy: false, approved_s: 0, last_http_status: 200 }); skEmit(); wsConnect(); }, 900);
        return json(res, 202, { status: "verifying" });
      }
      if (r === "/sk/publish" && m === "POST") { if (!needJson(req, res)) return; sk.ws.pending++; return json(res, 202, { status: "queued" }); }
      // ---- logs
      if (r === "/logs" && m === "GET") {
        const after = Number(url.searchParams.get("after") ?? 0);
        const limit = Math.min(Number(url.searchParams.get("limit") ?? 200) || 200, 1000);
        const first = logs[0]?.seq ?? logSeq;
        const gap = after + 1 < first;
        const from = gap ? first : after + 1;
        const lines = logs.filter((l) => l.seq >= from).slice(0, limit);
        return json(res, 200, { first, next: logSeq, dropped: first - 1, size: 16384, used: logs.reduce((a, l) => a + l.line.length + 2, 0), gap, from, lines: lines.map((l) => l.line) });
      }
      if (r === "/logs/level" && m === "PUT") {
        if (!needJson(req, res)) return;
        let doc; try { doc = JSON.parse(await body(req)); } catch { doc = null; }
        const levels = ["none", "error", "warn", "info", "debug", "verbose"];
        if (!doc || !levels.includes(doc.level)) return err(res, 400, "validation", "expected {\"level\": none|error|warn|info|debug|verbose[, \"tag\": \"...\"]}");
        logAndMark("I", "espos_httpd", `log level ${doc.tag ?? "*"} = ${doc.level}`);
        return json(res, 200, { tag: doc.tag ?? "*", level: doc.level });
      }
      // ---- events
      if (r === "/events" && m === "GET") {
        res.writeHead(200, { "Content-Type": "text/event-stream", "Cache-Control": "no-cache", Connection: "keep-alive" });
        res.write("retry: 3000\n\n");
        res.write(`event: wifi\ndata: ${JSON.stringify(wifiStatus())}\n\n`);
        res.write(`event: sk\ndata: ${JSON.stringify(skStatus())}\n\n`);
        res.write(`event: sk_servers\ndata: ${JSON.stringify(serversDoc())}\n\n`);
        clients.add(res);
        const ping = setInterval(() => res.write(": ping\n\n"), 15000);
        req.on("close", () => { clients.delete(res); clearInterval(ping); });
        return;
      }
      if (["PUT", "POST", "DELETE"].includes(m)) return err(res, 404, "not_found", "no such resource");
      return err(res, 404, "not_found", "no such resource");
    } catch (e) {
      return err(res, 500, "internal", String(e));
    }
  });
  server.listen(port, "127.0.0.1", () => console.log(`  [mock] espOS API mock on http://127.0.0.1:${port}/api/v1`));
  logAndMark("I", "espos_config", "ready: 4 namespace(s)");
  logAndMark("I", "espos_httpd", "listening on :80");
  wifiEval();
  setInterval(() => logAndMark("I", "app", `heartbeat ${Math.round((Date.now() - boot) / 1000)}`), 7000);
  return server;
}

function hash(s) { let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0; return h; }

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  startMock(Number(process.argv[2] ?? 8484));
}
