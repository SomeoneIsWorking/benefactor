#!/usr/bin/env python3
"""Android packaging flow for the future native/interpreter product."""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from tools.launcher import runtime_blocker

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "android"
ABI = "arm64-v8a"
MIN_API = 21


def refuse(message: str) -> None:
    raise SystemExit(f"android: {message}")


def run(command: list[str], *, cwd: Path = ROOT, environment: dict[str, str] | None = None) -> None:
    print("android:", " ".join(command))
    subprocess.run(command, cwd=cwd, env=environment, check=True)


def required_directory(variable: str) -> Path:
    value = os.environ.get(variable)
    if not value:
        refuse(f"{variable} must name its source checkout")
    directory = Path(value).expanduser().resolve()
    if not directory.is_dir():
        refuse(f"{variable} is not a directory: {directory}")
    return directory


def android_sdk() -> Path:
    for variable in ("ANDROID_SDK_ROOT", "ANDROID_HOME"):
        value = os.environ.get(variable)
        if value and Path(value).is_dir():
            return Path(value).resolve()
    refuse("set ANDROID_SDK_ROOT to a complete Android SDK")


def android_ndk(sdk: Path) -> Path:
    expected = sdk / "ndk" / "28.2.13676358"
    if not expected.is_dir():
        refuse(f"Android NDK 28.2.13676358 is missing: {expected}")
    return expected


def shared_android_port_tool():
    configured = os.environ.get("BENEFACTOR_ANDROID_PORT_DIR")
    candidates = (
        [Path(configured).expanduser()] if configured else [ROOT.parent / "shared" / "android-port"]
    )
    for candidate in candidates:
        tool = candidate.resolve() / "tools" / "android_port.py"
        if not tool.is_file():
            continue
        specification = importlib.util.spec_from_file_location("benefactor_android_port", tool)
        if specification is None or specification.loader is None:
            break
        module = importlib.util.module_from_spec(specification)
        sys.modules[specification.name] = module
        specification.loader.exec_module(module)
        return module
    refuse(
        "cannot find shared Android packaging tool; tried: "
        + ", ".join(str(path) for path in candidates)
    )


def required_jdk() -> Path:
    value = os.environ.get("BENEFACTOR_JAVA_HOME") or os.environ.get("JAVA_HOME")
    if not value:
        refuse("BENEFACTOR_JAVA_HOME or JAVA_HOME must name a JDK 26 installation")
    home = Path(value).expanduser().resolve()
    java = home / "bin" / "java"
    javac = home / "bin" / "javac"
    if not java.is_file() or not javac.is_file():
        refuse(f"BENEFACTOR_JAVA_HOME must contain bin/java and bin/javac: {home}")

    def major_version(executable: Path) -> int | None:
        result = subprocess.run(
            [str(executable), "-version"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        match = re.search(r"(?:version )?\"?(\d+)(?:[._]|\")", result.stdout)
        return int(match.group(1)) if result.returncode == 0 and match else None

    java_major = major_version(java)
    javac_major = major_version(javac)
    if java_major != 26 or javac_major != 26:
        refuse(
            "BENEFACTOR_JAVA_HOME must provide matching JDK 26 java/javac "
            f"(found {java_major}/{javac_major})"
        )
    return home


def configure_native(ndk: Path, profile, lucent: Path) -> Path:
    native = BUILD / "native"
    toolchain = ndk / "build/cmake/android.toolchain.cmake"
    run(
        [
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(native),
            "-G",
            "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            f"-DANDROID_ABI={ABI}",
            f"-DANDROID_PLATFORM=android-{MIN_API}",
            "-DANDROID_STL=c++_shared",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DBENEFACTOR_SDL3_PREFIX={profile.prefix}",
            f"-DBENEFACTOR_LUCENT_DIR={lucent}",
            "-DVulkan_FOUND=FALSE",
        ]
    )
    run(["cmake", "--build", str(native), "--target", "benefactor_product", "--parallel"])
    library = native / "libmain.so"
    if not library.is_file():
        refuse(f"native build did not produce {library}")
    return library


def copy_required(source: Path, destination: Path) -> None:
    if not source.is_file():
        refuse(f"required build output is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def stage_gradle_project(profile) -> Path:
    project = BUILD / "project"
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    copy_required(ROOT / "platforms/android/build.gradle", project / "build.gradle")
    copy_required(ROOT / "platforms/android/settings.gradle", project / "settings.gradle")
    copy_required(
        ROOT / "platforms/android/gradle-wrapper.properties",
        project / "gradle/wrapper/gradle-wrapper.properties",
    )
    shutil.copytree(ROOT / "platforms/android/app", project / "app")
    android_port = shared_android_port_tool()
    android_port.stage_gradle_runtime(profile.prefix, project)
    return project


def prepare_signing_environment(environment: dict[str, str], jdk: Path) -> None:
    signing_names = (
        "BENEFACTOR_ANDROID_KEYSTORE",
        "BENEFACTOR_ANDROID_KEY_ALIAS",
        "BENEFACTOR_ANDROID_STORE_PASSWORD",
        "BENEFACTOR_ANDROID_KEY_PASSWORD",
    )
    configured = [environment.get(name) for name in signing_names]
    if all(configured):
        return
    if any(configured):
        refuse("all BENEFACTOR_ANDROID_KEY_* and password variables are required together")
    if environment.get("BENEFACTOR_ANDROID_EPHEMERAL_SIGNING") != "1":
        refuse(
            "release APK assembly needs maintainer signing variables or explicit "
            "BENEFACTOR_ANDROID_EPHEMERAL_SIGNING=1"
        )

    keytool = jdk / "bin" / "keytool"
    if not keytool.is_file():
        refuse(f"JDK keytool is missing: {keytool}")
    keystore = BUILD / "ci-test.keystore"
    password = "benefactor-ci-only"
    if not keystore.is_file():
        run(
            [
                str(keytool),
                "-genkeypair",
                "-noprompt",
                "-keystore",
                str(keystore),
                "-storepass",
                password,
                "-keypass",
                password,
                "-alias",
                "benefactor-ci",
                "-keyalg",
                "RSA",
                "-keysize",
                "2048",
                "-validity",
                "1",
                "-dname",
                "CN=Benefactor CI",
            ],
            environment=environment,
        )
    environment.update(
        {
            "BENEFACTOR_ANDROID_KEYSTORE": str(keystore),
            "BENEFACTOR_ANDROID_KEY_ALIAS": "benefactor-ci",
            "BENEFACTOR_ANDROID_STORE_PASSWORD": password,
            "BENEFACTOR_ANDROID_KEY_PASSWORD": password,
        }
    )


def inspect_apk(apk: Path) -> None:
    if not apk.is_file():
        refuse(f"Gradle did not produce {apk}")
    names = shared_android_port_tool().inspect_apk_runtime(apk, ABI)
    forbidden = [name for name in names if Path(name).name.lower().startswith("disk.")]
    required = {
        f"lib/{ABI}/libmain.so",
        f"lib/{ABI}/libSDL3.so",
        f"lib/{ABI}/libc++_shared.so",
        "resources.arsc",
    }
    missing = sorted(required.difference(names))
    if forbidden:
        refuse("APK contains prohibited disk image paths: " + ", ".join(forbidden))
    if missing:
        refuse("APK is missing required contents: " + ", ".join(missing))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--release", action="store_true", help="assemble a signing-required release APK"
    )
    args = parser.parse_args()
    blocker = runtime_blocker()
    if blocker:
        refuse(f"Benefactor gameplay product unavailable: {blocker}")
    sdk = android_sdk()
    ndk = android_ndk(sdk)
    jdk = required_jdk()
    lucent = required_directory("BENEFACTOR_LUCENT_DIR")
    android_port = shared_android_port_tool()
    profile = android_port.load_android_port_profile(
        ROOT / "platforms/android/android-port-profile.json"
    )
    if profile.abi != ABI or profile.api != MIN_API:
        refuse(
            "Android profile must target "
            f"{ABI}/android-{MIN_API}, found {profile.abi}/android-{profile.api}"
        )
    android_port.build_native_dependencies(
        android_port.native_dependency_request_for_profile(profile, ndk),
        max(1, min(os.cpu_count() or 1, 4)),
    )
    native = configure_native(ndk, profile, lucent)
    if native != profile.native_library:
        refuse(
            "Android profile package.nativeLibrary must point at the configured native build: "
            f"{native}"
        )
    project = stage_gradle_project(profile)
    android_port.stage_package_runtime(profile)
    environment = dict(os.environ)
    environment["ANDROID_SDK_ROOT"] = str(sdk)
    environment["JAVA_HOME"] = str(jdk)
    prepare_signing_environment(environment, jdk)
    task = ":app:assembleRelease" if args.release else ":app:assembleDebug"
    run(["./gradlew", "--no-daemon", task], cwd=project, environment=environment)
    variant = "release" if args.release else "debug"
    apk = project / f"app/build/outputs/apk/{variant}/app-{variant}.apk"
    inspect_apk(apk)
    output = BUILD / f"Benefactor-{ABI}-{variant}.apk"
    copy_required(apk, output)
    print(f"android: wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
