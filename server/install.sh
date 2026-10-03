#!/usr/bin/env bash
# Instala o actualiza la API del monitor.
#
# Uso: sudo ./install.sh [opciones]
#   --token TOKEN                  usa este token en lugar de generar uno aleatorio
#   --port PUERTO                  puerto en el que escucha la API (por defecto 8787)
#   --proxmox-url URL              https://host:8006/api2/json
#   --proxmox-token-id ID          usuario@realm!nombre
#   --proxmox-token-secret SECRET  secreto del token de Proxmox
#   --proxmox-cert-sha256 HUELLA   huella SHA-256 del certificado, sin dos puntos
# Sin opciones conserva el .env existente o crea uno con un token aleatorio.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "Ejecuta con sudo"; exit 1; fi
INSTALL_DIR=/opt/rpi-monitor
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$INSTALL_DIR/server/.env"

declare -A SETTINGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --token)                  SETTINGS[MONITOR_API_TOKEN]="${2:-}" ;;
    --port)                   SETTINGS[MONITOR_PORT]="${2:-}" ;;
    --proxmox-url)            SETTINGS[PROXMOX_API_URL]="${2:-}" ;;
    --proxmox-token-id)       SETTINGS[PROXMOX_TOKEN_ID]="${2:-}" ;;
    --proxmox-token-secret)   SETTINGS[PROXMOX_TOKEN_SECRET]="${2:-}" ;;
    --proxmox-cert-sha256)    SETTINGS[PROXMOX_CERT_SHA256]="${2:-}" ;;
    -h|--help) sed -n '2,11p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "Opcion desconocida: $1"; exit 1 ;;
  esac
  shift 2 || { echo "Falta el valor de $1"; exit 1; }
done

for key in "${!SETTINGS[@]}"; do
  value="${SETTINGS[$key]}"
  if [[ -z "$value" || "$value" == *$'\n'* || "$value" == *$'\r'* ]]; then
    echo "Valor no valido para $key"; exit 1
  fi
done
if [[ -n "${SETTINGS[MONITOR_API_TOKEN]:-}" ]]; then
  token="${SETTINGS[MONITOR_API_TOKEN]}"
  if (( ${#token} < 16 || ${#token} > 128 )) || [[ "$token" =~ [[:space:]] ]]; then
    echo "El token debe tener entre 16 y 128 caracteres y no contener espacios"; exit 1
  fi
fi
if [[ -n "${SETTINGS[PROXMOX_API_URL]:-}" ]] && ! [[ "${SETTINGS[PROXMOX_API_URL]}" =~ ^https://[^/[:space:]]+ ]]; then
  echo "La URL de Proxmox debe empezar por https://, por ejemplo https://proxmox.lan:8006/api2/json"; exit 1
fi
if [[ -n "${SETTINGS[PROXMOX_TOKEN_ID]:-}" ]] && ! [[ "${SETTINGS[PROXMOX_TOKEN_ID]}" =~ ^[^@[:space:]]+@[^!@[:space:]]+![^!@[:space:]]+$ ]]; then
  echo "El ID del token de Proxmox debe tener la forma usuario@realm!nombre"; exit 1
fi
if [[ -n "${SETTINGS[PROXMOX_CERT_SHA256]:-}" ]]; then
  fingerprint="$(printf '%s' "${SETTINGS[PROXMOX_CERT_SHA256]}" | tr -d ': ' | tr 'A-F' 'a-f')"
  if ! [[ "$fingerprint" =~ ^[0-9a-f]{64}$ ]]; then
    echo "La huella de Proxmox debe tener 64 caracteres hexadecimales (los dos puntos se ignoran)"; exit 1
  fi
  SETTINGS[PROXMOX_CERT_SHA256]="$fingerprint"
fi

if [[ -n "${SETTINGS[MONITOR_PORT]:-}" ]] &&    ! [[ "${SETTINGS[MONITOR_PORT]}" =~ ^[0-9]+$ && ${SETTINGS[MONITOR_PORT]} -ge 1 && ${SETTINGS[MONITOR_PORT]} -le 65535 ]]; then
  echo "El puerto debe ser un numero entre 1 y 65535"; exit 1
fi

# Crea o actualiza KEY=VALUE en el .env sin duplicar claves.
set_env() {
  KEY="$1" VALUE="$2" FILE="$ENV_FILE" python3 - <<'PY'
import os
from pathlib import Path

key, value, path = os.environ["KEY"], os.environ["VALUE"], Path(os.environ["FILE"])
lines = path.read_text().splitlines() if path.exists() else []
output, written = [], False
for line in lines:
    if line.startswith(key + "="):
        if not written:
            output.append(f"{key}={value}")
            written = True
    else:
        output.append(line)
if not written:
    output.append(f"{key}={value}")
path.write_text("\n".join(output) + "\n")
PY
}

apt-get update
apt-get install -y python3-venv avahi-daemon
# FastAPI necesita Python 3.10 o posterior (Raspberry Pi OS Bookworm trae 3.11).
if ! python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)'; then
  echo "Se necesita Python 3.10 o posterior; actualiza el sistema (Debian 12 / Raspberry Pi OS Bookworm)."
  exit 1
fi
mkdir -p "$INSTALL_DIR/server"
mkdir -p "$INSTALL_DIR/server/app" "$INSTALL_DIR/server/systemd"
install -m 0644 "$SCRIPT_DIR/app/main.py" "$INSTALL_DIR/server/app/main.py"
install -m 0644 "$SCRIPT_DIR/requirements.txt" "$INSTALL_DIR/server/requirements.txt"
install -m 0644 "$SCRIPT_DIR/systemd/rpi-monitor.service" "$INSTALL_DIR/server/systemd/rpi-monitor.service"
python3 -m venv "$INSTALL_DIR/venv"
"$INSTALL_DIR/venv/bin/pip" install --upgrade pip
"$INSTALL_DIR/venv/bin/pip" install -r "$INSTALL_DIR/server/requirements.txt"

if [[ ! -f "$ENV_FILE" ]]; then
  TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
  printf 'MONITOR_API_TOKEN=%s\n' "$TOKEN" > "$ENV_FILE"
  chmod 640 "$ENV_FILE"
fi
grep -q '^MONITOR_PORT=' "$ENV_FILE" || set_env MONITOR_PORT 8787
for key in "${!SETTINGS[@]}"; do
  set_env "$key" "${SETTINGS[$key]}"
done

install -m 0644 "$INSTALL_DIR/server/systemd/rpi-monitor.service" /etc/systemd/system/rpi-monitor.service
chown -R root:root "$INSTALL_DIR"
chmod 750 "$INSTALL_DIR" "$INSTALL_DIR/server"
chmod 640 "$ENV_FILE"
systemctl daemon-reload
systemctl enable --now avahi-daemon
systemctl enable rpi-monitor.service
systemctl restart rpi-monitor.service

echo
PORT="$(sed -n 's/^MONITOR_PORT=//p' "$ENV_FILE" | head -1)"
echo "Instalado. API: http://$(hostname).local:${PORT:-8787}"
echo "Token conservado en $ENV_FILE (no se muestra en pantalla)."
