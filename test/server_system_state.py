#!/usr/bin/env python3
"""
Integration tests for the System State Viewer (Calamansi Juice 2 addition,
not present in upstream lemonade-sdk/lemonade). Covers:

- GET /system/state returns all 5 top-level sections, each independently
  well-formed even when its underlying tools/hardware aren't present.
- Every leaf "Field" object (available/reason or available/value shape) is
  internally consistent - never a blank/crash when a collector is missing
  its tool or lacks permission.
- ~60s server-side caching + ?refresh=true bypass.
- The app's own version string is exposed and traces back to the upstream
  Lemonade version it tracks.

Mirrors the subprocess-per-class pattern in server_websocket_telemetry.py /
server_telemetry_history.py rather than utils.server_base.ServerTestBase.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import requests

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from utils.test_models import get_default_lemond_binary  # noqa: E402


def find_free_port():
    s = socket.socket()
    s.bind(("", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def assert_field_shape(test, field, path):
    """A "Field" leaf is always {"available": bool, ...}; when False it must
    carry a non-empty "reason" explaining *why* (permission denied / tool
    not installed / not applicable), never a blank result.
    """
    test.assertIsInstance(field, dict, f"{path} should be an object")
    test.assertIn("available", field, f"{path} missing 'available'")
    test.assertIsInstance(field["available"], bool, f"{path}.available should be a bool")
    if not field["available"]:
        test.assertIn("reason", field, f"{path} is unavailable but has no 'reason'")
        test.assertTrue(field["reason"], f"{path}.reason should be non-empty")


class SystemStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lemond_bin = get_default_lemond_binary()
        if not os.path.exists(cls.lemond_bin):
            raise RuntimeError(f"lemond binary not found at {cls.lemond_bin}. Build it first.")

        cls.cache_dir = tempfile.mkdtemp(prefix="calamansi_system_state_test_")
        cls.port = find_free_port()
        log_path = os.path.join(cls.cache_dir, "server.log")
        cls.log_file = open(log_path, "w")
        cls.proc = subprocess.Popen(
            [cls.lemond_bin, cls.cache_dir, "--port", str(cls.port)],
            stdout=cls.log_file,
            stderr=cls.log_file,
        )

        for _ in range(100):
            try:
                res = requests.get(f"http://localhost:{cls.port}/live", timeout=0.2)
                if res.status_code == 200:
                    break
            except Exception:
                pass
            time.sleep(0.05)
        else:
            cls.proc.terminate()
            with open(log_path) as f:
                raise RuntimeError(f"Failed to start lemond server. Log:\n{f.read()}")

    @classmethod
    def tearDownClass(cls):
        if cls.proc.poll() is None:
            cls.proc.terminate()
            try:
                cls.proc.wait(timeout=5)
            except Exception:
                cls.proc.kill()
                cls.proc.wait()
        try:
            cls.log_file.close()
        except Exception:
            pass
        try:
            shutil.rmtree(cls.cache_dir)
        except Exception:
            pass

    def get_state(self, refresh=False):
        params = {"refresh": "true"} if refresh else {}
        resp = requests.get(f"http://localhost:{self.port}/api/v1/system/state", params=params, timeout=10)
        self.assertEqual(resp.status_code, 200)
        return resp.json()

    def test_001_top_level_sections_present(self):
        data = self.get_state()
        for key in ("generated_at_ms", "cached", "hardware", "os_kernel", "firmware", "ai_stack", "driver_stack"):
            self.assertIn(key, data)

    def test_002_hardware_section_well_formed(self):
        hw = self.get_state()["hardware"]
        # cpu/gpu/npu are richer than plain Field objects but always carry
        # "available"; a genuine detection failure must never blank the
        # whole section (no KeyError/None instead of a dict).
        for key in ("cpu", "gpu", "npu"):
            self.assertIn(key, hw)
            self.assertIn("available", hw[key])
        assert_field_shape(self, hw["memory"], "hardware.memory")
        assert_field_shape(self, hw["uma"], "hardware.uma")
        # UMA framing: if available, must name which kernel param is active
        # (or explicitly say module defaults apply) - never a flat number.
        if hw["uma"]["available"]:
            self.assertIn("active_param", hw["uma"])

    def test_003_os_kernel_section_well_formed(self):
        os_kernel = self.get_state()["os_kernel"]
        for key in ("distro", "kernel_version", "boot_mode", "amd_iommu"):
            assert_field_shape(self, os_kernel[key], f"os_kernel.{key}")

    def test_004_firmware_section_degrades_gracefully(self):
        """Acceptance bar: loads without root/ROCm, with clearly labeled
        unavailable fields rather than errors - never a 500.
        """
        firmware = self.get_state()["firmware"]
        assert_field_shape(self, firmware["bios"], "firmware.bios")
        assert_field_shape(self, firmware["gpu_vbios"], "firmware.gpu_vbios")
        if firmware["bios"]["available"]:
            self.assertIn("version", firmware["bios"])

    def test_005_ai_stack_section_well_formed(self):
        ai_stack = self.get_state()["ai_stack"]
        for key in ("rocm_version", "hip_version", "mesa_version", "python_version"):
            assert_field_shape(self, ai_stack[key], f"ai_stack.{key}")

        self.assertIsInstance(ai_stack["backends"], dict)
        for name, field in ai_stack["backends"].items():
            assert_field_shape(self, field, f"ai_stack.backends.{name}")

        app_version = ai_stack["app_version"]
        self.assertIn("full_version", app_version)
        self.assertIn("tracks_upstream_lemonade", app_version)
        self.assertIn("+cj", app_version["full_version"])
        self.assertTrue(app_version["full_version"].startswith(app_version["tracks_upstream_lemonade"]))

    def test_006_driver_stack_section_well_formed(self):
        driver_stack = self.get_state()["driver_stack"]
        for tool in ("asusctl", "supergfxctl", "z13ctl", "tuned_adm"):
            self.assertIn(tool, driver_stack)
            self.assertIn("installed", driver_stack[tool])
            self.assertIsInstance(driver_stack[tool]["installed"], bool)

        # z13ctl and tuned-adm carry an extra Field-shaped leaf beyond plain
        # presence (raw `z13ctl status` output / the active TuneD profile) -
        # each degrades to {available: false, reason: "..."} independently
        # of "installed" if the tool exists but the command itself fails.
        assert_field_shape(self, driver_stack["z13ctl"]["status"], "driver_stack.z13ctl.status")
        assert_field_shape(
            self, driver_stack["tuned_adm"]["active_profile"], "driver_stack.tuned_adm.active_profile"
        )

    def test_007_caching_and_refresh_bypass(self):
        first = self.get_state()
        second = self.get_state()
        self.assertTrue(second["cached"])
        self.assertEqual(first["generated_at_ms"], second["generated_at_ms"])

        time.sleep(1.1)
        refreshed = self.get_state(refresh=True)
        self.assertFalse(refreshed["cached"])
        self.assertGreater(refreshed["generated_at_ms"], first["generated_at_ms"])

    def test_008_head_request(self):
        resp = requests.head(f"http://localhost:{self.port}/api/v1/system/state", timeout=5)
        self.assertEqual(resp.status_code, 200)


if __name__ == "__main__":
    unittest.main()
