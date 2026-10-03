"""The firmware fixture must keep matching what the API really sends."""

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "firmware"))

import make_snapshot  # noqa: E402


def key_paths(value, prefix=""):
    if isinstance(value, dict):
        paths = {prefix} if prefix else set()
        for key, item in value.items():
            # collection_status keys depend on which collectors exist; compare their shape.
            name = "*" if prefix == "collection_status" else key
            paths |= key_paths(item, f"{prefix}.{name}" if prefix else name)
        return paths
    if isinstance(value, list):
        paths = {prefix + "[]"}
        for item in value:
            paths |= key_paths(item, prefix + "[]")
        return paths
    return {prefix}


class FirmwareContractTests(unittest.TestCase):
    def test_fixture_matches_the_api_output(self):
        fixture = json.loads((ROOT / "tests" / "firmware" / "snapshot.json").read_text())
        current = make_snapshot.build()
        self.assertEqual(key_paths(fixture), key_paths(current),
                         "Regenera tests/firmware/snapshot.json con tests/firmware/make_snapshot.py")


if __name__ == "__main__":
    unittest.main()
