// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
//
// The page list, and the hook a firmware uses to add to it.
//
// espOS's C side has a registration point for everything a firmware extends:
// espos_httpd_register() for endpoints, espos_config_add_descriptor() for
// settings. The UI had none — its routes were a const array in app.tsx — so a
// firmware that wanted a page of its own had to fork the SPA, and every
// espOS device shipped exactly the same six tabs whether or not the
// components behind them were in the build.
//
// A page can also say whether it belongs on *this* device: `available()` is
// awaited once at startup, and the tab appears only if it resolves true. That
// is how the BLE page shows up on a gateway and stays out of the way on a
// board with no Bluetooth, without the firmware having to declare anything.

import type { ComponentType } from "preact";
import { get } from "./api";
import { StatusPage } from "./pages/status";
import { WifiPage } from "./pages/wifi";
import { SignalKPage } from "./pages/signalk";
import { BlePage } from "./pages/ble";
import { ConfigPage } from "./pages/config";
import { LogsPage } from "./pages/logs";
import { OtaPage } from "./pages/ota";

export interface Route {
  path: string;
  title: string;
  page: ComponentType;
  /** Resolve false to leave this page out on this device. Called once. */
  available?: () => Promise<boolean>;
  /** Lower sorts earlier; the core pages sit on 10..70. */
  order?: number;
}

/** True when an endpoint answers at all — i.e. its component is in the build. */
export async function endpointExists(path: string): Promise<boolean> {
  try {
    await get(path);
    return true;
  } catch (e) {
    // A 404 means the component is absent. Anything else (a 500, a dropped
    // connection) says nothing about that, and hiding a page because the
    // device was momentarily busy would be worse than showing a broken one.
    return !(e instanceof Error && "status" in e && (e as { status: number }).status === 404);
  }
}

const CORE_ROUTES: Route[] = [
  { path: "/", title: "Status", page: StatusPage, order: 10 },
  { path: "/wifi", title: "WiFi", page: WifiPage, order: 20 },
  { path: "/signalk", title: "SignalK", page: SignalKPage, order: 30 },
  { path: "/ble", title: "BLE", page: BlePage, order: 40, available: () => endpointExists("/ble/status") },
  { path: "/config", title: "Config", page: ConfigPage, order: 50 },
  { path: "/logs", title: "Logs", page: LogsPage, order: 60 },
  { path: "/ota", title: "OTA", page: OtaPage, order: 70 },
];

const extra: Route[] = [];

/**
 * Add a page. Call before mount() — see docs/ui.md. A path that already
 * exists replaces the core page at it, which is how a firmware overrides
 * Status with something boat-specific.
 */
export function registerPage(route: Route): void {
  const i = extra.findIndex((r) => r.path === route.path);
  if (i >= 0) extra[i] = route;
  else extra.push(route);
}

function merged(): Route[] {
  const byPath = new Map<string, Route>();
  for (const r of CORE_ROUTES) byPath.set(r.path, r);
  for (const r of extra) byPath.set(r.path, r);
  return [...byPath.values()].sort((a, b) => (a.order ?? 100) - (b.order ?? 100));
}

/**
 * The pages that need no probing — what the shell renders immediately, so a
 * slow device shows its nav instead of a blank page while the gated ones are
 * still being decided.
 */
export function staticRoutes(): Route[] {
  return merged().filter((r) => !r.available);
}

/** Core pages plus registered ones, in display order, minus the unavailable. */
export async function resolveRoutes(): Promise<Route[]> {
  const routes = merged();
  const keep = await Promise.all(
    routes.map(async (r) => {
      if (!r.available) return true;
      try {
        return await r.available();
      } catch {
        return false;
      }
    }),
  );
  return routes.filter((_, i) => keep[i]);
}
