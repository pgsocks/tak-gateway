"""Exercise container bootstrap argument handling without Docker or secrets."""

import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
ENTRYPOINT = ROOT / "docker" / "entrypoint.sh"

FAKE_GATEWAY = r'''#!/usr/bin/env python3
import json, os, pathlib, signal, sys, time
args = sys.argv[1:]
state = os.environ.get("TAK_GATEWAY_STATE_DIR", "/var/lib/tak-gateway")
if "--state-dir" in args:
    i = args.index("--state-dir")
    if i + 1 < len(args):
        state = args[i + 1]
with open(os.environ["ENTRYPOINT_LOG"], "a") as output:
    output.write(json.dumps({"args": args, "pid": os.getpid(),
                            "state": state}) + "\n")
if args and args[0] == "config" and "--if-uninitialized" in args:
    current = pathlib.Path(state) / "CURRENT"
    if not current.exists():
        code = int(os.environ.get("ENTRYPOINT_INIT_EXIT", "0"))
        if code:
            sys.exit(code)
        current.parent.mkdir(parents=True, exist_ok=True)
        current.write_text("first configuration")
if args and args[0] == "run" and os.environ.get("ENTRYPOINT_WAIT"):
    def stop(number, frame):
        pathlib.Path(os.environ["ENTRYPOINT_SIGNAL"]).write_text(str(number))
        sys.exit(128 + number)
    signal.signal(signal.SIGTERM, stop)
    pathlib.Path(os.environ["ENTRYPOINT_READY"]).write_text("ready")
    while True:
        time.sleep(0.01)
'''


class EntrypointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        binary = self.bin / "tak-gateway"
        binary.write_text(FAKE_GATEWAY)
        binary.chmod(0o700)
        self.log = self.root / "arguments.jsonl"
        self.state = self.root / "state"
        self.environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith("TAK_GATEWAY_")
        }
        self.environment.update({
            "PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
            "ENTRYPOINT_LOG": str(self.log),
            "TAK_GATEWAY_STATE_DIR": str(self.state),
        })

    def invoke(self, args=(), **variables):
        environment = dict(self.environment, **variables)
        return subprocess.run(["sh", str(ENTRYPOINT), *args],
                              env=environment, capture_output=True, text=True,
                              timeout=5)

    def calls(self):
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_default_run_initializes_management_and_local_output(self):
        for args in [(), ("run",)]:
            with self.subTest(args=args):
                self.log.unlink(missing_ok=True)
                state = self.root / f"state-{len(args)}"
                result = self.invoke(args, TAK_GATEWAY_STATE_DIR=str(state))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([x["args"] for x in self.calls()], [
                    ["config", "--if-uninitialized", "--state-dir", str(state),
                     "--output", "local=tcp://0.0.0.0:9000",
                     "--web-bind", "0.0.0.0:8080"], ["run"]])
                self.assertTrue((state / "CURRENT").exists())

    def test_explicit_environment_replaces_container_defaults(self):
        result = self.invoke(("run",),
                             TAK_GATEWAY_OUTPUTS="monitor=udp://127.0.0.1:9001",
                             TAK_GATEWAY_WEB_BIND="127.0.0.1:18080")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0]["args"], [
            "config", "--if-uninitialized", "--state-dir", str(self.state),
            "--output", "monitor=udp://127.0.0.1:9001",
            "--web-bind", "127.0.0.1:18080"])

    def test_empty_outputs_keeps_default_management_without_output(self):
        result = self.invoke(("run",), TAK_GATEWAY_OUTPUTS="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0]["args"], [
            "config", "--if-uninitialized", "--state-dir", str(self.state),
            "--web-bind", "0.0.0.0:8080"])
        self.assertTrue((self.state / "CURRENT").exists())

    def test_empty_web_bind_keeps_default_output_without_management(self):
        result = self.invoke(("run",), TAK_GATEWAY_WEB_BIND="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0]["args"], [
            "config", "--if-uninitialized", "--state-dir", str(self.state),
            "--output", "local=tcp://0.0.0.0:9000"])
        self.assertTrue((self.state / "CURRENT").exists())

    def test_explicitly_empty_defaults_do_not_seed_an_empty_store(self):
        result = self.invoke(("run",), TAK_GATEWAY_OUTPUTS="",
                             TAK_GATEWAY_WEB_BIND="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([x["args"] for x in self.calls()], [["run"]])
        self.assertFalse(self.state.exists())

    def test_initial_environment_is_one_offline_configuration(self):
        result = self.invoke(
            ("run",), TAK_GATEWAY_CONFIG_FILE="/config/base file.toml",
            TAK_GATEWAY_IMPORT_ZIP="/credentials/config package.zip",
            TAK_GATEWAY_OUTPUTS="local=tcp://0.0.0.0:9000\n"
                                "monitor=udp://127.0.0.1:9001",
            TAK_GATEWAY_WEB_BIND="0.0.0.0:8080",
            TAK_GATEWAY_STATUS_FILE="/run/tak-gateway/status.json",
            TAK_GATEWAY_LOG_LEVEL="debug", TAK_GATEWAY_LOG_FORMAT="json",
            TAK_GATEWAY_SET='limits.queue_records=64\nprocess.log_level="info"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0]["args"], [
            "config", "--if-uninitialized", "--state-dir", str(self.state),
            "--config", "/config/base file.toml", "--import-zip",
            "/credentials/config package.zip", "--output",
            "local=tcp://0.0.0.0:9000", "--output",
            "monitor=udp://127.0.0.1:9001", "--web-bind", "0.0.0.0:8080",
            "--status-file", "/run/tak-gateway/status.json", "--log-level",
            "debug", "--log-format", "json", "--set", "limits.queue_records=64",
            "--set", 'process.log_level="info"'])
        self.assertEqual(self.calls()[1]["args"], ["run"])

    def test_empty_state_environment_uses_container_default(self):
        result = self.invoke(("run",), TAK_GATEWAY_STATE_DIR="",
                             TAK_GATEWAY_OUTPUTS="", TAK_GATEWAY_WEB_BIND="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0]["state"], "/var/lib/tak-gateway")

    def test_existing_store_keeps_configuration(self):
        self.state.mkdir()
        current = self.state / "CURRENT"
        current.write_text("GUI configuration")
        result = self.invoke(("run",))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(current.read_text(), "GUI configuration")

    def test_administrative_commands_bypass_initial_configuration(self):
        for command in [("config", "--log-level", "debug"), ("validate",),
                        ("redact",), ("--help",)]:
            with self.subTest(command=command):
                self.log.unlink(missing_ok=True)
                result = self.invoke(command,
                                     TAK_GATEWAY_WEB_BIND="0.0.0.0:8080")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([x["args"] for x in self.calls()],
                                 [list(command)])

    def test_run_state_directory_selects_bootstrap_store(self):
        selected = self.root / "selected store"
        result = self.invoke(("run", "--state-dir", str(selected)),
                             TAK_GATEWAY_WEB_BIND="127.0.0.1:8080")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls()[0]["state"], str(selected))
        self.assertEqual(self.calls()[1]["state"], str(selected))
        self.assertTrue((selected / "CURRENT").exists())
        self.assertFalse(self.state.exists())

    def test_help_or_invalid_run_arguments_do_not_seed(self):
        for args in [("run", "--help"), ("run", "--state-dir"),
                     ("run", "--unknown", "value"),
                     ("run", "--state-dir", ""),
                     ("run", "--state-dir", "first", "--state-dir", "second")]:
            with self.subTest(args=args):
                self.log.unlink(missing_ok=True)
                result = self.invoke(args,
                                     TAK_GATEWAY_WEB_BIND="127.0.0.1:8080")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual([x["args"] for x in self.calls()], [list(args)])
                self.assertFalse(self.state.exists())

    def test_failed_initialization_does_not_start_gateway(self):
        result = self.invoke(("run",), ENTRYPOINT_INIT_EXIT="7")
        self.assertEqual(result.returncode, 7)
        self.assertEqual(len(self.calls()), 1)
        self.assertEqual(self.calls()[0]["args"][0], "config")

    def test_environment_values_are_literal_not_shell_code(self):
        marker = self.root / "must not exist"
        value = 'process.status_file="$(touch ' + str(marker) + ') `id`; *"'
        result = self.invoke(("run",), TAK_GATEWAY_SET=value,
                             TAK_GATEWAY_CONFIG_FILE="file name ' quotes.toml")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(value, self.calls()[0]["args"])
        self.assertIn("file name ' quotes.toml", self.calls()[0]["args"])
        self.assertFalse(marker.exists())
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")

    def test_empty_list_lines_do_not_create_empty_arguments(self):
        result = self.invoke(("run",), TAK_GATEWAY_OUTPUTS="\nlocal=tcp://127.0.0.1:9000\n\n",
                             TAK_GATEWAY_SET="\nlimits.queue_records=4\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("", self.calls()[0]["args"])
        self.assertEqual(self.calls()[0]["args"].count("--output"), 1)

    def test_exec_preserves_pid_and_delivers_sigterm(self):
        ready = self.root / "ready"
        received = self.root / "signal"
        environment = dict(self.environment, ENTRYPOINT_WAIT="1",
                           ENTRYPOINT_READY=str(ready),
                           ENTRYPOINT_SIGNAL=str(received))
        process = subprocess.Popen(["sh", str(ENTRYPOINT), "run"],
                                   env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        try:
            limit = time.monotonic() + 5
            while not ready.exists() and process.poll() is None:
                if time.monotonic() >= limit:
                    self.fail("gateway did not become ready")
                time.sleep(0.01)
            self.assertTrue(ready.exists())
            self.assertEqual(self.calls()[-1]["pid"], process.pid)
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 128 + signal.SIGTERM)
            self.assertEqual(received.read_text(), str(signal.SIGTERM))
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
            else:
                process.communicate()


if __name__ == "__main__":
    unittest.main()
