// Lógica pura del instalador: formato del bloque de configuración, validación y comandos.
// El formato debe coincidir con firmware/include/runtime_config.h.

export const CONFIG_OFFSET = 0x310000; // inicio de la partición "spiffs" (huge_app.csv)
export const SECTOR_SIZE = 4096;
export const HEADER_SIZE = 16;
export const REPO = "https://github.com/Mayky23/rpi5-cyd-monitor.git";

export const EDITIONS = {
  combined: { branch: "rpi5-proxmox", proxmox: true },
  rpi5: { branch: "rpi5", proxmox: false },
  proxmox: { branch: "proxmox", proxmox: true },
};

// Piezas del firmware y su dirección en la flash de una ESP32.
export const FIRMWARE_PARTS = [
  { file: "bootloader.bin", offset: 0x1000 },
  { file: "partitions.bin", offset: 0x8000 },
  { file: "boot_app0.bin", offset: 0xe000 },
  { file: "firmware.bin", offset: 0x10000 },
];

const encoder = new TextEncoder();
const byteLength = (text) => encoder.encode(text).length;

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function buildConfigBlock({ ssid, wifiPass, api, token, deviceName }) {
  const json = encoder.encode(JSON.stringify({
    ssid,
    pass: wifiPass,
    api: api.replace(/\/+$/, ""),
    token,
    name: deviceName,
  }));
  if (json.length > SECTOR_SIZE - HEADER_SIZE) throw new Error("La configuración es demasiado larga");
  const block = new Uint8Array(SECTOR_SIZE).fill(0xff);
  block.set(encoder.encode("CYDCFG01"), 0);
  const view = new DataView(block.buffer);
  view.setUint16(8, json.length, true);
  view.setUint16(10, 0, true);
  view.setUint32(12, crc32(json), true);
  block.set(json, HEADER_SIZE);
  return block;
}

export function validate(values) {
  const errors = {};
  const fail = (field, message) => { errors[field] ??= message; };

  if (!values.ssid) fail("ssid", "Escribe el nombre de tu Wi-Fi.");
  else if (byteLength(values.ssid) > 32) fail("ssid", "El nombre de una Wi-Fi no puede pasar de 32 caracteres.");
  if (byteLength(values.wifiPass) > 63 && !/^[0-9a-fA-F]{64}$/.test(values.wifiPass)) {
    fail("wifiPass", "La contraseña de la Wi-Fi puede tener como máximo 63 caracteres (o 64 si es una clave hexadecimal).");
  }
  if (!/^[A-Za-z0-9-]{1,32}$/.test(values.deviceName)) {
    fail("deviceName", "Usa solo letras, números y guiones, sin espacios (máximo 32).");
  }

  let url = null;
  if (!values.api) fail("api", "Escribe la dirección de la API.");
  else {
    try { url = new URL(values.api); } catch { /* se informa abajo */ }
    if (!url || !["http:", "https:"].includes(url.protocol) || !url.hostname) {
      url = null;
      fail("api", "Debe empezar por http:// o https://, por ejemplo http://192.168.1.50:8787");
    }
  }
  if (!values.token) fail("token", "Pulsa «Generar» o escribe tu propio token.");
  else if (values.token.length < 16 || values.token.length > 128 || /\s/.test(values.token)) {
    fail("token", "Entre 16 y 128 caracteres y sin espacios.");
  }
  return { errors, problems: Object.values(errors), url };
}

export function randomToken() {
  const bytes = crypto.getRandomValues(new Uint8Array(32));
  return btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export const shellQuote = (value) => `'${String(value).replace(/'/g, `'\\''`)}'`;

export function buildManifest({ baseUrl, version, configUrl, configOnly }) {
  const parts = configOnly ? [] : FIRMWARE_PARTS.map((part) => ({ path: new URL(part.file, baseUrl).href, offset: part.offset }));
  parts.push({ path: configUrl, offset: CONFIG_OFFSET });
  return {
    name: "CYD Monitor",
    version,
    new_install_prompt_erase: false,
    builds: [{ chipFamily: "ESP32", parts }],
  };
}

export function buildCommands(values, edition, url) {
  const info = EDITIONS[edition];
  const token = values.token || "TU_TOKEN";
  const lines = [
    `git clone --branch ${info.branch} --single-branch ${REPO}`,
    "cd rpi5-cyd-monitor/server",
  ];
  const flags = [`--token ${shellQuote(token)}`];
  // La API escucha en el puerto de la dirección elegida. Con https suele haber un proxy delante y no se toca.
  if (url && url.protocol === "http:") flags.push(`--port ${url.port || 80}`);
  if (info.proxmox) {
    if (values.pveUrl) flags.push(`--proxmox-url ${shellQuote(values.pveUrl)}`);
    if (values.pveId) flags.push(`--proxmox-token-id ${shellQuote(values.pveId)}`);
    if (values.pveSecret) flags.push(`--proxmox-token-secret ${shellQuote(values.pveSecret)}`);
    if (values.pveCert) flags.push(`--proxmox-cert-sha256 ${shellQuote(values.pveCert.replace(/:/g, "").toLowerCase())}`);
  }
  lines.push(`sudo ./install.sh ${flags.join(" \\\n  ")}`);

  const apiBase = url ? url.origin : "http://IP_DE_LA_API:8787";
  const check = `curl -s -H ${shellQuote(`X-API-Key: ${token}`)} ${apiBase}/health`;

  let pveHost = "proxmox.example.lan:8006";
  try { pveHost = new URL(values.pveUrl).host; } catch { /* valor de ejemplo */ }
  const fingerprint = [
    `openssl s_client -connect ${pveHost} </dev/null 2>/dev/null \\`,
    "  | openssl x509 -noout -fingerprint -sha256 \\",
    "  | cut -d= -f2 | tr -d ':'",
  ].join("\n");
  return { install: lines.join("\n"), check, fingerprint };
}
