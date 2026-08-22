// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
// The one place that knows the REST contract (docs/api.md): typed fetch
// helpers, the SSE connection and a tiny subscribable store per event.
import { useEffect, useState } from "preact/hooks";

export const BASE = "/api/v1";

export interface ApiError { error: string; message: string; path?: string }
export class ApiFailure extends Error {
  constructor(public status: number, public body: ApiError | null) {
    super(body?.message ?? `HTTP ${status}`);
  }
}

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const init: RequestInit = { method, headers: {} };
  if (body !== undefined) {
    init.headers = { "Content-Type": "application/json" };
    init.body = JSON.stringify(body);
  } else if (method !== "GET") {
    init.headers = { "Content-Type": "application/json" };
    init.body = "{}";
  }
  const r = await fetch(BASE + path, init);
  const text = await r.text();
  let js: unknown = null;
  try { js = text ? JSON.parse(text) : null; } catch { js = null; }
  if (!r.ok) throw new ApiFailure(r.status, js as ApiError | null);
  return js as T;
}
export const get = <T,>(path: string) => call<T>("GET", path);
export const put = <T,>(path: string, body: unknown) => call<T>("PUT", path, body);
export const post = <T,>(path: string, body?: unknown) => call<T>("POST", path, body);
export const del = <T,>(path: string) => call<T>("DELETE", path);

// ---- documents (shapes per docs/api.md; only the fields the UI reads)
export interface WifiStatus {
  state: "disabled" | "unconfigured" | "connecting" | "obtaining_ip" | "connected" | "backoff";
  sta_enabled: boolean; hostname: string; reason: { code: number; text: string };
  ssid?: string; bssid?: string; rssi?: number; channel?: number; ip?: string; gateway?: string; netmask?: string;
  network_index?: number; backoff_ms?: number; attempt?: number; connect_count?: number; disconnect_count?: number;
  portal: { active: boolean; ssid: string; ip?: string; clients?: number };
}
export interface ScanResult { ssid: string; bssid: string; rssi: number; channel: number; auth: string }
export interface ScanDoc { scanning: boolean; age_s: number | null; results: ScanResult[] }
export interface SkServer { host: string; port: number; self: string; name: string; roles?: string; swname?: string; swvers?: string; seen_s?: number; selected?: boolean }
export interface SkServersDoc { servers: SkServer[]; last_s: number | null }
export interface SkWs {
  enabled: boolean; connected: boolean; connected_s?: number; reconnects: number; sent: number; send_errors: number;
  next_retry_s?: number; pending: number; buffered: number; buffered_bytes: number; dropped: number; last_error: string;
  meta: { declared: number; reconciled: number };
  in?: { subs: number; frames: number; received: number };
  put?: { pending: number; ok: number; failed: number };
}
export interface BleStatus {
  enabled: boolean; scanning: boolean; mac: string;
  scan_hits: number; adv_received: number; adv_posted: number; adv_dropped: number; adv_pending: number;
  post_success: number; post_fail: number; ws_connected: boolean;
  gatt_sessions: number; gatt_max: number;
}
export interface SkStatus {
  token: { state: string; has_token: boolean; busy: boolean; approved_s?: number; pending_s?: number; pending_href?: string;
    next_action_s?: number; last_check_s?: number; last_http_status: number; last_error: string;
    counts: { requests: number; approved: number; denied: number; unauthorized: number } };
  server: { host?: string; port?: number; self?: string; source: "discovered" | "manual" | "pinned" | "none"; name?: string; swname?: string; swvers?: string };
  ws?: SkWs; client_id: string; description: string; permissions: string;
  discovery: { enabled: boolean; count: number; last_s: number | null };
}
export interface SystemInfo {
  app: string; version: string; idf_version: string; chip: string; chip_revision: number; cores: number;
  uptime_s: number; free_heap: number; min_free_heap: number; reset_reason: string; config_storage_reset: boolean;
  schema_etag: string; ui_storage?: boolean;
}
export interface LogsDoc { first: number; next: number; dropped: number; size: number; used: number; gap: boolean; from: number; lines: string[] }
export interface Coredump {
  present: boolean; size: number; valid: boolean; task?: string; pc?: string; app_elf_sha256?: string; version?: number;
  exc_cause?: number; exc_vaddr?: string; backtrace?: string[]; backtrace_corrupted?: boolean;
  mcause?: number; mtval?: string; ra?: string; sp?: string; stackdump_bytes?: number; summary_error?: string;
}
export interface OtaStatus {
  state: "idle" | "checking" | "available" | "downloading" | "verifying" | "ready" | "failed";
  last_error: string;
  running: { version: string; project: string; target: string; slot: string; image_state: string; pending_verify: boolean; confirmed: boolean;
    other_slot: string; other_version: string; rolled_back: boolean; built: string; idf: string };
  manifest: { url: string; channel: string; auto_check: boolean; auto_install: boolean; last_check_s: number | null; next_check_s: number | null };
  progress: { received: number; total: number };
  available: { version: string; url: string; size: number; sha256: string; notes: string; newer: boolean } | null;
}
export type ConfigDoc = Record<string, Record<string, unknown>>;
export interface JsonSchemaProp {
  title?: string; description?: string; type: "string" | "integer" | "number" | "boolean";
  default?: unknown; minimum?: number; maximum?: number; maxLength?: number; enum?: string[]; pattern?: string;
  "x-espos-secret"?: boolean; "x-espos-restartRequired"?: boolean; "x-espos-unit"?: string; "x-espos-type"?: string; "x-espos-maxBytes"?: number;
}
export interface JsonSchemaNs { title?: string; description?: string; "x-espos-version"?: number; properties: Record<string, JsonSchemaProp> }
export interface ConfigSchema { properties: Record<string, JsonSchemaNs> }
export interface PutResult { changed: string[]; restart_required: boolean }

// ---- live state over SSE
type Listener = () => void;
class Store<T> {
  private v: T | undefined;
  private ls = new Set<Listener>();
  get value(): T | undefined { return this.v; }
  set(v: T) { this.v = v; for (const l of this.ls) l(); }
  subscribe(l: Listener) { this.ls.add(l); return () => { this.ls.delete(l); }; }
}
export const wifiStore = new Store<WifiStatus>();
export const scanStore = new Store<ScanDoc>();
export const skStore = new Store<SkStatus>();
export const skServersStore = new Store<SkServersDoc>();
export const skWsStore = new Store<SkWs>();
export const logsSeqStore = new Store<number>();
export const otaStore = new Store<OtaStatus>();
export const bleStore = new Store<BleStatus>();
export const configChangeStore = new Store<{ ns: string; key: string; n: number }>();
export const linkStore = new Store<"connecting" | "open" | "lost">();

export function useStore<T>(s: Store<T>): T | undefined {
  const [, tick] = useState(0);
  useEffect(() => s.subscribe(() => tick((n) => n + 1)), [s]);
  return s.value;
}

let es: EventSource | null = null;
let changes = 0;
export function connectEvents() {
  if (es) return;
  linkStore.set("connecting");
  es = new EventSource(BASE + "/events");
  const on = <T,>(name: string, store: Store<T>, map?: (d: T) => void) =>
    es!.addEventListener(name, (e) => {
      const d = JSON.parse((e as MessageEvent).data) as T;
      store.set(d);
      map?.(d);
    });
  es.onopen = () => linkStore.set("open");
  es.onerror = () => linkStore.set("lost");     // EventSource reconnects by itself (retry: 3000)
  on("wifi", wifiStore);
  on("wifi_scan", scanStore);
  on("sk", skStore, (d) => { if (d.ws) skWsStore.set(d.ws); });
  on("sk_servers", skServersStore);
  on("sk_ws", skWsStore);
  on("ota", otaStore);
  on("ble", bleStore);
  es.addEventListener("logs", (e) => logsSeqStore.set((JSON.parse((e as MessageEvent).data) as { next: number }).next));
  es.addEventListener("config", (e) => {
    const d = JSON.parse((e as MessageEvent).data) as { ns: string; key: string };
    configChangeStore.set({ ...d, n: ++changes });
  });
}

// ---- helpers
export function fmtDuration(s: number | undefined | null): string {
  if (s === undefined || s === null) return "–";
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
  return `${Math.floor(s / 86400)}d ${Math.floor((s % 86400) / 3600)}h`;
}
export function fmtBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / 1024 / 1024).toFixed(2)} MiB`;
}
export function errText(e: unknown): string {
  if (e instanceof ApiFailure) return e.body ? `${e.body.message}${e.body.path ? ` (${e.body.path})` : ""}` : `HTTP ${e.status}`;
  return e instanceof Error ? e.message : String(e);
}
