from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.disk_identity import DiskIdentity, validate_disk


class DiskIdentityTests(unittest.TestCase):
    def test_accepts_exact_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "disk"
            path.write_bytes(b"abc")
            validate_disk(
                path,
                DiskIdentity(
                    "disk", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
                ),
            )

    def test_rejects_wrong_digest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "disk"
            path.write_bytes(b"abc")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                validate_disk(path, DiskIdentity("disk", 3, "0" * 64))


if __name__ == "__main__":
    unittest.main()
