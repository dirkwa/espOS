// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// Entry point for any firmware's web UI, espOS's own included.
//
//   import { registerPage, mount } from "<espos>/ui/src/mount";
//   registerPage({ path: "/tanks", title: "Tanks", page: TanksPage });
//   void mount();
//
// See docs/ui.md.
import { render } from "preact";
import { useEffect, useState } from "preact/hooks";
import { App } from "./app";
import { authStore, bootstrapAuth, connectEvents, useStore } from "./api";
import { LoginPage } from "./pages/login";
import { resolveRoutes, staticRoutes, type Route } from "./routes";
import "./style.css";

export { registerPage, endpointExists } from "./routes";
export type { Route } from "./routes";

// The app, or the login page, as the device says (docs/security.md). Each
// time a session begins the app is mounted afresh — a new key on the pages'
// fetches, a new EventSource carrying the new cookie — rather than teaching
// every page to retry what it failed to load while logged out.
function Root() {
  const auth = useStore(authStore);
  const live = auth === "ok" || auth === "open";
  const [routes, setRoutes] = useState<Route[]>(() => staticRoutes());
  const [session, setSession] = useState(0);
  useEffect(() => {
    if (!live) return;
    connectEvents();
    setSession((n) => n + 1);
    // Paint the pages we already know about, then swap in the full list once
    // the gated ones have answered: a nav that grows, not a blank page.
    void resolveRoutes().then(setRoutes);
  }, [live]);
  if (auth === undefined) return null;
  if (!live) return <LoginPage />;
  return <App key={session} routes={routes} />;
}

export function mount(el: HTMLElement | null = document.getElementById("app")): void {
  if (!el) throw new Error("mount: no #app element");
  render(<Root />, el);
  void bootstrapAuth();
}
