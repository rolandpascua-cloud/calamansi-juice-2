#!/usr/bin/env python3
"""
Integration tests for the Resource Dashboard (Calamansi Juice 2 addition,
not present in upstream lemonade-sdk/lemonade). Covers:

- GET /resources/system returns cpu/memory/gpu/npu/disk/sensors, each
  independently well-formed even when its underlying tool/hardware is
  missing - never a blank/crash, per-section {"error": "..."} instead.
- CPU percent priming: the first poll after server start reports null
  per-core percentages (no prior /proc/stat sample to diff against); a
  second poll shortly after reports real numbers.
- GET /resources/inference detects other local inference services and
  never includes this app itself (lemond/calamansid would be redundant
  with the app's own state, per the feature's own scoping).
- Both endpoints respond quickly enough to actually support polling
  (system every ~2s, inference every ~4-5s).
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


class ResourceDashboardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lemond_bin = get_default_lemond_binary()
        if not os.path.exists(cls.lemond_bin):
            raise RuntimeError(f"lemond binary not found at {cls.lemond_bin}. Build it first.")

        cls.cache_dir = tempfile.mkdtemp(prefix="calamansi_resources_test_")
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

    def get_resources_system(self):
        resp = requests.get(f"http://localhost:{self.port}/api/v1/resources/system", timeout=10)
        self.assertEqual(resp.status_code, 200)
        return resp.json()

    def get_resources_inference(self):
        resp = requests.get(f"http://localhost:{self.port}/api/v1/resources/inference", timeout=10)
        self.assertEqual(resp.status_code, 200)
        return resp.json()

    def test_001_system_top_level_sections_present(self):
        data = self.get_resources_system()
        for key in ("cpu", "memory", "gpu", "npu", "disk", "sensors", "timestamp"):
            self.assertIn(key, data)
            # Every section is a dict (either real data or {"error"/"note": ...}),
            # never null/missing/a bare string - that's the "never blanks the
            # whole response" contract.
            if key != "timestamp":
                self.assertIsInstance(data[key], dict, f"{key} should be an object")

    def test_002_cpu_percent_priming_then_real_values(self):
        first = self.get_resources_system()["cpu"]
        if "error" in first:
            self.skipTest(f"CPU collector unavailable in this environment: {first['error']}")

        self.assertIn("per_core_percent", first)
        self.assertIn("core_count_logical", first)
        self.assertGreater(first["core_count_logical"] or 0, 0)

        # First sample right after server start may or may not still be the
        # priming sample depending on test ordering vs other tests already
        # having polled - either way, a second poll after a short real delay
        # must produce a numeric aggregate_percent (not stuck on null).
        time.sleep(1.0)
        second = self.get_resources_system()["cpu"]
        self.assertIsInstance(second.get("aggregate_percent"), (int, float))
        self.assertTrue(0 <= second["aggregate_percent"] <= 100)
        self.assertIsInstance(second["per_core_percent"], list)
        self.assertEqual(len(second["per_core_percent"]), first["core_count_logical"])

    def test_003_memory_section_and_uma(self):
        memory = self.get_resources_system()["memory"]
        if "error" in memory:
            self.skipTest(f"Memory collector unavailable in this environment: {memory['error']}")
        for key in ("total", "used", "free", "percent"):
            self.assertIn(key, memory)
        self.assertGreater(memory["total"], 0)
        self.assertIn("uma", memory)
        uma = memory["uma"]
        if "error" not in uma:
            for key in (
                "amdgpu_gttsize_set_on_cmdline",
                "ttm_pages_limit_set_on_cmdline",
                "note",
            ):
                self.assertIn(key, uma)

    def test_004_gpu_degrades_gracefully(self):
        """Acceptance bar: pulling ROCm (or never having it) must not 500 -
        {"error": "..."} instead, same contract as every other section."""
        gpu = self.get_resources_system()["gpu"]
        self.assertIsInstance(gpu, dict)
        if "error" not in gpu:
            self.assertIn("source", gpu)
            self.assertIn(gpu["source"], ("rocm-smi", "amd-smi"))

    def test_005_npu_section_well_formed(self):
        npu = self.get_resources_system()["npu"]
        if "error" in npu:
            self.skipTest(f"NPU collector unavailable in this environment: {npu['error']}")
        self.assertIn("amdxdna_module_loaded", npu)
        self.assertIn("utilization_available", npu)
        self.assertFalse(npu["utilization_available"])  # documented limitation, not a bug

    def test_006_disk_section_never_errors_on_permission(self):
        disk = self.get_resources_system()["disk"]
        if "error" in disk:
            self.skipTest(f"Disk collector unavailable in this environment: {disk['error']}")
        for key in ("total", "used", "free", "percent"):
            self.assertIn(key, disk)
        self.assertIn("btrfs_status", disk)
        self.assertIn(disk["btrfs_status"], ("not_installed", "ok", "permission_denied_or_error", "error"))

    def test_007_inference_excludes_this_app(self):
        data = self.get_resources_inference()
        for key in ("lmstudio", "llama_server", "vllm", "fastflowlm", "timestamp"):
            self.assertIn(key, data)
        # This app IS an inference backend (lemond/calamansid) - it must not
        # report on itself here, since that's redundant with the app's own
        # state (health, loaded models, etc. already exposed elsewhere).
        self.assertNotIn("lemonade", data)
        self.assertNotIn("calamansi", data)

    def test_008_inference_services_well_formed(self):
        data = self.get_resources_inference()
        # lmstudio's shape intentionally differs (app_process_running/running
        # rather than process_running) - it mirrors the LM Studio CLI's own
        # "ps"/app-process split, matching the reference dashboard's shape.
        for key in ("llama_server", "vllm", "fastflowlm"):
            status = data[key]
            self.assertIsInstance(status, dict)
            if "error" not in status:
                self.assertIn("process_running", status)
                self.assertIsInstance(status["process_running"], bool)

        lmstudio = data["lmstudio"]
        self.assertIsInstance(lmstudio, dict)
        if "error" not in lmstudio:
            self.assertIn("cli_found", lmstudio)
            self.assertIn("app_process_running", lmstudio)

    def test_009_head_requests(self):
        for path in ("/resources/system", "/resources/inference"):
            resp = requests.head(f"http://localhost:{self.port}/api/v1{path}", timeout=5)
            self.assertEqual(resp.status_code, 200)

    def test_010_responds_fast_enough_to_poll(self):
        """/resources/system is meant to be polled every ~2s - it must
        comfortably finish well within that budget."""
        start = time.time()
        self.get_resources_system()
        elapsed = time.time() - start
        self.assertLess(elapsed, 1.5, "resources/system too slow for a 2s poll interval")


if __name__ == "__main__":
    unittest.main()
