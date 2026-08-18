#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Drives build/espos_httpd_test.elf through the REST API contract in docs/api.md.
Standard library only. Exit code 0 == all checks passed.
"""
import gzip
import http.client
import itertools
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
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
        # Keep draining stdout so the harness never blocks on a full pipe and
        # tests can grep self.log.
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()
        for _ in range(100):
            if not free_port_check(PORT):
                return
            time.sleep(0.05)
        self.stop()
        raise RuntimeError("harness port never opened:\n" + "".join(self.log))

    def _drain(self):
        try:
            for line in self.proc.stdout:
                self.log.append(line)
        except (ValueError, OSError):
            pass

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


# ------------------------------------------------------------ SignalK mock

import http.server
import uuid as _uuid


class MockSignalK:
    """Just enough of signalk-server's security API for the token flow, plus
    /__test/* controls. Runs in a thread on an ephemeral port."""

    def __init__(self, self_urn="urn:mrn:signalk:uuid:0e6d1a1a-1111-4111-8111-000000000099"):
        self.self_urn = self_urn
        self.security = True
        self.device_requests = True
        self.requests = {}      # requestId -> dict(clientId, state, permission, token)
        self.tokens = {}        # token -> clientId (valid)
        self.log = []
        self.deltas = []        # decoded delta documents received on the stream
        self.ws_open = 0
        self.ws_accept = True   # False → refuse the upgrade (simulates an unreachable stream)
        self.ws_auth = None     # last Authorization header seen on the stream
        self.meta = {}          # path -> meta dict
        self.meta_puts = []
        mock = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _send(self, code, obj=None, raw=None, ctype="application/json"):
                body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _ws(self):
                import base64, hashlib, struct
                mock.ws_auth = self.headers.get("Authorization")
                if not mock.ws_accept:
                    return self._send(503, raw=b"no", ctype="text/plain")
                key = self.headers.get("Sec-WebSocket-Key", "")
                acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                self.send_response(101)
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", acc)
                self.end_headers()
                sock = self.connection
                sock.settimeout(1.0)
                hello = json.dumps({"name": "signalk-server", "version": "2.31.1", "self": "vessels." + mock.self_urn}).encode()
                sock.sendall(bytes([0x81, len(hello)]) + hello)
                mock.ws_open += 1
                buf = b""
                try:
                    while mock.ws_accept:
                        try:
                            chunk = sock.recv(4096)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        buf += chunk
                        while len(buf) >= 2:
                            fin_op, m_len = buf[0], buf[1]
                            masked = m_len & 0x80
                            ln = m_len & 0x7F
                            off = 2
                            if ln == 126:
                                if len(buf) < 4:
                                    break
                                ln = struct.unpack(">H", buf[2:4])[0]
                                off = 4
                            elif ln == 127:
                                if len(buf) < 10:
                                    break
                                ln = struct.unpack(">Q", buf[2:10])[0]
                                off = 10
                            need = off + (4 if masked else 0) + ln
                            if len(buf) < need:
                                break
                            mask = buf[off:off + 4] if masked else b""
                            payload = buf[off + (4 if masked else 0):need]
                            if masked:
                                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                            buf = buf[need:]
                            op = fin_op & 0x0F
                            if op == 0x1:
                                try:
                                    mock.deltas.append(json.loads(payload.decode()))
                                except Exception:
                                    mock.deltas.append({"raw": payload.decode(errors="replace")})
                            elif op == 0x8:
                                raise ConnectionError("close")
                            elif op == 0x9:
                                sock.sendall(bytes([0x8A, len(payload)]) + payload)
                except (ConnectionError, OSError):
                    pass
                finally:
                    mock.ws_open -= 1
                    try:
                        sock.close()
                    except OSError:
                        pass
                self.close_connection = True

            def do_PUT(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                if self.path.startswith("/signalk/v1/api/vessels/self/") and self.path.endswith("/meta"):
                    auth = self.headers.get("Authorization", "")
                    tok = auth[7:] if auth.startswith("Bearer ") else ""
                    if mock.security and tok not in mock.tokens:
                        return self._send(401, raw=b"Unauthorized", ctype="text/plain")
                    path = self.path[len("/signalk/v1/api/vessels/self/"):-len("/meta")].replace("/", ".")
                    mock.meta[path] = body.get("value")
                    mock.meta_puts.append(path)
                    return self._send(200, {"state": "COMPLETED", "statusCode": 200})
                return self._send(404, {"error": "nope"})

            def do_GET(self):
                mock.log.append(("GET", self.path, self.headers.get("Authorization")))
                if self.path.startswith("/signalk/v1/stream"):
                    return self._ws()
                if self.path.startswith("/signalk/v1/api/vessels/self/") and self.path.endswith("/meta"):
                    path = self.path[len("/signalk/v1/api/vessels/self/"):-len("/meta")].replace("/", ".")
                    if path in mock.meta:
                        return self._send(200, mock.meta[path])
                    return self._send(404, {"error": "no meta"})
                if self.path == "/signalk":
                    return self._send(200, {"endpoints": {"v1": {"version": "2.31.1"}}})
                if self.path.startswith("/signalk/v1/requests/"):
                    rid = self.path.rsplit("/", 1)[1]
                    r = mock.requests.get(rid)
                    if not r:
                        return self._send(500, raw=b"Unable to check request: not found", ctype="text/plain")
                    reply = {"state": r["state"], "requestId": rid, "statusCode": 202 if r["state"] == "PENDING" else 200,
                             "href": "/signalk/v1/requests/" + rid}
                    if r["state"] == "COMPLETED":
                        reply["accessRequest"] = {"permission": r["permission"]}
                        if r.get("token"):
                            reply["accessRequest"]["token"] = r["token"]
                    return self._send(200, reply)
                if self.path == "/signalk/v1/api/self":
                    if not mock.security:
                        return self._send(200, "vessels." + mock.self_urn)
                    auth = self.headers.get("Authorization", "")
                    tok = auth[7:] if auth.startswith("Bearer ") else ""
                    if tok and tok in mock.tokens:
                        return self._send(200, "vessels." + mock.self_urn)
                    return self._send(401, raw=b"Unauthorized", ctype="text/plain")
                if self.path.startswith("/__test/"):
                    parts = self.path.split("/")
                    cmd, arg = parts[2], (parts[3] if len(parts) > 3 else "")
                    if cmd == "approve":
                        for rid, r in mock.requests.items():
                            if r["clientId"] == arg and r["state"] == "PENDING":
                                tok = "mock." + _uuid.uuid4().hex
                                r.update(state="COMPLETED", permission="APPROVED", token=tok)
                                mock.tokens[tok] = arg
                                return self._send(200, {"token": tok})
                        return self._send(404, {"error": "no pending"})
                    if cmd == "deny":
                        for rid, r in mock.requests.items():
                            if r["clientId"] == arg and r["state"] == "PENDING":
                                r.update(state="COMPLETED", permission="DENIED")
                                return self._send(200, {})
                        return self._send(404, {"error": "no pending"})
                    if cmd == "revoke":
                        mock.tokens = {t: c for t, c in mock.tokens.items() if c != arg}
                        return self._send(200, {})
                    if cmd == "forget":
                        mock.requests.clear()
                        return self._send(200, {})
                    if cmd == "issue":       # mint a token without a request (manual paste)
                        tok = "manual." + _uuid.uuid4().hex
                        mock.tokens[tok] = arg
                        return self._send(200, {"token": tok})
                    if cmd == "security":
                        mock.security = arg == "on"
                        return self._send(200, {})
                    if cmd == "devreq":
                        mock.device_requests = arg == "on"
                        return self._send(200, {})
                    if cmd == "log":
                        return self._send(200, mock.log)
                return self._send(404, {"error": "nope"})

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(n) or b"{}")
                mock.log.append(("POST", self.path, body))
                if self.path == "/signalk/v1/access/requests":
                    if not mock.security:
                        return self._send(404, {"message": "Access requests not available. Server security is not enabled."})
                    if not mock.device_requests:
                        return self._send(403, {"state": "COMPLETED", "statusCode": 403})
                    cid = body.get("clientId")
                    for rid, r in mock.requests.items():
                        if r["clientId"] == cid and r["state"] == "PENDING":
                            return self._send(400, {"state": "COMPLETED", "statusCode": 400,
                                                    "message": f"A device with clientId '{cid}' has already requested access"})
                    rid = str(_uuid.uuid4())
                    mock.requests[rid] = {"clientId": cid, "state": "PENDING", "permission": "", "description": body.get("description")}
                    return self._send(202, {"state": "PENDING", "requestId": rid, "statusCode": 202,
                                            "href": "/signalk/v1/requests/" + rid})
                return self._send(404, {"error": "nope"})

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def ctl(self, cmd, arg=""):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request("GET", f"/__test/{cmd}/{arg}")
        r = c.getresponse()
        raw = r.read()
        c.close()
        return r.status, (json.loads(raw) if raw else None)

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


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
        self.assertTrue(all(e in ("retry", "wifi", "sk", "sk_servers", "sk_ws", "ota", "logs", "comment") for e, _ in got), got)
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


class UiAndLogsTests(unittest.TestCase):
    """M5 firmware side: static UI from a directory, log ring, coredump (absent on host)."""

    @classmethod
    def setUpClass(cls):
        cls.www = tempfile.mkdtemp(prefix="espos-www-")
        os.makedirs(os.path.join(cls.www, "assets"))
        with open(os.path.join(cls.www, "index.html"), "wb") as f:
            f.write(b"<html>SPA</html>")
        with gzip.open(os.path.join(cls.www, "assets", "app-abc123.js.gz"), "wb") as f:
            f.write(b"console.log('hi')")
        with open(os.path.join(cls.www, "plain.txt"), "wb") as f:
            f.write(b"plain")
        cls.h = Harness(fresh=True, extra_env={"ESPOS_WWW_DIR": cls.www})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        shutil.rmtree(cls.www, ignore_errors=True)

    def test_01_index_and_spa_fallback(self):
        st, hd, raw, _ = req("GET", "/")
        self.assertEqual(st, 200)
        self.assertEqual(raw, b"<html>SPA</html>")
        self.assertIn("text/html", hd["Content-Type"])
        self.assertEqual(hd["Cache-Control"], "no-cache")
        for route in ("/wifi", "/config/sk", "/index.html"):
            st, hd, raw, _ = req("GET", route)
            self.assertEqual((route, st), (route, 200))
            self.assertEqual(raw, b"<html>SPA</html>")
        # a missing file with an extension is a real 404 (JSON, per contract)
        st, hd, raw, js = req("GET", "/missing.png")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        # the API namespace never falls back to the SPA
        st, hd, raw, js = req("GET", "/api/v1/nope")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        # no directory traversal
        st, _, _, _ = req("GET", "/../etc/passwd")
        self.assertEqual(st, 404)

    def test_02_gzip_asset_and_plain(self):
        st, hd, raw, _ = req("GET", "/assets/app-abc123.js")
        self.assertEqual(st, 200)
        self.assertEqual(hd.get("Content-Encoding"), "gzip")
        self.assertIn("javascript", hd["Content-Type"])
        self.assertIn("immutable", hd["Cache-Control"])
        self.assertEqual(gzip.decompress(raw), b"console.log('hi')")
        st, hd, raw, _ = req("GET", "/plain.txt")
        self.assertEqual(st, 200)
        self.assertNotIn("Content-Encoding", hd)
        self.assertEqual(raw, b"plain")
        st, _, _, js = req("GET", "/api/v1/system/info")
        self.assertTrue(js["ui_storage"])

    def test_03_logs_ring_and_paging(self):
        st, _, _, js = req("GET", "/api/v1/logs")
        self.assertEqual(st, 200)
        self.assertGreater(js["next"], js["first"])
        self.assertEqual(js["from"], js["first"])
        self.assertEqual(len(js["lines"]), js["next"] - js["first"])
        self.assertTrue(any("espos_httpd" in l for l in js["lines"]))
        self.assertFalse(any("\x1b" in l for l in js["lines"]))     # no colour codes
        # paging: after + limit
        st, _, _, page = req("GET", f"/api/v1/logs?after={js['first']}&limit=2")
        self.assertEqual(page["from"], js["first"] + 1)
        self.assertEqual(page["lines"], js["lines"][1:3])
        # a config change is logged → shows up after the last seq
        req("PUT", "/api/v1/config", {"httpd": {"port": PORT}})
        js2 = wait_for(lambda: (lambda r: r[3] if r[3]["next"] > js["next"] else None)(req("GET", f"/api/v1/logs?after={js['next'] - 1}")))
        self.assertIsNotNone(js2)
        self.assertEqual(js2["from"], js["next"])
        # a too-old "after" is reported as a gap and clamped
        st, _, _, g = req("GET", "/api/v1/logs?after=0")
        self.assertFalse(g["gap"])       # 0 is "from the start", not a gap
        self.assertEqual(g["from"], g["first"])

    def test_04_logs_level(self):
        st, _, _, js = req("PUT", "/api/v1/logs/level", {"tag": "espos_httpd", "level": "debug"})
        self.assertEqual(st, 200)
        self.assertEqual(js, {"tag": "espos_httpd", "level": "debug"})
        st, _, _, js = req("PUT", "/api/v1/logs/level", {"level": "bogus"})
        self.assertEqual(st, 400)
        self.assertEqual(js["error"], "validation")
        st, _, _, js = req("PUT", "/api/v1/logs/level", {"tag": "espos_httpd", "level": "info"})
        self.assertEqual(st, 200)

    def test_05_logs_sse_event(self):
        sse = SseReader()
        req("PUT", "/api/v1/logs/level", {"tag": "espos_sse_probe", "level": "info"})   # any request that logs
        req("PUT", "/api/v1/config", {"httpd": {"port": PORT}})
        seen = [e for e, _ in sse.events(timeout=3)]
        sse.close()
        self.assertIn("logs", seen)

    def test_06_coredump_absent_on_host(self):
        st, _, _, js = req("GET", "/api/v1/system/coredump")
        self.assertEqual(st, 404)
        self.assertEqual(js["error"], "not_found")
        st, _, _, js = req("GET", "/api/v1/system/coredump/raw")
        self.assertEqual(st, 404)
        st, _, _, js = req("DELETE", "/api/v1/system/coredump")
        self.assertEqual(st, 200)


class FirmwareServer:
    """Serves a manifest and fake images (sim format: first line 'ESPOS-IMAGE <project> <version>')."""

    def __init__(self):
        self.files = {}
        srv = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def handle(self):
                try:
                    super().handle()
                except ConnectionResetError:
                    pass            # keep-alive socket reset by an aborting client

            def do_GET(self):
                body = srv.files.get(self.path)
                if body is None:
                    self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass            # the device aborts rejected images mid-download

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        self.base = f"http://127.0.0.1:{self.port}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def image(self, path, project="espos", version="0.7.0", size=40000, badsig=False):
        head = f"ESPOS-IMAGE {project} {version}{' BADSIG' if badsig else ''}\n".encode()
        self.files[path] = head + b"x" * (size - len(head))

    def manifest(self, path, builds, app="espos"):
        self.files[path] = json.dumps({"schema": 1, "app": app, "builds": builds}).encode()

    def stop(self):
        self.httpd.shutdown()


class OtaTests(unittest.TestCase):
    """espos_ota against the sim port: manifest check, install from manifest/URL,
    rejected images, SSE, confirm-on-network policy."""

    @classmethod
    def setUpClass(cls):
        cls.fw = FirmwareServer()
        cls.fw.image("/fw/espos-0.7.0.bin", version="0.7.0")
        cls.fw.image("/fw/other.bin", project="otherapp")
        cls.fw.image("/fw/badsig.bin", badsig=True)
        cls.fw.files["/fw/notimage.bin"] = b"<html>nope</html>"
        cls.fw.manifest("/fw/manifest.json", [
            {"version": "0.7.0", "target": "linux", "url": "espos-0.7.0.bin", "size": 40000, "notes": "test build"},
            {"version": "0.6.0", "target": "linux", "url": "old.bin"},
            {"version": "9.9.9", "target": "esp32p4", "url": "p4.bin"},
        ])
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_OTA_PENDING": "1", "ESPOS_SIM_WIFI": "connect"})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.fw.stop()

    def ota(self):
        return req("GET", "/api/v1/ota/status")[3]

    def wait_state(self, *states, timeout=15):
        return wait_for(lambda: (lambda o: o if o["state"] in states else None)(self.ota()), timeout=timeout)

    def test_01_status_and_pending_image_confirms_once_connected(self):
        js = self.ota()
        self.assertEqual(js["running"]["version"], "0.6.0")
        self.assertEqual(js["running"]["target"], "linux")
        self.assertEqual(js["running"]["slot"], "ota_0")
        self.assertIsNone(js["available"])
        # boots PENDING_VERIFY; WiFi is unconfigured → no confirmation yet
        self.assertTrue(js["running"]["pending_verify"])
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}})
        js = wait_for(lambda: (lambda o: o if o["running"]["confirmed"] else None)(self.ota()), timeout=15)
        self.assertIsNotNone(js, "image must confirm once the network is up")
        self.assertFalse(js["running"]["pending_verify"])
        self.assertEqual(js["running"]["image_state"], "valid")

    def test_02_check_without_manifest_fails_cleanly(self):
        st, _, _, js = req("POST", "/api/v1/ota/check")
        self.assertEqual(st, 202)
        js = self.wait_state("failed")
        self.assertIsNotNone(js)
        self.assertIn("no manifest URL", js["last_error"])

    def test_03_manifest_check_finds_newer_build(self):
        sse = SseReader()
        st, _, _, js = req("PUT", "/api/v1/config", {"ota": {"manifest_url": self.fw.base + "/fw/manifest.json"}})
        self.assertEqual(st, 200)
        st, _, _, js = req("POST", "/api/v1/ota/check")
        self.assertEqual(st, 202)
        js = self.wait_state("available")
        self.assertIsNotNone(js, self.ota())
        self.assertEqual(js["available"]["version"], "0.7.0")
        self.assertEqual(js["available"]["url"], self.fw.base + "/fw/espos-0.7.0.bin")   # relative → resolved
        self.assertTrue(js["available"]["newer"])
        self.assertEqual(js["available"]["notes"], "test build")
        self.assertIsNotNone(js["manifest"]["last_check_s"])
        events = [e for e, _ in sse.events(timeout=1.5)]
        sse.close()
        self.assertIn("ota", events)

    def test_04_install_available_downloads_with_progress(self):
        st, _, _, js = req("POST", "/api/v1/ota", {})
        self.assertEqual(st, 202)
        prog = wait_for(lambda: (lambda o: o if o["state"] == "downloading" and o["progress"]["received"] > 0 else None)(self.ota()), timeout=10)
        self.assertIsNotNone(prog, self.ota())
        self.assertEqual(prog["progress"]["total"], 40000)
        # a second install while busy is refused
        st, _, _, js = req("POST", "/api/v1/ota", {"url": self.fw.base + "/fw/espos-0.7.0.bin"})
        self.assertEqual(st, 409)
        self.assertEqual(js["error"], "busy")
        js = self.wait_state("ready", "idle", timeout=20)
        self.assertIsNotNone(js, self.ota())
        # sim "reboots" and comes back idle
        js = self.wait_state("idle", timeout=5)
        self.assertIsNotNone(js)
        self.assertIn("reboot", "".join(self.h.log[-40:]).lower())

    def test_05_rejected_images(self):
        for path, expect in (("/fw/other.bin", "this device runs"), ("/fw/badsig.bin", "rejected"),
                             ("/fw/notimage.bin", "not a firmware image"), ("/fw/missing.bin", "HTTP 404")):
            st, _, _, js = req("POST", "/api/v1/ota", {"url": self.fw.base + path})
            self.assertEqual(st, 202, (path, js))
            js = self.wait_state("failed", timeout=15)
            self.assertIsNotNone(js, (path, self.ota()))
            self.assertIn(expect, js["last_error"], path)
        st, _, _, js = req("POST", "/api/v1/ota", {"url": "ftp://x/y"})
        self.assertEqual(st, 400)

    def test_06_manual_rollback(self):
        st, _, _, js = req("POST", "/api/v1/ota/rollback")
        self.assertEqual(st, 202)
        js = wait_for(lambda: (lambda o: o if o["running"]["rolled_back"] else None)(self.ota()), timeout=5)
        self.assertIsNotNone(js)
        self.assertEqual(js["running"]["image_state"], "invalid")


class OtaRollbackTimeoutTests(unittest.TestCase):
    """A pending image that never reaches the network is rolled back after ota.confirm_tmo_s."""

    @classmethod
    def setUpClass(cls):
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_OTA_PENDING": "1", "ESPOS_SIM_WIFI": "fail:99"})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()

    def test_01_rollback_after_timeout(self):
        st, _, _, js = req("PUT", "/api/v1/config", {"ota": {"confirm_tmo_s": 30}, "wifi": {"ssid0": "Boat", "psk0": "secret12"}})
        self.assertEqual(st, 200)
        js = req("GET", "/api/v1/ota/status")[3]
        self.assertTrue(js["running"]["pending_verify"])
        js = wait_for(lambda: (lambda o: o if o["running"]["rolled_back"] else None)(req("GET", "/api/v1/ota/status")[3]), timeout=45, step=1)
        self.assertIsNotNone(js, req("GET", "/api/v1/ota/status")[3])
        self.assertIn("rollback", js["last_error"])
        self.assertIn("rolling back", "".join(self.h.log).lower())


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


def sk_status():
    st, _, _, js = req("GET", "/api/v1/sk/status")
    assert st == 200, js
    return js


def wait_sk(pred, timeout=15.0):
    return wait_for(lambda: (lambda js: js if pred(js) else None)(sk_status()), timeout=timeout, step=0.25)


class SkTests(unittest.TestCase):
    """SignalK discovery + token flow against MockSignalK (real HTTP)."""

    @classmethod
    def setUpClass(cls):
        cls.mock = MockSignalK()
        servers = f"127.0.0.1,{cls.mock.port},{cls.mock.self_urn},mockboat"
        cls.h = Harness(fresh=True, extra_env={"ESPOS_SIM_SK_SERVERS": servers})
        # simulated WiFi must be up for discovery to fire promptly
        req("PUT", "/api/v1/config", {"wifi": {"ssid0": "Boat", "psk0": "secret12"}, "sk": {"check_s": 10}})

    @classmethod
    def tearDownClass(cls):
        cls.h.stop()
        cls.mock.stop()

    def test_01_discovery_selects_server_and_requests_access(self):
        js = wait_sk(lambda j: j["token"]["state"] == "pending", timeout=20)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["server"]["host"], "127.0.0.1")
        self.assertEqual(js["server"]["port"], self.mock.port)
        self.assertEqual(js["server"]["self"], self.mock.self_urn)
        self.assertEqual(js["server"]["source"], "discovered")
        self.assertTrue(js["token"]["pending_href"].startswith("/signalk/v1/requests/"))
        self.assertEqual(len(js["client_id"]), 36)
        self.assertEqual(js["description"], "espOS espos-1a2b")
        self.assertEqual(js["permissions"], "readwrite")
        st, _, _, srv = req("GET", "/api/v1/sk/servers")
        self.assertEqual(st, 200)
        self.assertEqual(len(srv["servers"]), 1)
        self.assertTrue(srv["servers"][0]["selected"])
        # the request the mock saw carries our identity
        posted = [b for m, p, b in self.mock.ctl("log")[1] if m == "POST"]
        self.assertEqual(posted[-1]["clientId"], js["client_id"])
        self.assertEqual(posted[-1]["permissions"], "readwrite")

    def test_02_approve_yields_token_verified(self):
        cid = sk_status()["client_id"]
        st, js = self.mock.ctl("approve", cid)
        self.assertEqual(st, 200, js)
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=15)
        self.assertIsNotNone(js, sk_status())
        self.assertTrue(js["token"]["has_token"])
        self.assertEqual(js["token"]["counts"]["approved"], 1)
        self.assertNotIn("pending_href", js["token"])
        # secrets: token never appears in the status document
        self.assertNotIn("mock.", json.dumps(js))

    def test_03_revoke_is_detected_and_re_requested(self):
        cid = sk_status()["client_id"]
        self.mock.ctl("revoke", cid)
        js = wait_sk(lambda j: j["token"]["state"] == "pending", timeout=25)   # check_s = 10
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["token"]["counts"]["unauthorized"], 1)
        self.assertFalse(js["token"]["has_token"])

    def test_04_deny_then_user_retry(self):
        cid = sk_status()["client_id"]
        self.mock.ctl("deny", cid)
        js = wait_sk(lambda j: j["token"]["state"] == "denied", timeout=70)     # poll backoff ≤ 60 s
        self.assertIsNotNone(js, sk_status())
        self.assertIn("denied", js["token"]["last_error"])
        time.sleep(1.0)
        self.assertEqual(sk_status()["token"]["state"], "denied")           # no auto retry
        st, _, _, _ = req("POST", "/api/v1/sk/request", None)
        self.assertEqual(st, 202)
        js = wait_sk(lambda j: j["token"]["state"] == "pending", timeout=10)
        self.assertIsNotNone(js)

    def test_05_server_forgets_request_404_re_request(self):
        before = sk_status()["token"]["counts"]["requests"]
        self.mock.ctl("forget")
        js = wait_sk(lambda j: j["token"]["counts"]["requests"] > before, timeout=70)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["token"]["state"], "pending")

    def test_06_manual_token_paste(self):
        cid = sk_status()["client_id"]
        st, js = self.mock.ctl("issue", cid)
        tok = js["token"]
        st, _, _, js = req("POST", "/api/v1/sk/token", {"token": tok})
        self.assertEqual(st, 202)
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=10)
        self.assertIsNotNone(js, sk_status())
        # bad requests
        st, _, _, js = req("POST", "/api/v1/sk/token", {"nope": 1})
        self.assertEqual(st, 400)
        st, _, _, js = req("POST", "/api/v1/sk/token", {"token": "x"}, content_type=None)
        self.assertEqual(st, 415)

    def test_07_forget_and_sse_events(self):
        sse = SseReader()
        hello = list(itertools.islice(sse.events(timeout=3), 4))
        kinds = [e for e, _ in hello]
        self.assertIn("sk", kinds)
        self.assertIn("sk_servers", kinds)
        self.mock.ctl("forget")   # the request left pending on the server would 400 (duplicate)
        st, _, _, _ = req("POST", "/api/v1/sk/forget", None)
        self.assertEqual(st, 202)
        seen = []
        for ev, data in sse.events(timeout=10):
            if ev == "sk":
                seen.append(json.loads(data)["token"]["state"])
                if seen[-1] == "pending":
                    break
        sse.close()
        self.assertIn("pending", seen)
        self.assertFalse(sk_status()["token"]["has_token"])

    def test_08_security_disabled_means_open(self):
        self.mock.ctl("security", "off")
        self.mock.ctl("forget")
        st, _, _, _ = req("POST", "/api/v1/sk/forget", None)   # start from scratch → POST → 404 → open
        js = wait_sk(lambda j: j["token"]["state"] == "open", timeout=10)
        self.assertIsNotNone(js, sk_status())
        self.mock.ctl("security", "on")

    def test_08b_stream_sends_deltas_and_reconciles_meta(self):
        # ensure approved with a token again (test_08 left it "open"/re-requesting)
        cid = sk_status()["client_id"]
        self.mock.ctl("forget")
        req("POST", "/api/v1/sk/request", None)
        wait_sk(lambda j: j["token"]["state"] == "pending", timeout=10)
        self.mock.ctl("approve", cid)
        js = wait_sk(lambda j: j["token"]["state"] == "approved", timeout=15)
        self.assertIsNotNone(js, sk_status())
        js = wait_sk(lambda j: j["ws"]["connected"], timeout=15)
        self.assertIsNotNone(js, sk_status())
        self.assertTrue(self.mock.ws_auth.startswith("Bearer "))
        # publish two values quickly → one delta with both; declared meta reconciled by PUT
        n0 = len(self.mock.deltas)
        st, _, _, js = req("POST", "/api/v1/sk/publish", {"path": "espos.test.a", "value": 1.5,
                                                          "meta": {"units": "V"}, "period_ms": 1000})
        self.assertEqual(st, 202, js)
        st, _, _, js = req("POST", "/api/v1/sk/publish", {"path": "espos.test.b", "value": "hello"})
        self.assertEqual(st, 202, js)
        ok = wait_for(lambda: len(self.mock.deltas) > n0 or None, timeout=5)
        self.assertTrue(ok, "no delta arrived")
        d = self.mock.deltas[n0]
        self.assertEqual(d["context"], "vessels.self")
        upd = d["updates"][0]
        self.assertEqual(upd["source"]["label"], "espos-1a2b")
        vals = {v["path"]: v["value"] for v in upd["values"]}
        self.assertEqual(vals.get("espos.test.a"), 1.5)
        self.assertEqual(vals.get("espos.test.b"), "hello")
        ok = wait_for(lambda: "espos.test.a" in self.mock.meta or None, timeout=5)
        self.assertTrue(ok, "meta not reconciled")
        self.assertEqual(self.mock.meta["espos.test.a"]["units"], "V")
        self.assertEqual(self.mock.meta["espos.test.a"]["timeout"], 2.5)
        # server-side edits win: a redeclare against existing meta does not PUT again
        self.mock.meta["espos.test.a"] = {"units": "kV"}
        puts0 = len(self.mock.meta_puts)
        req("POST", "/api/v1/sk/publish", {"path": "espos.test.a", "value": 2, "meta": {"units": "V"}})
        time.sleep(1.5)
        self.assertEqual(len(self.mock.meta_puts), puts0)
        self.assertEqual(self.mock.meta["espos.test.a"]["units"], "kV")
        # health values arrive under espos.<label>.*
        js = wait_for(lambda: any(v["path"].endswith(".uptime") for d in self.mock.deltas for v in d["updates"][0]["values"]) or None, timeout=15)
        self.assertTrue(js, "no health delta")

    def test_08c_offline_buffer_drains_in_order_after_reconnect(self):
        js = wait_sk(lambda j: j["ws"]["connected"], timeout=15)
        self.assertIsNotNone(js)
        self.mock.ws_accept = False          # server drops the stream and refuses upgrades
        js = wait_sk(lambda j: not j["ws"]["connected"], timeout=15)
        self.assertIsNotNone(js, sk_status())
        n0 = len(self.mock.deltas)
        for i in range(10):
            req("POST", "/api/v1/sk/publish", {"path": "espos.test.seq", "value": i})
            time.sleep(0.4)                  # well beyond the batch window: one message each
        js = wait_sk(lambda j: j["ws"]["buffered"] >= 10, timeout=5)
        self.assertIsNotNone(js, sk_status())
        self.assertFalse(js["ws"]["connected"])
        self.mock.ws_accept = True
        js = wait_sk(lambda j: j["ws"]["connected"] and j["ws"]["buffered"] == 0, timeout=70)
        self.assertIsNotNone(js, sk_status())
        seq = [v["value"] for d in self.mock.deltas[n0:] for v in d["updates"][0]["values"] if v["path"] == "espos.test.seq"]
        self.assertEqual(seq, list(range(10)))     # complete and in order

    def test_09_manual_host_config_wins_over_discovery(self):
        st, _, _, _ = req("PUT", "/api/v1/config", {"sk": {"server_host": "127.0.0.1", "server_port": self.mock.port}})
        self.assertEqual(st, 200)
        js = wait_sk(lambda j: j["server"].get("source") == "manual", timeout=10)
        self.assertIsNotNone(js, sk_status())
        self.assertEqual(js["server"]["host"], "127.0.0.1")
        req("PUT", "/api/v1/config", {"sk": {"server_host": None, "server_port": None}})
        js = wait_sk(lambda j: j["server"].get("source") == "discovered", timeout=10)
        self.assertIsNotNone(js, sk_status())


if __name__ == "__main__":
    if not os.path.exists(ELF):
        print(f"missing {ELF}; build first (idf.py --preview set-target linux && idf.py build)")
        sys.exit(2)
    unittest.main(verbosity=2)
