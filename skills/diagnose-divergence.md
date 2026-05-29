# Skill: Diagnose Divergence

Structured root-cause analysis of a PUAE vs PC frame divergence. Use when frames_differ() triggers to make the cause immediately apparent: identifies which copper instruction differs, maps to a register, traces to the writing function, and proposes the fix category.

> **Self-evolution:** If you extend or build a tool that changes this skill's procedure, update this file in the same step.

## Procedure (run top to bottom, stop when cause is identified)

### Step 0: Enable structured ROOT logs (recommended)
```bash
export BENEFACTOR_ROOT_TRACE=1
bash run_harness_headless.sh 2>&1 | tee logs/harness_run.txt
```
This emits parseable lines like `[ROOT_DIFF]`, `[ROOT_CPU16]`, `[ROOT_BLT]`, `[ROOT_PUAE_CPU16]`, `[ROOT_PUAE_BLT16]`
into per-type files (`logs/*_frame_*.log`).

### Step 1: Which copper word differs?
```bash
grep "word\[" logs/harness_report.txt | head -10
```
Or from the coplist comparison in the report. Find `word[N]` where PUAE ≠ PC.

Convert word index → address:
```
instruction = N / 2
address = $86CC + instruction * 4   (reg-word)
address = $86CC + instruction * 4 + 2  (val-word if N is odd)
```

### Step 2: What does that copper instruction control?
Decode reg-word `$01xx`:
| Reg    | DFF Offset | Register   | Controls         |
|--------|-----------|-----------|-----------------|
| `$0100`| DFF100    | BPLCON0   | bitplane count   |
| `$0102`| DFF102    | BPLCON1   | scroll           |
| `$0104`| DFF104    | BPLCON2   | sprite priority  |
| `$0108`| DFF108    | BPL1MOD   | bitplane modulo  |
| `$010A`| DFF10A    | BPL2MOD   | bitplane modulo  |
| `$0180`| DFF180    | COLOR00   | palette          |
| `$01E0`| DFF1E0    | BPL1PTH   | bitplane 1 ptr high |
| `$01E2`| DFF1E2    | BPL1PTL   | bitplane 1 ptr low  |

### Step 3: Is the address in the blitter-clobbered zone?
Blitter fill writes `$FFFF` to `$8720–$872C` (instr 21–23: BPLCON0/1/2).

- **If yes**: PC port never rebuilds it → need `native_rebuild_copper_static` or call missing init fn.
- **If no**: The value was written by `rt_write16`/`rt_write32` with wrong data → trace the computation.

### Step 4: Find who writes the address
```bash
# Check WATCH16 output for the address
grep "WATCH16.*ADDR" logs/harness_run.txt
# Check blitter
grep "WATCH_BLT.*ADDR" logs/harness_run.txt
```

If WATCH16 fires: look at `D0`, `A0`, `A1` registers and call stack `[stack N]`.
If WATCH_BLT fires: blitter wrote it — check bltcon0 and source pointers.
If neither: address is not written at all → initialization gap.

### Step 5: Categorize the fix

| Category | Symptom | Fix |
|----------|---------|-----|
| **Missing rebuild** | $FFFF after blitter fill, no WATCH16 | Add native override or call_fn to rebuild |
| **Wrong computation** | WATCH16 fires with bad value | Trace D0 computation in game.c, fix native override |
| **Wrong init** | Value from PUAE chip dump differs from expected | Check level-init call_fn sequence |
| **Timing** | Value written AFTER snap | Reorder call_fn calls before hw_present_frame() |
| **Recompiler bug** | gfn_XXXXX produces wrong output | Add native override to replace function |

### Step 6: Propose fix

Write a one-line summary:
```
CAUSE: <category> — <register> at $ADDR is <FFFF/wrong_val> because <reason>.
FIX: <add native override | add call_fn(0xXXXX) before hw_present_frame | fix computation in gfn_XXXXX>
```

Then invoke the `create-override` skill if needed.

### Optional automation: summarize root cause from logs
```bash
python3 tools/analyze_divergence_root_cause.py
```
The script reads `logs/harness_report.txt`, `logs/harness_run.txt`, and `logs/*_frame_*.log`,
finds the first `[ROOT_DIFF]`, traces last PUAE/PC writes to the diverging address, and prints:
1. candidate root-cause category
2. concrete cause statement
3. suggested fix direction

## Known Resolved Divergences (Do Not Re-Diagnose)

| Frame | Word | Register | Root Cause | Fix Status |
|-------|------|----------|------------|------------|
| F000  | coplist[44] (instr 22) | BPLCON0 | Blitter fills $8720 with $FFFF, PC doesn't rebuild | **PENDING** native_rebuild |
| F000  | coplist[47] (instr 23 val) | BPLCON2 | Same blitter fill | **PENDING** |
| F002  | coplist[94/95] (instr 47) | BPL1PTH | gfn_00405C gets corrupt D0 (screen_num wrong) | **PENDING** |
