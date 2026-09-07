# main/ — the reference app

This is the application espOS's own CI builds for every target. It names
every component — the optional radios and codecs included — so a change
that breaks `espos_ble`, `espos_n2k` or `espos_voice` on some chip fails
here. Its `main.c` is a reference, not a tutorial: it reads its settings
once and on change, declares meta for a path of its own, subscribes to
`app.watch_path` and publishes a heartbeat.

To learn espOS, start at [`examples/README.md`](../examples/README.md):
eleven small projects, one thing each, every one buildable on its own.
`components/espos_core/examples/minimal` is the template a new firmware
starts from.
