from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import time
from collections import deque
from pathlib import Path
from threading import Lock
from typing import Any, Callable

import psutil
from fastapi import Depends, FastAPI, Header, HTTPException
from fastapi.middleware.gzip import GZipMiddleware
from pydantic import BaseModel

APP_VERSION = "3.0.0"
SCHEMA_VERSION = 2
API_TOKEN = os.getenv("MONITOR_API_TOKEN", "change-me")
STARTED_MONOTONIC = time.monotonic()
BOOT_TIME = psutil.boot_time()

app = FastAPI(title="Raspberry Pi Monitor API", version=APP_VERSION, docs_url=None, redoc_url=None)
app.add_middleware(GZipMiddleware, minimum_size=700)

_last_net = psutil.net_io_counters()
_last_disk = psutil.disk_io_counters()
_last_sample_time = time.monotonic()
_history: deque[dict[str, float]] = deque(maxlen=60)
_sample_lock = Lock()
_cache: dict[str, tuple[float, Any]] = {}


class Health(BaseModel):
    ok: bool
    version: str
    schema: int
    server_time: int


def auth(x_api_key: str | None = Header(default=None)) -> None:
    if API_TOKEN and x_api_key != API_TOKEN:
        raise HTTPException(status_code=401, detail="invalid api key")


def cached(name: str, ttl: float, loader: Callable[[], Any]) -> Any:
    now = time.monotonic()
    previous = _cache.get(name)
    if previous and now - previous[0] < ttl:
        return previous[1]
    try:
        value = loader()
    except Exception:
        value = previous[1] if previous else []
    _cache[name] = (now, value)
    return value


def local_ip() -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("1.1.1.1", 80))
        return sock.getsockname()[0]
    except OSError:
        return "0.0.0.0"
    finally:
        sock.close()


def cpu_temperature() -> float | None:
    try:
        temps = psutil.sensors_temperatures(fahrenheit=False)
        for name in ("cpu_thermal", "coretemp", "soc_thermal"):
            if name in temps and temps[name]:
                return round(float(temps[name][0].current), 1)
        thermal = Path("/sys/class/thermal/thermal_zone0/temp")
        if thermal.exists():
            return round(int(thermal.read_text().strip()) / 1000.0, 1)
    except (OSError, ValueError, IndexError):
        pass
    return None


def all_temperatures() -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    try:
        for chip, entries in psutil.sensors_temperatures(fahrenheit=False).items():
            for index, entry in enumerate(entries):
                label = entry.label or f"sensor {index + 1}"
                output.append({
                    "name": f"{chip}:{label}"[:30],
                    "current_c": round(float(entry.current), 1),
                    "high_c": round(float(entry.high), 1) if entry.high is not None else None,
                    "critical_c": round(float(entry.critical), 1) if entry.critical is not None else None,
                })
    except (OSError, ValueError):
        pass
    if not output:
        temp = cpu_temperature()
        if temp is not None:
            output.append({"name": "CPU", "current_c": temp, "high_c": 80.0, "critical_c": 85.0})
    return output[:8]


def cpu_frequency() -> int | None:
    freq = psutil.cpu_freq()
    return round(freq.current) if freq else None


def mount_points() -> list[dict[str, Any]]:
    mounts: list[dict[str, Any]] = []
    ignored = {"tmpfs", "devtmpfs", "squashfs", "overlay", "proc", "sysfs", "cgroup2"}
    seen: set[str] = set()
    for part in psutil.disk_partitions(all=False):
        if part.fstype in ignored or part.mountpoint in seen:
            continue
        seen.add(part.mountpoint)
        try:
            usage = psutil.disk_usage(part.mountpoint)
        except (PermissionError, OSError):
            continue
        mounts.append({
            "device": Path(part.device).name or part.device,
            "mount": part.mountpoint,
            "fstype": part.fstype,
            "percent": round(usage.percent, 1),
            "used_bytes": usage.used,
            "total_bytes": usage.total,
            "free_bytes": usage.free,
        })
    mounts.sort(key=lambda item: (item["mount"] != "/", item["mount"]))
    return mounts[:8]


def network_interfaces() -> list[dict[str, Any]]:
    addresses = psutil.net_if_addrs()
    stats = psutil.net_if_stats()
    counters = psutil.net_io_counters(pernic=True)
    result: list[dict[str, Any]] = []
    for name, addr_list in addresses.items():
        if name == "lo":
            continue
        ipv4 = next((a.address for a in addr_list if a.family == socket.AF_INET), "")
        stat = stats.get(name)
        io = counters.get(name)
        if not ipv4 and not (stat and stat.isup):
            continue
        result.append({
            "name": name,
            "ipv4": ipv4 or "--",
            "up": bool(stat and stat.isup),
            "speed_mbps": int(stat.speed) if stat and stat.speed > 0 else None,
            "rx_total_bytes": io.bytes_recv if io else 0,
            "tx_total_bytes": io.bytes_sent if io else 0,
        })
    result.sort(key=lambda item: (not item["up"], item["name"]))
    return result[:8]


def process_name(pid: int | None) -> str:
    if not pid:
        return "kernel/unknown"
    try:
        return psutil.Process(pid).name()[:28]
    except (psutil.Error, OSError):
        return "unknown"


def listening_ports() -> list[dict[str, Any]]:
    ports: list[dict[str, Any]] = []
    try:
        connections = psutil.net_connections(kind="inet")
    except (psutil.AccessDenied, OSError):
        connections = []
    for conn in connections:
        if conn.status != psutil.CONN_LISTEN or not conn.laddr:
            continue
        address = conn.laddr.ip if hasattr(conn.laddr, "ip") else conn.laddr[0]
        port = conn.laddr.port if hasattr(conn.laddr, "port") else conn.laddr[1]
        protocol = "tcp6" if ":" in address else "tcp"
        ports.append({
            "port": int(port),
            "protocol": protocol,
            "address": address,
            "service": process_name(conn.pid),
            "pid": conn.pid,
        })
    unique = {(p["protocol"], p["address"], p["port"], p["service"]): p for p in ports}
    return sorted(unique.values(), key=lambda item: (item["port"], item["service"]))[:16]


def docker_containers() -> dict[str, Any]:
    if shutil.which("docker") is None:
        return {"available": False, "error": "docker no instalado", "containers": []}
    command = [
        "docker", "ps", "-a", "--no-trunc",
        "--format", "{{json .}}",
    ]
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=3, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return {"available": False, "error": "docker no disponible", "containers": []}
    if completed.returncode != 0:
        message = completed.stderr.strip().splitlines()
        return {"available": False, "error": (message[-1] if message else "sin permisos")[:80], "containers": []}
    containers: list[dict[str, Any]] = []
    for line in completed.stdout.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        state = str(item.get("State", "unknown")).lower()
        containers.append({
            "name": str(item.get("Names", "--"))[:28],
            "image": str(item.get("Image", "--"))[:36],
            "state": state,
            "status": str(item.get("Status", "--"))[:48],
            "ports": str(item.get("Ports", ""))[:64],
        })
    containers.sort(key=lambda item: (item["state"] != "running", item["name"]))
    return {
        "available": True,
        "running": sum(1 for item in containers if item["state"] == "running"),
        "total": len(containers),
        "containers": containers[:12],
    }


def sample_realtime() -> dict[str, Any]:
    global _last_net, _last_disk, _last_sample_time
    with _sample_lock:
        now_mono = time.monotonic()
        elapsed = max(now_mono - _last_sample_time, 0.001)
        cpu_total = psutil.cpu_percent(interval=None)
        cpu_per_core = psutil.cpu_percent(interval=None, percpu=True)
        memory = psutil.virtual_memory()
        swap = psutil.swap_memory()
        root_disk = psutil.disk_usage("/")
        load1, load5, load15 = os.getloadavg()
        net = psutil.net_io_counters()
        disk_io = psutil.disk_io_counters()
        net_rx = max(0.0, (net.bytes_recv - _last_net.bytes_recv) / elapsed)
        net_tx = max(0.0, (net.bytes_sent - _last_net.bytes_sent) / elapsed)
        read_rate = write_rate = 0.0
        if _last_disk and disk_io:
            read_rate = max(0.0, (disk_io.read_bytes - _last_disk.read_bytes) / elapsed)
            write_rate = max(0.0, (disk_io.write_bytes - _last_disk.write_bytes) / elapsed)
        _last_net, _last_disk, _last_sample_time = net, disk_io, now_mono

    temp = cpu_temperature()
    _history.append({"cpu": round(cpu_total, 1), "ram": round(memory.percent, 1), "temp": temp or 0.0})
    return {
        "sample_interval_ms": round(elapsed * 1000),
        "system": {
            "hostname": socket.gethostname(), "ip": local_ip(),
            "uptime_s": int(time.time() - BOOT_TIME),
            "agent_uptime_s": int(now_mono - STARTED_MONOTONIC),
        },
        "cpu": {
            "percent": round(cpu_total, 1), "per_core": [round(v, 1) for v in cpu_per_core],
            "temperature_c": temp, "frequency_mhz": cpu_frequency(),
            "load": [round(load1, 2), round(load5, 2), round(load15, 2)],
        },
        "memory": {
            "percent": round(memory.percent, 1), "used_bytes": memory.used,
            "total_bytes": memory.total, "available_bytes": memory.available,
            "swap_percent": round(swap.percent, 1), "swap_used_bytes": swap.used,
            "swap_total_bytes": swap.total,
        },
        "disk": {
            "percent": round(root_disk.percent, 1), "used_bytes": root_disk.used,
            "total_bytes": root_disk.total, "free_bytes": root_disk.free,
            "read_bps": round(read_rate), "write_bps": round(write_rate),
        },
        "network": {
            "rx_bps": round(net_rx), "tx_bps": round(net_tx),
            "rx_total_bytes": net.bytes_recv, "tx_total_bytes": net.bytes_sent,
        },
    }


@app.get("/health", response_model=Health)
def health(_: None = Depends(auth)) -> Health:
    return Health(ok=True, version=APP_VERSION, schema=SCHEMA_VERSION, server_time=int(time.time()))


@app.get("/api/v2/snapshot")
def snapshot(_: None = Depends(auth)) -> dict[str, Any]:
    realtime = sample_realtime()
    return {
        "schema": SCHEMA_VERSION,
        "sequence": int(time.monotonic() * 1000),
        "server_time": int(time.time()),
        **realtime,
        "storage": {"mounts": cached("mounts", 15.0, mount_points)},
        "interfaces": cached("interfaces", 10.0, network_interfaces),
        "temperatures": cached("temperatures", 5.0, all_temperatures),
        "ports": cached("ports", 8.0, listening_ports),
        "docker": cached("docker", 8.0, docker_containers),
        "history": list(_history),
    }


@app.get("/api/v1/stats")
def legacy_stats(_: None = Depends(auth)) -> dict[str, Any]:
    """Compatibilidad temporal con firmware 1.x."""
    realtime = sample_realtime()
    return {
        "schema": 1,
        "sequence": int(time.monotonic() * 1000),
        "server_time": int(time.time()),
        **realtime,
        "history": list(_history),
    }
