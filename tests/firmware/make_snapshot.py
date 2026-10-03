"""Build a realistic /api/v2/snapshot answer with the real API code and fake collectors.

    python tests/firmware/make_snapshot.py > tests/firmware/snapshot.json

The firmware host test parses that file with the same JSON filter as the panel.
"""
import json
import os
import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
os.environ.setdefault("MONITOR_API_TOKEN", "test-token-not-for-production")  # same as tests/test_server.py

from server.app import main as monitor  # noqa: E402

REALTIME = {
    "sample_interval_ms": 2500,
    "system": {"hostname": "raspberrypi", "ip": "192.0.2.10", "uptime_s": 936520, "agent_uptime_s": 3600},
    "cpu": {"percent": 13.2, "per_core": [12.0, 7.5, 33.1, 10.0], "temperature_c": 57.3,
            "frequency_mhz": 2400, "load": [0.75, 0.6, 0.5]},
    "memory": {"percent": 34.1, "used_bytes": 2900000000, "total_bytes": 8400000000,
               "available_bytes": 5500000000, "swap_percent": 0.0, "swap_used_bytes": 0,
               "swap_total_bytes": 536870912},
    "disk": {"status": "ok", "error": None, "percent": 42.0, "used_bytes": 420000000000,
             "total_bytes": 1000000000000, "free_bytes": 580000000000, "io_status": "ok",
             "io_error": None, "read_bps": 12500000, "write_bps": 834000},
    "network": {"rx_bps": 12345678, "tx_bps": 987654, "rx_total_bytes": 10, "tx_total_bytes": 20},
}
MOUNTS = [
    {"device": "nvme0n1", "label": "Samsung SSD", "model": "Samsung SSD 980", "transport": "nvme",
     "removable": False, "size_bytes": 1000204886016, "mount": "/, /boot/firmware", "mounts": ["/", "/boot/firmware"],
     "fstype": "ext4, vfat", "status": "ok", "percent": 42.0, "used_bytes": 420000000000,
     "total_bytes": 1000000000000, "free_bytes": 580000000000, "error": None},
    {"device": "sda", "label": "sda", "model": None, "transport": "usb", "removable": True,
     "size_bytes": 64000000000, "mount": None, "mounts": [], "fstype": None, "status": "unmounted",
     "percent": None, "used_bytes": None, "total_bytes": None, "free_bytes": None, "error": None},
]
INTERFACES = [
    {"name": "eth0", "ipv4": "192.0.2.10", "up": True, "speed_mbps": 1000, "rx_total_bytes": 1, "tx_total_bytes": 2},
    {"name": "br-1a947a33ee86", "ipv4": "172.20.0.1", "up": True, "speed_mbps": 10000,
     "rx_total_bytes": 1, "tx_total_bytes": 2},
]
TEMPERATURES = [{"name": "cpu_thermal:sensor 1", "current_c": 57.3, "high_c": None, "critical_c": 110.0}]
PORTS = [{"port": 22, "protocol": "tcp", "address": "0.0.0.0", "service": "sshd", "pid": 812},
         {"port": 22, "protocol": "tcp6", "address": "::", "service": "sshd", "pid": 812}]
DOCKER = {"available": True, "running": 2, "total": 3, "containers": [
    {"name": f"service-{i}", "image": "ghcr.io/example/image:latest", "state": "running" if i < 2 else "exited",
     "status": "Up 3 days (healthy)", "ports": "0.0.0.0:8080->80/tcp, [::]:8080->80/tcp"} for i in range(3)]}
PROXMOX_RAW = [
    {"version": "9.2.11", "release": "9.2"},
    [
        {"type": "node", "node": "proxmox", "status": "online", "cpu": 0.185, "maxcpu": 8,
         "mem": 13000000000, "maxmem": 32000000000, "uptime": 864000},
        {"type": "qemu", "vmid": 100, "name": "firewall", "node": "proxmox", "status": "running",
         "cpu": 0.12, "maxcpu": 2, "mem": 3, "maxmem": 8, "disk": 31, "maxdisk": 100,
         "netin": 1, "netout": 2, "uptime": 600},
        {"type": "lxc", "vmid": 101, "name": "dns", "node": "proxmox", "status": "stopped", "maxcpu": 1},
        {"type": "storage", "storage": "local-lvm", "node": "proxmox", "status": "available",
         "disk": 61, "maxdisk": 100, "shared": 0},
    ],
    [{"type": "vzdump", "status": "OK", "node": "proxmox", "user": "root@pam", "starttime": 1790000000,
      "endtime": 1790000600, "id": "100"},
     {"type": "vncshell", "status": "command failed: received interrupt", "node": "proxmox",
      "user": "root@pam", "starttime": 1789990000, "endtime": 1789990100}],
]


def build() -> dict:
    monitor._cache.clear()
    monitor._cache_status.clear()
    monitor._cache_retry_after.clear()
    monitor._history.clear()
    monitor._proxmox_history.clear()
    monitor._history.extend({"cpu": 8.0 + i % 11, "ram": 32.0 + i % 3, "temp": 50.0, "time": 1790000000 + i * 5}
                            for i in range(60))
    with patch.object(monitor, "sample_realtime", return_value=REALTIME), \
         patch.object(monitor, "block_devices", return_value=MOUNTS), \
         patch.object(monitor, "network_interfaces", return_value=INTERFACES), \
         patch.object(monitor, "all_temperatures", return_value=TEMPERATURES), \
         patch.object(monitor, "listening_ports", return_value=PORTS), \
         patch.object(monitor, "docker_containers", return_value=DOCKER), \
         patch.object(monitor, "proxmox_configured", return_value=True), \
         patch.object(monitor, "proxmox_fetch", return_value=PROXMOX_RAW):
        result = monitor.snapshot()
    result["sequence"] = 1
    result["server_time"] = 1790000300
    for item in result["proxmox"]["history"]:
        item["time"] = 1790000300
    for item in result["proxmox"]["tasks"]:
        item["started"] = "21/09 12:00"
    return result


if __name__ == "__main__":
    print(json.dumps(build(), indent=1, sort_keys=True))
