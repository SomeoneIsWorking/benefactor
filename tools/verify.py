from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from tools.paths import ROOT, SCRATCH, amigaport_dir

PYTHON_PATHS = ("bootstrap.py", "tools", "tests")
C_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".m", ".mm"}
C_FORMAT_PATHS = tuple(
    str(path.relative_to(ROOT))
    for source_root in (ROOT / "src", ROOT / "tests", ROOT / "platforms")
    for path in sorted(source_root.rglob("*"))
    if path.is_file() and path.suffix in C_SOURCE_SUFFIXES
)

C_TIDY_PATHS = (
    "src/common/log.c",
    "src/harness/artifacts.c",
    "src/harness/puae_options.c",
    "src/port/config.c",
    "src/port/project_paths.c",
    "src/port/overrides/audio.c",
    "tests/test_log.c",
    "tests/test_project_paths.c",
)

CXX_TIDY_PATHS = (
    "src/runtime/guest_runtime.cpp",
    "src/runtime/guest_call_policy.cpp",
    "src/platform/disk_selection_store.cpp",
    "tests/test_disk_selection_store.cpp",
    "tests/test_guest_call_policy.cpp",
)


#: Tools this run shells out to that the repository does not ship. Named here
#: so a machine without one is told which package to install, rather than being
#: handed a FileNotFoundError from inside subprocess.
EXTERNAL_TOOLS = ("clang-format", "clang-tidy", "node")

#: Where each of them comes from, for the message.
_INSTALLED_BY = {
    "clang-format": "brew install clang-format",
    "clang-tidy": "brew install llvm (clang-tidy is not in Apple's command line tools)",
    "node": "brew install node",
}


def _require(tool: str) -> str:
    """`tool` if it is on PATH; otherwise exit saying which one and how to get it."""
    found = shutil.which(tool)
    if found:
        return tool
    remedy = _INSTALLED_BY.get(tool, f"install {tool} and put it on PATH")
    raise SystemExit(f"verify needs {tool}, which is not on PATH — {remedy}")


def _run(arguments: list[str], cwd: Path = ROOT) -> None:
    subprocess.run(arguments, cwd=cwd, check=True)


def _compile_and_run_c_test(compiler: list[str], name: str, sources: list[str]) -> None:
    executable = ROOT / "build" / "verification" / name
    executable.parent.mkdir(parents=True, exist_ok=True)
    _run(
        [
            *compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Isrc",
            *sources,
            "-o",
            str(executable),
        ]
    )
    _run([str(executable)])


def _compile_and_run_cpp_test(compiler: list[str], name: str, sources: list[str]) -> None:
    executable = ROOT / "build" / "verification" / name
    executable.parent.mkdir(parents=True, exist_ok=True)
    _run(
        [
            *compiler,
            "-std=c++20",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Isrc",
            *sources,
            "-o",
            str(executable),
        ]
    )
    activity = SCRATCH / "verification" / name
    activity.parent.mkdir(parents=True, exist_ok=True)
    _run([str(executable), str(activity)])


def main() -> int:
    _run([sys.executable, "-m", "ruff", "format", "--check", *PYTHON_PATHS])
    _run([sys.executable, "-m", "ruff", "check", *PYTHON_PATHS])
    _run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v"])
    for tool in EXTERNAL_TOOLS:
        _require(tool)
    _run(["node", "--test", "tests/web_disk_setup.test.mjs"])
    sdl_source = os.environ.get("BENEFACTOR_SDL3_DIR")
    sdl_include_args: list[str] = []
    if sdl_source:
        sdl_include = Path(sdl_source).expanduser().resolve() / "include"
        if not (sdl_include / "SDL3" / "SDL.h").is_file():
            raise SystemExit(f"verify needs SDL3 headers at {sdl_include}")
        sdl_include_args.append(f"-I{sdl_include}")
    _run(["clang-format", "--dry-run", "--Werror", *C_FORMAT_PATHS])
    _run(["clang-tidy", *C_TIDY_PATHS, "--", "-std=c11", "-Isrc", *sdl_include_args])
    shared_include = amigaport_dir() / "include"
    if not (shared_include / "amigaport" / "executor.hpp").is_file():
        raise SystemExit(f"verify needs shared/amigaport headers at {shared_include}")
    _run(
        [
            "clang-tidy",
            *CXX_TIDY_PATHS,
            "--",
            "-std=c++20",
            "-Isrc",
            f"-I{shared_include}",
            *sdl_include_args,
        ]
    )
    compiler = shlex.split(os.environ.get("CC", "cc"))
    _compile_and_run_c_test(compiler, "test_log", ["src/common/log.c", "tests/test_log.c"])
    _compile_and_run_c_test(
        compiler,
        "test_project_paths",
        ["src/port/project_paths.c", "tests/test_project_paths.c"],
    )
    # The lockstep protocol is a header both products include, so its test
    # needs no product objects — and must pass before either is measured
    # against the other (tools/lockstep.py).
    _compile_and_run_c_test(compiler, "test_lockstep_digest", ["tests/test_lockstep_digest.c"])
    # The busy-wait recogniser decides where this product's frames begin and
    # end (src/port/wait_idiom.h), so it is pure and header-only on purpose.
    _compile_and_run_c_test(compiler, "test_wait_idiom", ["tests/test_wait_idiom.c"])
    cpp_compiler = shlex.split(os.environ.get("CXX", "c++"))
    _compile_and_run_cpp_test(
        cpp_compiler,
        "disk-selection-store",
        ["src/platform/disk_selection_store.cpp", "tests/test_disk_selection_store.cpp"],
    )
    _compile_and_run_cpp_test(
        cpp_compiler, "guest-call-policy", ["tests/test_guest_call_policy.cpp"]
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
