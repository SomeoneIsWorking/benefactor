# Benefactor PC Port — Master Workflow

**Primary goal: port Amiga hardware-driving M68K functions into native C in `pc_overrides.c`.** But porting must only happen on a verified baseline — the harness must show PERFECT MATCH first.

> **Self-evolution:** This file governs how the project improves itself. Every session should leave the tools, skills, and instructions in a better state than it found them — better meaning more correct, more structured, more capable. Never better meaning faster to hack around a problem.

---

## Session Start — Do This First

1. Build and run the harness to see current state:
   ```bash
   cd <repo>/build && cmake --build . --target benefactor-harness -j$(nproc)
   cd <repo> && bash run_harness_headless.sh 2>&1 | tee logs/harness_run.txt
   grep -E "WATCH|DIFF|ok$|MATCH" logs/harness_run.txt
   ```
2. If `PERFECT MATCH` → move to porting (replace a recompiled `gfn_` with native C).
3. If `DIFF` → diagnose and fix the divergence first. Load the relevant skill only when you need it (see Debugging Workflow below).

---

## Problem-Solving Instinct

**When facing any problem, the first move is always to narrow it down — never to reason about it.**

Incremental narrowing loop:
1. **Observe** — what does the tool output say? (harness diff, watchpoint, disassembly)
2. **Hypothesize** — one candidate cause
3. **Test** — add a watchpoint, write a test case, add a `printf`, run the harness with fewer frames
4. **Eliminate** — the test either confirms or rules out the candidate
5. Repeat with a narrower target

If step 3 has no existing tool to make the observation, **build the tool first**.

**Never "fix" a "likely" cause. Always find the root problem through observable evidence.**

**TDD for narrowing:** when a recompiler or override bug is suspected, write a failing test in `test_recomp.py` that encodes the specific M68K bytes and asserts the expected C output. The test IS the narrowing — it either fails (bug confirmed) or passes (look elsewhere). Fix the emitter until the test passes, then verify with the harness. Never fix code without a test that would have caught the bug.

---

## Debugging Loop (The Core Rule)

```
Analyze the problem → Find the root cause
         │
         ▼
    Is it a recompiler bug?
         ├── YES → Fix recomp.py (add test, fix, regenerate, verify)
         │
         └── Is it an engine/hardware behavior issue?
                  ├── YES → Add a PC native override in pc_overrides.c
                  │
                  └── Are BOTH true?
                           └── YES → Do BOTH: fix the recompiler AND create a PC native override
```

**Never shortcut this loop.** If the root cause isn't clear, make it clear using logs and Python processing. Build tools when existing ones are insufficient.

**Two-phase workflow:**
1. **Verify** — run the harness. If any frame diverges from PUAE, do a full root-cause analysis before touching anything:
   - What is PUAE/the M68K game actually doing? (disassembly, watchpoints, chip RAM)
   - What is the PC port doing differently, and why?
   - Only then implement the correct behavior as a native C override.
   - **No manual assembly tracing.** Never mentally simulate what registers will contain after a sequence of instructions — you will get it wrong. Use tools: run the harness with a watchpoint, disassemble with Python + Capstone, or add a `printf` to the override. Observable output only.
   Divergence investigation IS the porting process — each resolved difference is a function correctly understood and re-implemented.
   **Never resolve a divergence by copying PUAE state into PC memory, hardcoding output values to match, or any other shortcut that produces a green harness without genuine reimplementation. A fake match is worse than a visible divergence.**
2. **Port** — once the harness is green, replace additional recompiled `gfn_XXXXX` functions with native C, using the same root-cause-first approach. Each override is a porting milestone away from generated code.
   - Porting priority: Amiga hardware drivers (copper, blitter, CIA) first → render pipeline → game logic.
   - Wrapper hooks at `$0040B6`–`$004236` are already in place for incremental render porting.
   - Never edit `generated/game.c` directly — always add a native override instead.
   - If a native override is a stepping stone, note what the next step toward full native code is.

The game was originally written for Amiga OCS hardware (copper lists, blitter, CIA timers). Every native override that replaces an Amiga-hardware-driving function with clean PC code moves the port forward. The static recompiler bootstrapped a working binary — native overrides are how we port away from it.

---

## Build & Run Commands

```bash
# Build the comparison harness (most common)
cd <repo>/build
cmake --build . --target benefactor-harness -j$(nproc)

# Build the standalone game executable
cmake --build . --target benefactor-pc -j$(nproc)

# Run harness (headless, outputs to logs/)
cd <repo>
bash run_harness_headless.sh 2>&1 | tee logs/harness_run.txt

# Run harness (interactive, side-by-side PUAE | PC display, requires SDL2)
bash run_harness_interactive.sh 2>&1

# Run harness (indefinite interactive)
bash run_harness_interactive.sh --frames 0 2>&1

# Run recompiler tests (single test suite)
python3 tools/test_recomp.py

# Standalone game (not harness)
./build/benefactor-pc \
    chip_ram_dump.bin \
    Disk.1 \
    Disk.2 \
    Disk.3
```

**Check harness output:** `grep -E "WATCH|SNAP|DIFF|ok$|MATCH" logs/harness_run.txt`

---

## Architecture & Key Files

| File | Role |
|------|------|
| `src/recomp/rt.c` / `rt.h` | Memory routing, dispatch table, `M68KCtx` struct |
| `src/recomp/hw.c` / `hw.h` | SDL2 hardware layer: OCS/CIA register shadows, rendering |
| `src/generated/game.c` / `game.h` | **AUTO-GENERATED** by `recomp.py` — do not edit manually |
| `src/pc.c` / `pc.h` | PC port core: frame loop, state machine, init/fini |
| `src/pc_internal.h` | Shared declarations for `pc.c` and `pc_overrides.c` |
| `src/pc_overrides.c` | Native C overrides for broken recompiled functions |
| `tools/recomp/recomp.py` | 68k→C static recompiler (Python + capstone) |
| `tools/recomp/entries.py` | Entry points with descriptive function names |
| `chip_ram_dump.bin` | Extracted 68k binary (148526 bytes @ offset `$3000` from `chip_ram_dump.bin`) |

**Do not delete `src/generated/game.c`** unless forcing a recompile. Sources in `src/amiga/` and `src/platform/` are **not compiled**.

To manually recompile:
```bash
python3 tools/recomp/recomp.py build/chip_ram_dump.bin \
    --out-c src/generated/game.c --out-h src/generated/game.h
```

---

## Critical Conventions

- **Hardware addresses are routed, not plain RAM:** `is_hw()` gates `$BFD000`, `$BFE000`, `$DFF000`.
- **A5 = `$00531C`**, **A6 = `$00DFF002`** (hardware base offset by 2). Both defined in `pc_internal.h`.
- **Copper lists:** `$7BC8` = loading screen, `$86CC` = gameplay. The game rebuilds these each frame.
- **Native override pattern:** Write `native_XXXXXX(M68KCtx *ctx)` in `pc_overrides.c`, register with `rt_register_override(0xXXXXXXu, native_XXXXXX)` in `pc_register_overrides()`. Use for hardware waits, recompiler bugs, or per-frame side effects.
- **Macros in `pc_internal.h`:** `r8/r16/r32(addr)`, `w8/w16/w32(addr,v)`, `hw_write16(reg,v)`, `call_fn(ctx, addr)`, `A5`, `A6`.
- **MR8/MR16/MR32/MW8/MW16/MW32** — used in `generated/game.c` (routes through `rt.c`); direct `r*/w*` macros used in `pc_overrides.c`.
- **Output paths are fixed** — always use `logs/`; never `/tmp/`.
- **PUAE/libretro-uae is compiled INTO the harness binary** (all sources in `vendor/libretro-uae/` are built as one TU). You can freely add tracing, watchpoints, or printf inside PUAE code — just rebuild the harness target. It is not a prebuilt library you cannot touch.

---

## Debugging Workflow

Use these skills in order for any frame divergence investigation:

| Skill | When to invoke |
|-------|----------------|
| `skills/rom-analysis.md` | Disassembling the M68K binary, tracing what writes to an address |
| `skills/recompiler.md` | Fixing mis-translated instruction patterns, adding entry points, improving output quality |
| `skills/run-harness.md` | Running the comparison harness and interpreting its output |
| `skills/diagnose-divergence.md` | Systematic root-cause analysis of a frame divergence |
| `skills/create-override.md` | Adding a native C override for a broken recompiled function |
| `skills/debug-journal.md` | Recording a confirmed fact via store_memory |

```
Frame diverges
  └─ diagnose-divergence skill
       ├─ Blitter clobbered, not rebuilt → create-override skill (native rebuilder)
       ├─ Wrong computation in gfn_XXXXX → rom-analysis skill → recompiler skill or create-override skill
       └─ Unknown write → rom-analysis skill (add watchpoint, trace)
  └─ Root cause confirmed → debug-journal skill
  └─ Fix verified → update instructions + store_memory (see Self-Evolution below)
```

**Start a scratchpad** (`session/scratchpad.md`) when a divergence is not obvious after 10 minutes or when exploring parameter spaces. See `instructions/scratchpad.md` for format and rules. The scratchpad prevents circular reasoning by forcing every hypothesis, test, and falsification to be written down.

**No manual assembly math.** Never compute bit-field decodes, address offsets, or copper list mappings in your head. Use Python:

```bash
# word index → copper list address
python3 -c "base=0x86CC; i=47; print(f'reg=\${base+(i//2)*4:04X} val=\${base+(i//2)*4+2:04X}')"
# decode bltcon0
python3 -c "b=0x19F0; print(f'use={b>>8&0xF:04b} LF={b&0xFF:#04x}')"
```

---

## Self-Evolution Rules

This project is self-evolving. Every session should leave the tools, skills, and instructions in a better state than it found them — better meaning more correct, more structured, more capable. Never better meaning faster to hack around a problem.

### Tooling & Skills (these evolve together)
- **If a task is painful to do manually, build a tool for it** and update the relevant skill to document how to use it. Good candidates: copper list decoder, chip RAM differ, M68K disassembly helper, blitter trace parser.
- **If an existing tool is incomplete or limited, extend it** — and update the skill in the same step.
- **If the harness is missing a capability** (e.g., interleaved frame comparison, better watchpoints, richer diff output), extend `harness_main.c` and update `run-harness` skill.
- A tool without a skill update is incomplete work. A skill that describes a non-existent tool is wrong.

### Instructions
- **Update `instructions/*.md`** when the understanding of a system deepens — better address maps, better call sequences, corrected assumptions.
- **Add DO NOT RE-EXAMINE entries** to `instructions/harness.md` for anything fully root-caused and fixed.
- **Update this file** when architecture, workflow, or conventions change.

### Facts
- **Store confirmed facts with `store_memory`** — one call per fact, call it **frequently** (after every confirmed root cause, every override added, every recompiler fix).
- **Never create markdown journal files** — they bloat the context window. `store_memory` is the only persistence mechanism for facts.
- **Keep `instructions/current-state.md` up to date** — this file holds active debugging state, current DIFFs, porting progress, and debugging hints. Update it whenever the state changes (new DIFF found, DIFF fixed, override changed, root cause confirmed).
- Only store verified facts, never hypotheses.

### The direction is always toward correct engineering
- If the right fix is harder than a workaround, do the right fix.
- If a native override is a stepping stone to full native code, note what the next step is.
- Never evolve the instructions, skills, or tools toward shortcuts that would need to be undone later.

---

## Key Established Facts (DO NOT RE-EXAMINE)

Full details: `instructions/harness.md`

- Copper list `$86CC` = gameplay; `$7BC8` = loading screen.
- Blitter fill `bltcon0=$19F0` clobbers `$8720–$872C` (BPLCON0/1/2 slots) with `$FFFF` each frame.
- `gfn_00405C` rebuilds BPL1PT–BPL4PT only — not BPLCON entries.
- `frames_differ()` compares coplist words, NOT register shadow values.
- `w32` writes across copper instruction boundaries corrupt adjacent reg-words — always use `w16`.
- Harness snap order (3-frame run): SNAP(F0, cop=`$86CC`), SNAP2(F1, cop=`$7BC8`), SNAP(F2, cop=`$86CC`).

---

## Output Paths (never use /tmp/)

| Output | Path |
|--------|------|
| PUAE chip RAM dump | `logs/harness_puae_chipram.bin` |
| Harness report | `logs/harness_report.txt` |
| Harness stderr capture | `logs/harness_run.txt` (via `tee`) |
