#!/usr/bin/env bash
# Regenerate src/generated/ (the recompiled 68k->C code) from the user's own
# Disk.* images — no PUAE, Kickstart, WHDLoad or slave. Idempotent and
# hash-gated: it does nothing if the generated code already exists and the
# recompiler/loaders/seeds haven't changed, so it's cheap to call on every CMake
# configure.
#
# Pipeline: build the bootstrap-clean bank dumper (tools/dump_banks.c, which
# links only the pure overlay loaders + disk boot, NOT the generated code) ->
# dump the four bank inputs from the disks -> run the recompiler on each.
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

GEN=src/engine/generated
HASHFILE="$GEN/.regen-hash"
DISKS=(Disk.1 Disk.2 Disk.3)

# Hash everything that affects the generated output: the recompiler, the seed
# lists, and the loader/dumper sources (they determine the bank inputs).
hash_inputs() {
  cat tools/recomp/*.py tools/recomp/*.txt tools/recomp/seeds/* \
      src/engine/overlay_load.c src/engine/overlay_load.h \
      src/engine/disk_boot.c tools/dump_banks.c 2>/dev/null | sha256sum | cut -d' ' -f1
}
WANT=$(hash_inputs)

if [ -f "$GEN/game.h" ] && [ -f "$HASHFILE" ] && [ "$(cat "$HASHFILE")" = "$WANT" ]; then
  echo "[regen] src/generated up-to-date (hash match) — skipping"
  exit 0
fi

for d in "${DISKS[@]}"; do
  [ -f "$d" ] || { echo "[regen] ERROR: missing $d — the recompiled code is generated from your own Benefactor disk images. Drop Disk.1/2/3 in the repo root." >&2; exit 1; }
done

export TMPDIR="$PWD/scratch/tmp"; mkdir -p "$TMPDIR"
SCR=scratch/regen; mkdir -p "$SCR" "$GEN"

echo "[regen] building bootstrap dumper"
# No SDL needed: disk_boot.c includes rt.h (not hw_private.h), so the dumper builds
# standalone. (Previously this relied on sdl2-config/pkg-config finding SDL, which
# breaks on macOS/Homebrew when neither is on PATH.)
cc -O2 -I src \
   tools/dump_banks.c src/engine/overlay_load.c src/engine/disk_boot.c \
   -o "$SCR/dumpbanks"

echo "[regen] dumping bank inputs from disks"
"$SCR/dumpbanks" "$SCR" "${DISKS[@]}"

echo "[regen] recompiling 4 banks -> $GEN"
python3 tools/recomp/recomp.py "$SCR/chip_ram_dump.bin" --chip-dump --out-dir "$GEN"
python3 tools/recomp/recomp.py "$SCR/chip_flow_gp.bin" --chip-dump --base 3000 \
  --code-size 46000 --seed "$(cat tools/recomp/gp_seeds.txt)" --bank gp \
  --areg 5=511E --out-dir "$GEN"
python3 tools/recomp/recomp.py "$SCR/gmem_after_load.bin" --chip-dump --base 3000 \
  --code-size 5A0000 --areg 5=57EE12 --bank gpl --out-dir "$GEN" \
  --seed-dir tools/recomp/seeds
python3 tools/recomp/recomp.py "$SCR/gmem_after_credits.bin" --chip-dump --base 3330 \
  --code-size 1888C --areg 5=355C --seed "$(cat tools/recomp/credits_seeds.txt)" \
  --bank credits --out-dir "$GEN"

echo "$WANT" > "$HASHFILE"
echo "[regen] done"
