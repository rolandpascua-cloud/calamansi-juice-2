#!/usr/bin/env python3
"""
Integration tests for the Telemetry History Viewer (Calamansi Juice 2
addition, not present in upstream lemonade-sdk/lemonade). Covers:

- A completed inference span becomes queryable via GET /telemetry/history.
- History survives a full server restart against the same cache dir
  (telemetry_history.db persistence) - this is the feature's acceptance bar.
- Prompt/response text is NOT stored by default (store_previews opt-in).
- model/route filters narrow results correctly.
- GET /telemetry/history/summary returns sane aggregate shapes.
- POST /internal/telemetry/history/clear empties the store.
- Access scoping: an open server (no API key) sees its own history; a
  server with an API key configured scopes history to the caller's token
  (admin sees all, a non-admin token sees only its own requests).

Mirrors the subprocess-per-class pattern in server_websocket_telemetry.py
rather than utils.server_base.ServerTestBase, since these tests need control
over the server process lifecycle (restart) and --cache-dir, not just a
already-running shared server.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import requests

# Make the `utils` package importable when this file is executed directly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from utils.test_models import get_default_lemond_binary  # noqa: E402


def find_free_port():
    s = socket.socket()
    s.bind(("", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def trigger_request(port, model="nonexistent-history-test-model", auth_token=None, timeout=5):
    """POSTs a chat/completions request that fails model lookup, which is
    enough to produce a completed (error) span - see trigger_span() in
    server_websocket_telemetry.py for the same pattern. Does not require a
    real loadable model.
    """
    headers = {}
    if auth_token:
        headers["Authorization"] = f"Bearer {auth_token}"
    try:
        requests.post(
            f"http://localhost:{port}/api/v1/chat/completions",
            json={"model": model, "messages": [{"role": "user", "content": "hello there"}]},
            headers=headers,
            timeout=timeout,
        )
    except requests.exceptions.RequestException:
        pass


def wait_for_history_count(port, expected_min, headers=None, timeout=5.0):
    """record_span() runs synchronously inside the span-listener callback
    fired by telemetry::emit_span(), but emit_span itself may run on a
    background/async completion path - poll briefly rather than assuming
    the row is visible the instant trigger_request() returns.
    """
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        resp = requests.get(
            f"http://localhost:{port}/api/v1/telemetry/history",
            headers=headers or {},
            timeout=2,
        )
        data = resp.json()
        last = data
        if data.get("total", 0) >= expected_min:
            return data
        time.sleep(0.1)
    return last


class TelemetryHistoryTestBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lemond_bin = get_default_lemond_binary()
        if not os.path.exists(cls.lemond_bin):
            raise RuntimeError(f"lemond binary not found at {cls.lemond_bin}. Build it first.")
        cls.procs = []
        cls.temp_dirs = []

    @classmethod
    def tearDownClass(cls):
        for proc, log_file, _log_file_path in cls.procs:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except Exception:
                    try:
                        proc.kill()
                        proc.wait()
                    except Exception:
                        pass
            try:
                log_file.close()
            except Exception:
                pass
        for temp_dir in cls.temp_dirs:
            try:
                shutil.rmtree(temp_dir)
            except Exception:
                pass

    @classmethod
    def start_server(cls, cache_dir, env_overrides=None, keep_temp_dir=True):
        """Starts a new lemond/calamansid process pointed at cache_dir. Safe
        to call more than once with the same cache_dir to simulate a
        restart (each call is a fresh process on a fresh port).
        """
        if keep_temp_dir:
            cls.temp_dirs.append(cache_dir)

        port = find_free_port()
        env = os.environ.copy()
        for k in ["LEMONADE_API_KEY", "LEMONADE_ADMIN_API_KEY"]:
            env.pop(k, None)
        if env_overrides:
            for k, v in env_overrides.items():
                if v is None:
                    env.pop(k, None)
                else:
                    env[k] = v

        log_file_path = os.path.join(cache_dir, f"server_{port}.log")
        log_file = open(log_file_path, "w")
        proc = subprocess.Popen(
            [cls.lemond_bin, cache_dir, "--port", str(port)],
            stdout=log_file,
            stderr=log_file,
            env=env,
        )
        cls.procs.append((proc, log_file, log_file_path))

        for _ in range(100):
            try:
                res = requests.get(f"http://localhost:{port}/live", timeout=0.2)
                if res.status_code == 200:
                    return port
            except Exception:
                pass
            time.sleep(0.05)

        proc.terminate()
        try:
            with open(log_file_path, "r") as f:
                log_content = f.read()
        except Exception:
            log_content = "Could not read log file"
        raise RuntimeError(f"Failed to start lemond server on port {port}. Log:\n{log_content}")

    @classmethod
    def stop_server(cls):
        """Stops the most recently started server without removing its
        cache dir, so a subsequent start_server() call against the same
        cache_dir exercises a real restart.
        """
        proc, log_file, _ = cls.procs[-1]
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()
                proc.wait()
        try:
            log_file.close()
        except Exception:
            pass


class TelemetryHistoryOpenServerTests(TelemetryHistoryTestBase):
    """Default (no API key) deployment - the common single-user local case."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.cache_dir = tempfile.mkdtemp(prefix="calamansi_history_test_")
        cls.port = cls.start_server(cls.cache_dir)

    def test_001_new_request_is_queryable(self):
        trigger_request(self.port, model="history-test-model-open")
        data = wait_for_history_count(self.port, expected_min=1)
        self.assertGreaterEqual(data["total"], 1)
        record = next(r for r in data["records"] if r["model_name"] == "history-test-model-open")
        self.assertEqual(record["route"], "chat.completions")
        self.assertFalse(record["success"])
        self.assertIn("latency_ms", record)

    def test_002_preview_not_stored_by_default(self):
        trigger_request(self.port, model="history-test-model-preview-check")
        data = wait_for_history_count(self.port, expected_min=1)
        record = next(
            r for r in data["records"] if r["model_name"] == "history-test-model-preview-check"
        )
        self.assertEqual(record.get("preview", ""), "")

    def test_003_model_filter(self):
        trigger_request(self.port, model="history-test-model-filter-a")
        wait_for_history_count(self.port, expected_min=1)

        resp = requests.get(
            f"http://localhost:{self.port}/api/v1/telemetry/history",
            params={"model": "history-test-model-filter-a"},
            timeout=5,
        )
        data = resp.json()
        self.assertGreaterEqual(data["total"], 1)
        self.assertTrue(all(r["model_name"] == "history-test-model-filter-a" for r in data["records"]))

        resp = requests.get(
            f"http://localhost:{self.port}/api/v1/telemetry/history",
            params={"model": "no-such-model-at-all"},
            timeout=5,
        )
        self.assertEqual(resp.json()["total"], 0)

    def test_004_summary_shape(self):
        trigger_request(self.port, model="history-test-model-summary")
        wait_for_history_count(self.port, expected_min=1)

        resp = requests.get(f"http://localhost:{self.port}/api/v1/telemetry/history/summary", timeout=5)
        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        for key in ("avg_tokens_per_second_by_model", "requests_by_day", "overall"):
            self.assertIn(key, data)
        self.assertIn("total_requests", data["overall"])
        self.assertIn("error_rate", data["overall"])
        self.assertGreaterEqual(data["overall"]["total_requests"], 1)

    def test_005_restart_persists_history(self):
        trigger_request(self.port, model="history-test-model-restart")
        before = wait_for_history_count(self.port, expected_min=1)
        before_total = before["total"]
        self.assertGreaterEqual(before_total, 1)

        # Restart against the SAME cache dir - this is the feature's
        # acceptance bar: historical data from before the restart must
        # still be queryable.
        self.stop_server()
        new_port = self.start_server(self.cache_dir, keep_temp_dir=False)

        resp = requests.get(f"http://localhost:{new_port}/api/v1/telemetry/history", timeout=5)
        after = resp.json()
        self.assertGreaterEqual(after["total"], before_total)
        self.assertTrue(
            any(r["model_name"] == "history-test-model-restart" for r in after["records"])
        )

        # Route subsequent tests in this class to the new port/process.
        self.__class__.port = new_port

    def test_006_clear_history(self):
        trigger_request(self.port, model="history-test-model-clear")
        data = wait_for_history_count(self.port, expected_min=1)
        self.assertGreaterEqual(data["total"], 1)

        resp = requests.post(
            f"http://localhost:{self.port}/internal/telemetry/history/clear",
            headers={"Content-Type": "application/json"},
            json={},
            timeout=5,
        )
        self.assertEqual(resp.status_code, 200)
        self.assertTrue(resp.json().get("cleared"))

        resp = requests.get(f"http://localhost:{self.port}/api/v1/telemetry/history", timeout=5)
        self.assertEqual(resp.json()["total"], 0)


class TelemetryHistoryAuthScopingTests(TelemetryHistoryTestBase):
    """Scoping behavior when the server has API keys configured."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.cache_dir = tempfile.mkdtemp(prefix="calamansi_history_auth_test_")
        cls.port = cls.start_server(
            cls.cache_dir,
            {"LEMONADE_API_KEY": "user_key", "LEMONADE_ADMIN_API_KEY": "admin_key"},
        )

    def test_001_unauthenticated_request_rejected(self):
        resp = requests.get(f"http://localhost:{self.port}/api/v1/telemetry/history", timeout=5)
        self.assertEqual(resp.status_code, 401)

    def test_002_admin_sees_all_own_and_others(self):
        trigger_request(self.port, model="history-test-model-user", auth_token="user_key")
        wait_for_history_count(
            self.port, expected_min=1, headers={"Authorization": "Bearer admin_key"}
        )

        resp = requests.get(
            f"http://localhost:{self.port}/api/v1/telemetry/history",
            headers={"Authorization": "Bearer admin_key"},
            timeout=5,
        )
        data = resp.json()
        self.assertTrue(any(r["model_name"] == "history-test-model-user" for r in data["records"]))

        # Own-token rule: the same non-admin token that made the request can
        # also see it (scoped to its own auth_token_hash).
        resp = requests.get(
            f"http://localhost:{self.port}/api/v1/telemetry/history",
            headers={"Authorization": "Bearer user_key"},
            timeout=5,
        )
        data = resp.json()
        self.assertTrue(any(r["model_name"] == "history-test-model-user" for r in data["records"]))

    def test_003_clear_requires_admin_key(self):
        resp = requests.post(
            f"http://localhost:{self.port}/internal/telemetry/history/clear",
            headers={"Authorization": "Bearer user_key", "Content-Type": "application/json"},
            json={},
            timeout=5,
        )
        # user_key == admin_key would pass too, but here they're distinct,
        # so a non-admin token must be rejected from this /internal/* route.
        self.assertEqual(resp.status_code, 401)

        resp = requests.post(
            f"http://localhost:{self.port}/internal/telemetry/history/clear",
            headers={"Authorization": "Bearer admin_key", "Content-Type": "application/json"},
            json={},
            timeout=5,
        )
        self.assertEqual(resp.status_code, 200)


if __name__ == "__main__":
    unittest.main()
