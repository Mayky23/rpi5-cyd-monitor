"""Focused regressions for authentication, stale data and collection completeness."""

import json
import os
import subprocess
import sys
import unittest
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
             patch.object(monitor, "proxmox_request", side_effect=responses):
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
             patch.object(monitor, "proxmox_request",
                          side_effect=monitor.ProxmoxOfflineError("sin conexion")):
            result = monitor.proxmox_stats()
        self.assertFalse(result["available"])
        self.assertEqual(result["state"], "offline")
        self.assertEqual(result["nodes"], [])
        self.assertEqual(result["history"], [])
        self.assertEqual(list(monitor._proxmox_history), [])

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

    def test_install_preserves_env_and_restarts_service(self):
        install = (Path(__file__).resolve().parents[1] / "server" / "install.sh").read_text()
        self.assertNotIn('cp -a "$SCRIPT_DIR/."', install)
        self.assertIn("systemctl restart rpi-monitor.service", install)


if __name__ == "__main__":
    unittest.main()
