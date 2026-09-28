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
