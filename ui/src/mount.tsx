// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
//
// Entry point for any firmware's web UI, espOS's own included.
//
//   import { registerPage, mount } from "<espos>/ui/src/mount";
//   registerPage({ path: "/tanks", title: "Tanks", page: TanksPage });
//   void mount();
//
// See docs/ui.md.
import { render } from "preact";
import { App } from "./app";
import { connectEvents } from "./api";
import { resolveRoutes, staticRoutes } from "./routes";
import "./style.css";

export { registerPage, endpointExists } from "./routes";
export type { Route } from "./routes";

export function mount(el: HTMLElement | null = document.getElementById("app")): void {
  if (!el) throw new Error("mount: no #app element");
  connectEvents();
  // Paint the pages we already know about, then swap in the full list once
  // the gated ones have answered. Preact updates the same tree, so this is a
  // nav that grows rather than a flash of blank page.
  render(<App routes={staticRoutes()} />, el);
  void resolveRoutes().then((routes) => render(<App routes={routes} />, el));
}
