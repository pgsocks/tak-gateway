#!/usr/bin/env python3
"""Browser contract checks; --gateway also exercises the real web controller.

Requires Python Playwright and Chromium. All certificates and ZIPs are generated
inside a temporary directory. No real server credentials are used.
"""

import argparse
import copy
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.request
import zipfile

from playwright.sync_api import sync_playwright


ROOT = Path(__file__).resolve().parents[2]
OPTIONS, UNITTEST_ARGS = argparse.ArgumentParser(add_help=False), None
OPTIONS.add_argument("--gateway", type=Path)
OPTIONS.add_argument("--chromium", default=shutil.which("chromium"))
OPTIONS, UNITTEST_ARGS = OPTIONS.parse_known_args()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def initial_state():
    return {
        "revision": 1,
        "token": "browser-test-token",
        "config": {"inputs": [], "outputs": [], "process": {}, "limits": {}},
        "status": {"state": "RUNNING", "inputs": [], "outputs": []},
        "jobs": [],
    }


class BrowserApi(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, data):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/state":
            if self.server.offline:
                return self.reply(503, {"error": {"message": "Offline"}})
            return self.reply(200, self.server.data)
        if self.path.startswith("/api/jobs/"):
            return self.reply(200, {"state": "ready", "preview": {
                "inputs": [{"id": "imported", "name": "Imported TAK",
                            "kind": "tls", "host": "127.0.0.1",
                            "port": 28089, "uid": "browser-import",
                            "protocol": "xml-only", "enabled": True,
                            "identity_mode": "pem", "tls": {"has_ca": True,
                            "has_certificate": True, "has_key": True}}]}})
        assets = {"/": ("index.html", "text/html"),
                  "/app.js": ("app.js", "application/javascript"),
                  "/styles.css": ("styles.css", "text/css")}
        if self.path in assets:
            filename, content_type = assets[self.path]
            body = (ROOT / "src/web" / filename).read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            return self.wfile.write(body)
        self.reply(404, {})

    def mutation(self, method):
        if self.headers.get("X-TG-Token") != self.server.data["token"]:
            return self.reply(403, {"error": {"message": "Missing token"}})
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/api/assets"):
            self.server.uploads.append(body)
            return self.reply(200, {"asset_id": "test-asset"})
        if self.path == "/api/import":
            self.server.uploads.append(body)
            return self.reply(202, {"job_id": "test-import"})
        if self.path.startswith("/api/jobs/") and method == "DELETE":
            self.server.cancelled.append(self.path.rsplit("/", 1)[-1])
            return self.reply(200, {"state": "cancelled"})
        data = json.loads(body or b"{}")
        if self.server.conflict or data.get("revision") != self.server.data["revision"]:
            return self.reply(409, {"error": {"message": "Revision conflict"}})
        if self.path == "/api/import/test-import/confirm":
            self.server.data["config"]["inputs"].extend(data["inputs"])
        else:
            _, _, plural, *identifier = self.path.split("/")
            singular = "input" if plural == "inputs" else "output"
            collection = self.server.data["config"][plural]
            endpoint_id = identifier[0] if identifier else data.get("id") or data[singular]["id"]
            old = next((item for item in collection if item["id"] == endpoint_id), None)
            if method == "DELETE":
                collection.remove(old)
            elif old:
                old.update(data[singular])
            else:
                collection.append(data[singular])
            self.server.requests.append((method, plural, copy.deepcopy(data)))
        self.server.data["revision"] += 1
        self.reply(200, {"revision": self.server.data["revision"]})

    def do_POST(self):
        self.mutation("POST")

    def do_PUT(self):
        self.mutation("PUT")

    def do_DELETE(self):
        self.mutation("DELETE")


class BrowserContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.playwright = sync_playwright().start()
        cls.browser = cls.playwright.chromium.launch(
            executable_path=OPTIONS.chromium, headless=True,
            args=["--no-sandbox"])

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()

    def setUp(self):
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), BrowserApi)
        self.server.data, self.server.requests, self.server.uploads = initial_state(), [], []
        self.server.cancelled = []
        self.server.conflict = self.server.offline = False
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.page = self.browser.new_page()
        self.page.set_default_timeout(5000)
        self.errors = []
        self.page.on("pageerror", lambda error: self.errors.append(str(error)))
        self.url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.page.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.assertEqual(self.errors, [])

    def load(self):
        self.page.goto(self.url)
        self.page.wait_for_function("document.querySelector('#revision').textContent.includes('1')")

    def add_output(self, name="Local consumer"):
        self.page.get_by_role("tab", name="Outputs", exact=True).click()
        self.page.get_by_role("button", name="Add output", exact=True).click()
        self.page.get_by_label("Display name", exact=True).fill(name)
        self.page.get_by_label("Port", exact=True).fill("19000")

    def test_first_setup_and_output_kind_fields(self):
        self.load()
        self.assertTrue(self.page.get_by_role("heading", name="No TAK connections yet").is_visible())
        self.add_output()
        self.page.get_by_label("Output type", exact=True).select_option("udp")
        self.page.get_by_label("Destination host", exact=True).fill("127.0.0.1")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.wait_for_selector("#output-rows tr")
        payload = self.server.requests[-1][2]["output"]
        self.assertEqual(payload["kind"], "udp")
        self.assertEqual(payload["port"], 19000)
        self.assertNotIn("bind", payload)
        self.assertNotIn("framing", payload)
        self.assertNotIn("max_clients", payload)
        self.assertTrue(self.page.get_by_text("Local consumer", exact=True).is_visible())

    def test_connection_form_upload_and_validation(self):
        self.load()
        self.page.get_by_role("button", name="Add connection", exact=True).click()
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.assertEqual(self.server.requests, [])
        self.page.get_by_label("Display name", exact=True).fill("Manual TAK")
        self.page.get_by_label("Server hostname or IP", exact=True).fill("127.0.0.1")
        self.page.get_by_label("CA trust certificate (PEM)", exact=True).set_input_files(
            {"name": "ca.pem", "mimeType": "application/x-pem-file",
             "buffer": b"test-only-certificate-upload"})
        self.page.get_by_label("Authentication", exact=True).select_option("password")
        self.page.get_by_label("Username", exact=True).fill("test-user")
        self.page.get_by_label("New streaming password", exact=True).fill("test-only-password")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.wait_for_selector("#connection-rows tr")
        payload = self.server.requests[-1][2]["input"]
        self.assertEqual(payload["tls"]["ca_asset"], "test-asset")
        self.assertEqual(payload["auth"]["password"], "test-only-password")
        self.assertEqual(self.server.uploads, [b"test-only-certificate-upload"])

    def test_conflict_keeps_form_and_does_not_silently_overwrite(self):
        self.load()
        self.add_output("Unsaved output")
        self.server.conflict = True
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.get_by_role("alert").wait_for()
        self.assertEqual(self.page.get_by_label("Display name", exact=True).input_value(), "Unsaved output")
        self.assertIn("another window", self.page.get_by_role("alert").inner_text())
        self.assertEqual(self.server.data["config"]["outputs"], [])

    def test_ambiguous_authentication_safe_rendering_and_offline_status(self):
        name = '<img src=x onerror="alert(1)">'
        self.server.data["config"]["inputs"] = [{"id": "source", "name": name,
            "kind": "tls", "host": "127.0.0.1", "port": 28089}]
        self.server.data["status"]["inputs"] = [{"id": "source", "state": "BACKOFF",
            "credentials_sent": True, "reconnects": 1,
            "diagnostic": {"category": "transport", "stage": "read",
                "message": "Server closed after credentials; reason not provided"}}]
        self.load()
        self.assertTrue(self.page.locator("#connection-rows").get_by_text(name, exact=True).is_visible())
        self.assertEqual(self.page.locator("#connection-rows img").count(), 0)
        self.assertTrue(self.page.get_by_text("Connection failed", exact=True).is_visible())
        self.assertFalse(self.page.get_by_text("Authentication rejected", exact=True).is_visible())
        self.page.get_by_role("button", name="Details", exact=True).click()
        self.assertIn("not proof of authentication", self.page.locator("#details-dialog").inner_text())
        self.page.get_by_role("button", name="Close details", exact=True).click()
        self.server.offline = True
        self.page.get_by_text("Gateway unavailable", exact=True).wait_for()
        self.assertTrue(self.page.locator("#connection-rows").get_by_text(name, exact=True).is_visible())

    def test_explicit_certificate_rejection_is_distinct_from_server_verification(self):
        self.server.data["config"]["inputs"] = [{"id": "source", "name": "Certificate TAK",
            "kind": "tls", "host": "127.0.0.1", "port": 28089}]
        self.server.data["status"]["inputs"] = [{"id": "source", "state": "BACKOFF",
            "diagnostic": {"code": "tls-client-certificate-rejected", "stage": "tls-read",
                "message": "Server rejected the client certificate", "tls_alert": 42,
                "native_domain": "openssl", "native_code": 1}}]
        self.load()
        self.page.get_by_text("Authentication rejected", exact=True).wait_for()
        self.page.get_by_role("button", name="Details", exact=True).click()
        self.assertIn("tls-client-certificate-rejected", self.page.locator("#details-dialog").inner_text())
        self.page.get_by_role("button", name="Close details", exact=True).click()
        self.server.data["status"]["inputs"][0]["diagnostic"] = {
            "code": "tls-server-certificate-untrusted", "message": "Server certificate is not trusted"}
        self.page.get_by_text("TLS verification failed", exact=True).wait_for()

    def test_import_preview_is_editable_before_confirmation(self):
        self.load()
        self.page.get_by_role("button", name="Import ZIP", exact=True).click()
        self.page.locator("#zip-file").set_input_files({"name": "test-package.zip",
            "mimeType": "application/zip", "buffer": b"PK-test-only-upload"})
        field = self.page.get_by_label("Display name for imported connection 1", exact=True)
        field.wait_for()
        field.fill("Reviewed import")
        self.assertEqual(self.server.data["config"]["inputs"], [])
        self.page.get_by_role("button", name="Import connections", exact=True).click()
        self.page.get_by_text("Reviewed import", exact=True).wait_for()

    def test_cancelled_import_discards_ready_preview(self):
        self.load()
        self.page.get_by_role("button", name="Import ZIP", exact=True).click()
        self.page.locator("#zip-file").set_input_files({"name": "test-package.zip",
            "mimeType": "application/zip", "buffer": b"PK-test-only-upload"})
        self.page.get_by_label("Display name for imported connection 1", exact=True).wait_for()
        with self.page.expect_response(lambda response:
                response.url.endswith("/api/jobs/test-import") and response.request.method == "DELETE"):
            self.page.locator("#import-dialog").get_by_role("button", name="Cancel", exact=True).click()
        self.assertEqual(self.server.cancelled, ["test-import"])
        self.assertEqual(self.server.data["config"]["inputs"], [])

    def test_mobile_layout_stays_within_viewport(self):
        self.page.set_viewport_size({"width": 390, "height": 844})
        self.load()
        self.assertLessEqual(self.page.evaluate("document.documentElement.scrollWidth"), 390)
        self.page.get_by_role("button", name="Add connection", exact=True).click()
        self.assertTrue(self.page.get_by_label("Server hostname or IP", exact=True).is_visible())
        self.assertLessEqual(self.page.evaluate("document.documentElement.scrollWidth"), 390)

    def test_blank_optional_fields_restore_defaults(self):
        self.server.data["config"]["inputs"] = [{"id": "source", "name": "Old label",
            "kind": "tls", "host": "old.example.invalid", "port": 28089,
            "uid": "old-uid", "callsign": "old-uid", "protocol": "xml-only",
            "tls": {"server_name": "old.example.invalid", "has_ca": True},
            "maintenance": {"ping_interval_ms": 30000, "ping_timeout_ms": 10000}}]
        self.load()
        self.page.get_by_role("button", name="Edit", exact=True).click()
        self.page.get_by_label("Display name", exact=True).fill("")
        self.page.get_by_label("Server hostname or IP", exact=True).fill("new.example.invalid")
        self.page.get_by_text("Advanced settings", exact=True).click()
        self.page.get_by_label("Ping interval (ms)", exact=True).fill("")
        self.page.get_by_label("Ping timeout (ms)", exact=True).fill("")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.wait_for_function("!document.querySelector('#editor-dialog').open")
        payload = self.server.requests[-1][2]["input"]
        self.assertIsNone(payload["name"])
        self.assertIsNone(payload["callsign"])
        self.assertIsNone(payload["tls"]["server_name"])
        self.assertEqual(payload["maintenance"], {"ping_interval_ms": None, "ping_timeout_ms": None})

    def test_saved_but_not_applied_banner_preserves_open_form(self):
        self.load()
        self.add_output("Unsaved form values")
        self.server.data["revision"] = 2
        self.server.data["active_revision"] = 1
        self.server.data["apply_error"] = {"message": "Output bind failed"}
        self.page.get_by_text("Saved revision 2; active revision 1. Output bind failed", exact=True).wait_for()
        self.assertEqual(self.page.get_by_label("Display name", exact=True).input_value(), "Unsaved form values")

    def test_imported_hostname_verification_can_be_preserved_and_enabled(self):
        self.server.data["config"]["inputs"] = [{"id": "source", "name": "Imported TAK",
            "kind": "tls", "host": "tak.example.invalid", "port": 28089,
            "uid": "imported", "protocol": "xml-only", "tls": {
                "has_ca": True, "verify_hostname": False}}]
        self.load()
        self.page.get_by_role("button", name="Edit", exact=True).click()
        verification = self.page.get_by_label("Verify server hostname", exact=True)
        self.assertFalse(verification.is_checked())
        self.page.get_by_label("Display name", exact=True).fill("Renamed TAK")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.wait_for_function("!document.querySelector('#editor-dialog').open")
        self.assertIs(self.server.requests[-1][2]["input"]["tls"]["verify_hostname"], False)
        self.page.get_by_role("button", name="Edit", exact=True).click()
        self.page.get_by_label("Verify server hostname", exact=True).check()
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.wait_for_function("!document.querySelector('#editor-dialog').open")
        self.assertIs(self.server.requests[-1][2]["input"]["tls"]["verify_hostname"], True)

    def test_new_connections_require_hostname_verification_by_default(self):
        self.load()
        self.page.get_by_role("button", name="Add connection", exact=True).click()
        self.assertTrue(self.page.get_by_label("Verify server hostname", exact=True).is_checked())

    def test_existing_utf8_identifier_edit_toggle_and_remove(self):
        identifier = "west/a .é"
        self.server.data["config"]["outputs"] = [{"id": identifier, "name": "Old output",
            "kind": "udp", "host": "127.0.0.1", "port": 19001, "enabled": True}]
        self.load()
        self.page.get_by_role("tab", name="Outputs", exact=True).click()
        self.page.get_by_role("button", name="Edit", exact=True).click()
        self.page.get_by_label("Display name", exact=True).fill("Edited output")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.get_by_text("Edited output", exact=True).wait_for()
        self.assertEqual(self.server.requests[-1][2]["id"], identifier)
        self.page.get_by_role("button", name="Disable", exact=True).click()
        self.page.get_by_text("Disabled", exact=True).wait_for()
        self.assertEqual(self.server.requests[-1][2]["id"], identifier)
        self.page.get_by_role("button", name="Remove", exact=True).click()
        self.page.locator("#confirm-remove").click()
        self.page.get_by_role("heading", name="No outputs configured").wait_for()
        self.assertEqual(self.server.requests[-1][2]["id"], identifier)

    def test_pipe_edit_preserves_explicit_creation_setting(self):
        self.server.data["config"]["outputs"] = [{"id": "pipe", "name": "Pipe output",
            "kind": "pipe", "path": "/tmp/tak-browser-pipe", "permissions": "0600",
            "create": False, "enabled": True}]
        self.load()
        self.page.get_by_role("tab", name="Outputs", exact=True).click()
        self.page.get_by_role("button", name="Edit", exact=True).click()
        self.assertFalse(self.page.get_by_label("Create pipe if missing", exact=True).is_checked())
        self.page.get_by_label("Display name", exact=True).fill("Edited pipe")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.get_by_text("Edited pipe", exact=True).wait_for()
        self.assertFalse(self.server.requests[-1][2]["output"]["create"])


def generate_package(directory, port):
    environment = dict(os.environ, TG_BROWSER_PASSWORD="test-only-bundle-password")

    def openssl(*args):
        subprocess.run(["openssl", *args], cwd=directory, env=environment,
                       check=True, capture_output=True)

    openssl("req", "-x509", "-newkey", "ec", "-pkeyopt",
            "ec_paramgen_curve:P-256", "-noenc", "-days", "1",
            "-subj", "/CN=Browser test CA", "-keyout", "ca.key", "-out", "ca.pem",
            "-addext", "basicConstraints=critical,CA:TRUE")
    openssl("req", "-new", "-newkey", "ec", "-pkeyopt",
            "ec_paramgen_curve:P-256", "-noenc", "-subj", "/CN=Browser test client",
            "-keyout", "client.key", "-out", "client.csr")
    openssl("x509", "-req", "-in", "client.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
            "-CAcreateserial", "-days", "1", "-out", "client.pem")
    openssl("pkcs12", "-export", "-legacy", "-nokeys", "-in", "ca.pem", "-out", "trust.p12",
            "-passout", "env:TG_BROWSER_PASSWORD")
    openssl("pkcs12", "-export", "-legacy", "-in", "client.pem", "-inkey", "client.key",
            "-certfile", "ca.pem", "-out", "client.p12", "-passout",
            "env:TG_BROWSER_PASSWORD")
    manifest = """<MissionPackageManifest version="2"><Configuration/>
<Contents><Content ignore="false" zipEntry="secure.pref"/>
<Content ignore="false" zipEntry="trust.p12"/>
<Content ignore="false" zipEntry="client.p12"/></Contents>
</MissionPackageManifest>"""
    pref = f"""<preferences><preference name="cot_streams" version="1">
<entry key="count">1</entry><entry key="enabled0">true</entry>
<entry key="connectString0">127.0.0.1:{port}:ssl</entry>
<entry key="caLocation0">trust.p12</entry>
<entry key="caPassword0">test-only-bundle-password</entry>
<entry key="certificateLocation0">client.p12</entry>
<entry key="clientPassword0">test-only-bundle-password</entry>
</preference></preferences>"""
    archive = directory / "browser-package.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        output.writestr("package/MANIFEST/manifest.xml", manifest)
        output.writestr("package/secure.pref", pref)
        output.write(directory / "trust.p12", "package/trust.p12")
        output.write(directory / "client.p12", "package/client.p12")
    return archive


@unittest.skipUnless(OPTIONS.gateway, "supply --gateway for real-controller checks")
class LiveGateway(BrowserContract):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="tak-web-browser-")
        self.root = Path(self.directory.name)
        self.state_dir = self.root / "state"
        self.web_port, self.output_port, self.tak_port = free_port(), free_port(), free_port()
        self.archive = generate_package(self.root, self.tak_port)
        self.binary = str(OPTIONS.gateway.resolve())
        subprocess.run([self.binary, "config", "--state-dir", str(self.state_dir),
                        "--web-bind", f"127.0.0.1:{self.web_port}", "--output",
                        f"local=tcp://127.0.0.1:{self.output_port}"],
                       check=True, capture_output=True)
        self.process = None
        self.start_gateway()
        self.page = self.browser.new_page()
        self.page.set_default_timeout(10000)
        self.errors = []
        self.job_errors = []
        def inspect_response(response):
            if "/api/jobs/" in response.url:
                try:
                    payload = response.json()
                    if payload.get("state") == "failed":
                        self.job_errors.append(payload.get("error"))
                except Exception:
                    pass
        self.page.on("response", inspect_response)
        self.page.on("pageerror", lambda error: self.errors.append(str(error)))
        self.url = f"http://127.0.0.1:{self.web_port}"

    def start_gateway(self):
        self.process = subprocess.Popen([self.binary, "run", "--state-dir", str(self.state_dir)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                opener.open(f"http://127.0.0.1:{self.web_port}/api/state", timeout=.5).close()
                return
            except OSError:
                if self.process.poll() is not None:
                    self.fail(self.process.communicate()[1].decode())
                time.sleep(.05)
        self.fail("Gateway HTTP listener did not start")

    def stop_gateway(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.communicate(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.communicate()

    def wait_saved(self, name):
        self.page.wait_for_function("""name =>
            document.querySelector('#connection-rows').textContent.includes(name)
            || !document.querySelector('#form-error').hidden""", arg=name)
        self.assertTrue(self.page.locator("#form-error").is_hidden(),
                        (self.page.locator("#form-error").inner_text(), self.job_errors))
        self.page.locator("#connection-rows").get_by_text(name, exact=True).wait_for()

    def tearDown(self):
        self.page.close()
        self.stop_gateway()
        self.directory.cleanup()
        self.assertEqual(self.errors, [])

    # Mock-contract cases intentionally use the mock class only.
    test_first_setup_and_output_kind_fields = None
    test_connection_form_upload_and_validation = None
    test_conflict_keeps_form_and_does_not_silently_overwrite = None
    test_ambiguous_authentication_safe_rendering_and_offline_status = None
    test_import_preview_is_editable_before_confirmation = None
    test_cancelled_import_discards_ready_preview = None
    test_mobile_layout_stays_within_viewport = None
    test_explicit_certificate_rejection_is_distinct_from_server_verification = None
    test_blank_optional_fields_restore_defaults = None
    test_saved_but_not_applied_banner_preserves_open_form = None
    test_existing_utf8_identifier_edit_toggle_and_remove = None
    test_pipe_edit_preserves_explicit_creation_setting = None

    def test_imported_hostname_verification_can_be_preserved_and_enabled(self):
        self.page.goto(self.url)
        self.page.get_by_role("heading", name="No TAK connections yet").wait_for()
        self.page.get_by_role("button", name="Import ZIP", exact=True).click()
        self.page.locator("#zip-file").set_input_files(self.archive)
        self.page.get_by_label("Display name for imported connection 1", exact=True).fill("Policy TAK")
        self.page.get_by_role("button", name="Import connections", exact=True).click()
        self.page.get_by_text("Policy TAK", exact=True).wait_for()
        self.page.get_by_role("button", name="Edit", exact=True).click()
        self.assertFalse(self.page.get_by_label("Verify server hostname", exact=True).is_checked())
        self.page.get_by_label("Display name", exact=True).fill("Saved policy TAK")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.wait_saved("Saved policy TAK")
        self.stop_gateway()
        self.start_gateway()
        self.page.reload()
        self.page.get_by_text("Saved policy TAK", exact=True).wait_for()
        self.page.get_by_role("button", name="Edit", exact=True).click()
        verification = self.page.get_by_label("Verify server hostname", exact=True)
        self.assertFalse(verification.is_checked())
        verification.check()
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.locator("#editor-dialog").wait_for(state="hidden")
        saved = self.page.request.get(self.url + "/api/state").json()
        self.assertIs(saved["config"]["inputs"][0]["tls"]["verify_hostname"], True)

    def test_real_upload_manual_edit_output_and_restart_persistence(self):
        self.page.goto(self.url)
        self.page.get_by_role("heading", name="No TAK connections yet").wait_for()
        self.page.get_by_role("button", name="Add connection", exact=True).click()
        self.page.get_by_label("Display name", exact=True).fill("Browser manual TAK")
        self.page.get_by_label("Server hostname or IP", exact=True).fill("127.0.0.1")
        self.page.get_by_label("Streaming port", exact=True).fill(str(self.tak_port))
        self.page.get_by_label("CA trust certificate (PEM)", exact=True).set_input_files(self.root / "ca.pem")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.wait_saved("Browser manual TAK")
        row = self.page.locator("#connection-rows tr").filter(has_text="Browser manual TAK")
        row.get_by_role("button", name="Edit", exact=True).click()
        self.page.get_by_label("Display name", exact=True).fill("Edited browser TAK")
        self.page.get_by_label("Client identity", exact=True).select_option("pkcs12")
        self.page.get_by_label("PKCS#12 bundle", exact=True).set_input_files(self.root / "client.p12")
        self.page.get_by_label("New bundle password", exact=True).fill("test-only-bundle-password")
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.wait_saved("Edited browser TAK")
        state_json = self.page.request.get(self.url + "/api/state").text()
        self.assertNotIn("test-only-bundle-password", state_json)
        self.assertNotIn("PRIVATE KEY", state_json)
        self.page.get_by_role("button", name="Import ZIP", exact=True).click()
        self.page.locator("#zip-file").set_input_files(self.archive)
        self.page.get_by_label("Display name for imported connection 1", exact=True).fill("Browser imported TAK")
        self.page.get_by_role("button", name="Import connections", exact=True).click()
        self.page.get_by_text("Browser imported TAK", exact=True).wait_for()
        self.add_output("Browser UDP")
        self.page.get_by_label("Output type", exact=True).select_option("udp")
        self.page.get_by_label("Port", exact=True).fill(str(free_port()))
        self.page.get_by_role("button", name="Save and apply", exact=True).click()
        self.page.get_by_text("Browser UDP", exact=True).wait_for()
        self.stop_gateway()
        self.start_gateway()
        self.page.reload()
        self.page.get_by_text("Edited browser TAK", exact=True).wait_for()
        self.page.get_by_text("Browser imported TAK", exact=True).wait_for()
        self.page.get_by_role("tab", name="Outputs", exact=True).click()
        self.page.get_by_text("Browser UDP", exact=True).wait_for()


if __name__ == "__main__":
    unittest.main(argv=[__file__, *UNITTEST_ARGS])
