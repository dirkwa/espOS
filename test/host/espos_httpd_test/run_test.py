#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Drives build/espos_httpd_test.elf through the REST API contract in docs/api.md.
Standard library only. Exit code 0 == all checks passed.
"""
import http.client
import itertools
import json
import os
import select
import socket
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, "build", "espos_httpd_test.elf")
PORT = 0  # chosen per harness instance, see pick_port()
SENTINEL = "********"


def free_port_check(port):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) != 0


def pick_port():
    """A fresh ephemeral port per harness instance. esp_http_server does not
    set SO_REUSEADDR, so re-binding the previous port right after a restart
    can fail while old connections sit in TIME_WAIT."""
    forced = os.environ.get("ESPOS_TEST_PORT")
    if forced:
        return int(forced)
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Harness:
    def __init__(self, fresh=True, extra_env=None):
        global PORT
        PORT = pick_port()
        if not free_port_check(PORT):
            raise RuntimeError(f"port {PORT} is already in use; set ESPOS_TEST_PORT to a free port")
        env = dict(os.environ, ESPOS_TEST_PORT=str(PORT))
        env.update(extra_env or {})
        if fresh:
            env["ESPOS_TEST_FRESH"] = "1"
        self.proc = subprocess.Popen([ELF], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, env=env, cwd=HERE)
        deadline = time.time() + 20
        self.log = []
        ready = False
        # Ready when the harness says so OR when the port answers (the READY
        # line travels through stdio, the port is the ground truth).
        while time.time() < deadline:
            if self.proc.poll() is not None:
                break
            r, _, _ = select.select([self.proc.stdout], [], [], 0.2)
            if r:
                line = self.proc.stdout.readline()
                if not line:
                    break
                self.log.append(line)
                if "ESPOS_HARNESS_READY" in line:
                    ready = True
                    break
            if not free_port_check(PORT):
                ready = True
                break
        if not ready:
            self.stop()
            raise RuntimeError("harness did not become ready:\n" + "".join(self.log))
        for _ in range(100):
            if not free_port_check(PORT):
                return
            time.sleep(0.05)
        self.stop()
        raise RuntimeError("harness port never opened:\n" + "".join(self.log))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        try:
            self.proc.stdout.close()
        except Exception:
            pass
        self.proc.wait()

    def wait_exit(self, timeout=5.0):
        try:
            code = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        self.proc.stdout.close()
        return code


def req(method, path, body=None, headers=None, content_type="application/json"):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    data = None
    hdrs = dict(headers or {})
    if body is not None:
        data = body if isinstance(body, (bytes, str)) else json.dumps(body)
    if method in ("PUT", "POST") and content_type:
        hdrs.setdefault("Content-Type", content_type)
    conn.request(method, path, body=data, headers=hdrs)
    r = conn.getresponse()
    raw = r.read()
    conn.close()
    parsed = None
    ctype = r.getheader("Content-Type", "")
    if "json" in ctype and raw:
        parsed = json.loads(raw)
    return r.status, dict(r.getheaders()), raw, parsed


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True)

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    # ---- schema
    def test_01_schema(self):
        st, hd, raw, js = req("GET", "/api/v1/config/schema")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Content-Type"), "application/schema+json")
        etag = hd.get("ETag")
        self.assertTrue(etag and etag.startswith('"') and etag.endswith('"'))
        self.assertEqual(js["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertIn("app", js["properties"])
        self.assertIn("httpd", js["properties"])
        self.assertEqual(js["properties"]["httpd"]["properties"]["port"]["default"], 80)
        self.assertTrue(js["properties"]["app"]["properties"]["api_key"]["writeOnly"])
        # conditional GET
        st2, hd2, raw2, _ = req("GET", "/api/v1/config/schema", headers={"If-None-Match": etag})
        self.assertEqual(st2, 304)
        self.assertEqual(raw2, b"")
        st3, _, _, _ = req("GET", "/api/v1/config/schema", headers={"If-None-Match": '"stale"'})
        self.assertEqual(st3, 200)

    # ---- system
    def test_02_system_info(self):
        st, hd, raw, js = req("GET", "/api/v1/system/info")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Content-Type"), "application/json")
        for k in ("app", "version", "idf_version", "chip", "uptime_s", "free_heap", "min_free_heap",
                  "reset_reason", "config_storage_reset", "schema_etag"):
            self.assertIn(k, js, k)
        self.assertEqual(js["app"], "espos_httpd_test")
        self.assertIsInstance(js["uptime_s"], int)
        self.assertFalse(js["config_storage_reset"])
        # schema_etag matches the ETag header served with the schema
        _, hd2, _, _ = req("GET", "/api/v1/config/schema")
        self.assertEqual(hd2.get("ETag"), '"%s"' % js["schema_etag"])

    # ---- config GET
    def test_03_config_get_defaults(self):
        st, hd, raw, js = req("GET", "/api/v1/config")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Cache-Control"), "no-store")
        self.assertTrue({"app", "httpd", "wifi"} <= set(js.keys()))
        app = js["app"]
        self.assertEqual(app["label"], "espOS device")
        self.assertTrue(app["enabled"])
        self.assertEqual(app["interval_ms"], 1000)
        self.assertEqual(app["scale"], 1.0)
        self.assertEqual(app["mode"], "auto")
        self.assertEqual(app["api_key"], "")       # unset secret is "", not the sentinel
        self.assertEqual(app["cal_table"], "")
        self.assertEqual(js["httpd"]["port"], PORT)  # harness overrode it
        # namespace filter
        st, _, _, js2 = req("GET", "/api/v1/config?ns=app")
        self.assertEqual(st, 200)
        self.assertEqual(list(js2.keys()), ["app"])
        st, _, _, js3 = req("GET", "/api/v1/config?ns=nope")
        self.assertEqual(st, 404)
        self.assertEqual(js3["error"], "unknown_namespace")

    # ---- config PUT
    def test_04_config_put_roundtrip(self):
        body = {"app": {"label": "boat", "interval_ms": 250, "scale": 0.1, "mode": "manual",
                        "api_key": "s3cret", "cal_table": "AQIDBA==", "enabled": False}}
        st, _, _, js = req("PUT", "/api/v1/config", body)
        self.assertEqual(st, 200, js)
        self.assertEqual(sorted(js["changed"]),
                         sorted(["app.label", "app.interval_ms", "app.scale", "app.mode",
                                 "app.api_key", "app.cal_table", "app.enabled"]))
        self.assertFalse(js["restart_required"])
        st, _, raw, js = req("GET", "/api/v1/config?ns=app")
        self.assertEqual(js["app"]["label"], "boat")
        self.assertEqual(js["app"]["interval_ms"], 250)
        self.assertEqual(js["app"]["scale"], 0.1)
        self.assertIn(b'"scale":0.1,', raw)  # canonical float rendering
        self.assertEqual(js["app"]["mode"], "manual")
        self.assertEqual(js["app"]["api_key"], SENTINEL)
        self.assertEqual(js["app"]["cal_table"], "AQIDBA==")
        self.assertFalse(js["app"]["enabled"])
        # idempotent re-PUT of what we read back: sentinel ignored, nothing changes
        st, _, _, js2 = req("PUT", "/api/v1/config", js)
        self.assertEqual(st, 200)
        self.assertEqual(js2["changed"], [])
        # null resets, restart_required surfaces for httpd.port
        st, _, _, js3 = req("PUT", "/api/v1/config", {"app": {"label": None}, "httpd": {"port": PORT + 1}})
        self.assertEqual(st, 200)
        self.assertEqual(sorted(js3["changed"]), ["app.label", "httpd.port"])
        self.assertTrue(js3["restart_required"])
        st, _, _, js4 = req("GET", "/api/v1/config")
        self.assertEqual(js4["app"]["label"], "espOS device")
        self.assertEqual(js4["httpd"]["port"], PORT + 1)
        # put it back so a restart of the harness stays reachable
        req("PUT", "/api/v1/config", {"httpd": {"port": PORT}})

    def test_05_config_put_validation(self):
        cases = [
            ({"app": {"interval_ms": 5}}, "app.interval_ms", "out of range"),
            ({"app": {"interval_ms": "5"}}, "app.interval_ms", "expected integer"),
            ({"app": {"mode": "off"}}, "app.mode", "not an allowed"),
            ({"app": {"mode": "automatic"}}, "app.mode", "longer than"),  # enum maxLength = longest value
            ({"app": {"cal_table": "###"}}, "app.cal_table", "invalid base64"),
            ({"app": {"nope": 1}}, "app.nope", "unknown key"),
            ({"nope": {}}, "nope", "unknown namespace"),
            ({"app": 3}, "app", "expected object"),
            ({"app": {"scale": 1000}}, "app.scale", "above maximum"),
        ]
        for body, path, msg in cases:
            st, _, _, js = req("PUT", "/api/v1/config", body)
            self.assertEqual(st, 400, (body, js))
            self.assertEqual(js["error"], "validation")
            self.assertEqual(js["path"], path)
            self.assertIn(msg, js["message"])
        # malformed JSON
        st, _, _, js = req("PUT", "/api/v1/config", "{not json")
        self.assertEqual(st, 400)
        self.assertEqual(js["error"], "validation")
        self.assertIn("malformed", js["message"])
        # all-or-nothing: valid + invalid in one document writes nothing
        st, _, _, _ = req("PUT", "/api/v1/config", {"app": {"label": "partial", "interval_ms": 1}})
        self.assertEqual(st, 400)
        _, _, _, js = req("GET", "/api/v1/config?ns=app")
        self.assertNotEqual(js["app"]["label"], "partial")

    def test_06_body_too_large(self):
        big = {"app": {"label": "x" * 5000}}
        st, _, _, js = req("PUT", "/api/v1/config", big)
        self.assertEqual(st, 413)
        self.assertEqual(js["error"], "too_large")

    def test_07_not_found_and_methods(self):
        st, _, _, js = req("GET", "/api/v1/nope")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        # wrong method on a known resource: still a JSON error body
        st, hd, _, js = req("DELETE", "/api/v1/config")
        self.assertIn(st, (404, 405))
        self.assertEqual(hd.get("Content-Type"), "application/json")
        self.assertIn(js["error"], ("not_found", "method_not_allowed"))
        st, hd, raw, _ = req("GET", "/")
        self.assertEqual(st, 200)
        self.assertTrue(hd.get("Content-Type", "").startswith("text/html"))
        self.assertIn(b"espOS", raw)
        self.assertFalse(raw.endswith(b"\x00"), "embedded NUL must not be served")
        self.assertEqual(int(hd.get("Content-Length")), len(raw))
        st, _, _, _ = req("GET", "/index.html")
        self.assertEqual(st, 200)
        # over-long ?ns= is an unknown namespace, not "everything"
        st, _, _, js = req("GET", "/api/v1/config?ns=" + "x" * 40)
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "unknown_namespace")
        st, _, _, js = req("GET", "/api/v1/config?" + "a" * 300)
        self.assertEqual(st, 414)
        # empty PUT body, non-object roots, empty object
        st, _, _, js = req("PUT", "/api/v1/config", "")
        self.assertEqual(st, 400)
        self.assertEqual(js["error"], "validation")
        st, _, _, js = req("PUT", "/api/v1/config", "[]")
        self.assertEqual(st, 400)
        self.assertIn("expected object", js["message"])
        st, _, _, js = req("PUT", "/api/v1/config", "{}")
        self.assertEqual(st, 200)
        self.assertEqual(js["changed"], [])
        # CSRF guard: state-changing requests without a JSON content type are refused
        for m, path, body in (("PUT", "/api/v1/config", "{}"),
                              ("POST", "/api/v1/system/reboot", None),
                              ("POST", "/api/v1/system/factory-reset", None)):
            st, _, _, js = req(m, path, body, content_type=None)
            self.assertEqual(st, 415, (m, path))
            self.assertEqual(js["error"], "unsupported_media_type")
            st, _, _, js = req(m, path, body, content_type="text/plain")
            self.assertEqual(st, 415, (m, path))
        # ...and a charset suffix is fine
        st, _, _, js = req("PUT", "/api/v1/config", "{}", content_type="application/json; charset=utf-8")
        self.assertEqual(st, 200)

    def test_08_persistence_across_restart(self):
        # values written above must survive a process restart (emulated NVS
        # lives in a file only for the process lifetime, so re-plant then
        # restart WITHOUT the fresh flag against a preserved flash file).
        # The emulated flash file is per process → persistence is covered by
        # the espos_config_test NVS cases; here we just check reboot semantics.
        st, _, _, js = req("POST", "/api/v1/system/reboot")
        self.assertEqual(st, 202)
        self.assertEqual(js["status"], "rebooting")
        # writes are refused while the restart is pending
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "late"}})
        self.assertEqual(st, 503)
        self.assertEqual(js["error"], "restarting")
        code = self.h.wait_exit(timeout=5.0)
        self.assertEqual(code, 0, "harness must exit cleanly on reboot (esp_restart → exit(0) on linux)")
        # start a new instance for the remaining tests
        type(self).h = Harness(fresh=True)

    def test_09_factory_reset(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": "wipe-me"}})
        self.assertEqual(st, 200)
        st, _, _, js = req("POST", "/api/v1/system/factory-reset")
        self.assertEqual(st, 202)
        self.assertEqual(js["status"], "factory_reset")
        self.assertTrue(js["rebooting"])
        # before the restart lands the store already serves defaults
        _, _, _, js = req("GET", "/api/v1/config?ns=app")
        self.assertEqual(js["app"]["label"], "espOS device")
        code = self.h.wait_exit(timeout=5.0)
        self.assertEqual(code, 0, "harness must exit cleanly on factory reset")
        type(self).h = Harness(fresh=True)

    def test_10_concurrent_requests(self):
        # interleaved PUT/GET from several threads: every response must be
        # well-formed and every GET must observe a value some PUT wrote
        import threading
        errors = []
        labels = [f"w{i}" for i in range(6)]

        def worker(label):
            try:
                for _ in range(8):
                    st, _, _, js = req("PUT", "/api/v1/config", {"app": {"label": label}})
                    if st != 200:
                        errors.append(("put", st, js))
                    st, _, _, js = req("GET", "/api/v1/config?ns=app")
                    if st != 200 or js["app"]["label"] not in labels + ["espOS device"]:
                        errors.append(("get", st, js))
            except Exception as e:  # noqa: BLE001
                errors.append(("exc", repr(e)))

        threads = [threading.Thread(target=worker, args=(l,)) for l in labels]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
        self.assertEqual(errors, [])
        # more sockets than max_open_sockets in quick succession (LRU purge)
        for _ in range(30):
            st, _, _, _ = req("GET", "/api/v1/system/info")
            self.assertEqual(st, 200)


class SseReader:
    """Minimal text/event-stream client on a raw socket (stdlib only)."""

    def __init__(self, path="/api/v1/events", timeout=5.0):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        self.sock.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n".encode())
        self.buf = b""
        # headers
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("closed during headers")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        self.status = head.split(b"\r\n")[0].decode()
        self.headers = dict(l.decode().split(": ", 1) for l in head.split(b"\r\n")[1:] if b": " in l)

    def _dechunk(self):
        # Transfer-Encoding: chunked; we only ever get whole small chunks
        out = b""
        while True:
            if b"\r\n" not in self.buf:
                break
            size_line, rest = self.buf.split(b"\r\n", 1)
            try:
                n = int(size_line.strip(), 16)
            except ValueError:
                # not a chunk header — treat buffer as raw data
                out += self.buf
                self.buf = b""
                break
            if len(rest) < n + 2:
                break
            out += rest[:n]
            self.buf = rest[n + 2:]
        return out

    def events(self, timeout=5.0):
        """Yield (event, data) tuples until timeout."""
        deadline = time.time() + timeout
        data_acc = b""
        while time.time() < deadline:
            data_acc += self._dechunk()
            while b"\n\n" in data_acc:
                block, data_acc = data_acc.split(b"\n\n", 1)
                ev, dat = None, []
                for line in block.decode().split("\n"):
                    if line.startswith("event: "):
                        ev = line[7:]
                    elif line.startswith("data: "):
                        dat.append(line[6:])
                    elif line.startswith("retry: ") or line.startswith(":"):
                        ev = ev or ("retry" if line.startswith("retry") else "comment")
                yield ev, "\n".join(dat)
            self.sock.settimeout(max(0.05, deadline - time.time()))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                return
            self.buf += chunk

    def close(self):
        self.sock.close()


def wait_for(pred, timeout=5.0, step=0.1):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(step)
    return last


class WifiTests(unittest.TestCase):
    """WiFi state machine over HTTP with the simulated driver (port_sim.c)."""

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True)

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_01_unconfigured_brings_the_portal_up(self):
        st, _, _, js = req("GET", "/api/v1/wifi/status")
        self.assertEqual(st, 200)
        self.assertEqual(js["state"], "unconfigured")
        self.assertTrue(js["sta_enabled"])
        self.assertEqual(js["hostname"], "espos-1a2b")
        self.assertTrue(js["portal"]["active"])
        self.assertEqual(js["portal"]["ssid"], "espOS-1a2b")
        self.assertEqual(js["portal"]["ip"], "192.168.4.1")
        self.assertEqual(js["reason"], {"code": 0, "text": ""})
        self.assertNotIn("ip", js)

    def test_01b_short_psk_slot_is_skipped(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "short"}})
        self.assertEqual(st, 200)
        time.sleep(0.5)
        st, _, _, js = req("GET", "/api/v1/wifi/status")
        self.assertEqual(js["state"], "unconfigured")   # 1..7 char WPA passwords are not valid
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": None, "psk0": None}})
        time.sleep(0.5)

    def test_02_scan(self):
        st, _, _, js = req("POST", "/api/v1/wifi/scan", None, content_type=None)
        self.assertEqual(st, 415)
        st, _, _, js = req("POST", "/api/v1/wifi/scan", None)
        self.assertEqual(st, 202)
        st, _, _, js = req("GET", "/api/v1/wifi/scan")
        self.assertEqual(st, 200)
        self.assertFalse(js["scanning"])
        names = sorted(r["ssid"] for r in js["results"])
        self.assertEqual(names, ["Boat", "Marina-Guest"])
        boat = [r for r in js["results"] if r["ssid"] == "Boat"][0]
        self.assertEqual(boat["auth"], "wpa2")
        self.assertEqual(boat["bssid"], "de:ad:be:ef:00:01")

    def test_03_configure_and_connect_with_sse(self):
        sse = SseReader()
        self.assertIn("200", sse.status)
        self.assertEqual(sse.headers.get("Content-Type"), "text/event-stream")
        # hello: retry + a wifi snapshot
        first = list(itertools.islice(sse.events(timeout=3), 2))
        self.assertEqual(first[0][0], "retry")
        self.assertEqual(first[1][0], "wifi")
        self.assertEqual(json.loads(first[1][1])["state"], "unconfigured")

        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})
        self.assertEqual(st, 200)
        seen = []
        for ev, data in sse.events(timeout=4):
            seen.append((ev, data))
            if ev == "wifi" and json.loads(data)["state"] == "connected":
                break
        kinds = [e for e, _ in seen]
        self.assertIn("config", kinds)               # config change was broadcast
        states = [json.loads(d)["state"] for e, d in seen if e == "wifi"]
        self.assertIn("connecting", states)
        self.assertIn("obtaining_ip", states)
        self.assertEqual(states[-1], "connected")
        sse.close()

        st, _, _, js = req("GET", "/api/v1/wifi/status")
        self.assertEqual(js["state"], "connected")
        self.assertEqual(js["ssid"], "Boat")
        self.assertEqual(js["ip"], "10.0.0.2")
        self.assertEqual(js["gateway"], "10.0.0.1")
        self.assertEqual(js["rssi"], -55)
        self.assertEqual(js["network_index"], 0)
        self.assertEqual(js["connect_count"], 1)
        self.assertFalse(js["portal"]["active"])      # portal goes down once connected
        self.assertEqual(js["reason"]["code"], 0)
        # secrets stay secret
        _, _, _, cfg = req("GET", "/api/v1/config?ns=wifi")
        self.assertEqual(cfg["wifi"]["psk0"], SENTINEL)
        self.assertEqual(cfg["wifi"]["ssid0"], "Boat")

    def test_03b_sse_eviction_when_full(self):
        readers = [SseReader() for _ in range(3)]
        for r in readers:
            self.assertIn("200", r.status)
        # a 4th stream evicts the oldest instead of failing
        r4 = SseReader()
        self.assertIn("200", r4.status)
        first = list(itertools.islice(r4.events(timeout=3), 2))
        self.assertEqual(first[1][0], "wifi")
        # the oldest reader now sees EOF (its socket was shut down)
        got = list(readers[0].events(timeout=2))
        self.assertTrue(all(e in ("retry", "wifi", "comment") for e, _ in got))
        readers[0].sock.settimeout(1.0)
        try:
            eof = readers[0].sock.recv(10) == b""
        except socket.timeout:
            eof = False
        self.assertTrue(eof, "evicted stream must be closed by the server")
        for r in readers + [r4]:
            r.close()
        time.sleep(0.5)

    def test_04_disable_and_reenable(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"sta_enabled": False}})
        self.assertEqual(st, 200)
        js = wait_for(lambda: (lambda r: r[3] if r[3]["state"] == "disabled" else None)(req("GET", "/api/v1/wifi/status")))
        self.assertIsNotNone(js)
        self.assertEqual(js["reason"]["code"], 1005)
        self.assertTrue(js["portal"]["active"])
        req("PUT", "/api/v1/config", {"wifi": {"sta_enabled": True}})
        js = wait_for(lambda: (lambda r: r[3] if r[3]["state"] == "connected" else None)(req("GET", "/api/v1/wifi/status")))
        self.assertIsNotNone(js)
        self.assertEqual(js["connect_count"], 2)


class WifiFailureTests(unittest.TestCase):
    """Driver reports AUTH_FAIL (202) on every attempt."""

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_WIFI": "fail:202"})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_wrong_password_is_reported_with_backoff(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "wrongpass", "ssid1": "Other", "psk1": "xxxxxxxx"}})
        self.assertEqual(st, 200)
        # after both networks fail once we must be in backoff with the reason
        js = wait_for(lambda: (lambda r: r[3] if r[3]["state"] == "backoff" else None)(req("GET", "/api/v1/wifi/status")), timeout=6)
        self.assertIsNotNone(js)
        self.assertEqual(js["reason"]["code"], 202)
        self.assertEqual(js["reason"]["text"], "wrong password")
        self.assertGreater(js["backoff_ms"], 0)
        self.assertLessEqual(js["backoff_ms"], 1300 * (2 ** (js["round"] - 1)))   # 1 s·2^(round-1) ±25 %
        self.assertGreaterEqual(js["round"], 1)
        self.assertEqual(js["attempt"], 2 * js["round"])   # both networks tried every round
        self.assertEqual(js["network_index"], 0)
        # a second round follows automatically
        js2 = wait_for(lambda: (lambda r: r[3] if r[3]["round"] >= 2 else None)(req("GET", "/api/v1/wifi/status")), timeout=6)
        self.assertIsNotNone(js2)
        self.assertGreaterEqual(js2["attempt"], 3)


if __name__ == "__main__":
    if not os.path.exists(ELF):
        print(f"missing {ELF}; build first (idf.py --preview set-target linux && idf.py build)")
        sys.exit(2)
    unittest.main(verbosity=2)
