#!/usr/bin/env bash
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "Ejecuta con sudo"; exit 1; fi
INSTALL_DIR=/opt/rpi-monitor
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

apt-get update
apt-get install -y python3-venv avahi-daemon
mkdir -p "$INSTALL_DIR/server"
cp -a "$SCRIPT_DIR/." "$INSTALL_DIR/server/"
python3 -m venv "$INSTALL_DIR/venv"
"$INSTALL_DIR/venv/bin/pip" install --upgrade pip
"$INSTALL_DIR/venv/bin/pip" install -r "$INSTALL_DIR/server/requirements.txt"

if [[ ! -f "$INSTALL_DIR/server/.env" ]]; then
  TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(32))')"
  printf 'MONITOR_API_TOKEN=%s\n' "$TOKEN" > "$INSTALL_DIR/server/.env"
  chmod 640 "$INSTALL_DIR/server/.env"
fi

install -m 0644 "$INSTALL_DIR/server/systemd/rpi-monitor.service" /etc/systemd/system/rpi-monitor.service
chown -R root:root "$INSTALL_DIR"
chmod 750 "$INSTALL_DIR" "$INSTALL_DIR/server"
chmod 640 "$INSTALL_DIR/server/.env"
systemctl daemon-reload
systemctl enable --now avahi-daemon rpi-monitor.service

echo
echo "Instalado. API: http://$(hostname).local:8787"
echo "Token para firmware: $(cut -d= -f2- "$INSTALL_DIR/server/.env")"
