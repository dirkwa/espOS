// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
// Shell: nav + a hand-rolled history router (no router dependency).
import { useEffect, useState } from "preact/hooks";
import type { ComponentType } from "preact";
import { linkStore, useStore, wifiStore, skStore } from "./api";
import { StatusPage } from "./pages/status";
import { WifiPage } from "./pages/wifi";
import { SignalKPage } from "./pages/signalk";
import { ConfigPage } from "./pages/config";
import { LogsPage } from "./pages/logs";
import { OtaPage } from "./pages/ota";

interface Route { path: string; title: string; page: ComponentType }
const ROUTES: Route[] = [
  { path: "/", title: "Status", page: StatusPage },
  { path: "/wifi", title: "WiFi", page: WifiPage },
  { path: "/signalk", title: "SignalK", page: SignalKPage },
  { path: "/config", title: "Config", page: ConfigPage },
  { path: "/logs", title: "Logs", page: LogsPage },
  { path: "/ota", title: "OTA", page: OtaPage },
];

export function navigate(path: string) {
  history.pushState(null, "", path);
  dispatchEvent(new PopStateEvent("popstate"));
}

function usePath(): string {
  const [p, setP] = useState(location.pathname);
  useEffect(() => {
    const h = () => setP(location.pathname);
    addEventListener("popstate", h);
    return () => removeEventListener("popstate", h);
  }, []);
  return p;
}

export function App() {
  const path = usePath();
  const route = ROUTES.find((r) => r.path === path) ?? ROUTES[0]!;
  const link = useStore(linkStore);
  const wifi = useStore(wifiStore);
  const sk = useStore(skStore);
  const Page = route.page;
  useEffect(() => { document.title = `${route.title} · espOS`; }, [route]);
  return (
    <>
      <header class="top">
        <a class="brand" href="/" onClick={(e) => { e.preventDefault(); navigate("/"); }}>
          <span class="logo">◈</span> espOS <small>{wifi?.hostname ?? ""}</small>
        </a>
        <nav>
          {ROUTES.map((r) => (
            <a key={r.path} href={r.path} class={r === route ? "active" : ""} onClick={(e) => { e.preventDefault(); navigate(r.path); }}>
              {r.title}
              {r.path === "/wifi" && wifi && <Dot ok={wifi.state === "connected"} warn={wifi.state === "connecting" || wifi.state === "obtaining_ip"} />}
              {r.path === "/signalk" && sk && <Dot ok={sk.token.state === "approved" && !!sk.ws?.connected} warn={sk.token.state === "pending" || sk.token.state === "approved"} />}
            </a>
          ))}
        </nav>
        <span class={`link link-${link ?? "connecting"}`} title="live connection to the device">
          {link === "open" ? "live" : link === "lost" ? "reconnecting…" : "connecting…"}
        </span>
      </header>
      <main>
        <Page />
      </main>
    </>
  );
}

export function Dot({ ok, warn }: { ok: boolean; warn?: boolean }) {
  return <span class={`dot ${ok ? "ok" : warn ? "warn" : "bad"}`} />;
}

// ---- small shared widgets
export function Badge({ kind, children }: { kind: "ok" | "warn" | "bad" | "muted"; children: preact.ComponentChildren }) {
  return <span class={`badge ${kind}`}>{children}</span>;
}
export function Row({ k, children }: { k: string; children: preact.ComponentChildren }) {
  return (
    <div class="kv">
      <span class="k">{k}</span>
      <span class="v">{children}</span>
    </div>
  );
}
export function Msg({ text, kind = "bad" }: { text: string; kind?: "ok" | "bad" | "warn" }) {
  return text ? <div class={`msg ${kind}`}>{text}</div> : null;
}
export function useAsync<T>(fn: () => Promise<T>, deps: unknown[] = []): { data: T | undefined; error: string; reload: () => void; loading: boolean } {
  const [data, setData] = useState<T | undefined>(undefined);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [n, setN] = useState(0);
  useEffect(() => {
    let live = true;
    setLoading(true);
    fn().then((d) => { if (live) { setData(d); setError(""); } }, (e: unknown) => { if (live) setError(e instanceof Error ? e.message : String(e)); })
      .finally(() => { if (live) setLoading(false); });
    return () => { live = false; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [...deps, n]);
  return { data, error, reload: () => setN((x) => x + 1), loading };
}
