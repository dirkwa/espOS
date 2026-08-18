# ui/

The espOS web UI: Preact + TypeScript + Vite, served gzipped from the
LittleFS `storage` partition. See [docs/ui.md](../docs/ui.md).

```sh
npm ci
npm run dev                                   # against the built-in API mock
ESPOS_API=http://<device-ip> npm run dev      # against a device (or the host harness)
npm run build                                 # → dist-gz/, picked up by idf.py build/flash
```
