"""Executable persistence and offline configuration workflows."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import tomllib
import unittest


BINARY = os.environ.get("TAK_GATEWAY_BINARY", "build/src/tak-gateway")


class Commands(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="tak-cli-")
        self.state = Path(self.directory.name) / "state"

    def tearDown(self):
        self.directory.cleanup()

    def command(self, command, *arguments):
        return subprocess.run(
            [BINARY, command, "--state-dir", str(self.state), *arguments],
            capture_output=True, text=True, timeout=15)

    def config(self, *arguments):
        result = self.command("config", *arguments)
        self.assertEqual(result.returncode, 0, result.stderr)

    def snapshot(self):
        result = self.command("redact")
        self.assertEqual(result.returncode, 0, result.stderr)
        return tomllib.loads(result.stdout)

    def test_config_persists_and_later_edits_keep_unrelated_settings(self):
        self.config("--output", "local=udp://127.0.0.1:19000",
                    "--log-level", "debug")
        first = self.snapshot()
        self.assertEqual(first["outputs"][0]["port"], 19000)
        self.config("--queue-records", "256")
        second = self.snapshot()
        self.assertEqual(second["outputs"], first["outputs"])
        self.assertEqual(second["process"]["log_level"], "debug")
        self.assertEqual(second["limits"]["queue_records"], 256)

    def test_initialize_guard_preserves_saved_configuration(self):
        self.config("--output", "local=udp://127.0.0.1:19000")
        first = self.snapshot()
        self.config("--if-uninitialized", "--set", "not valid TOML")
        self.assertEqual(self.snapshot(), first)

    def test_run_rejects_configuration_arguments(self):
        self.config("--output", "local=udp://127.0.0.1:19000")
        first = self.snapshot()
        result = self.command("run", "--log-level", "debug")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.snapshot(), first)

    def test_configuration_errors_do_not_replace_saved_settings(self):
        self.config("--output", "local=udp://127.0.0.1:19000")
        first = self.snapshot()
        self.assertEqual(self.command("config", "--set",
                                     "outputs[0].port=0").returncode, 2)
        self.assertEqual(self.snapshot(), first)

    def test_running_process_owns_store_until_clean_shutdown(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        self.config("--output", f"local=tcp://127.0.0.1:{port}")
        process = subprocess.Popen(
            [BINARY, "run", "--state-dir", str(self.state)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", port), 0.2):
                        break
                except OSError:
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.02)
            result = self.command("config", "--log-level", "debug")
            self.assertNotEqual(result.returncode, 0)
            process.send_signal(signal.SIGTERM)
            _, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0, error.decode())
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
        self.config("--log-level", "debug")


if __name__ == "__main__":
    unittest.main()
