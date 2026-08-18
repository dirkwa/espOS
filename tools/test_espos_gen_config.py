# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
"""Unit tests for espos_gen_config.py (run: python3 -m unittest discover -s tools -p 'test_*.py')."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import espos_gen_config as g  # noqa: E402


def write(tmp, name, obj):
    p = os.path.join(tmp, name)
    with open(p, "w") as f:
        json.dump(obj, f)
    return p


BASE = {
    "namespace": "t",
    "version": 1,
    "keys": [{"name": "k", "type": "int", "default": 1, "min": 0, "max": 9}],
}


class GenTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def run_gen(self, *descs):
        outs = [os.path.join(self.tmp, n) for n in ("schema.json", "gen.c", "keys.h")]
        ns = g.generate(list(descs), *outs)
        with open(outs[0]) as f:
            schema = json.load(f)
        with open(outs[1]) as f:
            c = f.read()
        with open(outs[2]) as f:
            h = f.read()
        return ns, schema, c, h

    def test_minimal_roundtrip(self):
        ns, schema, c, h = self.run_gen(write(self.tmp, "a.json", BASE))
        self.assertEqual([n["namespace"] for n in ns], ["t"])
        self.assertEqual(schema["properties"]["t"]["properties"]["k"]["minimum"], 0)
        self.assertIn('#define ESPOS_CFG_NS_T "t"', h)
        self.assertIn('#define ESPOS_CFG_T_K "k"', h)
        self.assertIn(".def.i = INT32_C(1)", c)
        self.assertIn("espos_cfg_namespace_count = 1", c)
        self.assertIn("espos_cfg_schema_etag[]", c)

    def test_sorted_and_deterministic(self):
        b = dict(BASE, namespace="b")
        a = dict(BASE, namespace="a")
        ns1, s1, c1, _ = self.run_gen(write(self.tmp, "b.json", b), write(self.tmp, "a.json", a))
        ns2, s2, c2, _ = self.run_gen(write(self.tmp, "a2.json", a), write(self.tmp, "b2.json", b))
        self.assertEqual([n["namespace"] for n in ns1], ["a", "b"])
        self.assertEqual(c1, c2)
        self.assertEqual(s1, s2)

    def test_rejects(self):
        bad = [
            (dict(BASE, namespace="TooLongNamespace"), "namespace"),
            (dict(BASE, namespace="Bad"), "namespace"),
            (dict(BASE, version=0), "version"),
            (dict(BASE, keys=[]), "keys"),
            (dict(BASE, keys=[{"name": "config_version", "type": "int"}]), "reserved"),
            (dict(BASE, keys=[{"name": "k", "type": "int"}, {"name": "k", "type": "bool"}]), "duplicate"),
            (dict(BASE, keys=[{"name": "sixteen_chars_xx", "type": "int"}]), "name"),
            (dict(BASE, keys=[{"name": "k", "type": "int", "default": 99, "max": 9}]), "out of"),
            (dict(BASE, keys=[{"name": "k", "type": "int", "min": 5, "max": 1}]), "min > max"),
            (dict(BASE, keys=[{"name": "k", "type": "int", "default": True}]), "integer"),
            (dict(BASE, keys=[{"name": "k", "type": "int", "default": 2**31}]), "out of"),
            (dict(BASE, keys=[{"name": "k", "type": "float", "default": "x"}]), "number"),
            (dict(BASE, keys=[{"name": "k", "type": "string", "default": "abc", "maxLength": 2}]), "longer"),
            (dict(BASE, keys=[{"name": "k", "type": "string", "enum": ["a"], "default": "b"}]), "enum"),
            (dict(BASE, keys=[{"name": "k", "type": "string", "pattern": "("}]), "regex"),
            (dict(BASE, keys=[{"name": "k", "type": "blob", "default": "AA=="}]), "blob"),
            (dict(BASE, keys=[{"name": "k", "type": "bool", "min": 0}]), "not allowed"),
            (dict(BASE, keys=[{"name": "k", "type": "int", "secret": True}]), "secret"),
            (dict(BASE, keys=[{"name": "k", "type": "banana"}]), "type"),
            (dict(BASE, keys=[{"name": "k", "type": "int", "bogus": 1}]), "unknown key field"),
            (dict(BASE, bogus=1), "unknown top-level"),
        ]
        for i, (d, msg) in enumerate(bad):
            with self.assertRaises(g.DescriptorError, msg=json.dumps(d)) as cm:
                self.run_gen(write(self.tmp, f"bad{i}.json", d))
            self.assertIn(msg, str(cm.exception), json.dumps(d))

    def test_duplicate_namespace_across_files(self):
        with self.assertRaises(g.DescriptorError) as cm:
            self.run_gen(write(self.tmp, "x.json", BASE), write(self.tmp, "y.json", BASE))
        self.assertIn("already registered", str(cm.exception))

    def test_identifier_collisions(self):
        a = {"namespace": "a_b", "version": 1, "keys": [{"name": "c", "type": "int"}]}
        b = {"namespace": "a", "version": 1, "keys": [{"name": "b_c", "type": "int"}]}
        with self.assertRaises(g.DescriptorError) as cm:
            self.run_gen(write(self.tmp, "a.json", a), write(self.tmp, "b.json", b))
        self.assertIn("ESPOS_CFG_A_B_C", str(cm.exception))
        # namespace named "ns" with a key clashing against ESPOS_CFG_NS_<X>
        c = {"namespace": "ns", "version": 1, "keys": [{"name": "x", "type": "int"}]}
        d = {"namespace": "x", "version": 1, "keys": [{"name": "k", "type": "int"}]}
        with self.assertRaises(g.DescriptorError):
            self.run_gen(write(self.tmp, "c.json", c), write(self.tmp, "d.json", d))
        # ESPOS_CFG_NS_<X>_VERSION and ESPOS_CFG_SCHEMA_ETAG are reserved too
        e = {"namespace": "ns", "version": 1, "keys": [{"name": "x_version", "type": "int"}]}
        with self.assertRaises(g.DescriptorError):
            self.run_gen(write(self.tmp, "e.json", e), write(self.tmp, "d2.json", d))
        f = {"namespace": "schema", "version": 1, "keys": [{"name": "etag", "type": "int"}]}
        with self.assertRaises(g.DescriptorError):
            self.run_gen(write(self.tmp, "f.json", f))

    def test_name_regex_is_strict(self):
        for bad in ("k\n", "k ", "K", "9k", "k-1", ""):
            with self.assertRaises(g.DescriptorError, msg=repr(bad)):
                self.run_gen(write(self.tmp, "n.json", dict(BASE, keys=[{"name": bad, "type": "int"}])))
        with self.assertRaises(g.DescriptorError):
            self.run_gen(write(self.tmp, "n2.json", dict(BASE, namespace="t\n")))

    def test_float32_range(self):
        for v in (1e39, -1e39):
            with self.assertRaises(g.DescriptorError, msg=repr(v)):
                self.run_gen(write(self.tmp, "f.json", dict(BASE, keys=[{"name": "f", "type": "float", "default": v}])))
        with self.assertRaises(g.DescriptorError):
            self.run_gen(write(self.tmp, "f2.json", dict(BASE, keys=[{"name": "f", "type": "float", "max": 1e39}])))
        # float32 max itself is fine
        self.run_gen(write(self.tmp, "f3.json", dict(BASE, keys=[{"name": "f", "type": "float", "default": 3.4e38, "max": 3.4e38}])))

    def test_no_descriptors(self):
        with self.assertRaises(g.DescriptorError):
            self.run_gen()

    def test_all_types_and_flags(self):
        d = {
            "namespace": "all", "version": 2, "title": "All", "description": "d",
            "keys": [
                {"name": "b", "type": "bool", "default": True, "restart_required": True},
                {"name": "i", "type": "int"},
                {"name": "f", "type": "float", "default": 0.5, "min": -1, "unit": "V"},
                {"name": "s", "type": "string", "default": "x", "maxLength": 4, "pattern": "^x", "secret": True},
                {"name": "e", "type": "string", "default": "aa", "enum": ["aa", "bbb"]},
                {"name": "z", "type": "blob", "maxLength": 3, "secret": True},
                {"name": "q", "type": "string", "default": "he said \"hi\"\\ é"},
            ],
        }
        ns, schema, c, h = self.run_gen(write(self.tmp, "all.json", d))
        p = schema["properties"]["all"]
        self.assertEqual(p["x-espos-version"], 2)
        self.assertEqual(p["properties"]["b"]["x-espos-restartRequired"], True)
        self.assertNotIn("minimum", p["properties"]["i"])  # full int32 range → no bounds
        self.assertEqual(p["properties"]["f"]["minimum"], -1.0)
        self.assertNotIn("maximum", p["properties"]["f"])
        self.assertEqual(p["properties"]["f"]["x-espos-unit"], "V")
        self.assertTrue(p["properties"]["s"]["writeOnly"])
        self.assertEqual(p["properties"]["s"]["pattern"], "^x")
        self.assertEqual(p["properties"]["e"]["maxLength"], 3)  # longest enum value
        self.assertEqual(p["properties"]["z"]["contentEncoding"], "base64")
        self.assertEqual(p["properties"]["z"]["x-espos-maxBytes"], 3)
        self.assertIn(".flags = ESPOS_CFG_FLAG_SECRET", c)
        self.assertIn("ESPOS_CFG_FLAG_RESTART_REQUIRED", c)
        self.assertIn(".has_min = true", c)
        self.assertNotIn(".has_max = true", c)
        self.assertIn('s_enum_all_e[] = { "aa", "bbb" }', c)
        # C string escaping of quotes, backslash and non-ASCII
        self.assertIn(r'he said \"hi\"\\ \303\251', c)
        # generated C compiles standalone
        src = os.path.join(self.tmp, "gen.c")
        inc = os.path.join(os.path.dirname(__file__), "..", "components", "espos_config", "include")
        r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-I", inc, src],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_write_if_changed_keeps_mtime(self):
        import time
        p = write(self.tmp, "a.json", BASE)
        _, _, _, _ = self.run_gen(p)
        out = os.path.join(self.tmp, "gen.c")
        m1 = os.stat(out).st_mtime_ns
        time.sleep(0.05)  # past the filesystem timestamp granularity
        _, _, _, _ = self.run_gen(p)
        self.assertEqual(m1, os.stat(out).st_mtime_ns)
        # a real change is written
        time.sleep(0.05)
        p2 = write(self.tmp, "a.json", dict(BASE, version=2))
        _, _, _, _ = self.run_gen(p2)
        self.assertNotEqual(m1, os.stat(out).st_mtime_ns)

    def test_schema_extensions_and_etag(self):
        d = {"namespace": "e", "version": 1, "keys": [
            {"name": "s", "type": "string", "secret": True},
            {"name": "b", "type": "blob", "maxLength": 3},
        ]}
        _, schema, c, h = self.run_gen(write(self.tmp, "e.json", d))
        ps = schema["properties"]["e"]["properties"]
        self.assertTrue(ps["s"]["x-espos-secret"])
        self.assertEqual(ps["s"]["format"], "password")
        self.assertEqual(ps["b"]["x-espos-type"], "blob")
        # etag = first 16 hex chars of sha256 over the compact schema text embedded in C
        import hashlib, re
        etag = re.search(r'ESPOS_CFG_SCHEMA_ETAG "([0-9a-f]{16})"', h).group(1)
        compact = json.dumps(schema, separators=(",", ":"), ensure_ascii=False)
        self.assertEqual(etag, hashlib.sha256(compact.encode()).hexdigest()[:16])
        # etag changes when the schema changes
        _, _, _, h2 = self.run_gen(write(self.tmp, "e.json", dict(d, version=2)))
        self.assertNotEqual(etag, re.search(r'ESPOS_CFG_SCHEMA_ETAG "([0-9a-f]{16})"', h2).group(1))

    def test_cli_semicolon_list(self):
        a = write(self.tmp, "a.json", BASE)
        b = write(self.tmp, "b.json", dict(BASE, namespace="u"))
        rc = g.main(["--schema-out", os.path.join(self.tmp, "s.json"), "--c-out", os.path.join(self.tmp, "g.c"),
                     "--h-out", os.path.join(self.tmp, "k.h"), f"{a};{b}"])
        self.assertEqual(rc, 0)
        rc = g.main(["--schema-out", os.path.join(self.tmp, "s.json"), "--c-out", os.path.join(self.tmp, "g.c"),
                     "--h-out", os.path.join(self.tmp, "k.h"), os.path.join(self.tmp, "missing.json")])
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
