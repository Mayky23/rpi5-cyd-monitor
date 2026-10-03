#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>
#include <cmath>

namespace Panel {
inline bool number(JsonVariantConst value) {
  return value.is<double>() && std::isfinite(value.as<double>());
}

struct Scale { float low, high; };

inline Scale historyScale(JsonArray history, const char *field) {
  float low = 100, high = 0;
  bool found = false;
  for (JsonObject item : history) {
    if (!number(item[field])) continue;
    const float value = constrain(item[field].as<float>(), 0.0f, 100.0f);
    low = min(low, value); high = max(high, value); found = true;
  }
  if (!found) return {0, 100};
  const float padding = max(1.0f, (high - low) * 0.18f);
  const float range = max(4.0f, high - low + padding * 2);
  const float start = max(0.0f, min(100.0f - range, (high + low - range) * 0.5f));
  return {floorf(start), min(100.0f, ceilf(start + range))};
}

inline int interfaceOrder(const String &name) {
  if (name.startsWith("eth") || name.startsWith("en")) return 0;
  if (name.startsWith("wl")) return 1;
  if (name.startsWith("tailscale") || name.startsWith("wg") || name.startsWith("tun")) return 2;
  if (name == "docker0" || name.startsWith("br-")) return 4;
  if (name.startsWith("veth")) return 5;
  return 3;
}

inline String interfaceName(const String &name) {
  const int order = interfaceOrder(name);
  if (order == 0) return "Ethernet / " + name;
  if (order == 1) return "WiFi / " + name;
  if (order == 2) return "VPN / " + name;
  if (name == "docker0") return "Docker / principal";
  if (name.startsWith("br-")) return "Docker / red";
  if (name.startsWith("veth")) return "Virtual / " + name.substring(4, 10);
  return name;
}

inline std::vector<JsonObject> interfaces(JsonArray source) {
  std::vector<JsonObject> rows;
  for (JsonObject item : source) rows.push_back(item);
  std::stable_sort(rows.begin(), rows.end(), [](JsonObject a, JsonObject b) {
    const int left = interfaceOrder(a["name"] | "");
    const int right = interfaceOrder(b["name"] | "");
    if (left != right) return left < right;
    return String(a["name"] | "") < String(b["name"] | "");
  });
  return rows;
}

inline String sensorName(String name, size_t index) {
  String key = name;
  key.toLowerCase();
  if (key.startsWith("cpu") || key.startsWith("soc") || key.startsWith("bcm")) return "CPU";
  if (key.startsWith("rp1")) return "Chip RP1";
  if (key.startsWith("nvme")) return "SSD NVMe";
  if (key.startsWith("coretemp")) return "CPU Intel";
  const int colon = name.indexOf(':');
  if (colon >= 0) {
    const String label = name.substring(colon + 1);
    name = label.startsWith("sensor ") ? name.substring(0, colon) : label;
  }
  name.replace("_", " ");
  return name.length() ? name : "Sensor " + String(index + 1);
}

inline String serviceName(const String &name) {
  if (name == "sshd") return "SSH";
  if (name == "pihole-FTL") return "Pi-hole";
  if (name == "docker-proxy") return "Docker";
  if (name == "rpcbind") return "RPC";
  if (name == "cupsd") return "Impresion";
  return name;
}

struct Port {
  unsigned number;
  String protocol, service, address;
  bool v4, v6;
};

inline std::vector<Port> ports(JsonArray source) {
  std::vector<Port> rows;
  for (JsonObject item : source) {
    String protocol = item["protocol"] | "tcp";
    String address = item["address"] | "*";
    const bool v6 = address.indexOf(':') >= 0 || protocol.endsWith("6");
    if (protocol.endsWith("6")) protocol.remove(protocol.length() - 1);
    if (address == "*" || address == "0.0.0.0" || address == "::") address = "Todas las IP";
    else if (address.startsWith("127.") || address == "::1") address = "Solo esta RPi";
    const String service = item["service"] | "Desconocido";
    const unsigned number = item["port"] | 0u;
    auto match = std::find_if(rows.begin(), rows.end(), [&](const Port &row) {
      return row.number == number && row.protocol == protocol && row.service == service && row.address == address;
    });
    if (match == rows.end()) {
      rows.push_back({number, protocol, service, address, !v6, v6});
    } else {
      match->v4 |= !v6;
      match->v6 |= v6;
    }
  }
  std::stable_sort(rows.begin(), rows.end(), [](const Port &a, const Port &b) { return a.number < b.number; });
  return rows;
}

// Only the fields the panel draws are kept, so long Docker/port/Proxmox lists
// fit in the ESP32 heap (two snapshots coexist while a new one is parsed).
inline void buildSnapshotFilter(JsonDocument &filter) {
  filter.clear();
  filter["schema"] = true;
  filter["system"]["hostname"] = true;
  filter["system"]["ip"] = true;
  filter["system"]["uptime_s"] = true;
  for (const char *field : {"percent", "temperature_c", "frequency_mhz", "load", "per_core"})
    filter["cpu"][field] = true;
  filter["memory"]["percent"] = true;
  for (const char *field : {"status", "percent", "free_bytes", "io_status", "read_bps", "write_bps"})
    filter["disk"][field] = true;
  filter["network"]["rx_bps"] = true;
  filter["network"]["tx_bps"] = true;
  for (const char *field : {"device", "mount", "status", "percent", "used_bytes", "total_bytes", "size_bytes"})
    filter["storage"]["mounts"][0][field] = true;
  for (const char *field : {"name", "display_name", "ipv4", "up", "speed_mbps"})
    filter["interfaces"][0][field] = true;
  filter["temperatures"][0]["name"] = true;
  filter["temperatures"][0]["current_c"] = true;
  for (const char *field : {"port", "protocol", "address", "service"})
    filter["ports"][0][field] = true;
  for (const char *field : {"available", "running", "total", "error"})
    filter["docker"][field] = true;
  filter["docker"]["containers"][0]["name"] = true;
  filter["docker"]["containers"][0]["state"] = true;
  JsonObject pve = filter["proxmox"].to<JsonObject>();
  for (const char *field : {"available", "state", "error", "guests_running", "guests_total",
                            "tasks_failed", "tasks_cancelled", "history_interval_ms"})
    pve[field] = true;
  pve["history"][0]["cpu"] = true;
  pve["history"][0]["ram"] = true;
  for (const char *field : {"status", "cpu_percent", "memory_percent", "uptime_s"})
    pve["nodes"][0][field] = true;
  for (const char *field : {"id", "name", "type", "status", "cpu_percent", "memory_percent", "disk_percent"})
    pve["guests"][0][field] = true;
  for (const char *field : {"name", "status", "percent", "used_bytes", "total_bytes"})
    pve["storage"][0][field] = true;
  for (const char *field : {"label", "state", "started", "detail"})
    pve["tasks"][0][field] = true;
  filter["collection_status"]["*"]["ok"] = true;
  filter["history_interval_ms"] = true;
  filter["history"][0]["cpu"] = true;
  filter["history"][0]["ram"] = true;
}

// A failed collector only turns the status into PARCIAL when this edition shows it.
// profile: 0 = combined, 1 = Raspberry Pi only, 2 = Proxmox only.
inline bool sectionShown(int profile, const char *section) {
  const bool proxmox = strcmp(section, "proxmox") == 0;
  if (profile == 1) return !proxmox;
  if (profile == 2) return proxmox;
  return true;
}

// Splits "https://host[:port][/path]" for the certificate-pinned connection.
inline bool httpsEndpoint(const String &url, String &host, uint16_t &port) {
  if (!url.startsWith("https://")) return false;
  String rest = url.substring(8);
  const int slash = rest.indexOf('/');
  if (slash >= 0) rest = rest.substring(0, slash);
  port = 443;
  if (rest.startsWith("[")) {
    const int close = rest.indexOf(']');
    if (close < 0) return false;
    host = rest.substring(1, close);
    rest = rest.substring(close + 1);
    if (rest.length() && !rest.startsWith(":")) return false;
    if (rest.length()) rest = rest.substring(1);
    else rest = "";
  } else {
    const int colon = rest.indexOf(':');
    host = colon >= 0 ? rest.substring(0, colon) : rest;
    rest = colon >= 0 ? rest.substring(colon + 1) : "";
  }
  if (rest.length()) {
    long value = 0;
    for (size_t i = 0; i < rest.length(); ++i) {
      if (!isdigit(static_cast<unsigned char>(rest[i]))) return false;
      value = value * 10 + (rest[i] - '0');
      if (value > 65535) return false;
    }
    if (value < 1) return false;
    port = static_cast<uint16_t>(value);
  }
  return host.length() > 0;
}

// A held or bouncing contact must never activate controls on the next page.
class TouchLatch {
 public:
  bool accept(bool pressed, uint32_t now) {
    if (pressed) {
      releasedAt = 0;
      if (latched) return false;
      latched = true;
      return true;
    }
    if (!releasedAt) releasedAt = now;
    if (now - releasedAt >= 70) latched = false;
    return false;
  }
 private:
  bool latched = false;
  uint32_t releasedAt = 0;
};
}  // namespace Panel
