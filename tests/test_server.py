"""Focused regressions for authentication, stale data and collection completeness."""

import hashlib
import http.server
import json
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from collections import namedtuple
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from fastapi.testclient import TestClient

os.environ.setdefault("MONITOR_API_TOKEN", "test-token-not-for-production")

from server.app import main as monitor


class ServerTests(unittest.TestCase):
    def setUp(self):
        monitor._cache.clear()
        monitor._cache_status.clear()
        monitor._cache_retry_after.clear()
        monitor._history.clear()
        monitor._proxmox_history.clear()
        with monitor._bridge_labels_lock:
            monitor._bridge_labels = {}

    def test_auth_requires_correct_token(self):
        for token in (None, "", "wrong"):
            with self.assertRaises(monitor.HTTPException) as failure:
                monitor.auth(token)
            self.assertEqual(failure.exception.status_code, 401)
        monitor.auth("test-token-not-for-production")

    def test_startup_rejects_missing_token(self):
        env = os.environ.copy()
        env.pop("MONITOR_API_TOKEN", None)
        result = subprocess.run([sys.executable, "-c", "import server.app.main"], env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MONITOR_API_TOKEN", result.stderr)

    def test_health_route_requires_token_and_keeps_schema_key(self):
        client = TestClient(monitor.app)
        self.assertEqual(client.get("/health").status_code, 401)
        response = client.get("/health", headers={"X-API-Key": "test-token-not-for-production"})
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["schema"], 2)

    def test_failed_refresh_preserves_last_good_age_and_retries(self):
        with patch.object(monitor.time, "monotonic", return_value=100):
            self.assertEqual(monitor.cached("ports", 8, lambda: [1]), [1])
        def fail():
            raise RuntimeError("permission denied")
        with patch.object(monitor.time, "monotonic", return_value=110):
            self.assertEqual(monitor.cached("ports", 8, fail), [1])
        self.assertEqual(monitor._cache_status["ports"]["age_s"], 10)
        self.assertTrue(monitor._cache_status["ports"]["stale"])
        with patch.object(monitor.time, "monotonic", return_value=111):
            self.assertEqual(monitor.cached("ports", 8, lambda: [2]), [1])
        with patch.object(monitor.time, "monotonic", return_value=116):
            self.assertEqual(monitor.cached("ports", 8, lambda: [2]), [2])
        self.assertTrue(monitor._cache_status["ports"]["ok"])

    def test_failure_without_previous_value_is_reported(self):
        with patch.object(monitor.time, "monotonic", return_value=100):
            def fail():
                raise RuntimeError("unavailable")
            self.assertIsNone(monitor.cached("devices", 15, fail))
        self.assertFalse(monitor._cache_status["devices"]["ok"])
        self.assertFalse(monitor._cache_status["devices"]["stale"])

    def test_docker_list_matches_total_beyond_old_cap(self):
        lines = [json.dumps({"Names": f"container-{i}", "Image": "image", "State": "running"})
                 for i in range(20)]
        result = SimpleNamespace(returncode=0, stdout="\n".join(lines), stderr="")
        with patch.object(monitor.shutil, "which", return_value="/usr/bin/docker"), \
             patch.object(monitor.subprocess, "run", return_value=result):
            docker = monitor.docker_containers()
        self.assertEqual(docker["total"], 20)
        self.assertEqual(docker["running"], 20)
        self.assertEqual(len(docker["containers"]), 20)

    def test_docker_bridge_uses_service_names_and_marks_shared_networks(self):
        ids = [letter * 64 for letter in "abc"]
        listed = SimpleNamespace(returncode=0, stderr="", stdout="\n".join(
            json.dumps({"ID": network_id, "Name": name}) for network_id, name in
            zip(ids, ("media_default", "shared_default", "empty_default"))))
        inspected = SimpleNamespace(returncode=0, stderr="", stdout=json.dumps([
            {"Id": ids[0], "Name": "media_default", "Containers": {"1": {"Name": "jellyfin"}}},
            {"Id": ids[1], "Name": "shared_default", "Containers": {
                "2": {"Name": "prometheus"}, "3": {"Name": "grafana"}}},
            {"Id": ids[2], "Name": "empty_default", "Containers": {}},
        ]))
        with patch.object(monitor.shutil, "which", return_value="/usr/bin/docker"), \
             patch.object(monitor.subprocess, "run", side_effect=[listed, inspected]) as run:
            labels = monitor.docker_bridge_labels()
        self.assertEqual(labels[f"br-{ids[0][:12]}"], "Docker / jellyfin")
        self.assertEqual(labels[f"br-{ids[1][:12]}"], "Docker / grafana +1")
        self.assertEqual(labels[f"br-{ids[2][:12]}"], "Docker / empty_default")
        self.assertEqual(run.call_count, 2)
        with monitor._bridge_labels_lock:
            monitor._bridge_labels = labels
        original = [{"name": f"br-{ids[0][:12]}", "up": True}]
        self.assertEqual(monitor.named_interfaces(original)[0]["display_name"], "Docker / jellyfin")
        self.assertNotIn("display_name", original[0])

    def test_non_ascii_token_is_rejected_without_server_error(self):
        client = TestClient(monitor.app, raise_server_exceptions=False)
        response = client.get("/health", headers={"X-API-Key": "ñandú-token".encode("latin-1")})
        self.assertEqual(response.status_code, 401)

    def test_proxmox_stats_normalizes_nodes_guests_and_storage(self):
        responses = [
            {"version": "9.2.11"},
            [
                {"type": "node", "node": "proxmox", "status": "online",
                 "cpu": 0.125, "maxcpu": 8, "mem": 8, "maxmem": 32, "uptime": 900},
                {"type": "qemu", "vmid": 100, "name": "firewall", "node": "proxmox",
                 "status": "running", "cpu": 0.2, "maxcpu": 4, "mem": 4, "maxmem": 8,
                 "disk": 10, "maxdisk": 40, "netin": 100, "netout": 200, "uptime": 60},
                {"type": "lxc", "vmid": 101, "name": "dns", "node": "proxmox",
                 "status": "stopped", "maxcpu": 2, "maxmem": 4},
                {"type": "storage", "storage": "local-lvm", "node": "proxmox",
                 "status": "available", "disk": 25, "maxdisk": 100},
            ],
            [{"type": "vzdump", "status": "OK", "node": "proxmox",
              "user": "root@pam", "starttime": 123}],
        ]
        with patch.object(monitor, "proxmox_configured", return_value=True), \
             patch.object(monitor, "proxmox_fetch", return_value=responses):
            result = monitor.proxmox_stats()
        self.assertTrue(result["available"])
        self.assertEqual(result["version"], "9.2.11")
        self.assertEqual(result["nodes"][0]["cpu_percent"], 12.5)
        self.assertEqual(result["nodes"][0]["memory_percent"], 25.0)
        self.assertEqual(result["guests_running"], 1)
        self.assertEqual(result["guests_total"], 2)
        self.assertEqual(result["guests"][0]["disk_percent"], 25.0)
        self.assertIsNone(result["guests"][1]["disk_percent"])
        self.assertEqual(result["storage"][0]["percent"], 25.0)
        self.assertEqual(result["tasks"][0]["status"], "OK")
        self.assertEqual(result["tasks"][0]["label"], "Copia de seguridad")
        self.assertEqual(result["tasks"][0]["state"], "OK")
        self.assertEqual(result["tasks_failed"], 0)
        self.assertEqual(result["history"][0], {"cpu": 12.5, "ram": 25.0,
                                                 "time": result["history"][0]["time"]})

    def test_proxmox_interrupted_console_is_cancelled_not_failed(self):
        task = monitor._proxmox_task({
            "type": "vncshell", "node": "proxmox", "user": "root@pam",
            "status": "command failed: received interrupt", "starttime": 123,
            "endtime": 124,
        })
        self.assertEqual(task["label"], "Consola del nodo")
        self.assertEqual(task["state"], "CANCELLED")
        self.assertEqual(task["detail"], "Consola cerrada por el usuario")

    def test_proxmox_unconfigured_is_explicit(self):
        with patch.object(monitor, "proxmox_configured", return_value=False):
            result = monitor.proxmox_stats()
        self.assertFalse(result["available"])
        self.assertEqual(result["error"], "no configurado")
        self.assertEqual(result["guests"], [])

    def test_proxmox_offline_discards_stale_metrics_without_failing_collector(self):
        monitor._proxmox_history.append({"cpu": 90.0, "ram": 80.0, "time": 1})
        with patch.object(monitor, "proxmox_configured", return_value=True), \
             patch.object(monitor, "proxmox_fetch",
                          side_effect=monitor.ProxmoxOfflineError("sin conexion")):
            result = monitor.proxmox_stats()
        self.assertFalse(result["available"])
        self.assertEqual(result["state"], "offline")
        self.assertEqual(result["nodes"], [])
        self.assertEqual(result["history"], [])
        self.assertEqual(list(monitor._proxmox_history), [])

    def test_proxmox_tasks_are_the_most_recent_and_bad_payloads_fail(self):
        tasks = [{"type": "qmstart", "status": "OK", "starttime": start} for start in range(30)]
        with patch.object(monitor, "proxmox_configured", return_value=True), \
             patch.object(monitor, "proxmox_fetch", return_value=[{"version": "9"}, [], tasks]):
            result = monitor.proxmox_stats()
        self.assertEqual(len(result["tasks"]), 20)
        self.assertEqual(result["tasks"][0]["start_time"], 29)
        with patch.object(monitor, "proxmox_configured", return_value=True), \
             patch.object(monitor, "proxmox_fetch", return_value=[None, {}, []]):
            with self.assertRaisesRegex(RuntimeError, "inesperada"):
                monitor.proxmox_stats()

    def test_history_interval_reports_the_real_sample_spacing(self):
        history = [{"cpu": 1, "ram": 1, "time": 1000 + i * 10} for i in range(6)]
        self.assertEqual(monitor._history_interval_ms(history, 8000), 10000)
        self.assertEqual(monitor._history_interval_ms(history[:1], 8000), 8000)

    def test_cpu_percent_uses_one_baseline_for_every_request_thread(self):
        Times = namedtuple("Times", "user nice system idle iowait")
        samples = iter([Times(10, 0, 0, 90, 0), Times(60, 0, 0, 140, 0),
                        Times(60, 0, 0, 240, 0), Times(60, 0, 0, 340, 0)])

        def cpu_times(percpu=False):
            value = next(samples) if not percpu else None
            return value if not percpu else [Times(0, 0, 0, 1, 0)]

        results = []
        with patch.object(monitor.psutil, "cpu_times", side_effect=cpu_times):
            monitor._last_cpu_times = Times(0, 0, 0, 0, 0)
            for _ in range(3):
                worker = threading.Thread(target=lambda: results.append(monitor.sample_realtime()["cpu"]["percent"]))
                worker.start()
                worker.join()
        # 10/100 busy, then 50/100, then 0/100: each new thread continues the shared baseline.
        self.assertEqual(results, [10.0, 50.0, 0.0])

    def test_slow_collector_does_not_block_other_collectors(self):
        started, release = threading.Event(), threading.Event()

        def slow():
            started.set()
            release.wait(2)
            return "slow"

        worker = threading.Thread(target=monitor.cached, args=("proxmox", 8, slow))
        worker.start()
        started.wait(2)
        begin = time.monotonic()
        self.assertEqual(monitor.cached("ports", 8, lambda: [22]), [22])
        self.assertLess(time.monotonic() - begin, 0.5)
        release.set()
        worker.join()

    def test_snapshot_collects_in_parallel(self):
        def slow(value):
            def loader():
                time.sleep(0.4)
                return value
            return loader
        with patch.object(monitor, "sample_realtime", return_value={}), \
             patch.object(monitor, "block_devices", side_effect=slow([])), \
             patch.object(monitor, "network_interfaces", side_effect=slow([])), \
             patch.object(monitor, "all_temperatures", side_effect=slow([])), \
             patch.object(monitor, "listening_ports", side_effect=slow([])), \
             patch.object(monitor, "docker_containers", side_effect=slow({"available": True, "containers": []})), \
             patch.object(monitor, "proxmox_stats", side_effect=slow({"available": False, "state": "unconfigured"})):
            begin = time.monotonic()
            result = monitor.snapshot()
        self.assertLess(time.monotonic() - begin, 1.5)
        self.assertEqual(result["proxmox"]["state"], "unconfigured")

    def test_port_permission_error_is_not_an_empty_list(self):
        with patch.object(monitor.psutil, "net_connections", side_effect=monitor.psutil.AccessDenied()):
            with self.assertRaisesRegex(RuntimeError, "puertos"):
                monitor.listening_ports()

    def test_history_sample_does_not_touch_network_rate_baseline(self):
        baseline = monitor._last_sample_time
        with patch.object(monitor.psutil, "cpu_percent", return_value=23.4), \
             patch.object(monitor.psutil, "virtual_memory", return_value=SimpleNamespace(percent=52.1)), \
             patch.object(monitor, "cpu_temperature", return_value=45.0):
            monitor._record_history()
        self.assertEqual(monitor._last_sample_time, baseline)
        self.assertEqual(monitor._history[-1]["cpu"], 23.4)
        self.assertEqual(monitor._history[-1]["ram"], 52.1)

    def test_snapshot_reports_collection_failure(self):
        with patch.object(monitor, "sample_realtime", return_value={}), \
             patch.object(monitor, "proxmox_stats", return_value={"available": False, "state": "unconfigured"}), \
             patch.object(monitor, "block_devices", side_effect=RuntimeError("disk offline")), \
             patch.object(monitor, "network_interfaces", return_value=[]), \
             patch.object(monitor, "all_temperatures", return_value=[]), \
             patch.object(monitor, "listening_ports", return_value=[]), \
             patch.object(monitor, "docker_containers", return_value={"available": False, "error": "docker no instalado", "containers": []}):
            result = monitor.snapshot()
        self.assertEqual(result["storage"]["mounts"], [])
        self.assertFalse(result["collection_status"]["block_devices"]["ok"])
        self.assertTrue(result["collection_status"]["interfaces"]["ok"])
        self.assertEqual(result["history_interval_ms"], 5000)

    def test_install_validates_proxmox_settings_and_python(self):
        install = (Path(__file__).resolve().parents[1] / "server" / "install.sh").read_text()
        self.assertIn("sys.version_info >= (3, 10)", install)
        self.assertIn("PROXMOX_CERT_SHA256", install)
        self.assertIn("^https://", install)

    def test_install_preserves_env_and_restarts_service(self):
        install = (Path(__file__).resolve().parents[1] / "server" / "install.sh").read_text()
        self.assertNotIn('cp -a "$SCRIPT_DIR/."', install)
        self.assertIn("systemctl restart rpi-monitor.service", install)


class PinnedConnectionTests(unittest.TestCase):
    """The Proxmox client talks to a real local TLS server with a throwaway certificate."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        key, cert = Path(cls.tmp.name) / "key.pem", Path(cls.tmp.name) / "cert.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-keyout", str(key), "-out", str(cert)],
                       check=True, capture_output=True)
        der = ssl.PEM_cert_to_DER_cert(cert.read_text())
        cls.fingerprint = hashlib.sha256(der).hexdigest()
        cls.requests = []

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"  # closes after every answer: forces reconnections

            def do_GET(handler):
                cls.requests.append((handler.path, handler.headers.get("Authorization")))
                body = json.dumps({"data": {"path": handler.path}}).encode()
                handler.send_response(200)
                handler.send_header("Content-Type", "application/json")
                handler.send_header("Content-Length", str(len(body)))
                handler.end_headers()
                handler.wfile.write(body)

            def log_message(handler, *args):
                pass

        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        cls.server.socket = context.wrap_socket(cls.server.socket, server_side=True)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = f"https://127.0.0.1:{cls.server.server_address[1]}/api2/json"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.tmp.cleanup()

    def configure(self, fingerprint):
        return patch.multiple(monitor, PROXMOX_API_URL=self.url, PROXMOX_TOKEN_ID="monitor@pve!esp32",
                              PROXMOX_TOKEN_SECRET="secret", PROXMOX_CERT_SHA256=fingerprint)

    def test_pinned_certificate_is_checked_on_every_connection(self):
        self.requests.clear()
        with self.configure(self.fingerprint):
            data = monitor.proxmox_fetch(("version", "cluster/resources", "cluster/tasks"))
        self.assertEqual([item["path"] for item in data],
                         ["/api2/json/version", "/api2/json/cluster/resources", "/api2/json/cluster/tasks"])
        self.assertEqual(self.requests[0][1], "PVEAPIToken=monitor@pve!esp32=secret")

    def test_wrong_certificate_never_receives_the_token(self):
        self.requests.clear()
        with self.configure("0" * 64):
            with self.assertRaisesRegex(RuntimeError, "no coincide"):
                monitor.proxmox_fetch(("version",))
        self.assertEqual(self.requests, [])

    def test_invalid_fingerprint_is_rejected_before_connecting(self):
        with self.configure("abc"):
            with self.assertRaisesRegex(RuntimeError, "huella"):
                monitor.proxmox_fetch(("version",))


if __name__ == "__main__":
    unittest.main()
