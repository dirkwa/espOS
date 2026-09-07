# app_endpoint_and_page — **Advanced**

A firmware's own REST endpoints on the espOS HTTP server, a live value on the
SSE stream the web UI already listens to, and how a page of your own joins that
UI. Replaces SensESP's frontend plugins. No wiring; the "value" is a counter.

## The endpoints

| | reply | in `main.c` |
|---|---|---|
| `GET /api/v1/app/status` | `{"app":"app_endpoint_and_page","counter":42}` | `espos_httpd_send_json()` |
| `POST /api/v1/app/reset` | `202 {"status":"reset"}`; `415` without `Content-Type: application/json` | `espos_httpd_require_json()` first — the API's CSRF guard |
| SSE `app.counter` on `GET /api/v1/events` | the status document, every second and once on connect | `espos_httpd_sse_publish()`, `espos_httpd_sse_on_connect()` |

```sh
curl http://<host>/api/v1/app/status
curl -X POST -H 'Content-Type: application/json' http://<host>/api/v1/app/reset
curl -N http://<host>/api/v1/events          # event: app.counter / data: {...} once a second
```

`/api/v1/app/` is the application's prefix; espOS never uses it. Handlers run on
the one `esp_http_server` task — build, send, return, nothing that waits
(docs/concepts.md, "Threading contracts") — and are registered after `espos_start()`,
because the server has to exist. `curl` is a client too: the C side needs none of what follows.

## A page in the web UI

The UI's page list is a registry (docs/ui.md, "Pages from a firmware"): a
firmware keeps a small Vite project of its own and uses espOS's entry point
instead of forking the SPA. With espOS as the `espos/` submodule:

```tsx
// <firmware>/ui/src/main.tsx — GET once, then follow the SSE event
import { registerPage, mount } from "../../espos/ui/src/mount";
import { get, post } from "../../espos/ui/src/api";
import { useEffect, useState } from "preact/hooks";
function CounterPage() {
  const [n, setN] = useState<number>();
  useEffect(() => {
    void get<{ counter: number }>("/app/status").then((d) => setN(d.counter));
    const es = new EventSource("/api/v1/events");
    es.addEventListener("app.counter", (e) => setN(JSON.parse((e as MessageEvent).data).counter));
    return () => es.close();
  }, []);
  return <section><h2>Counter: {n ?? "…"}</h2><button onClick={() => void post("/app/reset")}>Reset</button></section>;
}
registerPage({ path: "/counter", title: "Counter", page: CounterPage, order: 35 }); mount();
```

`order` places the tab (core pages sit on 10–70); `available()` on the route hides
it where the endpoint is missing. The page opens its own `EventSource` — the shell's
stream feeds only its own stores — and each stream is one of `CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS`
(3). `npm run build` writes `dist-gz/`; `espos_project_ui_partition(DIR ...)` flashes it instead of the stock bundle.

## Build

```sh
cd components/espos_httpd/examples/app_endpoint_and_page && . $IDF_PATH/export.sh
idf.py set-target esp32c6 && idf.py build flash monitor
```
