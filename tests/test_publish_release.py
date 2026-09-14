from __future__ import annotations

import hashlib
import sys
import unittest
import zipfile
from unittest import mock

from tools import pinned_revisions, publish_release
from tools.paths import SCRATCH


class ReleasePublicationTests(unittest.TestCase):
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

    def test_tag_must_match_the_version_file(self) -> None:
        version = (publish_release.ROOT / "version.txt").read_text(encoding="utf-8").strip()
        with (
            mock.patch.object(
                sys,
                "argv",
                ["publish_release.py", "--tag", "v9.9.9", "--artifacts", "build/release"],
            ),
            mock.patch.dict("os.environ", {"GITHUB_REF": "refs/tags/v9.9.9", "GH_TOKEN": "test"}),
            self.assertRaisesRegex(SystemExit, "does not match version.txt"),
        ):
            publish_release.main()
        self.assertRegex(version, r"^\d+\.\d+\.\d+$")

    def test_every_pin_is_read_from_the_workflow(self) -> None:
        # The workflow is the one list of pins; a second hand-kept list drifts.
        pins = pinned_revisions.pinned_revisions(publish_release.ROOT / pinned_revisions.WORKFLOW)
        self.assertGreaterEqual(len(pins), 5)
        for pin in pins:
            self.assertRegex(pin.revision, r"^[0-9a-f]{40}$")
            self.assertIn("/", pin.repository)

    def test_an_unpushed_pin_refuses_publication(self) -> None:
        # The failure this gate exists for: a revision that only exists locally.
        unpushed = pinned_revisions.Pin("SomeoneIsWorking/lucent", "0" * 40)
        with mock.patch.object(pinned_revisions, "revision_exists", return_value=False) as exists:
            missing = pinned_revisions.missing_revisions((unpushed,))
        exists.assert_called_once()
        self.assertEqual(missing, (unpushed,))
        with (
            mock.patch.object(pinned_revisions, "revision_exists", return_value=False),
            mock.patch.object(
                sys,
                "argv",
                ["pinned_revisions.py"],
            ),
            self.assertRaisesRegex(SystemExit, "not on the remote"),
        ):
            pinned_revisions.main()

    def test_branch_run_cannot_publish(self) -> None:
        version = (publish_release.ROOT / "version.txt").read_text(encoding="utf-8").strip()
        with (
            mock.patch.object(
                sys,
                "argv",
                ["publish_release.py", "--tag", f"v{version}", "--artifacts", "build/release"],
            ),
            mock.patch.dict("os.environ", {"GITHUB_REF": "refs/heads/main", "GH_TOKEN": "test"}),
            self.assertRaisesRegex(SystemExit, "matching pushed tag"),
        ):
            publish_release.main()


if __name__ == "__main__":
    unittest.main()
