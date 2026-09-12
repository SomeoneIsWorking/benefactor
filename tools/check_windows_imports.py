"""Reject a Windows release executable with unbundled DLL dependencies."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

SYSTEM_DLLS = frozenset(
    {
        "advapi32.dll",
        "gdi32.dll",
        "imm32.dll",
        "kernel32.dll",
        "msvcrt.dll",
        "ole32.dll",
        "oleaut32.dll",
        "setupapi.dll",
        "shell32.dll",
        "user32.dll",
        "version.dll",
        "winmm.dll",
        "ws2_32.dll",
    }
)


def unbundled_imports(import_table: str, packaged_dlls: set[str]) -> list[str]:
    """List PE imports that are neither Windows system DLLs nor in the package."""
    imports = {
        line.partition("DLL Name:")[2].strip().lower()
        for line in import_table.splitlines()
        if "DLL Name:" in line
    }
    if not imports:
        raise ValueError("objdump did not report any PE DLL imports")
    provided = SYSTEM_DLLS | {name.lower() for name in packaged_dlls}
    return sorted(imports - provided)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--objdump", default="objdump")
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        raise SystemExit(f"Windows import check: missing executable: {executable}")
    inspected = subprocess.run(
        [args.objdump, "-p", str(executable)],
        check=True,
        capture_output=True,
        text=True,
    )
    packaged = {path.name for path in executable.parent.glob("*.dll")}
    try:
        missing = unbundled_imports(inspected.stdout, packaged)
    except ValueError as error:
        raise SystemExit(f"Windows import check: {error}") from error
    if missing:
        raise SystemExit("Windows import check: unbundled DLLs: " + ", ".join(missing))
    print("Windows import check: all non-system DLL imports are bundled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
