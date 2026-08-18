// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
import { defineConfig, type Plugin } from "vite";
import preact from "@preact/preset-vite";
import { startMock } from "./mock/server.mjs";

// `npm run dev` talks to ESPOS_API (a device or the host harness) when set,
// otherwise to the in-process mock so no ESP32 is needed to work on the UI.
const target = process.env["ESPOS_API"] ?? "http://127.0.0.1:8484";

function mockPlugin(): Plugin {
  return {
    name: "espos-mock",
    apply: "serve",
    configureServer() {
      if (!process.env["ESPOS_API"]) startMock(8484);
    },
  };
}

export default defineConfig({
  plugins: [preact(), mockPlugin()],
  build: {
    outDir: "dist",
    emptyOutDir: true,
    sourcemap: false,
    target: "es2020",
    cssCodeSplit: false,
    rollupOptions: { output: { manualChunks: undefined } },
  },
  server: {
    port: 5173,
    proxy: { "/api": { target, changeOrigin: true, ws: false } },
  },
  preview: {
    port: 4173,
    proxy: { "/api": { target, changeOrigin: true, ws: false } },
  },
});
