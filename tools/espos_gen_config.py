#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
"""
espOS config generator.

Reads one or more config *descriptor* files (JSON, one per NVS namespace) and
emits, from that single source of truth:

  * a JSON Schema (draft 2020-12) describing the whole configuration document
    served at GET /api/v1/config/schema
  * a C translation unit with the descriptor tables the runtime uses for
    defaults, type checks and range validation, plus the schema text embedded
    as a C string
  * a C header with #define constants for every namespace and key name so
    application code never spells an NVS key by hand

Descriptor format (see docs/config.md):

  {
    "namespace": "httpd",           # ^[a-z][a-z0-9_]{0,14}$   (NVS limit: 15)
    "version": 1,                   # schema version, bumped on incompatible change
    "title": "HTTP server",
    "description": "...",
    "keys": [
      {"name": "port", "type": "int", "default": 80, "min": 1, "max": 65535,
       "title": "TCP port", "description": "...", "restart_required": true},
      ...
    ]
  }

Key types: bool | int | float | string | blob.
Only the Python standard library is used on purpose.
"""

import argparse
import hashlib
import json
import os
import re
import struct
import sys

NAME_RE = re.compile(r"[a-z][a-z0-9_]{0,14}")  # NVS_KEY_NAME_MAX_SIZE / NS max = 15 + NUL; used with fullmatch()
RESERVED_KEYS = {"config_version"}
TYPES = ("bool", "int", "float", "string", "blob")
INT32_MIN, INT32_MAX = -(2**31), 2**31 - 1
NVS_STR_MAX = 4000  # bytes incl. NUL terminator (nvs_set_str limit)
NVS_BLOB_MAX = 508000
DEFAULT_STR_MAX = 256
DEFAULT_BLOB_MAX = 1024

SCHEMA_ID = "https://json-schema.org/draft/2020-12/schema"


class DescriptorError(Exception):
    pass


def _err(path, msg):
    raise DescriptorError(f"{path}: {msg}")


def _c_str(s):
    """Return a C string literal for s (UTF-8 bytes escaped, safe for any input)."""
    out = ['"']
    for b in s.encode("utf-8"):
        c = chr(b)
        if c == '"':
            out.append('\\"')
        elif c == "\\":
            out.append("\\\\")
        elif c == "?":
            out.append("\\?")  # trigraph paranoia
        elif 0x20 <= b < 0x7F:
            out.append(c)
        else:
            out.append("\\%03o" % b)  # octal: fixed width, never over-consumes
    out.append('"')
    return "".join(out)


def _c_ident(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s).upper()


def load_descriptor(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        _err(path, f"cannot read/parse: {e}")
    if not isinstance(d, dict):
        _err(path, "top level must be an object")
    allowed_top = {"$comment", "namespace", "version", "title", "description", "keys"}
    for k in d:
        if k not in allowed_top:
            _err(path, f"unknown top-level field '{k}'")
    ns = d.get("namespace")
    if not isinstance(ns, str) or not NAME_RE.fullmatch(ns):
        _err(path, f"'namespace' must match {NAME_RE.pattern}, got {ns!r}")
    ver = d.get("version")
    if not isinstance(ver, int) or isinstance(ver, bool) or not (1 <= ver <= 65535):
        _err(path, "'version' must be an integer 1..65535")
    title = d.get("title", ns)
    if not isinstance(title, str):
        _err(path, "'title' must be a string")
    desc = d.get("description", "")
    if not isinstance(desc, str):
        _err(path, "'description' must be a string")
    keys = d.get("keys")
    if not isinstance(keys, list) or not keys:
        _err(path, "'keys' must be a non-empty array")
    seen = set()
    parsed_keys = []
    for i, k in enumerate(keys):
        parsed_keys.append(_parse_key(f"{path}:keys[{i}]", k, seen))
    return {
        "path": path,
        "namespace": ns,
        "version": ver,
        "title": title,
        "description": desc,
        "keys": parsed_keys,
    }


def _parse_key(where, k, seen):
    if not isinstance(k, dict):
        _err(where, "key entry must be an object")
    allowed = {
        "$comment", "name", "type", "default", "min", "max", "maxLength", "enum",
        "pattern", "title", "description", "secret", "restart_required", "unit",
    }
    for f in k:
        if f not in allowed:
            _err(where, f"unknown key field '{f}'")
    name = k.get("name")
    if not isinstance(name, str) or not NAME_RE.fullmatch(name):
        _err(where, f"'name' must match {NAME_RE.pattern}, got {name!r}")
    if name in RESERVED_KEYS:
        _err(where, f"key name '{name}' is reserved")
    if name in seen:
        _err(where, f"duplicate key '{name}'")
    seen.add(name)
    typ = k.get("type")
    if typ not in TYPES:
        _err(where, f"'type' must be one of {TYPES}, got {typ!r}")
    title = k.get("title", name)
    desc = k.get("description", "")
    unit = k.get("unit", "")
    for f, v in (("title", title), ("description", desc), ("unit", unit)):
        if not isinstance(v, str):
            _err(where, f"'{f}' must be a string")
    secret = bool(k.get("secret", False))
    restart = bool(k.get("restart_required", False))
    if not isinstance(k.get("secret", False), bool) or not isinstance(k.get("restart_required", False), bool):
        _err(where, "'secret' and 'restart_required' must be booleans")
    if secret and typ not in ("string", "blob"):
        _err(where, "'secret' is only supported for string and blob keys")
    out = {
        "name": name, "type": typ, "title": title, "description": desc, "unit": unit,
        "secret": secret, "restart_required": restart,
        "min": None, "max": None, "max_len": None, "enum": None, "pattern": None,
    }
    has_default = "default" in k
    dflt = k.get("default")

    if typ == "bool":
        if not has_default:
            dflt = False
        if not isinstance(dflt, bool):
            _err(where, "bool default must be true/false")
        for f in ("min", "max", "maxLength", "enum", "pattern"):
            if f in k:
                _err(where, f"'{f}' not allowed for bool")
    elif typ == "int":
        if not has_default:
            dflt = 0
        if not isinstance(dflt, int) or isinstance(dflt, bool):
            _err(where, "int default must be an integer")
        lo = k.get("min", INT32_MIN)
        hi = k.get("max", INT32_MAX)
        for f, v in (("min", lo), ("max", hi)):
            if not isinstance(v, int) or isinstance(v, bool) or not (INT32_MIN <= v <= INT32_MAX):
                _err(where, f"'{f}' must be an int32")
        if lo > hi:
            _err(where, "min > max")
        if not (lo <= dflt <= hi):
            _err(where, "default out of [min,max]")
        out["min"], out["max"] = lo, hi
        for f in ("maxLength", "enum", "pattern"):
            if f in k:
                _err(where, f"'{f}' not allowed for int")
    elif typ == "float":
        if not has_default:
            dflt = 0.0
        if isinstance(dflt, bool) or not isinstance(dflt, (int, float)):
            _err(where, "float default must be a number")
        dflt = float(dflt)
        lo = k.get("min")
        hi = k.get("max")
        for f, v in (("min", lo), ("max", hi)):
            if v is not None and (isinstance(v, bool) or not isinstance(v, (int, float))):
                _err(where, f"'{f}' must be a number")
        if lo is not None and hi is not None and lo > hi:
            _err(where, "min > max")
        if (lo is not None and dflt < lo) or (hi is not None and dflt > hi):
            _err(where, "default out of [min,max]")
        out["min"] = None if lo is None else float(lo)
        out["max"] = None if hi is None else float(hi)
        _check_float32(where, "default", dflt)
        for f, v in (("min", out["min"]), ("max", out["max"])):
            if v is not None:
                _check_float32(where, f, v)
        for f in ("maxLength", "enum", "pattern"):
            if f in k:
                _err(where, f"'{f}' not allowed for float")
    elif typ == "string":
        if not has_default:
            dflt = ""
        if not isinstance(dflt, str):
            _err(where, "string default must be a string")
        enum = k.get("enum")
        if enum is not None and not isinstance(enum, list):
            _err(where, "'enum' must be a non-empty array of strings")
        if "maxLength" in k:
            max_len = k["maxLength"]
        elif enum:
            # enum keys default to the longest allowed value
            max_len = max(len(str(e).encode("utf-8")) for e in enum)
        else:
            max_len = DEFAULT_STR_MAX
        if not isinstance(max_len, int) or isinstance(max_len, bool) or not (1 <= max_len <= NVS_STR_MAX - 1):
            _err(where, f"'maxLength' must be 1..{NVS_STR_MAX - 1} (bytes, excluding NUL)")
        if len(dflt.encode("utf-8")) > max_len:
            _err(where, "default longer than maxLength")
        if enum is not None:
            if not isinstance(enum, list) or not enum or not all(isinstance(e, str) for e in enum):
                _err(where, "'enum' must be a non-empty array of strings")
            if len(set(enum)) != len(enum):
                _err(where, "'enum' has duplicates")
            if dflt not in enum:
                _err(where, "default not in enum")
            for e in enum:
                if len(e.encode("utf-8")) > max_len:
                    _err(where, f"enum value {e!r} longer than maxLength")
        pattern = k.get("pattern")
        if pattern is not None:
            if not isinstance(pattern, str):
                _err(where, "'pattern' must be a string")
            try:
                re.compile(pattern)
            except re.error as e:
                _err(where, f"'pattern' is not a valid regex: {e}")
            if enum is not None:
                _err(where, "'pattern' and 'enum' are mutually exclusive")
        out["max_len"], out["enum"], out["pattern"] = max_len, enum, pattern
        for f in ("min", "max"):
            if f in k:
                _err(where, f"'{f}' not allowed for string")
    elif typ == "blob":
        if has_default:
            _err(where, "blob keys cannot have a default (absent blob == empty)")
        dflt = None
        max_len = k.get("maxLength", DEFAULT_BLOB_MAX)
        if not isinstance(max_len, int) or isinstance(max_len, bool) or not (1 <= max_len <= NVS_BLOB_MAX):
            _err(where, f"'maxLength' must be 1..{NVS_BLOB_MAX} (bytes)")
        out["max_len"] = max_len
        for f in ("min", "max", "enum", "pattern"):
            if f in k:
                _err(where, f"'{f}' not allowed for blob")
    out["default"] = dflt
    return out


# ---------------------------------------------------------------- JSON Schema

def build_schema(namespaces):
    props = {}
    for ns in namespaces:
        kprops = {}
        for k in ns["keys"]:
            p = {"title": k["title"]}
            if k["description"]:
                p["description"] = k["description"]
            t = k["type"]
            if t == "bool":
                p["type"] = "boolean"
                p["default"] = k["default"]
            elif t == "int":
                p["type"] = "integer"
                p["default"] = k["default"]
                if k["min"] != INT32_MIN:
                    p["minimum"] = k["min"]
                if k["max"] != INT32_MAX:
                    p["maximum"] = k["max"]
            elif t == "float":
                p["type"] = "number"
                p["default"] = k["default"]
                if k["min"] is not None:
                    p["minimum"] = k["min"]
                if k["max"] is not None:
                    p["maximum"] = k["max"]
            elif t == "string":
                p["type"] = "string"
                p["default"] = k["default"]
                p["maxLength"] = k["max_len"]
                if k["enum"] is not None:
                    p["enum"] = k["enum"]
                if k["pattern"] is not None:
                    p["pattern"] = k["pattern"]
            elif t == "blob":
                p["type"] = "string"
                p["contentEncoding"] = "base64"
                p["x-espos-type"] = "blob"
                p["x-espos-maxBytes"] = k["max_len"]
                p["default"] = ""
            if k["unit"]:
                p["x-espos-unit"] = k["unit"]
            if k["secret"]:
                p["writeOnly"] = True
                p["format"] = "password"
                p["x-espos-secret"] = True
            if k["restart_required"]:
                p["x-espos-restartRequired"] = True
            kprops[k["name"]] = p
        nsprop = {
            "type": "object",
            "title": ns["title"],
            "x-espos-version": ns["version"],
            "properties": kprops,
            "additionalProperties": False,
        }
        if ns["description"]:
            nsprop["description"] = ns["description"]
        props[ns["namespace"]] = nsprop
    return {
        "$schema": SCHEMA_ID,
        "$id": "urn:espos:config",
        "title": "espOS configuration",
        "description": "Every property is an NVS namespace; every nested property is a key in it. "
                       "Values omitted from a PUT are left untouched; null resets a key to its default; "
                       "secret values read back as \"********\" and that sentinel is ignored on write.",
        "type": "object",
        "properties": props,
        "additionalProperties": False,
    }


# -------------------------------------------------------------- C generation

def _check_float32(where, name, v):
    """Reject values that do not fit an IEEE-754 single (the on-device type)."""
    try:
        struct.pack("<f", float(v))
    except (OverflowError, struct.error):
        _err(where, f"'{name}' {v!r} is outside the float32 range")
    if v != v or v in (float("inf"), float("-inf")):
        _err(where, f"'{name}' must be finite")


def _c_float(f):
    r = repr(float(f))
    if r in ("inf", "-inf", "nan"):
        raise DescriptorError(f"non-finite float default {r}")
    if "e" not in r and "." not in r:
        r += ".0"
    return r + "f"


def build_c(namespaces, schema_text, schema_etag, keys_header_name):
    L = []
    L.append("/* Generated by tools/espos_gen_config.py — DO NOT EDIT. */")
    L.append("#include <stddef.h>")
    L.append("#include <stdint.h>")
    L.append('#include "espos_config_desc.h"')
    L.append("")
    for ns in namespaces:
        ident = _c_ident(ns["namespace"])
        L.append(f"static const espos_cfg_key_t s_keys_{ns['namespace']}[] = {{")
        for k in ns["keys"]:
            t = k["type"]
            fields = [
                f".name = {_c_str(k['name'])}",
                f".title = {_c_str(k['title'])}",
                f".description = {_c_str(k['description'])}",
                f".unit = {_c_str(k['unit'])}",
                f".type = ESPOS_CFG_TYPE_{t.upper()}",
            ]
            flags = []
            if k["secret"]:
                flags.append("ESPOS_CFG_FLAG_SECRET")
            if k["restart_required"]:
                flags.append("ESPOS_CFG_FLAG_RESTART_REQUIRED")
            fields.append(f".flags = {' | '.join(flags) if flags else '0'}")
            if t == "bool":
                fields.append(f".def.b = {'true' if k['default'] else 'false'}")
            elif t == "int":
                fields.append(f".def.i = INT32_C({k['default']})")
                fields.append(f".min.i = INT32_C({k['min']})")
                fields.append(f".max.i = INT32_C({k['max']})")
            elif t == "float":
                fields.append(f".def.f = {_c_float(k['default'])}")
                if k["min"] is not None:
                    fields.append(f".min.f = {_c_float(k['min'])}")
                    fields.append(".has_min = true")
                if k["max"] is not None:
                    fields.append(f".max.f = {_c_float(k['max'])}")
                    fields.append(".has_max = true")
            elif t == "string":
                fields.append(f".def.s = {_c_str(k['default'])}")
                fields.append(f".max_len = {k['max_len']}")
                if k["enum"] is not None:
                    fields.append(f".enum_values = s_enum_{ns['namespace']}_{k['name']}")
                    fields.append(f".enum_count = {len(k['enum'])}")
            elif t == "blob":
                fields.append(f".max_len = {k['max_len']}")
            L.append("    { " + ", ".join(fields) + " },")
        L.append("};")
        L.append("")
    # enum tables must precede use: emit them before, so re-order by inserting at top
    enum_lines = []
    for ns in namespaces:
        for k in ns["keys"]:
            if k["type"] == "string" and k["enum"] is not None:
                vals = ", ".join(_c_str(e) for e in k["enum"])
                enum_lines.append(f"static const char *const s_enum_{ns['namespace']}_{k['name']}[] = {{ {vals} }};")
    if enum_lines:
        insert_at = 4  # after includes
        L[insert_at:insert_at] = enum_lines + [""]
    L.append("const espos_cfg_ns_t espos_cfg_namespaces[] = {")
    for ns in namespaces:
        L.append(
            "    { "
            f".name = {_c_str(ns['namespace'])}, "
            f".title = {_c_str(ns['title'])}, "
            f".version = {ns['version']}, "
            f".keys = s_keys_{ns['namespace']}, "
            f".key_count = {len(ns['keys'])} "
            "},"
        )
    L.append("};")
    L.append(f"const size_t espos_cfg_namespace_count = {len(namespaces)};")
    L.append("")
    # schema text: chunked literals so no compiler line-length limit is hit
    L.append("const char espos_cfg_schema_json[] =")
    chunk = 100
    for i in range(0, len(schema_text), chunk):
        L.append("    " + _c_str(schema_text[i:i + chunk]))
    L.append("    ;")
    L.append(f"const size_t espos_cfg_schema_json_len = sizeof(espos_cfg_schema_json) - 1;")
    L.append(f"const char espos_cfg_schema_etag[] = {_c_str(schema_etag)};")
    L.append("")
    return "\n".join(L) + "\n"


def build_keys_header(namespaces, schema_etag):
    L = []
    L.append("/* Generated by tools/espos_gen_config.py — DO NOT EDIT. */")
    L.append("#pragma once")
    L.append("")
    L.append("/* Namespace names and schema versions */")
    for ns in namespaces:
        ident = _c_ident(ns["namespace"])
        L.append(f"#define ESPOS_CFG_NS_{ident} {_c_str(ns['namespace'])}")
        L.append(f"#define ESPOS_CFG_NS_{ident}_VERSION {ns['version']}")
    L.append("")
    L.append("/* Key names: ESPOS_CFG_<NAMESPACE>_<KEY> */")
    for ns in namespaces:
        nsid = _c_ident(ns["namespace"])
        for k in ns["keys"]:
            L.append(f"#define ESPOS_CFG_{nsid}_{_c_ident(k['name'])} {_c_str(k['name'])}")
    L.append("")
    L.append(f"#define ESPOS_CFG_SCHEMA_ETAG {_c_str(schema_etag)}")
    return "\n".join(L) + "\n"


def generate(descriptor_paths, schema_out, c_out, h_out):
    if not descriptor_paths:
        raise DescriptorError("no config descriptors registered (call espos_config_add_descriptor in a component)")
    namespaces = []
    seen = {}
    for p in descriptor_paths:
        d = load_descriptor(p)
        if d["namespace"] in seen:
            raise DescriptorError(
                f"{p}: namespace '{d['namespace']}' already registered by {seen[d['namespace']]}")
        seen[d["namespace"]] = p
        namespaces.append(d)
    namespaces.sort(key=lambda n: n["namespace"])  # deterministic output regardless of registration order
    _check_identifier_collisions(namespaces)
    schema = build_schema(namespaces)
    schema_text = json.dumps(schema, indent=None, separators=(",", ":"), ensure_ascii=False, sort_keys=False)
    etag = hashlib.sha256(schema_text.encode("utf-8")).hexdigest()[:16]
    c_text = build_c(namespaces, schema_text, etag, os.path.basename(h_out))
    h_text = build_keys_header(namespaces, etag)
    _write_if_changed(schema_out, json.dumps(schema, indent=2, ensure_ascii=False) + "\n")
    _write_if_changed(c_out, c_text)
    _write_if_changed(h_out, h_text)
    return namespaces


def _check_identifier_collisions(namespaces):
    """Names are joined with '_' into C identifiers (ESPOS_CFG_<NS>_<KEY>,
    s_enum_<ns>_<key>), so ns "a_b"/key "c" and ns "a"/key "b_c" would clash."""
    seen = {"SCHEMA_ETAG": "the reserved macro ESPOS_CFG_SCHEMA_ETAG"}
    for ns in namespaces:
        for k in ns["keys"]:
            ident = f"{_c_ident(ns['namespace'])}_{_c_ident(k['name'])}"
            other = seen.get(ident)
            if other:
                raise DescriptorError(
                    f"{ns['path']}: key '{ns['namespace']}.{k['name']}' and '{other}' both map to "
                    f"C identifier ESPOS_CFG_{ident}; rename one of them")
            seen[ident] = f"{ns['namespace']}.{k['name']}"
        # namespace macros vs. key macros: ESPOS_CFG_NS_<X>[_VERSION] could equal
        # ESPOS_CFG_<NS>_<KEY> for a namespace literally called "ns"
        for ident in (f"NS_{_c_ident(ns['namespace'])}", f"NS_{_c_ident(ns['namespace'])}_VERSION"):
            other = seen.get(ident)
            if other:
                raise DescriptorError(
                    f"{ns['path']}: namespace '{ns['namespace']}' macro ESPOS_CFG_{ident} collides with '{other}'")
            seen[ident] = f"namespace {ns['namespace']}"


def _write_if_changed(path, text):
    """Avoid touching mtime when content is identical → no needless rebuilds."""
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    try:
        with open(path, "r", encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--schema-out", required=True)
    ap.add_argument("--c-out", required=True)
    ap.add_argument("--h-out", required=True)
    ap.add_argument("descriptors", nargs="*", help="descriptor JSON files (a single ';'-separated arg is accepted)")
    a = ap.parse_args(argv)
    paths = []
    for d in a.descriptors:
        paths.extend(x for x in d.split(";") if x)
    try:
        ns = generate(paths, a.schema_out, a.c_out, a.h_out)
    except DescriptorError as e:
        print(f"espos_gen_config: error: {e}", file=sys.stderr)
        return 1
    print(f"espos_gen_config: {len(ns)} namespace(s): {', '.join(n['namespace'] for n in ns)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
