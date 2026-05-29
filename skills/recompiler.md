# Skill: Recompiler

Use, test, and improve the Benefactor 68k→C recompiler (`recomp.py`). Use when: a recompiled function produces wrong output and the fix belongs in the recompiler rather than a native override, when adding new entry points, or when improving instruction translation quality to reduce the override burden.

> **Self-evolution:** If you extend or build a tool that changes this skill's procedure, update this file in the same step.

## Files

| File | Role |
|------|------|
| `tools/recomp/recomp.py` | Main translator — `Translator.translate()` handles per-instruction emission |
| `tools/recomp/step1_disasm.py` | Capstone disassembly pass |
| `tools/recomp/step2_descent.py` | Recursive descent — discovers all reachable functions |
| `tools/recomp/entries.py` | Entry points with descriptive function names — **edit here to add new functions**. Merges tool-discovered targets from `discovered.py` |
| `tools/recomp/discover_indirect.py` | Fixpoint tool: finds indirect-dispatch handlers static descent misses (see below) |
| `tools/recomp/discovered.py` | Auto-maintained list of runtime-discovered call targets — do not hand-edit |
| `tools/recomp/analyze.py` | Static analysis helpers |
| `tools/recomp/validate.py` | Output validation |
| `tools/test_recomp.py` | Unit tests for the translator — **always run after any recompiler change** |
| `src/generated/game.c` | Output — auto-generated, do not edit |

## Discovering indirect-dispatch handlers (static descent misses these)

The static descent cannot follow register-indirect `jsr (an)` or pc-relative
jump-table dispatch (`lea TABLE(pc),a0; move.w (a0,dn.w),d0; jmp (a0,dn.w)`),
so handler functions reached only that way are never translated. At runtime
`rt_call` logs `no function at $X – skipping` for each — and that log is
**proof X is a real call target** (the game tried to call it).

Run the fixpoint discovery loop (from ``):
```bash
python3 tools/recomp/discover_indirect.py --frames 120 --max-iters 12
```
It regenerates → rebuilds `benefactor-pc` → runs it with `RT_CALLS=1` →
harvests skipped addresses → appends to `discovered.py` → repeats until no new
skips (fixpoint). Then rebuild the harness and re-measure. `--frames N`
controls how far into the game the discovery run goes (raise it to exercise
gameplay states, not just the title). Validate the effect with the
deterministic methodology in `instructions/harness.md` (PC is bit-exact;
PUAE is not — compare against PUAE-stable bytes only).

This was how the title object-animation subsystem (~23 handlers around
$5E48–$60CC) was recovered, extending the perfect match from 34 to 37 frames.

## Finding recompiler-fidelity bugs: PC-vs-PUAE instruction trace

When chip RAM diverges but you can't see why ("identical inputs, different
output"), compare the two CPUs instruction-by-instruction:
```bash
cmake -S benefactor-pc -B build -DBENEFACTOR_TRACE_INSNS=ON   # PC-side hooks
cmake --build build --target benefactor-harness -j$(nproc)
BENEFACTOR_M68K_TRACE=1 BENEFACTOR_M68K_RANGE=5500-6100 bash run_harness.sh --frames 3
# -> logs/pc_insn_trace.txt (recompiled) and logs/puae_insn_trace.txt (emulated)
```
Both log `PC d0 d1 d2 a0 a4 a6` per instruction in the range. Align them at a
marker that occurs once (e.g. the process-frame entry), then diff the address
and register columns. The first divergence is the bug. (PUAE's trace includes
boot/settle, so don't naively diff from line 1 — align on a content marker.)
Remember to reconfigure `-DBENEFACTOR_TRACE_INSNS=OFF` afterward.

**Bug class found this way — macro double-evaluation (rt.h):** `RT_SET_NZ_*`
evaluated their argument twice. The emitter passes side-effecting
statement-expressions for `(an)+`/`-(an)` operands (e.g. `tst.w (a0)+` →
`RT_SET_NZ_16(({ v=MR16(A[0]); A[0]+=2; v; }))`), so the post-increment ran
twice (a0 +4 not +2, flag from the wrong read). Symptom: identical registers
at an instruction yet `a0` advances by the wrong amount. If you write any new
flag/helper macro, evaluate each argument exactly once into a temp.

## Run the tests (always do this first and last)

```bash
cd <repo>
python3 tools/test_recomp.py
# or with pytest for verbose output:
python3 -m pytest tools/test_recomp.py -v
```

17/17 must pass. A recompiler change that breaks tests is not done.

## Regenerate game.c after a recompiler fix

```bash
python3 tools/recomp/recomp.py build/chip_ram_dump.bin \
    --out-c src/generated/game.c \
    --out-h src/generated/game.h
```

Then rebuild and run the harness to verify:
```bash
cd build && cmake --build . --target benefactor-harness -j$(nproc)
cd ../.. && bash run_harness_headless.sh 2>&1 | grep -E "DIFF|ok$|MATCH"
```

## When to fix in the recompiler vs a native override

| Fix in recompiler | Fix with native override |
|-------------------|--------------------------|
| Wrong C emitted for a class of M68K instructions | Single function with unique wrong behavior |
| An instruction pattern is systematically mis-translated | Function uses hardware waits |
| Fix applies to many functions at once | Function needs per-frame side effects |
| The generated C is provably wrong for the M68K semantics | Recompiler output is correct but incomplete for PC |

A recompiler fix reduces the override burden permanently. Prefer it when the bug is systematic.

## Output quality goal — the primary improvement target

The recompiler must emit **readable, macro-based C**. The following is the kind of output we are actively working to eliminate:

```c
/* BAD — what the recompiler currently emits for cmp.w d0, 6(a1) */
{ uint64_t _r = (uint64_t)(uint16_t)ctx->D[0] - (uint64_t)(uint16_t)MR16((uint32_t)(ctx->A[1] + 6u)); RT_SUB_FLAGS_16(ctx->D[0],MR16((uint32_t)(ctx->A[1] + 6u)),_r); }
```

Problems with the above:
- `MR16(...)` is evaluated **twice** — a correctness bug if the address has side effects
- Raw casts everywhere instead of named locals
- `RT_SUB_FLAGS_16` receives re-computed expressions instead of captured values
- No whitespace, no structure — unreadable

The target output style:

```c
/* GOOD — same cmp.w d0, 6(a1) */
{ uint16_t _s = MR16(ctx->A[1] + 6u); uint16_t _d = (uint16_t)ctx->D[0];
  RT_SUB_FLAGS_16(_d, _s, (uint64_t)_d - _s); }
```

Rules for all emitted code:
- **Capture memory reads into a named local first** — never pass `MR16(...)` directly into a macro
- **Use `uint{N}_t` locals with short intent-expressing names** (`_s`, `_d`, `_r`, `_bit`, etc.)
- **One expression per line** for multi-step operations
- Existing macros (`MR*/MW*`, `RT_SET_NZ_*`, `RT_ADD/SUB_FLAGS_*`, `RT_CC_*`) are the vocabulary — use them, don't inline their logic
- **A model reading any line of `game.c` must immediately understand what it does.** If it cannot, the emitter needs to be improved.

When a class of instructions emits the bad pattern, the fix goes in `Translator.translate()` in `recomp.py`, not in `generated/game.c`.

## Game-specific pattern detection

This recompiler is **not a generic M68K recompiler** — it is tailored specifically to the Benefactor binary. When a recurring idiom appears across many functions, teach the recompiler to recognise and emit it cleanly rather than translating it instruction-by-instruction.

Examples of the kind of patterns to look for and specialise:
- Hardware register writes via `A6` offsets → emit named HW macro if one exists (e.g. `hw_write16(DFF_BPLCON0, v)`)
- Copper list construction sequences → emit a comment or helper call that names the intent
- Blitter setup sequences (series of `move.w` to fixed `A6` offsets) → could collapse to a named setup call
- Common loop idioms (`dbra` over a buffer) → emit as a readable `for` loop comment block

**How to find patterns:**
```bash
# Find the most repeated MW16 target addresses across game.c
grep -oP 'MW16\([^,]+' src/generated/game.c | sort | uniq -c | sort -rn | head -20

# Find functions with many sequential MW16 writes (likely HW setup)
grep -c 'MW16' src/generated/game.c
```

When a pattern is identified and specialised in the recompiler, document it in this skill under a "Known Patterns" section and add a test in `test_recomp.py`.

## Workflow: fixing a mis-translated instruction

**Use TDD — the test is the narrowing step.** Before touching `recomp.py`:

1. **Write a failing test** in `test_recomp.py` that encodes the exact M68K bytes and asserts the correct C output. This confirms the bug and will catch regressions.
2. **Run tests** — confirm the new test fails, all others pass.
3. **Fix `Translator.translate()`** in `recomp.py`.
4. **Run tests** — all must pass including the new one.
5. **Regenerate `game.c`** and rebuild harness.
6. **Verify** with harness output.
7. **Store a memory** with the instruction pattern, the bug, and the fix.

## Adding a new entry point

Edit `tools/recomp/entries.py`:
```python
ENTRIES = [
    # ...
    (0xXXXXX, "gfn_descriptive_name"),
]
```
Then regenerate `game.c`. The new function will be emitted and callable via `call_fn(ctx, 0xXXXXX)`.

## Known Fixed Bugs (Do Not Re-Fix)
- `addq/subq` to An register: must NOT update CCR. Fixed. 17/17 tests pass.
