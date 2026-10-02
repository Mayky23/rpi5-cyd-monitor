"""The Pages site is assembled from the firmware built on the edition branches."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PARTS = ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin")
EDITIONS = ("combined", "rpi5", "proxmox")


def build(artifacts: Path, site: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(ROOT / "web/build_site.py"), "--artifacts", str(artifacts),
                           "--out", str(site)], capture_output=True, text=True)


class SiteTests(unittest.TestCase):
    def fake_artifacts(self, root: Path) -> Path:
        for edition in EDITIONS:
            folder = root / edition
            folder.mkdir(parents=True)
            for name in PARTS:
                (folder / name).write_bytes(b"\xe9" + name.encode())
            (folder / "VERSION").write_text("2.0\n", encoding="ascii")
            (folder / "COMMIT").write_text("abc1234\n", encoding="ascii")
        return root

    def test_site_contains_every_edition(self):
        with tempfile.TemporaryDirectory() as tmp:
            site = Path(tmp) / "site"
            result = build(self.fake_artifacts(Path(tmp) / "artifacts"), site)
            self.assertEqual(result.returncode, 0, result.stderr)
            for edition in EDITIONS:
                for name in (*PARTS, "info.json"):
                    self.assertTrue((site / "firmware" / edition / name).is_file(), f"{edition}/{name}")
                info = json.loads((site / "firmware" / edition / "info.json").read_text())
                self.assertEqual((info["version"], info["commit"]), ("2.0", "abc1234"))
            for name in ("index.html", "app.js", "lib.js", "style.css", ".nojekyll",
                         "img/raspberrypi.svg", "img/proxmox.svg", "fonts/Aileron-Bold.otf",
                         "img/screens/combined/00.png", "img/screens/rpi5/00.png", "img/screens/proxmox/00.png"):
                self.assertTrue((site / name).is_file(), name)

    def test_missing_edition_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifacts = self.fake_artifacts(Path(tmp) / "artifacts")
            (artifacts / "rpi5" / "firmware.bin").unlink()
            result = build(artifacts, Path(tmp) / "site")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("rpi5", result.stderr)

    def test_every_edition_maps_to_a_branch(self):
        lib = (ROOT / "web/lib.js").read_text(encoding="utf-8")
        for branch in ("rpi5-proxmox", "rpi5", "proxmox"):
            self.assertIn(f'branch: "{branch}"', lib)
        workflow = (ROOT / ".github/workflows/pages.yml").read_text(encoding="utf-8")
        for branch in ("rpi5-proxmox", "rpi5", "proxmox"):
            self.assertIn(f"branch: {branch}", workflow)


if __name__ == "__main__":
    unittest.main()
