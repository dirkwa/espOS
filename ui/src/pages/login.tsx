// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
// The login page: shown instead of the app while the device wants a key this
// browser does not hold (docs/security.md). One field, one button.
import { useState } from "preact/hooks";
import { ApiFailure, authStore, login, useStore, errText } from "../api";
import { Msg } from "../app";

export function LoginPage() {
  const auth = useStore(authStore);
  const [key, setKey] = useState("");
  const [msg, setMsg] = useState("");
  const [busy, setBusy] = useState(false);

  async function submit(e: Event) {
    e.preventDefault();
    if (!key || busy) return;
    setBusy(true); setMsg("");
    try {
      await login(key);
    } catch (err) {
      if (err instanceof ApiFailure && err.status === 401) setMsg("That key is not the device's.");
      else if (err instanceof ApiFailure && err.status === 429) setMsg("Too many wrong keys — the device refuses every key for 30 seconds.");
      else setMsg(errText(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <main class="login">
      <section class="card">
        <h1><span class="logo">◈</span> espOS</h1>
        {auth === "unconfigured" ? (
          <>
            <p>This firmware requires an API key before it answers, and none is set yet.</p>
            <p class="muted small">Set one from the device's own network: join its <code>espOS-xxxx</code> access point (a factory reset brings it back), open <code>http://192.168.4.1</code>, and enter a key on the Config page under HTTP server. Requests on that network need no key.</p>
          </>
        ) : (
          <form onSubmit={submit}>
            <p>This device asks for its API key.</p>
            <div class="row">
              <input type="password" autofocus autocomplete="current-password" placeholder="API key" value={key} style="flex:1;min-width:12rem"
                onInput={(e) => setKey((e.target as HTMLInputElement).value)} />
              <button type="submit" class="primary" disabled={!key || busy}>{busy ? "…" : "Log in"}</button>
            </div>
            <Msg text={msg} />
            <p class="muted small">The key is <code>httpd.api_key</code> on the Config page. Lost it? Join the device's setup access point after a factory reset and set a new one from there.</p>
          </form>
        )}
      </section>
    </main>
  );
}
