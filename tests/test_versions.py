"""VERSION, the API and the firmware must announce the same release."""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "firmware" / "tools"))

import check_panels  # noqa: E402


class VersionTests(unittest.TestCase):
    def test_version_is_the_same_everywhere(self):
        version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
        api = re.search(r'^APP_VERSION = "([^"]+)"', (ROOT / "server/app/main.py").read_text(), re.M)
        firmware = re.search(r'#define FIRMWARE_VERSION "([^"]+)"',
                             (ROOT / "firmware/include/version.h").read_text())
        self.assertEqual(api.group(1), version)
        self.assertEqual(firmware.group(1), version)

    def test_edition_matches_the_firmware_profile(self):
        edition = (ROOT / "EDITION").read_text(encoding="ascii").strip()
        profiles = {"combined": None, "rpi5": "1", "proxmox": "2"}
        ini = (ROOT / "firmware/platformio.ini").read_text()
        default_env = ini.split("[env:esp32-2432S028R]", 1)[1].split("[env:", 1)[0]
        profile = re.search(r"-D MONITOR_PROFILE=(\d)", default_env)
        self.assertEqual(profile.group(1) if profile else None, profiles[edition])


class PanelCaptureToolTests(unittest.TestCase):
    def test_pages_are_addressed_by_id_in_every_edition(self):
        rpi5 = check_panels.page_positions({"pages": 12, "page_ids": [0, 1, 2, 3, 4, 5, 6, 12, 13, 14, 15, 16]})
        self.assertEqual(rpi5[check_panels.BRIGHTNESS], 8)
        self.assertNotIn(check_panels.PROXMOX, rpi5)
        proxmox = check_panels.page_positions({"pages": 10, "page_ids": list(range(7, 17))})
        self.assertEqual(proxmox[check_panels.CUSTOMIZE], 7)
        self.assertNotIn(check_panels.DOCKER, proxmox)
        self.assertEqual(check_panels.page_positions({"pages": 17})[check_panels.SPLASH], 15)
        with self.assertRaises(RuntimeError):
            check_panels.page_positions({"pages": 12})


if __name__ == "__main__":
    unittest.main()
