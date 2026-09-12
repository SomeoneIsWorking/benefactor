from __future__ import annotations

import hashlib
import sys
import unittest
import zipfile
from unittest import mock

from tools import publish_release
from tools.paths import SCRATCH


class ReleasePublicationTests(unittest.TestCase):
    def test_qualification_requires_every_release_capability(self) -> None:
        rows = "\n".join(
            f"| {item} | capability | verified | — | G004 |"
            for item in publish_release.RELEASE_STATE_IDS
        )
        publish_release.require_qualified_state(rows)
        with self.assertRaisesRegex(SystemExit, "S023=partial"):
            publish_release.require_qualified_state(
                rows.replace("| S023 | capability | verified |", "| S023 | capability | partial |")
            )
        with self.assertRaisesRegex(SystemExit, "S031=missing"):
            publish_release.require_qualified_state(
                "\n".join(row for row in rows.splitlines() if "S031" not in row)
            )

    def test_publication_stages_only_asset_free_packages(self) -> None:
        activity = SCRATCH / "verification" / "release-publish"
        activity.mkdir(parents=True, exist_ok=True)
        all_names = (*publish_release.PACKAGE_NAMES, *publish_release.WEB_NAMES, "SHA256SUMS")
        try:
            for name in publish_release.PACKAGE_NAMES:
                path = activity / name
                if path.suffix in (".zip", ".apk"):
                    with zipfile.ZipFile(path, "w") as archive:
                        if "windows" in name:
                            archive.writestr("bin/benefactor-pc.exe", b"synthetic")
                        elif "macos" in name:
                            archive.writestr(
                                "Benefactor.app/Contents/MacOS/Benefactor", b"synthetic"
                            )
                        else:
                            archive.writestr("lib/arm64-v8a/libmain.so", b"synthetic")
                            archive.writestr("resources.arsc", b"synthetic")
                else:
                    path.write_bytes(b"\x7fELFsynthetic AppImage")
            for name in publish_release.WEB_NAMES:
                (activity / name).write_bytes(b"synthetic web")
            assets = publish_release.stage_assets(activity)
            self.assertEqual(
                [path.name for path in assets], [*publish_release.PACKAGE_NAMES, "SHA256SUMS"]
            )
            first = activity / publish_release.PACKAGE_NAMES[0]
            self.assertIn(hashlib.sha256(first.read_bytes()).hexdigest(), assets[-1].read_text())
            command = publish_release.release_command("v1.0.0", assets)
            self.assertIn("--verify-tag", command)
            self.assertIn("--fail-on-no-commits", command)
            self.assertIn("Disk.1, Disk.2, and Disk.3", " ".join(command))
            with zipfile.ZipFile(first, "w") as archive:
                archive.writestr("bin/benefactor-pc.exe", b"synthetic")
                archive.writestr("nested/Disk.1", b"player data")
            with self.assertRaisesRegex(SystemExit, "player-owned entry"):
                publish_release.stage_assets(activity)
        finally:
            for name in all_names:
                (activity / name).unlink(missing_ok=True)

    def test_branch_run_cannot_publish(self) -> None:
        with (
            mock.patch.object(
                sys,
                "argv",
                ["publish_release.py", "--tag", "v1.0.0", "--artifacts", "build/release"],
            ),
            mock.patch.dict("os.environ", {"GITHUB_REF": "refs/heads/main", "GH_TOKEN": "test"}),
            self.assertRaisesRegex(SystemExit, "matching pushed tag"),
        ):
            publish_release.main()


if __name__ == "__main__":
    unittest.main()
