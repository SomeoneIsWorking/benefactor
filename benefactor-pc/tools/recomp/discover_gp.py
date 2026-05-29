#!/usr/bin/env python3
"""Automated gameplay-overlay bank discovery driver.

The gameplay code is a disk overlay recompiled as a second bank (recomp.py
--bank gp).  Static descent + a5-base resolution finds the directly/indirectly
*called* functions, but handlers reached only via the IRQ vector table or
runtime-computed dispatch are missed; at runtime rt_call logs
"no function at $X – skipping" for each (RT_SKIPLOG=1), which is proof X is a
real gameplay call target.

This drives a regen -> build -> run -> collect loop to a fixpoint, accumulating
gameplay seed addresses into gp_seeds.txt (consumed by the build's recompile of
the gp bank).  Mirrors discover_indirect.py for the intro bank.

Run from the repo root (or anywhere; paths are resolved relative to this file):
    python3 benefactor-pc/tools/recomp/discover_gp.py [--max-iters N]
Requires the gameplay image at logs/chip_flow_256_0081D2.bin and the disks.
"""
import os, re, subprocess, sys

HERE   = os.path.dirname(os.path.abspath(__file__))
PC_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))   # benefactor-pc
ROOT   = os.path.abspath(os.path.join(PC_DIR, ".."))       # repo root
RECOMP = os.path.join(HERE, "recomp.py")
GEN    = os.path.join(PC_DIR, "src", "generated")
BUILD  = os.path.join(PC_DIR, "build")
BIN    = os.path.join(BUILD, "benefactor-harness")
SEEDS  = os.path.join(HERE, "gp_seeds.txt")
IMAGE  = os.path.join(ROOT, "logs", "chip_flow_256_0081D2.bin")

DISKS  = ["Hard Drives/DEVS/Kickstarts",
          "whdload/Benefactor/Benefactor.slave",
          "whdload/Benefactor/Disk.1",
          "whdload/Benefactor/Disk.2",
          "whdload/Benefactor/Disk.3"]

INIT_SEEDS = [0x3330, 0x3532, 0x3544]   # entry + level-3 + level-6 IRQ vectors
FIRE       = "170,216,262,308,354,400,446,492,538"  # pulse train to the menu
NFRAMES    = "540"                      # just past the menu (frame ~487) + IRQ ticks
LO, HI     = 0x3294, 0x80000            # gameplay (overlaid) address range
SKIP_RE    = re.compile(r"no function at \$([0-9A-Fa-f]+)")


def load_seeds():
    if os.path.exists(SEEDS):
        with open(SEEDS) as f:
            s = {int(x, 16) for x in f.read().replace("\n", ",").split(",") if x.strip()}
            return s | set(INIT_SEEDS)
    return set(INIT_SEEDS)


def save_seeds(seeds):
    with open(SEEDS, "w") as f:
        f.write(",".join(f"{a:X}" for a in sorted(seeds)) + "\n")


def regen(seeds):
    subprocess.run([sys.executable, RECOMP, IMAGE, "--chip-dump",
                    "--base", "3000", "--code-size", "46000",
                    "--seed", ",".join(f"{a:X}" for a in sorted(seeds)),
                    "--bank", "gp", "--areg", "5=511E", "--out-dir", GEN],
                   check=True, cwd=ROOT)


def build():
    subprocess.run(["cmake", "--build", BUILD, "--target", "benefactor-harness", "-j4"],
                   check=True, cwd=ROOT, stdout=subprocess.DEVNULL)


def run():
    env = dict(os.environ, SDL_VIDEODRIVER="offscreen", BOOT_DISK="1",
               BOOT_DISK_NFRAMES=NFRAMES, BOOT_DISK_FIRE=FIRE, RT_SKIPLOG="1")
    def dec(x):
        if x is None: return ""
        return x.decode("utf-8", "replace") if isinstance(x, (bytes, bytearray)) else x
    try:
        p = subprocess.run([BIN] + DISKS, env=env, cwd=ROOT,
                           capture_output=True, text=True, timeout=200)
        return dec(p.stdout) + dec(p.stderr)
    except subprocess.TimeoutExpired as e:
        return dec(e.stdout) + dec(e.stderr)


def collect(log):
    return {a for a in (int(m.group(1), 16) for m in SKIP_RE.finditer(log))
            if LO <= a < HI}


def main():
    max_iters = 40
    if "--max-iters" in sys.argv:
        max_iters = int(sys.argv[sys.argv.index("--max-iters") + 1])
    seeds = load_seeds()
    for it in range(max_iters):
        regen(seeds)
        build()
        log = run()
        reached_menu = "cop1lc=$0081D2" in log
        new = collect(log) - seeds
        print(f"[iter {it}] seeds={len(seeds)} menu={reached_menu} "
              f"new={len(new)}: {[f'{x:X}' for x in sorted(new)]}", flush=True)
        if not new:
            print(f"FIXPOINT at {len(seeds)} seeds"); break
        seeds |= new
        save_seeds(seeds)
    save_seeds(seeds)
    print(f"final: {len(seeds)} gameplay seeds -> {SEEDS}")


if __name__ == "__main__":
    main()
