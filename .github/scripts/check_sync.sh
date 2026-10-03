#!/usr/bin/env bash
# Avisa (sin hacer fallar el CI) si el código común de esta rama difiere del de
# las otras ediciones. Las tres ramas llevan una copia del mismo firmware, API y
# herramientas: un arreglo debe aplicarse en las tres.
set -uo pipefail
EDITIONS=(rpi5-proxmox rpi5 proxmox)
CURRENT="${GITHUB_BASE_REF:-${GITHUB_REF_NAME:-$(git rev-parse --abbrev-ref HEAD)}}"
# Archivos que cambian a propósito según la edición.
EXCLUDE=(README.md EDITION docs/panels.png firmware/platformio.ini firmware/include/config.h
         firmware/include/config.local.example.h server/.env.example .github/workflows/ci.yml)
pathspec=(.)
for path in "${EXCLUDE[@]}"; do pathspec+=(":(exclude)$path"); done

git fetch --quiet --no-tags --depth=1 origin "${EDITIONS[@]}" 2>/dev/null || true
for edition in "${EDITIONS[@]}"; do
  [[ "$edition" == "$CURRENT" ]] && continue
  git rev-parse --verify --quiet "origin/$edition" >/dev/null || continue
  changed="$(git diff --name-only "origin/$edition" HEAD -- "${pathspec[@]}")"
  if [[ -n "$changed" ]]; then
    echo "::warning title=Codigo comun distinto de $edition::$(echo "$changed" | tr '\n' ' ')"
  else
    echo "Codigo comun identico a $edition"
  fi
done
exit 0
