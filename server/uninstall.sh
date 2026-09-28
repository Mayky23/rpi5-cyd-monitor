#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Ejecuta con sudo: sudo ./uninstall.sh"
  exit 1
fi

systemctl disable --now rpi-monitor.service 2>/dev/null || true
rm -f /etc/systemd/system/rpi-monitor.service
systemctl daemon-reload
rm -rf /opt/rpi-monitor

echo "Raspberry Pi Monitor API desinstalada."
