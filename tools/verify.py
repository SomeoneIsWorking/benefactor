from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from tools.harness_tools import harness_tool
from tools.paths import ROOT, SCRATCH, amigaport_dir
from tools.product_version import read_version
from tools.shared_checkouts import TREES, vendored_subtrees

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
    "src/engine/frame_pacer.cpp",
    "tests/test_frame_pacer.cpp",
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

#: Homebrew keeps some formulae out of PATH. llvm is one, so a host that has
#: clang-tidy installed still answers `which clang-tidy` with nothing, and the
#: install command below is an instruction to do what is already done.
_KEG_ONLY_FORMULA = {"clang-format": "llvm", "clang-tidy": "llvm"}


#: A rasteriser is what lets the app icon be checked as a player meets it — at
#: launcher sizes, not as SVG text. Either ImageMagick 7 or 6's `convert` will do.
RASTERISERS = ("magick", "convert")


def _lucent_root() -> Path:
    """The lucent checkout this build uses, named when it is missing.

    Where a shared tree may be is `tools/shared_checkouts.py`'s to know, and
    this had grown a second, shorter copy of that list: it looked only beside
    the repository, so in a worktree — where "beside" is .claude/worktrees —
    verify could not find the checkout the resolver itself had just made, and
    stopped before the C++ gates. The candidates come from the resolver now.
    Resolving is all that is borrowed: verify reports what is on the machine and
    never checks anything out or moves a checkout onto the pin.
    """
    tree = next(shared for shared in TREES if shared.name == "lucent")
    configured = os.environ.get(tree.variable)
    candidates = ([Path(configured).expanduser().resolve()] if configured else []) + list(
        tree.candidates
    )
    for candidate in candidates:
        if (candidate / "include" / "lucent" / "version.h").is_file():
            return candidate
    tried = ", ".join(str(candidate) for candidate in candidates)
    raise SystemExit(f"verify needs the lucent checkout (include/lucent/version.h); tried {tried}")


def lucent_include_dir() -> Path:
    return _lucent_root() / "include"


def lucent_version_source() -> str:
    """lucent's version translation unit, which is self-contained."""
    return str(_lucent_root() / "src" / "version.cpp")


def _run_optional_winhttp_transport() -> None:
    """Run the Windows update transport here when this host can build and run it."""
    cross_compiler = shutil.which("i686-w64-mingw32-g++")
    wine = shutil.which("wine")
    missing = [
        name
        for name, found in (
            ("i686-w64-mingw32-g++", cross_compiler),
            ("i686-w64-mingw32-gcc", shutil.which("i686-w64-mingw32-gcc")),
            ("wine", wine),
        )
        if found is None
    ]
    if missing:
        print(
            f"winhttp transport: not run (missing {', '.join(missing)}); "
            "the Windows CI job runs the same check natively"
        )
        return
    executable = ROOT / "build" / "verification" / "winhttp-transport.exe"
    executable.parent.mkdir(parents=True, exist_ok=True)
    log_object = ROOT / "build" / "verification" / "log-mingw.o"
    # The C translation unit is compiled by the C compiler, as the build system
    # does, and only the C++ ones by the C++ compiler.
    _run(
        [
            shutil.which("i686-w64-mingw32-gcc") or str(cross_compiler),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Isrc",
            "-c",
            "src/common/log.c",
            "-o",
            str(log_object),
        ]
    )
    _run(
        [
            str(cross_compiler),
            "-std=c++20",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-static",
            "-Isrc",
            f"-I{lucent_include_dir()}",
            f'-DBENEFACTOR_VERSION="{read_version()}"',
            "src/platform/winhttp_update.cpp",
            "src/port/update_check.cpp",
            "tests/winhttp_transport.cpp",
            lucent_version_source(),
            str(log_object),
            "-lwinhttp",
            "-o",
            str(executable),
        ]
    )
    _run([str(wine), str(executable)], environment=dict(os.environ, WINEDEBUG="-all"))


def _keg_only_bin(tool: str) -> Path | None:
    """`tool` inside its keg-only Homebrew prefix, which is not on PATH."""
    formula = _KEG_ONLY_FORMULA.get(tool)
    brew = shutil.which("brew")
    if formula is None or brew is None:
        return None
    prefix = subprocess.run(
        [brew, "--prefix", formula], text=True, capture_output=True, check=False
    )
    if prefix.returncode != 0:
        return None
    candidate = Path(prefix.stdout.strip()) / "bin" / tool
    return candidate if candidate.is_file() else None


def _sdl_include_args() -> list[str]:
    """Where the SDL3 headers the tidy runs parse are, on this host.

    CI names a checkout; a developer machine usually has the package instead,
    and tidy reporting `SDL3/SDL.h` not found reads as broken sources rather
    than as an unset variable.
    """
    configured = os.environ.get("BENEFACTOR_SDL3_DIR")
    if configured:
        include = Path(configured).expanduser().resolve() / "include"
        if not (include / "SDL3" / "SDL.h").is_file():
            raise SystemExit(f"verify needs SDL3 headers at {include}")
        return [f"-I{include}"]
    for command in (["pkg-config", "--cflags-only-I", "sdl3"], ["brew", "--prefix", "sdl3"]):
        tool = shutil.which(command[0])
        if tool is None:
            continue
        found = subprocess.run([tool, *command[1:]], text=True, capture_output=True, check=False)
        if found.returncode != 0:
            continue
        for token in shlex.split(found.stdout.strip()):
            include = Path(token.removeprefix("-I"))
            if command[0] == "brew":
                include = include / "include"
            if (include / "SDL3" / "SDL.h").is_file():
                return [f"-I{include}"]
    raise SystemExit(
        "verify needs the SDL3 headers: install sdl3, or set BENEFACTOR_SDL3_DIR to a checkout"
    )


def _require(tool: str) -> str:
    """`tool` if this host has it; otherwise exit saying which one and how to get it.

    A keg-only prefix is searched as well as PATH, and put on PATH for the rest
    of the run, so an installed tool is used rather than reported missing.
    """
    if shutil.which(tool):
        return tool
    keg_only = _keg_only_bin(tool)
    if keg_only is not None:
        os.environ["PATH"] = os.pathsep.join([str(keg_only.parent), os.environ.get("PATH", "")])
        return tool
    remedy = _INSTALLED_BY.get(tool, f"install {tool} and put it on PATH")
    searched = "PATH"
    if tool in _KEG_ONLY_FORMULA:
        searched = f"PATH and the {_KEG_ONLY_FORMULA[tool]} Homebrew prefix"
    raise SystemExit(f"verify needs {tool}, which is on neither {searched} — {remedy}")


def _run(
    arguments: list[str],
    cwd: Path = ROOT,
    environment: dict[str, str] | None = None,
) -> None:
    print("$", shlex.join(str(argument) for argument in arguments), flush=True)
    subprocess.run(arguments, cwd=cwd, check=True, env=environment)


#: Where a configure step leaves a compile database. The ownership scan reads
#: the real commands the compiler was given, so it cannot run without one.
COMPILE_DATABASES = ("build/run/compile_commands.json", "build/debug/compile_commands.json")
#: The C boundary this port still holds, each site with its reason.
ACCEPTED_OWNERSHIP = "tools/cpp_ownership_accepted.txt"


def _run_cpp_policy() -> None:
    """The shared gates: the clang policy this project declares, and who owns what.

    `re-harness` owns both rules — braces on every body, no disabled defaults,
    warnings as errors; and no global function, header `extern` or hidden
    function-local static outside the sites this project has named in
    `tools/cpp_ownership_accepted.txt`. Either gate is skipped on a host that
    cannot run it, and says so by name: a check nobody can see failing is worse
    than one that is absent.
    """
    tool = harness_tool("cpp_policy.py")
    if tool is None:
        print(
            "cpp policy: not run (no re-harness checkout beside this one; set "
            "RE_HARNESS_DIR); the shared clang policy was NOT checked",
            flush=True,
        )
        return
    _run([sys.executable, str(tool), "--audit-config", "."])
    database = next((ROOT / name for name in COMPILE_DATABASES if (ROOT / name).is_file()), None)
    if database is None:
        print(
            "cpp ownership: not run (no compile database at "
            f"{' or '.join(COMPILE_DATABASES)}; configure a build first); "
            "C++ ownership was NOT checked",
            flush=True,
        )
        return
    #: The shared trees are consumed, and a checkout of one inside this
    #: repository is still not this repository's code. Without this the scan
    #: reads the compile database, finds RmlUi and lucent under the root, and
    #: reports their function-local statics as ownership violations — on CI,
    #: which always checks them out inside, and on any host without them
    #: beside it. Only paths the resolver may fill are named.
    excluded = []
    for subtree in vendored_subtrees():
        excluded.extend(["--exclude", str(subtree)])
    _run(
        [
            sys.executable,
            str(tool),
            "--root",
            str(ROOT),
            "--compile-commands",
            str(database),
            "--accept",
            str(ROOT / ACCEPTED_OWNERSHIP),
            *excluded,
        ]
    )


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


def _compile_and_run_cpp_test(
    compiler: list[str],
    name: str,
    sources: list[str],
    includes: tuple[str, ...] = (),
) -> None:
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
            *(f"-I{include}" for include in includes),
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
    if not any(shutil.which(tool) for tool in RASTERISERS):
        raise SystemExit(
            "verify needs ImageMagick (`magick` or `convert`) to rasterise the app icon "
            "at the sizes it ships in — sudo dnf install ImageMagick librsvg2-tools, or "
            "sudo apt install imagemagick librsvg2-bin"
        )
    _run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v"])
    for tool in EXTERNAL_TOOLS:
        _require(tool)
    _run(
        [
            "node",
            "--test",
            "tests/web_disk_setup.test.mjs",
            "tests/web_release_check.test.mjs",
        ]
    )
    sdl_include_args = _sdl_include_args()
    _run_cpp_policy()
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
    # The frame deadline is arithmetic against an injected clock, so it needs no
    # SDL and no product objects — which is the point of keeping the rule out of
    # the file that binds it to SDL.
    _compile_and_run_cpp_test(
        cpp_compiler,
        "frame-pacer",
        ["src/engine/frame_pacer.cpp", "tests/test_frame_pacer.cpp"],
    )
    # The picker report is the boundary between Android's staged documents and
    # the title's disk-set validation, and needs no product objects.
    _compile_and_run_cpp_test(
        cpp_compiler,
        "selection-report",
        ["src/platform/selection_report.cpp", "tests/test_selection_report.cpp"],
    )
    # The Windows transport is invisible to every other platform's build, and a
    # compile is not enough for it: the URL form it needs and the API's buffer
    # rules only fail when it runs. Where a Windows cross-compiler and Wine are
    # both present, run it against the real service; otherwise say it did not run
    # here rather than reporting it as passing — the Windows CI job runs the same
    # check natively.
    _run_optional_winhttp_transport()
    # The update check's rule is that a check which could not run never looks
    # like "up to date". It reads a version (lucent) and keeps state, so it needs
    # no product objects beyond its own translation unit.
    _compile_and_run_cpp_test(
        cpp_compiler,
        "update-policy",
        ["src/port/update_check.cpp", "tests/test_update_policy.cpp", lucent_version_source()],
        includes=(str(lucent_include_dir()),),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
