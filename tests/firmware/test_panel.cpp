// Host-side tests for the pure firmware logic (no ESP32 needed).
//
//   tests/firmware/run.sh [ArduinoJson/src]
#include <ArduinoJson.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "panel_data.h"
#include "runtime_config.h"

static int failures = 0;
#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": FAILED " #condition "\n";   \
      ++failures;                                                                 \
    }                                                                             \
  } while (0)

static std::vector<uint8_t> configBlock(const std::string &json, bool corrupt = false) {
  std::vector<uint8_t> sector(RuntimeConfig::SECTOR_SIZE, 0xFF);
  memcpy(sector.data(), "CYDCFG01", 8);
  sector[8] = json.size() & 0xFF;
  sector[9] = (json.size() >> 8) & 0xFF;
  sector[10] = sector[11] = 0;
  uint32_t crc = RuntimeConfig::crc32(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  if (corrupt) crc ^= 1;
  for (int i = 0; i < 4; ++i) sector[12 + i] = (crc >> (8 * i)) & 0xFF;
  memcpy(sector.data() + RuntimeConfig::HEADER_SIZE, json.data(), json.size());
  return sector;
}

static RuntimeConfig::Values compiledWithStamp(uint32_t stamp) {
  RuntimeConfig::Values values;
  values.ssid = "COMPILED";
  values.apiBase = "http://compiled:8787";
  values.token = "compiled-token-123456";
  values.certSha256 = "";
  values.stamp = stamp;
  return values;
}

static void testRuntimeConfig() {
  const char *hello = "hello";
  CHECK(RuntimeConfig::crc32(reinterpret_cast<const uint8_t *>(hello), 5) == 0x3610a686u);  // zlib.crc32

  const std::string web = R"({"ssid":"WEB","pass":"secret123","api":"http://web:8787//","token":"web-token-1234567890","name":"panel"})";
  // Without stamps (old web installer and CI firmware) the flash block wins, as before.
  auto chosen = RuntimeConfig::choose(compiledWithStamp(0), configBlock(web).data());
  CHECK(chosen.fromFlash && chosen.ssid == "WEB" && chosen.apiBase == "http://web:8787");
  // Install.ps1 stamps its build: an older or unstamped web block no longer hides it.
  chosen = RuntimeConfig::choose(compiledWithStamp(1000), configBlock(web).data());
  CHECK(!chosen.fromFlash && chosen.ssid == "COMPILED");
  const std::string older = R"({"ssid":"WEB","pass":"","api":"http://web","token":"web-token-1234567890","stamp":999})";
  CHECK(!RuntimeConfig::choose(compiledWithStamp(1000), configBlock(older).data()).fromFlash);
  // A web block written later than the build wins again.
  const std::string newer = R"({"ssid":"WEB","pass":"","api":"http://web","token":"web-token-1234567890","stamp":2000})";
  chosen = RuntimeConfig::choose(compiledWithStamp(1000), configBlock(newer).data());
  CHECK(chosen.fromFlash && chosen.stamp == 2000);
  CHECK(!RuntimeConfig::choose(compiledWithStamp(0), configBlock(web, true).data()).fromFlash);
  CHECK(!RuntimeConfig::choose(compiledWithStamp(0), nullptr).fromFlash);

  const std::string pinned = std::string(R"({"ssid":"WEB","pass":"","api":"https://web","token":"web-token-1234567890","pin":")") +
                             "AB:CD:" + std::string(60, 'E') + "\"}";
  chosen = RuntimeConfig::choose(compiledWithStamp(0), configBlock(pinned).data());
  CHECK(chosen.fromFlash && chosen.certSha256 == String(("abcd" + std::string(60, 'e')).c_str()));
  const std::string badPin = R"({"ssid":"WEB","pass":"","api":"https://web","token":"web-token-1234567890","pin":"xyz"})";
  CHECK(!RuntimeConfig::choose(compiledWithStamp(0), configBlock(badPin).data()).fromFlash);
  const std::string noUrl = R"({"ssid":"WEB","pass":"","api":"web:8787","token":"web-token-1234567890"})";
  CHECK(!RuntimeConfig::choose(compiledWithStamp(0), configBlock(noUrl).data()).fromFlash);

  auto compiled = compiledWithStamp(0);
  compiled.certSha256 = "not-a-fingerprint";
  CHECK(RuntimeConfig::choose(compiled, nullptr).certSha256.length() == 0);
}

static void testSectionsAndEndpoints() {
  CHECK(Panel::sectionShown(0, "proxmox") && Panel::sectionShown(0, "docker"));
  CHECK(!Panel::sectionShown(1, "proxmox") && Panel::sectionShown(1, "docker"));
  CHECK(Panel::sectionShown(2, "proxmox") && !Panel::sectionShown(2, "docker") && !Panel::sectionShown(2, "history"));

  String host;
  uint16_t port = 0;
  CHECK(Panel::httpsEndpoint("https://api.lan", host, port) && host == "api.lan" && port == 443);
  CHECK(Panel::httpsEndpoint("https://api.lan:8443/monitor", host, port) && host == "api.lan" && port == 8443);
  CHECK(Panel::httpsEndpoint("https://[fd00::5]:9443", host, port) && host == "fd00::5" && port == 9443);
  CHECK(!Panel::httpsEndpoint("http://api.lan", host, port));
  CHECK(!Panel::httpsEndpoint("https://api.lan:99999", host, port));
  CHECK(!Panel::httpsEndpoint("https://api.lan:80x", host, port));
  CHECK(!Panel::httpsEndpoint("https://:443", host, port));
}

static std::string readFile(const char *path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream content;
  content << file.rdbuf();
  return content.str();
}

static void testSnapshotFilter(const char *fixturePath) {
  const std::string source = readFile(fixturePath);
  CHECK(!source.empty());
  JsonDocument filter;
  Panel::buildSnapshotFilter(filter);
  JsonDocument doc;
  CHECK(!deserializeJson(doc, source, DeserializationOption::Filter(filter)));

  // Same acceptance rules as requestSnapshot() in main.cpp.
  CHECK((doc["schema"] | 0) == 2);
  CHECK(Panel::number(doc["cpu"]["percent"]) && Panel::number(doc["memory"]["percent"]));
  CHECK(doc["system"].is<JsonObject>() && doc["network"].is<JsonObject>());
  CHECK(doc["storage"]["mounts"].is<JsonArray>() && doc["interfaces"].is<JsonArray>());
  CHECK(doc["ports"].is<JsonArray>() && doc["docker"].is<JsonObject>());
  CHECK(doc["proxmox"].is<JsonObject>() && doc["history"].is<JsonArray>());

  // Every field the panel draws survives...
  CHECK(String(doc["system"]["ip"] | "") == "192.0.2.10" && Panel::number(doc["system"]["uptime_s"]));
  CHECK(doc["cpu"]["per_core"].size() == 4 && Panel::number(doc["cpu"]["load"][0]));
  CHECK(Panel::number(doc["cpu"]["temperature_c"]) && Panel::number(doc["cpu"]["frequency_mhz"]));
  for (const char *field : {"percent", "free_bytes", "read_bps", "write_bps"}) CHECK(Panel::number(doc["disk"][field]));
  CHECK(String(doc["disk"]["status"] | "") == "ok" && String(doc["disk"]["io_status"] | "") == "ok");
  CHECK(Panel::number(doc["network"]["rx_bps"]) && Panel::number(doc["network"]["tx_bps"]));
  JsonObject mount = doc["storage"]["mounts"][0];
  for (const char *field : {"device", "mount", "status", "percent", "used_bytes", "total_bytes", "size_bytes"})
    CHECK(!mount[field].isNull());
  CHECK(!doc["interfaces"][0]["speed_mbps"].isNull() && !doc["interfaces"][0]["up"].isNull());
  CHECK(String(doc["temperatures"][0]["name"] | "") == "cpu_thermal:sensor 1");
  CHECK(doc["docker"]["containers"].size() == 3 && String(doc["docker"]["containers"][2]["state"] | "") == "exited");
  CHECK((doc["docker"]["running"] | 0) == 2 && (doc["docker"]["total"] | 0) == 3);
  JsonObject pve = doc["proxmox"];
  CHECK((pve["available"] | false) && String(pve["state"] | "") == "online");
  CHECK(Panel::number(pve["nodes"][0]["cpu_percent"]) && Panel::number(pve["nodes"][0]["uptime_s"]));
  CHECK(String(pve["guests"][0]["name"] | "") == "firewall" && Panel::number(pve["guests"][0]["disk_percent"]));
  CHECK(Panel::number(pve["storage"][0]["percent"]) && Panel::number(pve["storage"][0]["total_bytes"]));
  CHECK(String(pve["tasks"][0]["label"] | "").length() && !pve["tasks"][0]["started"].isNull());
  CHECK(Panel::number(pve["history"][0]["cpu"]) && Panel::number(pve["history_interval_ms"]));
  CHECK((pve["guests_total"] | 0) == 2 && !pve["tasks_failed"].isNull());
  CHECK(doc["history"].size() == 60 && Panel::number(doc["history"][59]["ram"]));
  CHECK(doc["collection_status"]["proxmox"]["ok"] == true && doc["collection_status"].size() == 6);

  // ...and what it never draws is dropped before it uses heap.
  CHECK(doc["docker"]["containers"][0]["image"].isNull() && doc["docker"]["containers"][0]["ports"].isNull());
  CHECK(doc["ports"][0]["pid"].isNull() && mount["model"].isNull() && mount["mounts"].isNull());
  CHECK(pve["tasks"][0]["user"].isNull() && pve["guests"][0]["net_in_bytes"].isNull());
  CHECK(doc["history"][0]["temp"].isNull() && doc["temperatures"][0]["critical_c"].isNull());
  CHECK(measureJson(doc) * 10 < source.size() * 7);

  // Empty lists stay lists, so an idle server is not reported as an incomplete answer.
  JsonDocument empty;
  CHECK(!deserializeJson(empty, R"({"schema":2,"storage":{"mounts":[]},"ports":[],"interfaces":[],
                                     "docker":{"available":false,"containers":[]},"history":[],
                                     "proxmox":{"available":false,"state":"unconfigured","nodes":[]}})",
                         DeserializationOption::Filter(filter)));
  CHECK(empty["storage"]["mounts"].is<JsonArray>() && empty["ports"].is<JsonArray>());
  CHECK(empty["history"].is<JsonArray>() && empty["docker"]["containers"].is<JsonArray>());
  CHECK(String(empty["proxmox"]["state"] | "") == "unconfigured");

  const auto ports = Panel::ports(doc["ports"].as<JsonArray>());
  CHECK(ports.size() == 1 && ports[0].v4 && ports[0].v6 && ports[0].address == "Todas las IP");
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: test_panel tests/firmware/snapshot.json\n";
    return 2;
  }
  testRuntimeConfig();
  testSectionsAndEndpoints();
  testSnapshotFilter(argv[1]);
  if (failures) {
    std::cerr << failures << " firmware host check(s) failed\n";
    return 1;
  }
  std::cout << "Firmware host tests OK\n";
  return 0;
}
