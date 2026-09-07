# Roadmap

espOS is being built out as the ESP-IDF-native successor to SensESP for
Signal K devices. The work is cut into tranches; each one has a gate that is
checked before the next starts. Dates are intentions, not promises.

| Tranche | Theme | State |
|---|---|---|
| T0 | Foundation: Apache-2.0, registry manifests, contributor tooling, CI gates | done (v0.7.x) |
| T1 | Builds and boots the same for everyone: `espos_start()`, inherited sdkconfig, health policy, HTTP client, mDNS responder, ABI rules | done (v0.7.x) |
| T2 | A stranger reaches a live value: examples, template project, documentation site, SensESP migration guide, tutorials | in progress |
| T3 | Platform spine: `espos_net` (Ethernet, static IP), `espos_time` (SNTP, delta timestamps), TLS trust store (TOFU / private CA, `sk.scheme auto`), REST authentication | next |
| T4 | Data-flow layer: `espos_flow` (producer/consumer/transform graph, C++ facade), runtime-registered settings, SignalK nodes incl. inbound PUT handlers, sensors on IDF 6 drivers, marine formulas | planned |
| T5 | Devices, registry, fleet: high-level device classes, registry publishing, reusable release workflow, boat-side update plugin | planned |
| T6 | Hardware-gated: BLE provisioning, Ethernet boards, deep-sleep duty cycle, Web-Serial flasher, Thread spike | planned |

What "done" means for each tranche, and the decisions behind the order, are
recorded in [decisions.md](decisions.md); the changelog carries the
consumer-facing detail per release. Anything not on this list is open for
discussion in the repository's Discussions.
