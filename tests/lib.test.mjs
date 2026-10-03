// Pure logic of the web installer:  node --test tests/lib.test.mjs
import assert from "node:assert/strict";
import { copyFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { pathToFileURL } from "node:url";

// web/lib.js is an ES module without package.json "type": load it as .mjs on any Node version.
const copy = join(mkdtempSync(join(tmpdir(), "cyd-")), "lib.mjs");
copyFileSync(new URL("../web/lib.js", import.meta.url), copy);
const lib = await import(pathToFileURL(copy).href);

const base = { ssid: "MiWiFi", wifiPass: "secreto123", api: "http://192.168.1.50:8787", apiPin: "",
  token: "abcdefghijklmnop0123", deviceName: "rpi-panel", pveUrl: "", pveId: "", pveSecret: "", pveCert: "" };

function readBlock(block) {
  const view = new DataView(block.buffer);
  const length = view.getUint16(8, true);
  const json = block.slice(16, 16 + length);
  assert.equal(new TextDecoder().decode(block.slice(0, 8)), "CYDCFG01");
  assert.equal(view.getUint32(12, true), lib.crc32(json));
  return JSON.parse(new TextDecoder().decode(json));
}

test("crc32 matches zlib", () => {
  assert.equal(lib.crc32(new TextEncoder().encode("hello")), 0x3610a686);
});

test("the block carries a timestamp and an optional normalized pin", () => {
  const plain = readBlock(lib.buildConfigBlock({ ...base, stamp: 1790000000 }));
  assert.equal(plain.stamp, 1790000000);
  assert.equal(plain.pin, undefined);
  const fingerprint = "AB:CD:" + "EF".repeat(30);
  const pinned = readBlock(lib.buildConfigBlock({ ...base, api: "https://api.lan/", apiPin: fingerprint }));
  assert.equal(pinned.pin, "abcd" + "ef".repeat(30));
  assert.equal(pinned.api, "https://api.lan");
  assert.ok(Math.abs(pinned.stamp - Date.now() / 1000) < 5);
});

test("Wi-Fi validation follows WPA rules and keeps meaningful spaces", () => {
  assert.ok(lib.validate({ ...base, wifiPass: "corta" }).errors.wifiPass);
  assert.equal(lib.validate({ ...base, wifiPass: "" }).errors.wifiPass, undefined);
  const spaced = lib.validate({ ...base, ssid: "Casa " });
  assert.equal(spaced.errors.ssid, undefined);
  assert.ok(spaced.warnings.ssid);
  assert.ok(lib.validate({ ...base, ssid: "   " }).errors.ssid);
  assert.match(lib.validate({ ...base, token: "" }).errors.token, /Regenerar/);
});

test("the certificate pin is only checked for https", () => {
  assert.ok(lib.validate({ ...base, api: "https://api.lan", apiPin: "xyz" }).errors.apiPin);
  assert.equal(lib.validate({ ...base, api: "http://api.lan", apiPin: "xyz" }).errors.apiPin, undefined);
  assert.equal(lib.validate({ ...base, api: "https://api.lan", apiPin: "a".repeat(64) }).errors.apiPin, undefined);
});

test("install commands always use fresh code and stop at the first failure", () => {
  const url = new URL("http://192.168.1.50:8787/monitor/");
  const { install, check } = lib.buildCommands(base, "rpi5", url);
  assert.match(install, /mktemp -d/);
  assert.match(install, /git clone --depth 1 --branch rpi5 /);
  assert.ok(!/^cd rpi5-cyd-monitor/m.test(install));
  for (const line of install.split("\n").slice(0, 3)) assert.match(line, /&& \\$/);
  assert.match(install, /--port 8787/);
  assert.equal(check, "curl -s -H 'X-API-Key: abcdefghijklmnop0123' 'http://192.168.1.50:8787/monitor/health'");
});

test("the fingerprint command always names the port", () => {
  const withoutPort = lib.buildCommands({ ...base, pveUrl: "https://pve.lan/api2/json" }, "proxmox", null);
  assert.match(withoutPort.fingerprint, /-connect pve\.lan:443 /);
  const withPort = lib.buildCommands({ ...base, pveUrl: "https://pve.lan:8006/api2/json" }, "proxmox", null);
  assert.match(withPort.fingerprint, /-connect pve\.lan:8006 /);
});
