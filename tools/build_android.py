#!/usr/bin/env python3
"""Build a signed Benefactor Android APK from user-supplied disks.

The APK never contains disk images.  The generated recompiler output is made
locally from the user's disks before cross-compiling, while first launch uses
Lucent's SAF importer to stage a separate disk set in private app storage.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "android"
ABI = "arm64-v8a"
API = 24


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
    candidates = [Path(configured).expanduser()] if configured else [ROOT.parent / "shared" / "android-port"]
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
    refuse("cannot find shared Android packaging tool; tried: " + ", ".join(str(path) for path in candidates))


def required_jdk() -> Path:
    value = os.environ.get("BENEFACTOR_JAVA_HOME")
    if not value:
        refuse("BENEFACTOR_JAVA_HOME must name a JDK 26 installation")
    home = Path(value).expanduser().resolve()
    java = home / "bin" / "java"
    javac = home / "bin" / "javac"
    if not java.is_file() or not javac.is_file():
        refuse(f"BENEFACTOR_JAVA_HOME must contain bin/java and bin/javac: {home}")

    def major_version(executable: Path) -> int | None:
        result = subprocess.run([str(executable), "-version"], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        match = re.search(r"(?:version )?\"?(\d+)(?:[._]|\")", result.stdout)
        return int(match.group(1)) if result.returncode == 0 and match else None

    java_major = major_version(java)
    javac_major = major_version(javac)
    if java_major != 26 or javac_major != 26:
        refuse(f"BENEFACTOR_JAVA_HOME must provide matching JDK 26 java/javac (found {java_major}/{javac_major})")
    return home


def regenerate() -> None:
    required = [ROOT / f"Disk.{index}" for index in range(1, 4)]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        refuse("the release builder needs your original disk images: " + ", ".join(missing))
    run(["bash", "tools/regen.sh"])
    if not (ROOT / "src/engine/generated/game.h").is_file():
        refuse("tools/regen.sh did not produce src/engine/generated/game.h")


def configure_native(sdk: Path, ndk: Path, sdl: Path, lucent: Path) -> Path:
    native = BUILD / "native"
    toolchain = ndk / "build/cmake/android.toolchain.cmake"
    run([
        "cmake", "-S", str(ROOT), "-B", str(native), "-G", "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", f"-DANDROID_ABI={ABI}",
        f"-DANDROID_PLATFORM=android-{API}", "-DANDROID_STL=c++_shared",
        "-DCMAKE_BUILD_TYPE=Release", f"-DBENEFACTOR_SDL2_DIR={sdl}",
        f"-DBENEFACTOR_LUCENT_DIR={lucent}", "-DVulkan_FOUND=FALSE",
    ])
    run(["cmake", "--build", str(native), "--target", "benefactor-pc", "--parallel"])
    library = native / "libmain.so"
    if not library.is_file():
        refuse(f"native build did not produce {library}")
    return library


def copy_required(source: Path, destination: Path) -> None:
    if not source.is_file():
        refuse(f"required build output is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def find_unique(root: Path, name: str) -> Path:
    candidates = [path for path in root.rglob(name) if path.is_file()]
    if len(candidates) != 1:
        refuse(f"expected exactly one {name} under {root}, found {len(candidates)}")
    return candidates[0]


def stage_gradle_project(sdl: Path, lucent: Path, native: Path, ndk: Path) -> Path:
    android_project = sdl / "android-project"
    if not android_project.is_dir():
        refuse(f"SDL2 checkout has no android-project: {android_project}")
    project = BUILD / "project"
    if project.exists():
        shutil.rmtree(project)
    shutil.copytree(android_project, project)
    for relative in ("build.gradle", "settings.gradle", "gradle-wrapper.properties"):
        source = ROOT / "platforms/android" / relative
        destination = project / ("gradle/wrapper/gradle-wrapper.properties" if relative == "gradle-wrapper.properties" else relative)
        copy_required(source, destination)
    shutil.copytree(ROOT / "platforms/android/app", project / "app", dirs_exist_ok=True)
    java_root = project / "app/src/main/java"
    shutil.copytree(lucent / "platforms/android/java", java_root, dirs_exist_ok=True)
    libraries = project / "app/src/main/jniLibs" / ABI
    copy_required(native, libraries / "libmain.so")
    copy_required(find_unique(BUILD / "native", "libSDL2.so"), libraries / "libSDL2.so")
    copy_required(shared_android_port_tool().ndk_cxx_shared_library(ndk, ABI), libraries / "libc++_shared.so")
    return project


def inspect_apk(apk: Path) -> None:
    if not apk.is_file():
        refuse(f"Gradle did not produce {apk}")
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
    forbidden = [name for name in names if Path(name).name.lower().startswith("disk.")]
    required = {
        f"lib/{ABI}/libmain.so",
        f"lib/{ABI}/libSDL2.so",
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
    parser.add_argument("--release", action="store_true", help="assemble a signing-required release APK")
    args = parser.parse_args()
    sdk = android_sdk()
    ndk = android_ndk(sdk)
    jdk = required_jdk()
    sdl = required_directory("BENEFACTOR_SDL2_DIR")
    lucent = required_directory("BENEFACTOR_LUCENT_DIR")
    regenerate()
    native = configure_native(sdk, ndk, sdl, lucent)
    project = stage_gradle_project(sdl, lucent, native, ndk)
    environment = dict(os.environ)
    environment["ANDROID_SDK_ROOT"] = str(sdk)
    environment["JAVA_HOME"] = str(jdk)
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
