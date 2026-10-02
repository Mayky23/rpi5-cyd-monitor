import { EDITIONS, buildCommands, buildConfigBlock, buildManifest, randomToken, validate } from "./lib.js";

const $ = (id) => document.getElementById(id);
const fields = ["ssid", "wifiPass", "api", "token", "deviceName", "pveUrl", "pveId", "pveSecret", "pveCert"];
const remembered = ["ssid", "api", "deviceName", "pveUrl", "pveId"]; // nunca contraseñas, tokens ni secretos
const labels = { ssid: "tu Wi-Fi", wifiPass: "la contraseña", api: "la dirección de la API", token: "el token", deviceName: "el nombre de la pantalla" };

// Capturas reales de la pantalla (las que pinta el propio firmware) para la vista previa.
const SCREENS = {
  combined: [["00", "RPI5"], ["01", "Rendimiento"], ["02", "Discos"], ["03", "Red"], ["06", "Docker"], ["07", "PVE resumen"], ["09", "PVE VM / LXC"], ["11", "PVE tareas"], ["14", "Personalización"]],
  rpi5: [["00", "RPI5"], ["01", "Rendimiento"], ["02", "Discos"], ["03", "Red"], ["04", "CPU / Temperatura"], ["05", "Puertos"], ["06", "Docker"], ["09", "Personalización"]],
  proxmox: [["00", "PVE resumen"], ["01", "PVE rendimiento"], ["02", "PVE VM / LXC"], ["03", "PVE discos"], ["04", "PVE tareas"], ["07", "Personalización"]],
};
const EDITION_NAMES = { combined: "Raspberry Pi 5 y Proxmox", rpi5: "Raspberry Pi 5", proxmox: "Proxmox VE" };

const state = { info: null, blobs: [], touched: new Set(), shot: 0, timer: null, front: "shotA", vertical: false, auto: true, ticks: 0 };

const edition = () => document.querySelector("input[name=edition]:checked").value;
const values = () => Object.fromEntries(fields.map((id) => [id, id === "wifiPass" ? $(id).value : $(id).value.trim()]));

function store(action, key, value) {
  try {
    if (action === "get") return localStorage.getItem(`cyd.${key}`);
    localStorage.setItem(`cyd.${key}`, value);
  } catch { /* navegación privada o almacenamiento bloqueado */ }
  return null;
}

/* ---------- Vista previa de la pantalla ---------- */

function showShot(index, { manual = false } = {}) {
  const set = SCREENS[edition()];
  state.shot = (index + set.length) % set.length;
  const [file, name] = set[state.shot];
  const next = $(state.front === "shotA" ? "shotB" : "shotA");
  const current = $(state.front);
  next.onload = () => {
    next.classList.add("on");
    current.classList.remove("on");
    state.front = next.id;
    next.onload = null;
  };
  next.classList.toggle("v", state.vertical);
  next.src = `img/screens/${edition()}/${file}${state.vertical ? "-v" : ""}.png`;
  $("shotName").textContent = name;
  $("shotIndex").textContent = `${state.vertical ? "Vertical" : "Horizontal"} · ${state.shot + 1} / ${set.length}`;
  [...$("dots").children].forEach((dot, i) => dot.setAttribute("aria-selected", String(i === state.shot)));
  if (manual) restartTimer();
}

// Gira la placa 90°; a mitad del giro cambia a la captura de la otra orientación.
function setOrientation(vertical) {
  if (state.vertical === vertical) return;
  state.vertical = vertical;
  $("board").classList.toggle("is-v", vertical);
  for (const button of document.querySelectorAll(".orient button")) button.setAttribute("aria-pressed", String((button.dataset.o === "v") === vertical));
  const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;
  setTimeout(() => showShot(state.shot), reduced ? 0 : 420);
}

function buildDots() {
  const dots = $("dots");
  dots.replaceChildren(...SCREENS[edition()].map(([, name], i) => {
    const dot = document.createElement("button");
    dot.type = "button";
    dot.setAttribute("role", "tab");
    dot.setAttribute("aria-label", name);
    dot.addEventListener("click", () => showShot(i, { manual: true }));
    return dot;
  }));
}

function restartTimer() {
  clearInterval(state.timer);
  if (matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  state.timer = setInterval(() => {
    state.ticks += 1;
    if (state.auto && state.ticks % 4 === 0) setOrientation(!state.vertical); // cada 4 paneles muestra la otra orientación
    else showShot(state.shot + 1);
  }, 3600);
}

function setupPreview() {
  buildDots();
  showShot(0);
  restartTimer();
}

for (const button of document.querySelectorAll(".orient button")) {
  button.addEventListener("click", () => {
    state.auto = false; // si el usuario elige, se deja de girar solo
    setOrientation(button.dataset.o === "v");
    restartTimer();
  });
}

$("screen").addEventListener("click", (event) => {
  const box = event.currentTarget.getBoundingClientRect();
  showShot(state.shot + (event.clientX - box.left < box.width / 2 ? -1 : 1), { manual: true });
});

/* ---------- Firmware publicado ---------- */

async function loadInfo() {
  state.info = null;
  $("firmwareInfo").textContent = "Buscando el firmware…";
  try {
    const response = await fetch(`firmware/${edition()}/info.json`, { cache: "no-cache" });
    if (!response.ok) throw new Error(response.status);
    state.info = await response.json();
    $("firmwareInfo").textContent = `Firmware ${state.info.version} · compilado el ${state.info.built}.`;
  } catch {
    $("firmwareInfo").textContent = "El firmware de esta edición todavía no está publicado. Inténtalo de nuevo en unos minutos.";
  }
  update();
}

/* ---------- Estado de la página ---------- */

function blobUrl(data, type) {
  const url = URL.createObjectURL(new Blob([data], { type }));
  state.blobs.push(url);
  return url;
}

function mask(text) {
  return "•".repeat(Math.min(text.length, 12));
}

function renderSummary(current, kind, url) {
  const rows = [
    ["Edición", EDITION_NAMES[kind]],
    ["Firmware", state.info ? `${state.info.version} (${state.info.commit})` : "—"],
    ["Wi-Fi", current.ssid || "—"],
    ["Contraseña", current.wifiPass ? mask(current.wifiPass) : "sin contraseña"],
    ["API", url ? url.origin : "—", "mono"],
    ["Token", current.token ? `${current.token.slice(0, 4)}${mask(current.token.slice(4))}` : "—", "mono"],
    ["Nombre", current.deviceName || "—"],
  ];
  $("summary").replaceChildren(...rows.flatMap(([term, text, cls]) => {
    const dt = Object.assign(document.createElement("dt"), { textContent: term });
    const dd = Object.assign(document.createElement("dd"), { textContent: text });
    if (cls) dd.className = cls;
    return [dt, dd];
  }));
}

function update() {
  const current = values();
  const kind = edition();
  const { errors, url } = validate(current);

  for (const id of ["ssid", "wifiPass", "api", "token", "deviceName"]) {
    const show = state.touched.has(id) && errors[id];
    $(id).classList.toggle("invalid", Boolean(show));
    $(`err-${id}`).textContent = show ? errors[id] : "";
  }

  $("proxmoxGroup").hidden = !EDITIONS[kind].proxmox;
  const commands = buildCommands(current, kind, url);
  $("cmdInstall").textContent = commands.install;
  $("cmdCheck").textContent = commands.check;
  $("cmdFingerprint").textContent = commands.fingerprint;
  renderSummary(current, kind, url);

  const missing = Object.keys(errors).map((id) => labels[id]);
  const ready = missing.length === 0 && state.info !== null;
  const gate = $("gate");
  gate.classList.toggle("ok", ready);
  gate.textContent = ready ? "Todo listo. Conecta la placa y pulsa instalar."
    : state.info === null ? ""
    : `Para activar el botón revisa: ${missing.join(", ")}.`;

  for (const old of state.blobs.splice(0)) URL.revokeObjectURL(old);
  for (const [button, holder, configOnly] of [["btnFull", "installFull", false], ["btnConfig", "installConfig", true]]) {
    $(button).disabled = !ready;
    if (!ready) {
      $(holder).removeAttribute("manifest");
      continue;
    }
    const block = buildConfigBlock({ ...current, api: url.origin + url.pathname });
    const manifest = buildManifest({
      baseUrl: new URL(`firmware/${kind}/`, location.href),
      version: state.info.version,
      configUrl: blobUrl(block, "application/octet-stream"),
      configOnly,
    });
    $(holder).setAttribute("manifest", blobUrl(JSON.stringify(manifest), "application/json"));
  }
}

async function copy(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = Object.assign(document.createElement("textarea"), { value: text });
    document.body.append(area);
    area.select();
    const done = document.execCommand("copy");
    area.remove();
    return done;
  }
}

function flash(button, text) {
  const label = button.dataset.label ?? button.textContent;
  button.dataset.label = label;
  button.textContent = text;
  clearTimeout(button.flashTimer);
  button.flashTimer = setTimeout(() => { button.textContent = label; }, 1400);
}

/* ---------- Eventos ---------- */

if (!("serial" in navigator)) $("unsupported").hidden = false;

for (const id of fields) {
  $(id).addEventListener("input", () => {
    state.touched.add(id);
    if (remembered.includes(id)) store("set", id, $(id).value);
    update();
  });
  $(id).addEventListener("blur", () => { state.touched.add(id); update(); });
}

for (const radio of document.querySelectorAll("input[name=edition]")) {
  radio.addEventListener("change", () => {
    store("set", "edition", radio.value);
    setupPreview();
    loadInfo();
  });
}

$("genToken").addEventListener("click", () => {
  $("token").value = randomToken();
  state.touched.add("token");
  update();
});
$("copyToken").addEventListener("click", async (event) => flash(event.currentTarget, (await copy($("token").value)) ? "Copiado" : "Error"));

for (const button of document.querySelectorAll("[data-toggle]")) {
  button.addEventListener("click", () => {
    const input = $(button.dataset.toggle);
    const show = input.type === "password";
    input.type = show ? "text" : "password";
    button.textContent = show ? "Ocultar" : "Ver";
  });
}

for (const button of document.querySelectorAll("[data-copy]")) {
  button.addEventListener("click", async () => flash(button, (await copy($(button.dataset.copy).textContent)) ? "Copiado" : "Error"));
}

/* ---------- Arranque ---------- */

const savedEdition = store("get", "edition");
if (savedEdition && EDITIONS[savedEdition]) document.querySelector(`input[value=${savedEdition}]`).checked = true;
for (const id of remembered) {
  const saved = store("get", id);
  if (saved) $(id).value = saved;
}
$("token").value = randomToken();

setupPreview();
loadInfo();
