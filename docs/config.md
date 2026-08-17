# Configuration store (`espos_config`)

## One source of truth: the config descriptor

Every component that owns settings ships **one JSON descriptor per NVS
namespace** and registers it from its `CMakeLists.txt`:

```cmake
idf_component_register(...)
espos_config_add_descriptor(config/myns.json)   # after idf_component_register
```

At build time `tools/espos_gen_config.py` merges all registered descriptors
into

* `espos_cfg_keys.h` — `ESPOS_CFG_NS_<NS>` / `ESPOS_CFG_<NS>_<KEY>` name
  constants (never spell an NVS key by hand),
* the C descriptor tables `espos_cfg_namespaces[]` used at runtime for
  defaults, type checks and range validation,
* the JSON Schema served at `GET /api/v1/config/schema`.

Registration order does not matter; the generator runs after every
component's CMake has been processed. Duplicate namespaces, reserved names
and NVS length limits (15 chars for namespace and key) fail the build.

### Descriptor format

```json
{
  "namespace": "httpd",           // ^[a-z][a-z0-9_]{0,14}$
  "version": 1,                   // bump on incompatible layout change (see Migrations)
  "title": "HTTP server",
  "description": "optional",
  "keys": [
    {"name": "port", "type": "int", "default": 80, "min": 1, "max": 65535,
     "title": "TCP port", "description": "…", "unit": "", "restart_required": true}
  ]
}
```

| Field              | Types            | Notes                                                        |
|--------------------|------------------|--------------------------------------------------------------|
| `type`             | all              | `bool` `int` (int32) `float` `string` `blob`                 |
| `default`          | all but blob     | required to be valid against the constraints; blob default is empty |
| `min` / `max`      | int, float       | inclusive                                                     |
| `maxLength`        | string, blob     | bytes (string: excluding NUL, ≤ 3999; blob ≤ 508000). Default 256 / 1024; for `enum` keys defaults to the longest value |
| `enum`             | string           | allowed values                                                |
| `pattern`          | string           | regex, **schema only** (UI validates; device checks type/length/enum) |
| `secret`           | string, blob     | redacted on export, sentinel ignored on import                |
| `restart_required` | all              | surfaces in the PUT response and the schema                   |
| `unit`             | all              | display hint (`x-espos-unit`)                                 |

Key name `config_version` is reserved.

## Runtime API (`espos_config.h`)

* `espos_config_init(NULL, NULL)` — NVS backend on `CONFIG_ESPOS_CONFIG_NVS_PARTITION`
  (default `"nvs"`). Erases and re-inits the partition if NVS reports it
  unusable (`ESP_ERR_NVS_NO_FREE_PAGES`, `ESP_ERR_NVS_NEW_VERSION_FOUND`);
  `espos_config_storage_was_reset()` tells you it happened.
* Typed getters never fail for declared keys: a missing, wrong-typed,
  out-of-range or over-long stored value **falls back to the compiled-in
  default** (and `espos_config_is_set()` reports false).
* Typed setters validate first (`ESP_ERR_INVALID_ARG`), write, commit, and
  fire change callbacks (`espos_config_subscribe`) only when the effective
  value changed. Callbacks run on the caller's task without the store lock.
* `espos_config_export_json` / `espos_config_import_json` implement the
  document semantics in `docs/api.md`; import validates everything before
  writing anything.
* `espos_config_factory_reset()` erases the partition and re-initialises;
  the caller reboots.

Thread safety: one internal mutex around all storage access. Migrations run
inside `espos_config_init()` before anything else can touch the store.

## Migrations

Each namespace stores its descriptor `version` under `config_version`. On
init:

| Stored vs. current    | Action                                                    |
|-----------------------|-----------------------------------------------------------|
| absent                | fresh namespace: stamp current version                    |
| corrupt / wrong type  | re-stamp current version (values are validated on read anyway) |
| equal                 | nothing                                                   |
| older                 | run steps `stored→stored+1 … current-1→current`, stamping after each |
| newer (downgrade)     | leave alone, warn                                         |

Register steps before init:

```c
static esp_err_t app_migrate_1_to_2(espos_config_migrate_ctx_t *ctx, void *arg)
{
    int32_t seconds; size_t n = sizeof(seconds);
    if (espos_config_migrate_get(ctx, "speed", ESPOS_CFG_TYPE_INT, &seconds, &n) == ESP_OK) {
        int32_t ms = seconds * 1000;
        espos_config_migrate_set(ctx, "speed_ms", ESPOS_CFG_TYPE_INT, &ms, sizeof(ms));
        espos_config_migrate_erase(ctx, "speed");
    }
    return ESP_OK;
}
espos_config_register_migration(ESPOS_CFG_NS_APP, 1, app_migrate_1_to_2, NULL);
```

A step without a registered function is treated as **additive** (new keys
read their defaults). A failing step stops the chain; the stored version
stays where it was and the step is retried on the next boot. Migration
callbacks use raw accessors so they can read a key in its *old* type; the
NVS backend handles a type change by erase-and-rewrite.

Rules of thumb: adding a key → no bump needed (defaults do the work);
renaming, retyping, changing units or tightening a range → bump and write a
step.

## Storage mapping

| Descriptor type | NVS type | Note                              |
|-----------------|----------|-----------------------------------|
| bool            | u8       | 0/1; anything else reads as default |
| int             | i32      |                                   |
| float           | u32      | IEEE-754 bit pattern              |
| string          | str      | ≤ 4000 bytes incl. NUL            |
| blob            | blob     | empty blob == key erased          |

Secrets go into the same partition; enabling `CONFIG_NVS_ENCRYPTION`
(automatic with flash encryption) encrypts the whole partition
transparently — see `docs/security.md`.

## Testing without hardware

`test/host/espos_config_test` runs the store on the linux target against an
in-memory backend (with fault injection) **and** the real NVS backend on
IDF's file-backed flash emulation, including the "corrupt partition →
defaults" path. See `docs/development.md`.
