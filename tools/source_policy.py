from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".frag",
    ".glsl",
    ".h",
    ".hh",
    ".hpp",
    ".java",
    ".m",
    ".mm",
    ".vert",
}
DEFAULT_SOURCE_LINE_LIMIT = 1200
LEGACY_SOURCE_LINE_LIMITS = {
    # These frozen counts use the repository's tracked clang-format style so
    # formatting cannot masquerade as structural growth or reduction.
    "src/engine/hw.c": 2562,
    "src/harness/harness_main.c": 2665,
    "src/port/game_loop.c": 1361,
    "src/port/overrides/gameplay.c": 1360,
    "src/render/native_renderer.c": 1699,
    "src/render/present_vulkan.c": 1766,
}
RETIRED_PATHS = (
    "tools/recomp",
    "src/engine/generated",
    "src/engine/rt.c",
    "src/engine/rt.h",
)
STATIC_PATTERNS = {
    "generated source include": re.compile(r'#\s*include\s*[<"]engine/generated/'),
    "generated guest symbol": re.compile(r"\bgfn_[A-Za-z0-9_]+\s*\("),
    "generated-body call": re.compile(r"\brt_call_generated\s*\("),
    "static function symbol": re.compile(r"\bg_fn_[A-Za-z0-9_]+\b"),
    "static function table": re.compile(r"\bg_fn_(?:table|gp|gpl|credits)\b"),
    "static corpus function count": re.compile(r"\bGAME_FN_COUNT\b"),
    "static dispatcher call state": re.compile(r"\bg_rt_last_call\b"),
    "static fallback selector": re.compile(r"\bBENEFACTOR_RECOMP_[A-Z0-9_]+\b"),
    "retired bank-dump entry": re.compile(r"(?:\bpc_dump_banks_from_disk\b|--dump-banks\b)"),
}
STALE_STATIC_WORDING = {
    "generated-function vocabulary": re.compile(r"\bgfn_[A-Za-z0-9_]+\b"),
    "static-recompiler vocabulary": re.compile(
        r"\b(?:static[ -]recomp(?:iler|ilation)?|recompil(?:ed|er|ation|able)|recomp body)\b",
        re.IGNORECASE,
    ),
    "generated-code vocabulary": re.compile(
        r"\b(?:generated[ -](?:guest )?code|generated-C)\b", re.IGNORECASE
    ),
    "generated-handler vocabulary": re.compile(
        r"\bgenerated[ -](?:guest |host )?(?:function|handler|body)\b", re.IGNORECASE
    ),
    "retired recomp path vocabulary": re.compile(r"\brecomp/"),
}
FORBIDDEN_RUNTIME_PATH_PATTERNS = {
    "forbidden /tmp runtime path": re.compile(r"(?<![A-Za-z0-9_])/tmp(?:/|\b)"),
}
PRODUCT_COUPLING_PATTERNS = {
    "direct diagnostic-emulator product dependency": re.compile(
        r"(?:vendor/libretro-uae|benefactor-harness)"
    ),
}
PROCESS_OUTPUT_PATTERNS = {
    "direct process stream fprintf": re.compile(r"\bfprintf\s*\(\s*(?:stderr|stdout)\b"),
    "direct process stream fputs": re.compile(r"\bfputs\s*\([^,]+,\s*(?:stderr|stdout)\b"),
    "direct process stream flush": re.compile(r"\bfflush\s*\(\s*(?:stderr|stdout)\b"),
    "direct printf": re.compile(r"(?<![A-Za-z0-9_])printf\s*\("),
    "direct puts": re.compile(r"(?<![A-Za-z0-9_])puts\s*\("),
    "direct putchar": re.compile(r"(?<![A-Za-z0-9_])putchar\s*\("),
    "direct perror": re.compile(r"\bperror\s*\("),
    "direct descriptor-two write": re.compile(r"\bwrite\s*\(\s*2\s*,"),
    "direct STDERR_FILENO write": re.compile(r"\bwrite\s*\(\s*STDERR_FILENO\s*,"),
    "direct Java process stream": re.compile(r"\bSystem\s*\.\s*(?:err|out)\s*\."),
}
GETENV_PATTERN = re.compile(r"\bgetenv\s*\(")
DEBUG_GATED_LOG_PATTERN = re.compile(
    r"\bif\s*\([^)]*(?:debug|dbg|verbose|trace|_log)[^)]*\)"
    r"\s*\{?\s*(?:benefactor_log_write|GLOBAL_LOG|HW_LOG|HWTRACE)\s*\(",
    re.IGNORECASE | re.DOTALL,
)


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


def _without_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//.*", "", text)


def _line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _product_sources(root: Path) -> tuple[Path, ...]:
    return tuple(
        sorted(
            path
            for source_root in (root / "src", root / "tests", root / "platforms")
            if source_root.is_dir()
            for path in source_root.rglob("*")
            if path.is_file() and path.suffix in SOURCE_SUFFIXES
        )
    )


def check(root: Path) -> tuple[list[Finding], int]:
    findings: list[Finding] = []
    for relative in RETIRED_PATHS:
        path = root / relative
        if path.exists():
            findings.append(Finding(path, 1, "retired static path still exists"))

    for path in _product_sources(root):
        text = _without_comments(path.read_text(encoding="utf-8", errors="replace"))
        for label, pattern in STATIC_PATTERNS.items():
            for match in pattern.finditer(text):
                findings.append(Finding(path, _line_number(text, match.start()), label))
        raw_text = path.read_text(encoding="utf-8", errors="replace")
        for label, pattern in STALE_STATIC_WORDING.items():
            for match in pattern.finditer(raw_text):
                findings.append(Finding(path, _line_number(raw_text, match.start()), label))
        for label, pattern in FORBIDDEN_RUNTIME_PATH_PATTERNS.items():
            for match in pattern.finditer(raw_text):
                findings.append(Finding(path, _line_number(raw_text, match.start()), label))

    cmake = root / "CMakeLists.txt"
    launch_text = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in (cmake, root / "run.sh", root / "bootstrap.py", root / "tools/launcher.py")
        if path.is_file()
    )
    for label, pattern in PRODUCT_COUPLING_PATTERNS.items():
        match = pattern.search(launch_text)
        if match:
            findings.append(Finding(cmake, _line_number(launch_text, match.start()), label))

    product_sources = _product_sources(root)
    config_owner = root / "src/port/config.c"
    log_owner = root / "src/common/log.c"
    for path in product_sources:
        relative = path.relative_to(root).as_posix()
        line_count = len(path.read_text(encoding="utf-8", errors="replace").splitlines())
        line_limit = LEGACY_SOURCE_LINE_LIMITS.get(relative, DEFAULT_SOURCE_LINE_LIMIT)
        if line_count > line_limit:
            findings.append(
                Finding(
                    path,
                    line_limit + 1,
                    f"source has {line_count} lines; structural limit is {line_limit}",
                )
            )
        text = _without_comments(path.read_text(encoding="utf-8", errors="replace"))
        if path != log_owner:
            for label, pattern in PROCESS_OUTPUT_PATTERNS.items():
                for match in pattern.finditer(text):
                    findings.append(Finding(path, _line_number(text, match.start()), label))
        for match in DEBUG_GATED_LOG_PATTERN.finditer(text):
            findings.append(
                Finding(path, _line_number(text, match.start()), "debug-gated process log")
            )
        if path != config_owner:
            for match in GETENV_PATTERN.finditer(text):
                findings.append(
                    Finding(path, _line_number(text, match.start()), "getenv outside config owner")
                )

    excluded_tree_names = {".git", ".venv", "build", "scratch", "vendor"}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if any(part in excluded_tree_names for part in relative.parts) or relative == Path(
            "run.sh"
        ):
            continue
        is_shell_path = path.suffix == ".sh"
        try:
            with path.open("rb") as source:
                first_line = source.readline(256).decode("utf-8", errors="ignore")
        except OSError:
            first_line = ""
        is_shell_program = bool(re.match(r"^#!.*\b(?:ba|z|da|k)?sh\b", first_line))
        if is_shell_path or is_shell_program:
            findings.append(Finding(path, 1, "non-launcher shell tooling is forbidden"))

    return findings, len(product_sources)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    findings, product_source_count = check(root)
    if findings:
        for finding in findings:
            print(f"{finding.path.relative_to(root)}:{finding.line}: {finding.message}")
        print(f"source policy failed with {len(findings)} finding(s)")
        return 1
    print(
        "source policy passed: retired paths absent; static interfaces absent; "
        f"{product_source_count} retained first-party product/test source(s) inspected"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
