# Skill: Run Harness

Run the Benefactor comparison harness, interpret output, and summarize divergences. Use when you need to see current PUAE vs PC frame comparison results. Knows how to parse WATCH/SNAP/DIFF lines and map them to root causes.

> **Self-evolution:** If you extend or build a tool that changes this skill's procedure, update this file in the same step.

## Commands

```bash
# Standard 3-frame run (most common)
cd <repo>
bash run_harness.sh --frames 3 --boot-frames 600 2>&1 | tee logs/harness_run.txt

# Build first if code changed
cd benefactor-pc/build && cmake --build . --target benefactor-harness -j$(nproc) && cd -

# Quick filtered output (most useful view)
grep -E "WATCH|SNAP|DIFF|F00[0-9]|ok$" logs/harness_run.txt
```

## Output Line Reference

| Prefix | Meaning |
|--------|---------|
| `[WATCH16] write16 $ADDR = $VAL` | `rt_write16` hit watchpoint. Registers show caller context. |
| `[WATCH32] write32 $ADDR = $VAL` | `rt_write32` hit watchpoint. |
| `[WATCH_BLT] blitter write $ADDR` | Blitter wrote to watched address. Shows bltcon0, apt, cpt, dpt. |
| `[WATCH_TAB16] write16 $ADDR` | Write to sub-copper table $7C80–$7CA0. |
| `[SNAP] cop1lc=$XXXX g_mem[...]` | `hw_get_snap()` triggered; shows key memory values at snap time. |
| `[SNAP2] coplist[47]=...` | Always-on coplist word dump at snap time. |
| `[BLIT]` | Blitter operation log (bltcon0, size, addresses). |
| `F000: ... DIFF <<<` | Frame 0 diverges (coplist or palette mismatch). |
| `F000: ... PIXEL DIFF (N px, X%) <<<` | Frame 0 copper/palette match but framebuffer pixels differ. |
| `F000: ... ok` | Frame 0 matches copper, palette, AND framebuffer pixels. |
| `PERFECT MATCH` | All compared frames match on all three criteria. |

## Interpretation Guide

### "F000: bplcon0 PUAE=$0200 PC=$3600  DIFF"
- The `bplcon0` display is **cosmetic only** — NOT the comparison criterion.
- Actual diff is in `coplist[]` words. Check the report file `logs/harness_report.txt` for which word differs.
- Common cause: blitter fills copper area with $FFFF and PC port doesn't rebuild all entries.

### Multiple SNAP/WATCH16 before F000
- 3 WATCH16 + 2 SNAPs before F000 = expected: F0 uses cop=$86CC, F1 uses cop=$7BC8, F2 uses cop=$86CC.
- Second SNAP2 shows coplist[47]=$0040 from the $7BC8 list (unrelated).

### `[stack 0] $00405C` in WATCH16 output
- Means `gfn_00405C` is the writing function (called directly from pc.c with no deeper stack).

### `WATCH_BLT` fires before WATCH16
- Blitter clobbers the copper area FIRST, then `rt_write16` rebuilds SOME entries but not all.
- Find which entries are NOT rebuilt → those are the divergence source.

## Procedure: Diagnose a Divergence
1. Run harness, capture to `logs/harness_run.txt`.
2. Find the `DIFF <<<` line — note the frame number.
3. Check `logs/harness_report.txt` for word-by-word coplist diff.
4. Map differing word index back to copper instruction: `instr = word_idx / 2`, `addr = $86CC + instr*4`.
5. Cross-reference with known addresses in `instructions/harness.md`.
6. If blitter-clobbered: needs a native override or extra `call_fn`.
7. If wrong value from `rt_write16`: trace the computation in `game.c`.

## Report File
The harness writes `logs/harness_report.txt` with:
- Per-frame coplist diffs (first diverging word shown)
- Chip RAM diff (end-of-run PUAE vs PC)
