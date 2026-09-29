from __future__ import annotations

import json
import hmac
import hashlib
import http.client
import os
import shutil
import socket
import ssl
import subprocess
import time
from collections import deque
from contextlib import asynccontextmanager
from pathlib import Path
from threading import Event, Lock, Thread
from typing import Any, Callable
from urllib.parse import urlsplit

import psutil
from fastapi import Depends, FastAPI, Header, HTTPException
from fastapi.middleware.gzip import GZipMiddleware
from pydantic import BaseModel, Field

APP_VERSION = "1.0"
SCHEMA_VERSION = 2
API_TOKEN = os.getenv("MONITOR_API_TOKEN", "").strip()
if not API_TOKEN or API_TOKEN == "change-me":
    raise RuntimeError("MONITOR_API_TOKEN must be set to a non-default value")
PROXMOX_API_URL = os.getenv("PROXMOX_API_URL", "").strip().rstrip("/")
PROXMOX_TOKEN_ID = os.getenv("PROXMOX_TOKEN_ID", "").strip()
PROXMOX_TOKEN_SECRET = os.getenv("PROXMOX_TOKEN_SECRET", "").strip()
PROXMOX_CERT_SHA256 = os.getenv("PROXMOX_CERT_SHA256", "").replace(":", "").strip().lower()
STARTED_MONOTONIC = time.monotonic()
BOOT_TIME = psutil.boot_time()

HISTORY_INTERVAL_S = 5
_sampler_stop = Event()
_sampler_error: str | None = None
_bridge_labels: dict[str, str] = {}
_bridge_labels_lock = Lock()


def _record_history() -> None:
    global _sampler_error
    cpu = psutil.cpu_percent(interval=None)
    ram = psutil.virtual_memory().percent
    temp = cpu_temperature()
    with _sample_lock:
        _history.append({"cpu": round(cpu, 1), "ram": round(ram, 1),
                         "temp": temp or 0.0, "time": int(time.time())})
        _sampler_error = None


def _sample_loop() -> None:
    global _sampler_error
    while not _sampler_stop.is_set():
        try:
            _record_history()
        except Exception as exc:
            _sampler_error = str(exc)[:120]
        _sampler_stop.wait(HISTORY_INTERVAL_S)


def _bridge_label_loop() -> None:
    global _bridge_labels
    while not _sampler_stop.is_set():
        try:
            labels = docker_bridge_labels()
            with _bridge_labels_lock:
                _bridge_labels = labels
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
            pass  # Keep the last names if Docker is temporarily unavailable.
        _sampler_stop.wait(30)


@asynccontextmanager
async def lifespan(_: FastAPI):
    _sampler_stop.clear()
    sampler = Thread(target=_sample_loop, name="monitor-sampler", daemon=True)
    bridge_reader = Thread(target=_bridge_label_loop, name="docker-network-labels", daemon=True)
    sampler.start()
    bridge_reader.start()
    try:
        yield
    finally:
        _sampler_stop.set()
        sampler.join(timeout=2)
        bridge_reader.join(timeout=2)


app = FastAPI(title="Raspberry Pi Monitor API", version=APP_VERSION, docs_url=None, redoc_url=None, lifespan=lifespan)
app.add_middleware(GZipMiddleware, minimum_size=700)

_last_net = psutil.net_io_counters()
_last_disk = psutil.disk_io_counters()
_last_sample_time = time.monotonic()
_history: deque[dict[str, float]] = deque(maxlen=60)
_proxmox_history: deque[dict[str, float]] = deque(maxlen=60)
_sample_lock = Lock()
_cache: dict[str, tuple[float, Any]] = {}
_cache_status: dict[str, dict[str, Any]] = {}
_cache_retry_after: dict[str, float] = {}
_cache_lock = Lock()


class Health(BaseModel):
    ok: bool
    version: str
    schema_version: int = Field(alias="schema")
    server_time: int


def auth(x_api_key: str | None = Header(default=None)) -> None:
    if not x_api_key or not hmac.compare_digest(x_api_key, API_TOKEN):
        raise HTTPException(status_code=401, detail="invalid api key")


def cached(name: str, ttl: float, loader: Callable[[], Any]) -> Any:
    now = time.monotonic()
    with _cache_lock:
        previous = _cache.get(name)
        if previous and now - previous[0] < ttl:
            return previous[1]
        if now < _cache_retry_after.get(name, 0):
            if name in _cache_status and previous:
                _cache_status[name]["age_s"] = round(now - previous[0], 1)
            return previous[1] if previous else None
        try:
            value = loader()
        except Exception as exc:
            age = round(now - previous[0], 1) if previous else None
            _cache_status[name] = {"ok": False, "stale": previous is not None,
                                   "age_s": age, "error": str(exc)[:120]}
            _cache_retry_after[name] = now + min(ttl, 5)
            return previous[1] if previous else None
        _cache[name] = (now, value)
        _cache_retry_after.pop(name, None)
        _cache_status[name] = {"ok": True, "stale": False, "age_s": 0}
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
    return output


def cpu_frequency() -> int | None:
    freq = psutil.cpu_freq()
    return round(freq.current) if freq else None


def block_devices() -> list[dict[str, Any]]:
    """Return one entry per real disk, aggregating its mounted filesystems."""
    command = [
        "lsblk", "--json", "--bytes",
        "--output", "NAME,KNAME,PATH,TYPE,SIZE,PKNAME,MODEL,TRAN,RM",
    ]
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=3, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"lsblk: {exc}") from exc
    if completed.returncode != 0:
        message = completed.stderr.strip().splitlines()
        raise RuntimeError((message[-1] if message else "lsblk no disponible")[:80])
    try:
        roots = json.loads(completed.stdout).get("blockdevices", [])
    except (json.JSONDecodeError, AttributeError) as exc:
        raise RuntimeError(f"lsblk JSON: {exc}") from exc

    physical: list[tuple[dict[str, Any], set[str]]] = []

    def collect_paths(node: dict[str, Any], paths: set[str]) -> None:
        for key in ("path", "name", "kname"):
            value = str(node.get(key) or "").strip()
            if not value:
                continue
            path = value if value.startswith("/dev/") else f"/dev/{value}"
            paths.add(path)
            paths.add(os.path.realpath(path))
        for child in node.get("children") or []:
            collect_paths(child, paths)

    for root in roots:
        name = str(root.get("name") or "")
        if root.get("type") != "disk" or name.startswith(("loop", "ram", "zram")):
            continue
        paths: set[str] = set()
        collect_paths(root, paths)
        physical.append((root, paths))

    ignored_fs = {"tmpfs", "devtmpfs", "squashfs", "overlay", "proc", "sysfs", "cgroup2"}
    partitions = []
    for part in psutil.disk_partitions(all=True):
        if part.fstype in ignored_fs or not part.device.startswith("/dev/"):
            continue
        device_paths = {part.device, os.path.realpath(part.device)}
        owner = next((index for index, (_, paths) in enumerate(physical) if paths & device_paths), None)
        if owner is not None:
            partitions.append((owner, part))

    result: list[dict[str, Any]] = []
    for index, (device, _) in enumerate(physical):
        device_parts = [part for owner, part in partitions if owner == index]
        # A filesystem mounted more than once must only contribute once.
        unique_parts: dict[str, Any] = {}
        for part in sorted(device_parts, key=lambda item: (item.mountpoint != "/", len(item.mountpoint))):
            key = os.path.realpath(part.device)
            unique_parts.setdefault(key, part)

        used = total = free = 0
        errors: list[str] = []
        mounts: list[str] = []
        filesystems: list[str] = []
        for part in unique_parts.values():
            mounts.append(part.mountpoint)
            if part.fstype and part.fstype not in filesystems:
                filesystems.append(part.fstype)
            try:
                usage = psutil.disk_usage(part.mountpoint)
            except (PermissionError, OSError) as exc:
                errors.append(f"{part.mountpoint}: {exc}")
                continue
            used += usage.used
            total += usage.total
            free += usage.free

        name = str(device.get("name") or device.get("kname") or "disco")
        model = " ".join(str(device.get("model") or "").split())
        status = "error" if errors else ("ok" if unique_parts else "unmounted")
        result.append({
            "device": name,
            "label": (model or name)[:32],
            "model": model[:48] or None,
            "transport": device.get("tran"),
            "removable": bool(device.get("rm")),
            "size_bytes": int(device.get("size") or 0),
            "mount": ", ".join(mounts)[:48] or None,
            "mounts": mounts,
            "fstype": ", ".join(filesystems) or None,
            "status": status,
            "percent": round((used * 100.0) / total, 1) if status == "ok" and total else None,
            "used_bytes": used if status == "ok" else None,
            "total_bytes": total if status == "ok" else None,
            "free_bytes": free if status == "ok" else None,
            "error": "; ".join(errors)[:120] or None,
        })

    result.sort(key=lambda item: ("/" not in item["mounts"], item["removable"], item["device"]))
    return result


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
    return result


def docker_bridge_labels() -> dict[str, str]:
    if shutil.which("docker") is None:
        return {}
    listed = subprocess.run(
        ["docker", "network", "ls", "--no-trunc", "--filter", "driver=bridge", "--format", "{{json .}}"],
        capture_output=True, text=True, timeout=3, check=False,
    )
    if listed.returncode != 0:
        raise RuntimeError("docker network ls failed")
    networks = []
    for line in listed.stdout.splitlines():
        try:
            network = json.loads(line)
        except json.JSONDecodeError:
            continue
        network_id = str(network.get("ID") or "")
        if len(network_id) >= 12 and network.get("Name") != "bridge":
            networks.append(network_id)
    if not networks:
        return {}
    inspected = subprocess.run(
        ["docker", "network", "inspect", *networks],
        capture_output=True, text=True, timeout=3, check=False,
    )
    if inspected.returncode != 0:
        raise RuntimeError("docker network inspect failed")
    labels = {}
    for network in json.loads(inspected.stdout):
        network_id = str(network.get("Id") or "")
        if len(network_id) < 12:
            continue
        services = sorted({str(item.get("Name") or "").strip() for item in
                           (network.get("Containers") or {}).values()} - {""})
        label = services[0] if services else str(network.get("Name") or "red")
        if len(services) > 1:
            label += f" +{len(services) - 1}"
        labels[f"br-{network_id[:12]}"] = f"Docker / {label[:28]}"
    return labels


def named_interfaces(interfaces: list[dict[str, Any]]) -> list[dict[str, Any]]:
    with _bridge_labels_lock:
        labels = dict(_bridge_labels)
    return [{**item, "display_name": labels[item["name"]]} if item["name"] in labels else item
            for item in interfaces]


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
    except (psutil.AccessDenied, OSError) as exc:
        raise RuntimeError(f"puertos: {exc}") from exc
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
    return sorted(unique.values(), key=lambda item: (item["port"], item["service"]))


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
        "containers": containers,
    }


def proxmox_configured() -> bool:
    return all((PROXMOX_API_URL, PROXMOX_TOKEN_ID, PROXMOX_TOKEN_SECRET,
                PROXMOX_CERT_SHA256))


class ProxmoxOfflineError(RuntimeError):
    """The optional Proxmox host cannot currently be reached."""


def proxmox_request(path: str) -> Any:
    """Read one Proxmox endpoint while pinning the node certificate."""
    if not proxmox_configured():
        raise RuntimeError("Proxmox no configurado")
    endpoint = urlsplit(PROXMOX_API_URL)
    if endpoint.scheme != "https" or not endpoint.hostname:
        raise RuntimeError("URL Proxmox invalida")
    if len(PROXMOX_CERT_SHA256) != 64:
        raise RuntimeError("huella TLS Proxmox invalida")
    base_path = endpoint.path.rstrip("/")
    request_path = f"{base_path}/{path.lstrip('/')}"
    connection = http.client.HTTPSConnection(
        endpoint.hostname, endpoint.port or 443,
        timeout=2.5, context=ssl._create_unverified_context(),
    )
    try:
        connection.connect()
        certificate = connection.sock.getpeercert(binary_form=True) if connection.sock else b""
        fingerprint = hashlib.sha256(certificate).hexdigest()
        if not hmac.compare_digest(fingerprint, PROXMOX_CERT_SHA256):
            raise RuntimeError("certificado Proxmox no coincide")
        connection.request("GET", request_path, headers={
            "Authorization": f"PVEAPIToken={PROXMOX_TOKEN_ID}={PROXMOX_TOKEN_SECRET}",
            "Accept": "application/json",
        })
        response = connection.getresponse()
        payload = response.read()
        if response.status != 200:
            raise RuntimeError(f"Proxmox HTTP {response.status}")
        decoded = json.loads(payload)
        if "data" not in decoded:
            raise RuntimeError("respuesta Proxmox incompleta")
        return decoded["data"]
    except ssl.SSLError as exc:
        raise RuntimeError(f"Proxmox: {exc}") from exc
    except OSError as exc:
        raise ProxmoxOfflineError("Proxmox sin conexion") from exc
    except (json.JSONDecodeError, http.client.HTTPException) as exc:
        raise RuntimeError(f"Proxmox: {exc}") from exc
    finally:
        connection.close()


def _percent(used: Any, maximum: Any) -> float | None:
    try:
        maximum_value = float(maximum)
        if maximum_value <= 0:
            return None
        return round(float(used) * 100.0 / maximum_value, 1)
    except (TypeError, ValueError):
        return None


_PROXMOX_TASK_LABELS = {
    "clone": "Clonar maquina",
    "qmcreate": "Crear VM",
    "qmdestroy": "Eliminar VM",
    "qmigrate": "Migrar VM",
    "qmreboot": "Reiniciar VM",
    "qmshutdown": "Apagar VM",
    "qmstart": "Iniciar VM",
    "qmstop": "Detener VM",
    "startall": "Inicio general",
    "stopall": "Apagado general",
    "vncproxy": "Consola VM",
    "vncshell": "Consola del nodo",
    "vzcreate": "Crear LXC",
    "vzdestroy": "Eliminar LXC",
    "vzdump": "Copia de seguridad",
    "vzmigrate": "Migrar LXC",
    "vzreboot": "Reiniciar LXC",
    "vzshutdown": "Apagar LXC",
    "vzstart": "Iniciar LXC",
    "vzstop": "Detener LXC",
}


def _proxmox_task(item: dict[str, Any]) -> dict[str, Any]:
    task_type = str(item.get("type") or "--").lower()
    raw_status = str(item.get("status") or "running")
    status = raw_status.lower()
    if status == "ok":
        state = "OK"
        detail = "Completada correctamente"
    elif status in ("running", "stopped") and not item.get("endtime"):
        state = "RUNNING"
        detail = "En curso"
    elif "received interrupt" in status and task_type in ("vncshell", "vncproxy"):
        state = "CANCELLED"
        detail = "Consola cerrada por el usuario"
    else:
        state = "ERROR"
        detail = raw_status
    started = int(item.get("starttime") or 0)
    return {
        "type": task_type[:28],
        "label": _PROXMOX_TASK_LABELS.get(task_type, task_type or "Tarea")[:32],
        "state": state,
        "status": raw_status[:96],
        "detail": detail[:96],
        "id": str(item.get("id") or "")[:24],
        "node": str(item.get("node") or "--")[:32],
        "user": str(item.get("user") or "--")[:36],
        "start_time": started,
        "started": time.strftime("%d/%m %H:%M", time.localtime(started)) if started else "--",
    }


def _proxmox_offline() -> dict[str, Any]:
    with _sample_lock:
        _proxmox_history.clear()
    return {
        "available": False,
        "state": "offline",
        "error": "Proxmox sin conexion",
        "version": None,
        "nodes_online": 0,
        "nodes_total": 0,
        "guests_running": 0,
        "guests_total": 0,
        "tasks_failed": 0,
        "tasks_cancelled": 0,
        "history": [],
        "history_interval_ms": 8000,
        "nodes": [],
        "guests": [],
        "storage": [],
        "tasks": [],
    }


def proxmox_stats() -> dict[str, Any]:
    if not proxmox_configured():
        return {**_proxmox_offline(), "state": "unconfigured", "error": "no configurado"}
    try:
        version = proxmox_request("version")
        resources = proxmox_request("cluster/resources")
        tasks_raw = proxmox_request("cluster/tasks")
    except ProxmoxOfflineError:
        return _proxmox_offline()

    nodes: list[dict[str, Any]] = []
    guests: list[dict[str, Any]] = []
    storage: list[dict[str, Any]] = []
    for item in resources:
        kind = str(item.get("type") or "")
        if kind == "node":
            nodes.append({
                "name": str(item.get("node") or item.get("id") or "--")[:32],
                "status": str(item.get("status") or "unknown"),
                "cpu_percent": round(float(item.get("cpu") or 0) * 100, 1),
                "cpu_total": int(item.get("maxcpu") or 0),
                "memory_percent": _percent(item.get("mem"), item.get("maxmem")),
                "memory_used_bytes": item.get("mem"),
                "memory_total_bytes": item.get("maxmem"),
                "uptime_s": int(item.get("uptime") or 0),
            })
        elif kind in ("qemu", "lxc"):
            guests.append({
                "id": int(item.get("vmid") or 0),
                "name": str(item.get("name") or f"{kind}-{item.get('vmid', '--')}")[:36],
                "type": "VM" if kind == "qemu" else "LXC",
                "node": str(item.get("node") or "--")[:32],
                "status": str(item.get("status") or "unknown"),
                "cpu_percent": round(float(item.get("cpu") or 0) * 100, 1),
                "cpu_total": int(item.get("maxcpu") or 0),
                "memory_percent": _percent(item.get("mem"), item.get("maxmem")),
                "memory_used_bytes": item.get("mem"),
                "memory_total_bytes": item.get("maxmem"),
                "disk_percent": _percent(item.get("disk"), item.get("maxdisk")),
                "disk_used_bytes": item.get("disk"),
                "disk_total_bytes": item.get("maxdisk"),
                "net_in_bytes": item.get("netin"),
                "net_out_bytes": item.get("netout"),
                "uptime_s": int(item.get("uptime") or 0),
            })
        elif kind == "storage":
            storage.append({
                "name": str(item.get("storage") or item.get("id") or "--")[:36],
                "node": str(item.get("node") or "--")[:32],
                "status": str(item.get("status") or "unknown"),
                "shared": bool(item.get("shared")),
                "percent": _percent(item.get("disk"), item.get("maxdisk")),
                "used_bytes": item.get("disk"),
                "total_bytes": item.get("maxdisk"),
            })
    guests.sort(key=lambda item: (item["status"] != "running", item["id"]))
    storage.sort(key=lambda item: (item["status"] != "available", item["name"]))
    tasks = [_proxmox_task(item) for item in tasks_raw[:20]]
    task_priority = {"ERROR": 0, "CANCELLED": 1, "RUNNING": 2, "OK": 3}
    tasks.sort(key=lambda item: (task_priority.get(item["state"], 4),
                                 -item["start_time"]))
    failed_tasks = sum(1 for item in tasks if item["state"] == "ERROR")
    cancelled_tasks = sum(1 for item in tasks if item["state"] == "CANCELLED")
    primary_node = nodes[0] if nodes else None
    if primary_node and primary_node["cpu_percent"] is not None and \
            primary_node["memory_percent"] is not None:
        with _sample_lock:
            _proxmox_history.append({
                "cpu": primary_node["cpu_percent"],
                "ram": primary_node["memory_percent"],
                "time": int(time.time()),
            })
            proxmox_history = list(_proxmox_history)
    else:
        with _sample_lock:
            proxmox_history = list(_proxmox_history)
    return {
        "available": True,
        "state": "online",
        "version": str(version.get("version") or version.get("release") or "--")[:24],
        "nodes_online": sum(1 for item in nodes if item["status"] == "online"),
        "nodes_total": len(nodes),
        "guests_running": sum(1 for item in guests if item["status"] == "running"),
        "guests_total": len(guests),
        "tasks_failed": failed_tasks,
        "tasks_cancelled": cancelled_tasks,
        "history": proxmox_history,
        "history_interval_ms": 8000,
        "nodes": nodes,
        "guests": guests,
        "storage": storage,
        "tasks": tasks,
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
        try:
            root_disk = psutil.disk_usage("/")
            root_disk_error = None
        except (PermissionError, OSError) as exc:
            root_disk = None
            root_disk_error = str(exc)[:80]
        load1, load5, load15 = os.getloadavg()
        net = psutil.net_io_counters()
        try:
            disk_io = psutil.disk_io_counters()
            disk_io_error = None if disk_io is not None else "contadores no disponibles"
        except (PermissionError, OSError) as exc:
            disk_io = None
            disk_io_error = str(exc)[:80]
        net_rx = max(0.0, (net.bytes_recv - _last_net.bytes_recv) / elapsed)
        net_tx = max(0.0, (net.bytes_sent - _last_net.bytes_sent) / elapsed)
        read_rate = write_rate = 0.0
        if _last_disk and disk_io:
            read_rate = max(0.0, (disk_io.read_bytes - _last_disk.read_bytes) / elapsed)
            write_rate = max(0.0, (disk_io.write_bytes - _last_disk.write_bytes) / elapsed)
        _last_net, _last_disk, _last_sample_time = net, disk_io, now_mono
        temp = cpu_temperature()
    disk = {
        "status": "ok" if root_disk is not None else "error",
        "error": root_disk_error,
        "percent": round(root_disk.percent, 1) if root_disk is not None else None,
        "used_bytes": root_disk.used if root_disk is not None else None,
        "total_bytes": root_disk.total if root_disk is not None else None,
        "free_bytes": root_disk.free if root_disk is not None else None,
        "io_status": "ok" if disk_io is not None else "error",
        "io_error": disk_io_error,
        "read_bps": round(read_rate) if disk_io is not None else None,
        "write_bps": round(write_rate) if disk_io is not None else None,
    }
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
        "disk": disk,
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
    mounts = cached("block_devices", 15.0, block_devices)
    interfaces = cached("interfaces", 10.0, network_interfaces)
    temperatures = cached("temperatures", 5.0, all_temperatures)
    ports = cached("ports", 8.0, listening_ports)
    docker = cached("docker", 8.0, docker_containers)
    proxmox = cached("proxmox", 8.0, proxmox_stats)
    with _cache_lock:
        collection_status = {name: dict(state) for name, state in _cache_status.items()}
    if docker and not docker.get("available") and docker.get("error") != "docker no instalado":
        collection_status["docker"] = {"ok": False, "stale": False, "age_s": None,
                                       "error": docker.get("error", "docker no disponible")}
    if _sampler_error:
        collection_status["history"] = {"ok": False, "stale": bool(_history),
                                        "age_s": None, "error": _sampler_error}
    with _sample_lock:
        history = list(_history)
    return {
        "schema": SCHEMA_VERSION,
        "sequence": int(time.monotonic() * 1000),
        "server_time": int(time.time()),
        **realtime,
        "storage": {"mounts": mounts or []},
        "interfaces": named_interfaces(interfaces or []),
        "temperatures": temperatures or [],
        "ports": ports or [],
        "docker": docker or {"available": False, "error": "lectura no disponible", "containers": []},
        "proxmox": proxmox or {"available": False, "error": "lectura no disponible",
                               "nodes": [], "guests": [], "storage": [], "tasks": []},
        "collection_status": collection_status,
        "history_interval_ms": HISTORY_INTERVAL_S * 1000,
        "history": history,
    }


@app.get("/api/v1/stats")
def legacy_stats(_: None = Depends(auth)) -> dict[str, Any]:
    """Compatibilidad temporal con firmware 1.x."""
    realtime = sample_realtime()
    with _sample_lock:
        history = list(_history)
    return {
        "schema": 1,
        "sequence": int(time.monotonic() * 1000),
        "server_time": int(time.time()),
        **realtime,
        "history": history,
    }
