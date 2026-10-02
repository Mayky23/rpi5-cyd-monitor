"""Assemble the GitHub Pages site: the web installer plus the firmware of every edition.

    python web/build_site.py --artifacts DIR --out site

DIR contains one folder per edition (combined, rpi5, proxmox) with the files
bootloader.bin, partitions.bin, boot_app0.bin, firmware.bin, VERSION and COMMIT.
The Pages workflow builds them from the edition branches (rpi5-proxmox, rpi5, proxmox).
"""

from __future__ import annotations

import argparse
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

WEB = Path(__file__).resolve().parent
EDITIONS = ("combined", "rpi5", "proxmox")
PARTS = ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin")
SITE_FILES = ("index.html", "style.css", "app.js", "lib.js")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    for name in SITE_FILES:
        shutil.copy2(WEB / name, args.out / name)
    for folder in ("img", "fonts"):
        shutil.copytree(WEB / folder, args.out / folder)

    built = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    for edition in EDITIONS:
        source = args.artifacts / edition
        missing = [name for name in (*PARTS, "VERSION", "COMMIT") if not (source / name).is_file()]
        if missing:
            raise SystemExit(f"Faltan archivos de la edición {edition}: {', '.join(missing)}")
        target = args.out / "firmware" / edition
        target.mkdir(parents=True)
        for name in PARTS:
            shutil.copy2(source / name, target / name)
        info = {
            "version": (source / "VERSION").read_text(encoding="ascii").strip(),
            "commit": (source / "COMMIT").read_text(encoding="ascii").strip(),
            "built": built,
        }
        (target / "info.json").write_text(json.dumps(info), encoding="ascii")
    (args.out / ".nojekyll").write_text("", encoding="ascii")
    print(f"Sitio creado en {args.out}")


if __name__ == "__main__":
    main()
