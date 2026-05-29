#!/usr/bin/env python3
import argparse
import glob
import os
import re
from dataclasses import dataclass
from typing import List, Optional, Tuple


ROOT_DIFF_RE = re.compile(
    r"\[ROOT_DIFF\]\s+frame=(?P<frame>-?\d+)\s+word=(?P<word>\d+)\s+insn=(?P<insn>\d+)\s+"
    r"addr=\$(?P<addr>[0-9A-Fa-f]+)\s+reg=\$(?P<reg>[0-9A-Fa-f]+)\s+reg_name=(?P<reg_name>\S+)\s+"
    r"puae=\$(?P<puae>[0-9A-Fa-f]+)\s+pc=\$(?P<pc>[0-9A-Fa-f]+)"
)

COPLST_RE = re.compile(
    r"\[COPLST-DIFF\]\s+word\[(?P<word>\d+)\]\s+addr=\$(?P<addr>[0-9A-Fa-f]+)\s+"
    r"PUAE=\$(?P<puae>[0-9A-Fa-f]+)\s+PC=\$(?P<pc>[0-9A-Fa-f]+)"
)

WRITE_RE = re.compile(
    r"\[(?P<tag>ROOT_[A-Z0-9_]+)\]\s+frame=(?P<frame>-?\d+)\s+addr=\$(?P<addr>[0-9A-Fa-f]+)\s+"
    r"old=\$(?P<old>[0-9A-Fa-f]+)\s+new=\$(?P<new>[0-9A-Fa-f]+).*?pc=\$(?P<pc>[0-9A-Fa-f]+)"
)

REG_NAMES = {
    0x100: "BPLCON0",
    0x102: "BPLCON1",
    0x104: "BPLCON2",
    0x108: "BPL1MOD",
    0x10A: "BPL2MOD",
    0x180: "COLOR00",
    0x1E0: "BPL1PTH",
    0x1E2: "BPL1PTL",
}


@dataclass
class RootDiff:
    frame: int
    word: int
    insn: int
    addr: int
    reg: int
    reg_name: str
    puae: int
    pc: int
    source: str


@dataclass
class Write:
    tag: str
    frame: int
    addr: int
    old: int
    new: int
    pc: int
    path: str
    line_no: int

    @property
    def side(self) -> str:
        return "PUAE" if "PUAE" in self.tag else "PC"

    @property
    def kind(self) -> str:
        return "BLITTER" if "BLT" in self.tag else "CPU"


def read_lines(path: str) -> List[str]:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.readlines()
    except OSError:
        return []


def parse_root_diff(log_files: List[str], report_path: str, run_path: str) -> Optional[RootDiff]:
    for path in log_files + [report_path, run_path]:
        for ln in read_lines(path):
            m = ROOT_DIFF_RE.search(ln)
            if m:
                return RootDiff(
                    frame=int(m.group("frame")),
                    word=int(m.group("word")),
                    insn=int(m.group("insn")),
                    addr=int(m.group("addr"), 16),
                    reg=int(m.group("reg"), 16),
                    reg_name=m.group("reg_name"),
                    puae=int(m.group("puae"), 16),
                    pc=int(m.group("pc"), 16),
                    source=path,
                )
    for path in [report_path, run_path] + log_files:
        for ln in read_lines(path):
            m = COPLST_RE.search(ln)
            if m:
                word = int(m.group("word"))
                insn = word // 2
                return RootDiff(
                    frame=-1,
                    word=word,
                    insn=insn,
                    addr=int(m.group("addr"), 16),
                    reg=0,
                    reg_name="UNKNOWN",
                    puae=int(m.group("puae"), 16),
                    pc=int(m.group("pc"), 16),
                    source=path,
                )
    return None


def parse_writes(log_files: List[str], target_addr: int, target_frame: int) -> List[Write]:
    out: List[Write] = []
    for path in log_files:
        lines = read_lines(path)
        for i, ln in enumerate(lines, 1):
            m = WRITE_RE.search(ln)
            if not m:
                continue
            addr = int(m.group("addr"), 16)
            frame = int(m.group("frame"))
            if addr != target_addr:
                continue
            if target_frame >= 0 and frame != target_frame:
                continue
            out.append(
                Write(
                    tag=m.group("tag"),
                    frame=frame,
                    addr=addr,
                    old=int(m.group("old"), 16),
                    new=int(m.group("new"), 16),
                    pc=int(m.group("pc"), 16),
                    path=path,
                    line_no=i,
                )
            )
    return out


def infer_cause(diff: RootDiff, writes: List[Write]) -> Tuple[str, str]:
    pc_writes = [w for w in writes if w.side == "PC"]
    puae_writes = [w for w in writes if w.side == "PUAE"]
    last_pc = pc_writes[-1] if pc_writes else None
    last_puae = puae_writes[-1] if puae_writes else None

    reg_name = diff.reg_name
    if reg_name == "UNKNOWN" and diff.reg in REG_NAMES:
        reg_name = REG_NAMES[diff.reg]

    if not pc_writes and puae_writes:
        cause = f"Missing write/init — PUAE writes {reg_name} at ${diff.addr:06X} but PC has no write for the diverging frame."
        fix = "Add/restore the missing init/rebuild call before snapshot."
        return cause, fix

    if last_pc and last_pc.kind == "BLITTER" and last_pc.new == 0xFFFF and 0x8720 <= diff.addr <= 0x872C:
        cause = f"Missing rebuild after blitter clobber — BLITTER leaves ${diff.addr:06X} at $FFFF and no later CPU restore reaches PUAE value."
        fix = "Restore static copper words in native override after blit (native_rebuild_copper_static / sprite setup path)."
        return cause, fix

    if last_pc and last_pc.kind == "CPU" and last_pc.new != diff.puae:
        puae_hint = ""
        if last_puae:
            puae_hint = f" PUAE last write is ${last_puae.new:04X} at pc=${last_puae.pc:06X}."
        cause = (
            f"Wrong computation on PC CPU path — final PC write sets ${diff.addr:06X}={last_pc.new:04X}, "
            f"but PUAE snapshot expects ${diff.puae:04X}.{puae_hint}"
        )
        fix = "Trace this writer PC and reimplement/fix computation (native override or recompiler fix)."
        return cause, fix

    if last_pc and last_pc.kind == "BLITTER" and diff.puae != diff.pc:
        cause = f"Blitter data-stream divergence — last writer is BLITTER at pc=${last_pc.pc:06X} producing ${last_pc.new:04X} instead of ${diff.puae:04X}."
        fix = "Trace upstream blitter source pointers/control words and align launch parameters with PUAE."
        return cause, fix

    cause = "Insufficient structured evidence to auto-classify precisely."
    fix = "Enable BENEFACTOR_ROOT_TRACE=1 and rerun harness to collect ROOT_* write logs for both sides."
    return cause, fix


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze Benefactor divergence logs and propose root cause.")
    ap.add_argument("--logs-dir", default="logs", help="Directory containing *_frame_*.log files (default: logs)")
    ap.add_argument("--report", default="logs/harness_report.txt", help="Harness report path")
    ap.add_argument("--run-log", default="logs/harness_run.txt", help="Harness run log path")
    ap.add_argument("--frame", type=int, default=None, help="Override frame number to analyze")
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=None, help="Override address (e.g. 0x872A)")
    args = ap.parse_args()

    log_files = sorted(glob.glob(os.path.join(args.logs_dir, "*_frame_*.log")))
    if not log_files:
        print(f"No per-frame logs found in {args.logs_dir}.")
        print("Run harness with BENEFACTOR_ROOT_TRACE=1 to produce ROOT_* logs.")
        return 1

    diff = parse_root_diff(log_files, args.report, args.run_log)
    if not diff and (args.frame is None or args.addr is None):
        print("Could not find ROOT_DIFF/COPLST-DIFF in logs.")
        print("Provide --frame and --addr, or rerun harness to emit ROOT_DIFF.")
        return 1

    frame = args.frame if args.frame is not None else (diff.frame if diff else -1)
    addr = args.addr if args.addr is not None else (diff.addr if diff else 0)

    writes = parse_writes(log_files, addr, frame)
    writes.sort(key=lambda w: (w.path, w.line_no))

    print("=== Divergence Target ===")
    if diff:
        reg_name = diff.reg_name
        if reg_name == "UNKNOWN" and diff.reg in REG_NAMES:
            reg_name = REG_NAMES[diff.reg]
        print(
            f"frame={frame} word={diff.word} insn={diff.insn} addr=${addr:06X} reg=${diff.reg:03X} {reg_name} "
            f"PUAE=${diff.puae:04X} PC=${diff.pc:04X}"
        )
        print(f"source={diff.source}")
    else:
        print(f"frame={frame} addr=${addr:06X}")

    pc_writes = [w for w in writes if w.side == "PC"]
    puae_writes = [w for w in writes if w.side == "PUAE"]

    print("\n=== Last writes at target address ===")
    if not writes:
        print("No ROOT_* writes found for this address/frame.")
    else:
        for w in (puae_writes[-4:] + pc_writes[-4:]):
            print(
                f"{w.side:4} {w.kind:7} frame={w.frame:4d} addr=${w.addr:06X} old=${w.old:04X} "
                f"new=${w.new:04X} pc=${w.pc:06X} file={os.path.basename(w.path)}:{w.line_no}"
            )

    if diff:
        cause, fix = infer_cause(diff, writes)
        print("\n=== Root Cause Suggestion ===")
        print(f"CAUSE: {cause}")
        print(f"FIX:   {fix}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
