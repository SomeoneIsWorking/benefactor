from __future__ import annotations

import os
import shlex
import subprocess
import sys
from pathlib import Path

from tools.paths import ROOT

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
    "src/port/config.c",
    "src/port/project_paths.c",
    "tests/test_log.c",
    "tests/test_project_paths.c",
)


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


def main() -> int:
    _run([sys.executable, "-m", "ruff", "format", "--check", *PYTHON_PATHS])
    _run([sys.executable, "-m", "ruff", "check", *PYTHON_PATHS])
    _run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v"])
    _run(["clang-format", "--dry-run", "--Werror", *C_FORMAT_PATHS])
    _run(["clang-tidy", *C_TIDY_PATHS, "--", "-std=c11", "-Isrc"])
    compiler = shlex.split(os.environ.get("CC", "cc"))
    _compile_and_run_c_test(compiler, "test_log", ["src/common/log.c", "tests/test_log.c"])
    _compile_and_run_c_test(
        compiler,
        "test_project_paths",
        ["src/port/project_paths.c", "tests/test_project_paths.c"],
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
