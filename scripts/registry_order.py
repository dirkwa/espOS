#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""The order components must be uploaded to the Espressif Component Registry.

The registry resolves a component's dependencies when it accepts the upload,
so a component must not arrive before the ones it names. That makes the upload
a topological sort of the dependency graph -- leaves first.

Computed, not written down. docs/releasing.md carried a hand-maintained list
of eleven components while the tree had nineteen: every component added after
the list was written was missing from it, and the release would have failed on
the first one the registry could not resolve, *after* the tag existed.

    scripts/registry_order.py            # space-separated, for a workflow
    scripts/registry_order.py --json     # a JSON array, for a matrix
    scripts/registry_order.py --check    # verify the order is valid, exit 1 if not
"""
import argparse
import glob
import json
import os
import re
import sys

# Sibling dependencies are namespaced in the manifests
# (`signalk-espos/espos_sk:`); anything else is Espressif's or IDF's and the
# registry already has it.
DEP_RE = re.compile(r"^\s+signalk-espos/(espos_[a-z_0-9]+):", re.M)


def read_graph(root):
    """component -> set of espOS components it depends on."""
    graph = {}
    pattern = os.path.join(root, "components", "espos_*", "idf_component.yml")
    for manifest in sorted(glob.glob(pattern)):
        name = os.path.basename(os.path.dirname(manifest))
        with open(manifest, encoding="utf-8") as fh:
            graph[name] = set(DEP_RE.findall(fh.read()))
    return graph


def topo_order(graph):
    """Leaves first. Raises on a cycle, naming it."""
    order, done = [], set()

    def visit(node, stack):
        if node in done:
            return
        if node in stack:
            cycle = " -> ".join(stack[stack.index(node):] + [node])
            raise ValueError(f"dependency cycle: {cycle}")
        for dep in sorted(graph.get(node, ())):
            if dep not in graph:
                # Named but not present: a typo, or a component that was
                # removed without updating what depended on it. Either way the
                # registry would reject the upload.
                raise ValueError(f"{node} depends on {dep}, which is not in components/")
            visit(dep, stack + [node])
        done.add(node)
        order.append(node)

    for node in sorted(graph):
        visit(node, [])
    return order


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", action="store_true", help="print a JSON array")
    ap.add_argument("--check", action="store_true",
                    help="verify the computed order satisfies every dependency")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    graph = read_graph(root)
    if not graph:
        print("registry_order.py: no components/espos_*/idf_component.yml found",
              file=sys.stderr)
        return 1

    try:
        order = topo_order(graph)
    except ValueError as exc:
        print(f"registry_order.py: {exc}", file=sys.stderr)
        return 1

    if args.check:
        position = {name: i for i, name in enumerate(order)}
        bad = [(c, d) for c in order for d in graph[c] if position[d] > position[c]]
        if bad:
            for dependent, dep in bad:
                print(f"registry_order.py: {dep} would upload after {dependent}",
                      file=sys.stderr)
            return 1
        print(f"registry_order.py: {len(order)} components, order valid")
        return 0

    print(json.dumps(order) if args.json else " ".join(order))
    return 0


if __name__ == "__main__":
    sys.exit(main())
