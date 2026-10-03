#!/usr/bin/env bash
# Compila y ejecuta en el PC las pruebas de la lógica pura del firmware.
# Uso: tests/firmware/run.sh [carpeta src de ArduinoJson]
# Por defecto usa la copia que descarga PlatformIO al compilar el firmware.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JSON_SRC="${1:-$(ls -d "$ROOT"/firmware/.pio/libdeps/*/ArduinoJson/src 2>/dev/null | head -1)}"
if [[ -z "$JSON_SRC" || ! -f "$JSON_SRC/ArduinoJson.h" ]]; then
  echo "No encuentro ArduinoJson: compila antes el firmware (pio run -d firmware) o indica su carpeta src"; exit 1
fi
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
g++ -std=gnu++17 -Wall -Wextra -Werror -I "$ROOT/tests/firmware/shim" -I "$ROOT/firmware/include" \
  -I "$JSON_SRC" "$ROOT/tests/firmware/test_panel.cpp" -o "$OUT/test_panel"
"$OUT/test_panel" "$ROOT/tests/firmware/snapshot.json"
