from __future__ import annotations

import sys
import unittest
from unittest import mock

from tools import build_android
from tools.paths import SCRATCH


class AndroidBuildTests(unittest.TestCase):
    def test_persistent_signing_decodes_only_complete_credentials(self) -> None:
        activity = SCRATCH / "verification" / "android-signing"
        keystore = activity / "release.keystore"
        credentials = {
            "BENEFACTOR_ANDROID_KEYSTORE_B64": "c3ludGhldGljLWtleQ==",
            "BENEFACTOR_ANDROID_KEY_ALIAS": "release",
            "BENEFACTOR_ANDROID_STORE_PASSWORD": "synthetic-store",
            "BENEFACTOR_ANDROID_KEY_PASSWORD": "synthetic-key",
        }
        with mock.patch.object(build_android, "BUILD", activity):
            try:
                environment = credentials.copy()
                self.assertEqual(
                    build_android.prepare_signing_environment(environment, activity), keystore
                )
                self.assertEqual(keystore.read_bytes(), b"synthetic-key")
                self.assertEqual(keystore.stat().st_mode & 0o777, 0o600)
                self.assertEqual(environment["BENEFACTOR_ANDROID_KEYSTORE"], str(keystore))
                self.assertNotIn("BENEFACTOR_ANDROID_KEYSTORE_B64", environment)
            finally:
                keystore.unlink(missing_ok=True)
            for changes, message in (
                ({"BENEFACTOR_ANDROID_KEY_ALIAS": ""}, "requires alias"),
                ({"BENEFACTOR_ANDROID_KEYSTORE_B64": "invalid!"}, "not valid base64"),
                ({"BENEFACTOR_ANDROID_EPHEMERAL_SIGNING": "1"}, "cannot be selected"),
            ):
                with self.subTest(changes=changes):
                    with self.assertRaisesRegex(SystemExit, message):
                        build_android.prepare_signing_environment(credentials | changes, activity)
                    self.assertFalse(keystore.exists())
            with self.assertRaisesRegex(SystemExit, "cannot be selected together"):
                build_android.prepare_signing_environment(
                    {
                        "BENEFACTOR_ANDROID_KEYSTORE": str(activity / "existing.keystore"),
                        **{
                            name: credentials[name]
                            for name in credentials
                            if name.endswith("PASSWORD")
                        },
                        "BENEFACTOR_ANDROID_KEY_ALIAS": "release",
                        "BENEFACTOR_ANDROID_EPHEMERAL_SIGNING": "1",
                    },
                    activity,
                )

    def test_release_apk_preserves_published_signer(self) -> None:
        apk = SCRATCH / "verification" / "android-signing" / "synthetic.apk"
        apk.parent.mkdir(parents=True, exist_ok=True)
        apk.touch()
        shared = mock.Mock()
        shared.inspect_apk_runtime.return_value = (
            "lib/arm64-v8a/libmain.so",
            "lib/arm64-v8a/libSDL3.so",
            "lib/arm64-v8a/libc++_shared.so",
            "resources.arsc",
        )
        with mock.patch.object(build_android, "shared_android_port_tool", return_value=shared):
            shared.verify_apk_signature.return_value = build_android.PUBLISHED_CERT_SHA256
            build_android.inspect_apk(apk, apk.parent, release=True, ephemeral_signing=False)
            shared.verify_apk_signature.assert_called_with(
                apk, apk.parent, build_android.BUILD_TOOLS, build_android.MIN_API
            )
            shared.verify_apk_signature.return_value = "00" * 32
            with self.assertRaisesRegex(SystemExit, "signer differs"):
                build_android.inspect_apk(apk, apk.parent, release=True, ephemeral_signing=False)
            build_android.inspect_apk(apk, apk.parent, release=True, ephemeral_signing=True)
        apk.unlink()

    def test_runtime_blocker_precedes_android_dependency_probes(self) -> None:
        with (
            mock.patch.object(sys, "argv", ["build_android.py"]),
            mock.patch.object(
                build_android, "runtime_blocker", return_value="runtime adapter is missing"
            ),
            mock.patch.object(build_android, "android_sdk") as android_sdk,
            mock.patch.object(build_android, "required_jdk") as required_jdk,
            self.assertRaisesRegex(
                SystemExit,
                "Benefactor gameplay product unavailable: runtime adapter is missing",
            ),
        ):
            build_android.main()

        android_sdk.assert_not_called()
        required_jdk.assert_not_called()


if __name__ == "__main__":
    unittest.main()
