# Running the Comparison Harness

## Harness Execution Model

The intended model is **interleaved frame-by-frame comparison**:
```
1. Boot PUAE to game state (~270 frames / ~5s; exact count unknown,
   detected via cop1lc reaching $86CC — harness uses is_game_frame())
2. Boot PC port to game state (either from chip RAM dump in one step,
   or by running the same boot sequence — either is fine)
── repeat ──────────────────────────────────────────
3. Run one frame on PUAE  → capture state
4. Run one frame on PC    → capture state
5. Compare — stop and report if diverged
────────────────────────────────────────────────────
```

This catches divergences at the **earliest possible frame**, before compounding errors obscure the root cause.

> **Current harness implementation** (`harness_main.c`) runs three sequential phases: Phase 1 = all PUAE frames, Phase 2 = all PC frames, Phase 3 = pair-wise compare. Moving toward the interleaved model is a harness improvement goal.

## Commands

```bash
# Build (always do this after code changes)
cd <repo>/build
cmake --build . --target benefactor-harness -j$(nproc)

# Standard 3-frame run
cd <repo>
bash run_harness_headless.sh 2>&1 | tee logs/harness_run.txt

# Quick filtered view (most useful)
grep -E "WATCH|SNAP|DIFF|ok$|MATCH" logs/harness_run.txt

# One-shot capture + analysis
bash run_harness_and_analyze.sh
```

## Output Line Reference

| Prefix | Meaning |
|--------|---------|
| `[WATCH16] write16 $ADDR = $VAL` | `rt_write16` hit watchpoint; registers show caller context |
| `[WATCH32] write32 $ADDR = $VAL` | `rt_write32` hit watchpoint |
| `[WATCH_BLT] blitter write $ADDR` | Blitter wrote to watched address; shows bltcon0, apt, cpt, dpt |
| `[SNAP] cop1lc=$XXXX g_mem[...]` | `hw_get_snap()` triggered; key memory values at snap time |
| `[SNAP2] coplist[47]=...` | Always-on coplist word dump at snap time |
| `[BLIT]` | Blitter operation log |
| `F000: ... DIFF <<<` | Frame 0 coplist/palette mismatch |
| `F000: ... PIXEL DIFF (N px, X%) <<<` | Frame 0 copper/palette match but framebuffer pixels differ |
| `F000: ... ok` | Frame 0 matches copper, palette, AND framebuffer pixels |
| `PERFECT MATCH` | All compared frames match on **all three** criteria |

> **PERFECT MATCH requires**: (1) copper list words identical, (2) palette identical, (3) rendered framebuffer pixels identical. All three must pass.

## Interpreting Output

**`bplcon0 PUAE=$0200 PC=$3600` in DIFF line** — cosmetic only; the comparison criterion is `coplist[]` word content, not register shadows. Check `logs/harness_report.txt` for which word index differs.

**Expected snap sequence (3-frame run):** SNAP(F0, cop=`$86CC`) → SNAP2(F1, cop=`$7BC8`) → SNAP(F2, cop=`$86CC`). Three WATCH16 hits and two SNAPs is normal.

**`[stack 0] $00405C` in WATCH16** — `gfn_00405C` is the writing function, called directly from pc.c.

**`WATCH_BLT` fires before `WATCH16`** — blitter clobbers the copper area first, then `rt_write16` rebuilds some entries. Missing entries are the divergence source.

## Diagnosing a Divergence

1. Find the `DIFF <<<` line — note frame number.
2. Check `logs/harness_report.txt` for first differing word index.
3. Map word index → address using Python:
   ```bash
   python3 -c "base=0x86CC; i=WORD_IDX; print(f'reg=\${base+(i//2)*4:04X} val=\${base+(i//2)*4+2:04X}')"
   ```
4. Cross-reference with known addresses in `instructions/harness.md`.
5. If blitter-clobbered: needs rebuild in a native override.
6. If wrong value from `rt_write16`: trace computation in `game.c`.
