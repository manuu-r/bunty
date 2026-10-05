"""Verify credential staging/activation without cloud calls or device writes."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "provision_iot_gateway.py"
spec = importlib.util.spec_from_file_location("provision", SCRIPT)
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)
TOKEN = "iot_" + "a" * 43


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        folder = Path(self.temporary.name)
        self.header = folder / "IotGatewayCredentials.generated.h"
        self.marker = folder / "iot_gateway_activation_pending"
        for name, value in (("GENERATED_DIR", folder), ("CREDENTIAL_HEADER", self.header),
                            ("ACTIVATION_MARKER", self.marker)):
            patcher = patch.object(provision, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def run_main(self, gateway, flash):
        with patch("sys.argv", [str(SCRIPT), "--port", "test-port"]), \
             patch.object(provision, "gateway_command", side_effect=gateway), \
             patch.object(provision.subprocess, "run", side_effect=flash), \
             contextlib.redirect_stdout(io.StringIO()):
            provision.main()

    def test_activation_follows_successful_flash(self):
        events = []

        def gateway(action, token_only=False):
            events.append(action)
            if action == "activate":
                self.assertTrue(self.marker.exists())
            return TOKEN if action == "stage" else ""

        def flash(command, **kwargs):
            events.append("flash")
            self.assertIn(TOKEN, self.header.read_text())
            self.assertEqual(command[-3:], ["-p", "test-port", "flash"])
            self.assertNotIn("monitor", command)
            self.assertFalse(self.marker.exists())

        self.run_main(gateway, flash)
        self.assertEqual(events, ["stage", "flash", "activate"])
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.header.stat().st_mode & 0o777, 0o600)

    def test_failed_flash_never_activates(self):
        events = []

        def gateway(action, token_only=False):
            events.append(action)
            return TOKEN

        def flash(*args, **kwargs):
            raise subprocess.CalledProcessError(1, "idf.py")

        with self.assertRaises(subprocess.CalledProcessError):
            self.run_main(gateway, flash)
        self.assertEqual(events, ["stage"])
        self.assertFalse(self.marker.exists())

    def test_activation_failure_keeps_recovery_marker(self):
        def gateway(action, token_only=False):
            if action == "activate":
                raise RuntimeError("offline")
            return TOKEN

        with self.assertRaises(RuntimeError):
            self.run_main(gateway, lambda *args, **kwargs: None)
        self.assertTrue(self.marker.exists())
        events = []

        def recovered(action, token_only=False):
            events.append(action)
            return TOKEN if action == "stage" else ""

        self.run_main(recovered, lambda *args, **kwargs: None)
        self.assertEqual(events, ["activate", "stage", "activate"])
        self.assertFalse(self.marker.exists())

    def test_invalid_token_leaves_existing_header_intact(self):
        self.header.write_text("existing credential")
        with self.assertRaises(RuntimeError):
            provision.write_credential_header("invalid")
        self.assertEqual(self.header.read_text(), "existing credential")


if __name__ == "__main__":
    unittest.main()
