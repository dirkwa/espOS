#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""Summarise, and optionally diff, `idf.py size` JSON as a markdown table.

    idf.py -B build-esp32c6 size --format json2 --output-file size-esp32c6.json
    tools/espos_size_diff.py --head size-esp32c6.json --label esp32c6
    tools/espos_size_diff.py --base main/size-esp32c6.json --head size-esp32c6.json
    tools/espos_size_diff.py --head size-esp32c6.json \
        --bin build-esp32c6/espos.bin --partitions partitions/4mb.csv --max-percent 90

Standard library only, so CI can run it outside ESP-IDF's venv and paste the
result into the job summary.

Input format: the `json2` output of esp-idf-size 2.x, the only JSON format
`idf.py size` still emits in ESP-IDF 6 (`--format json` was removed there):

    {"version": "1.2", "total_size": N,
     "layout": [{"name": "Flash Code", "total": 0, "used": N, "free": 0,
                 "parts": {".text": {"size": N}, ".rodata": {"size": N}}}, ...]}

Region names differ per chip ("Flash Code"/"Flash Data" on esp32, one
"Flash" on esp32p4, "IRAM"+"DRAM" on esp32, "DIRAM" on esp32c6, and the c3
calls its unified RAM "DRAM"), so the summary rows are derived, not looked
up: text/data/rodata/bss are the section sizes summed over every region,
flash is everything in a region whose name starts with "Flash", iram is the
code (.text/.vectors) placed in RAM and dram the rest of what RAM holds. The
legacy flat format of esp-idf-size 1.x (`used_dram`, `flash_code`, ...) is
accepted too, so a consumer on an older IDF can use the same tool.

Exit status: 0 for a report; 1 when --bin and --partitions are given and the
image is larger than --max-percent of the app slot; 2 for unusable input.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass, field

RAM_REGIONS = {"iram", "dram", "diram"}
CODE_PARTS = {".text", ".vectors", ".iram0.text", ".iram0.vectors"}

SUMMARY_ROWS = (
    ("text", "Code (.text) in flash and RAM"),
    ("rodata", "Read-only data"),
    ("data", "Initialised data (.data)"),
    ("bss", "Zero-initialised data (.bss)"),
    ("flash", "Flash used by the image"),
    ("iram", "Code placed in RAM"),
    ("dram", "Data placed in RAM"),
    ("total", "Total image size"),
)


@dataclass
class Region:
    name: str
    used: int
    total: int  # 0 when the tool does not know the capacity (flash)
    parts: dict[str, int] = field(default_factory=dict)


@dataclass
class Summary:
    regions: list[Region]
    totals: dict[str, int]


def _int(value) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def load(path: str) -> Summary:
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise SystemExit(f"espos_size_diff: cannot read {path}: {e}")
    if not isinstance(doc, dict):
        raise SystemExit(f"espos_size_diff: {path}: not a JSON object")
    if isinstance(doc.get("layout"), list):
        return _from_json2(doc)
    if "total_size" in doc and ("used_dram" in doc or "flash_code" in doc):
        return _from_legacy(doc)
    raise SystemExit(f"espos_size_diff: {path}: not an idf.py size JSON (json2 or legacy)")


def _from_json2(doc: dict) -> Summary:
    regions: list[Region] = []
    for entry in doc["layout"]:
        parts = {name: _int(p.get("size")) for name, p in (entry.get("parts") or {}).items()}
        regions.append(Region(str(entry.get("name", "?")), _int(entry.get("used")), _int(entry.get("total")), parts))

    totals = {k: 0 for k, _ in SUMMARY_ROWS}
    for r in regions:
        for part, size in r.parts.items():
            if part in CODE_PARTS:
                totals["text"] += size
            elif part == ".rodata":
                totals["rodata"] += size
            elif part == ".data":
                totals["data"] += size
            elif part == ".bss":
                totals["bss"] += size
        if r.name.lower().startswith("flash"):
            totals["flash"] += r.used
        elif r.name.lower() in RAM_REGIONS:
            code = sum(size for part, size in r.parts.items() if part in CODE_PARTS)
            totals["iram"] += code
            totals["dram"] += max(r.used - code, 0)
    totals["total"] = _int(doc.get("total_size"))
    return Summary(regions, totals)


def _from_legacy(doc: dict) -> Summary:
    g = lambda k: _int(doc.get(k))  # noqa: E731 - a one-line accessor reads better here
    diram_code = g("diram_text") + g("diram_vectors")
    totals = {
        "text": g("flash_code") + g("iram_text") + g("iram_vectors") + g("diram_text") + g("diram_vectors"),
        "rodata": g("flash_rodata") + g("dram_rodata") + g("diram_rodata"),
        "data": g("dram_data") + g("diram_data"),
        "bss": g("dram_bss") + g("diram_bss"),
        "flash": g("used_flash_non_ram"),
        "iram": g("used_iram") + diram_code,
        "dram": g("used_dram") + max(g("used_diram") - diram_code, 0),
        "total": g("total_size"),
    }
    regions = []
    for name, used, total in (
        ("Flash", "used_flash_non_ram", None),
        ("IRAM", "used_iram", "iram_total"),
        ("DRAM", "used_dram", "dram_total"),
        ("DIRAM", "used_diram", "diram_total"),
    ):
        if used in doc:
            regions.append(Region(name, g(used), g(total) if total else 0))
    return Summary(regions, totals)


def fmt(n: int) -> str:
    return f"{n:,}"


def fmt_delta(base: int, head: int) -> str:
    d = head - base
    if d == 0:
        return "0"
    pct = f" ({d / base:+.2%})" if base else ""
    return f"{d:+,}{pct}"


def fmt_free(r: Region) -> str:
    if r.total <= 0:
        return "—"
    free = r.total - r.used
    return f"{fmt(free)} ({free / r.total:.1%})"


def render(head: Summary, base: Summary | None, label: str | None) -> list[str]:
    out: list[str] = []
    title = "Size report" + (f": {label}" if label else "")
    out.append(f"### {title}")
    out.append("")
    if base is None:
        out.append("| Section totals | Bytes |")
        out.append("|---|---:|")
        for key, desc in SUMMARY_ROWS:
            bold = "**" if key == "total" else ""
            out.append(f"| {bold}{key}{bold} — {desc} | {bold}{fmt(head.totals[key])}{bold} |")
        out.append("")
        out.append("| Region | Used | Capacity | Free |")
        out.append("|---|---:|---:|---:|")
        for r in head.regions:
            cap = fmt(r.total) if r.total > 0 else "—"
            out.append(f"| {r.name} | {fmt(r.used)} | {cap} | {fmt_free(r)} |")
    else:
        out.append("| Section totals | Base | Head | Delta |")
        out.append("|---|---:|---:|---:|")
        for key, desc in SUMMARY_ROWS:
            b, h = base.totals[key], head.totals[key]
            bold = "**" if key == "total" else ""
            out.append(f"| {bold}{key}{bold} — {desc} | {fmt(b)} | {bold}{fmt(h)}{bold} | {fmt_delta(b, h)} |")
        out.append("")
        out.append("| Region | Base used | Head used | Delta | Capacity | Free (head) |")
        out.append("|---|---:|---:|---:|---:|---:|")
        base_by_name = {r.name: r for r in base.regions}
        for r in head.regions:
            b = base_by_name.get(r.name)
            b_used = fmt(b.used) if b else "—"
            delta = fmt_delta(b.used, r.used) if b else "new"
            cap = fmt(r.total) if r.total > 0 else "—"
            out.append(f"| {r.name} | {b_used} | {fmt(r.used)} | {delta} | {cap} | {fmt_free(r)} |")
        for r in base.regions:
            if r.name not in {h.name for h in head.regions}:
                out.append(f"| {r.name} | {fmt(r.used)} | — | removed | | |")
    return out


# --------------------------------------------------------------- app slot

def parse_size(text: str) -> int:
    """Partition-table size field: 1600K, 2M, 0x190000 or plain bytes."""
    t = text.strip().upper()
    if not t:
        return 0
    if t.startswith("0X"):
        return int(t, 16)
    mult = 1
    if t.endswith("K"):
        mult, t = 1024, t[:-1]
    elif t.endswith("M"):
        mult, t = 1024 * 1024, t[:-1]
    return int(t) * mult


def slot_size(partitions_csv: str, slot: str) -> int:
    """Size of the named app partition from partitions/4mb.csv (gen_esp32part syntax)."""
    try:
        with open(partitions_csv, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise SystemExit(f"espos_size_diff: cannot read {partitions_csv}: {e}")
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[0] == slot:
            return parse_size(cols[4])
    raise SystemExit(f"espos_size_diff: no partition named {slot!r} in {partitions_csv}")


def check_slot(bin_path: str, partitions_csv: str, slot: str, max_percent: float) -> tuple[list[str], bool]:
    try:
        image = os.path.getsize(bin_path)
    except OSError as e:
        raise SystemExit(f"espos_size_diff: cannot stat {bin_path}: {e}")
    cap = slot_size(partitions_csv, slot)
    if cap <= 0:
        raise SystemExit(f"espos_size_diff: partition {slot!r} has no size")
    used = image / cap
    ok = used <= max_percent / 100.0
    verdict = "ok" if ok else f"**FAIL: over the {max_percent:g}% budget**"
    # The .bin, not the map's total_size: the signed, padded image is what has
    # to fit the slot, and it is a few tens of KB larger than the linker sum.
    lines = [
        "",
        f"App image `{os.path.basename(bin_path)}`: {fmt(image)} B = {used:.1%} of `{slot}` "
        f"({fmt(cap)} B); budget {max_percent:g}% — {verdict}",
    ]
    return lines, ok


# --------------------------------------------------------------------- cli

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="espos_size_diff.py",
        description="Markdown size report from `idf.py size --format json2` output.",
    )
    ap.add_argument("--head", required=True, metavar="size.json", help="the build to report")
    ap.add_argument("--base", metavar="size.json", help="an earlier build to diff against")
    ap.add_argument("--label", help="heading, e.g. the target name")
    ap.add_argument("--bin", metavar="app.bin", help="app image; with --partitions, checked against the slot")
    ap.add_argument("--partitions", metavar="partitions/4mb.csv", help="partition table the image has to fit")
    ap.add_argument("--slot", default="ota_0", help="app partition to measure against (default: ota_0)")
    ap.add_argument("--max-percent", type=float, default=90.0, help="fail above this share of the slot (default: 90)")
    args = ap.parse_args(argv)

    if bool(args.bin) != bool(args.partitions):
        ap.error("--bin and --partitions go together")

    head = load(args.head)
    base = load(args.base) if args.base else None
    lines = render(head, base, args.label)

    ok = True
    if args.bin:
        extra, ok = check_slot(args.bin, args.partitions, args.slot, args.max_percent)
        lines += extra

    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
