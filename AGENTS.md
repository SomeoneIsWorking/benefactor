# AGENTS.md — Benefactor Amiga → PC Port

Self-evolving workflow: every session should improve tools, skills, or instructions. Build tools for painful manual tasks; extend the harness when comparisons are insufficient; store verified facts via `store_memory` (no markdown journals). If docs conflict with build scripts or harness behavior, trust the executable source.

**This file is self-evolving.** After every session with confirmed findings, update this file and `instructions/current-state.md`. Add confirmed root causes to `instructions/harness.md` under "DO NOT RE-EXAMINE". If a new divergence category is found, add it to `skills/diagnose-divergence.md`. If a new tool is built, update the relevant skill. The session protocol below is mandatory.

---

## Mandatory Session Protocol

### Session Start (do these BEFORE touching any code)

1. Read `instructions/current-state.md` — know the current harness status before any action.
2. Build and run the harness: `bash run_harness_and_analyze.sh 2>&1 | tee logs/harness_run.txt`
3. Check output: `grep -E "WATCH|DIFF|MATCH|auto-rc|CAUSE|FIX" logs/harness_run.txt`
4. If `PERFECT MATCH` → skip to porting. If `DIFF` → read the `[auto-rc]` root cause output inline.

### Session End (do these AFTER any state change)

1. Update `instructions/current-state.md` with: new harness status, what changed, what was confirmed.
2. Add any newly confirmed root cause to `instructions/harness.md` under a `## Confirmed Root Cause Fixed` entry.
3. If the session discovered a new divergence category or pattern, update `skills/diagnose-divergence.md`.
4. If a new tool was built, update the relevant skill with usage instructions.
5. Run `python3 tools/test_recomp.py` after any recompiler change.

### Self-Evolution Rules

- Any tool that is painful to use manually → build a better one, update the skill.
- Any instruction that proved wrong → correct it and mark the old assumption as falsified.
- Any new confirmed fact → add to `instructions/harness.md` "DO NOT RE-EXAMINE" section.
- After 3 falsified hypotheses in a row → write a scratchpad (`session/scratchpad.md`) before continuing.
- If `AGENTS.md` itself is stale (facts changed) → update it in the same commit as the fix.

---

## Project Identity

Static recompilation port of *Benefactor* (Amiga 68k → native C + SDL2). The 68k binary is translated once offline by a Python recompiler; at runtime there is no interpreter. Hardware I/O (OCS custom chips, CIA) is intercepted and emulated in SDL2.

- **Active code:** `src/recomp/*`, `src/pc*.c`, `src/generated/*`
- **Dead code (do not build):** `src/amiga/*`, `src/platform/*`, `src/main.c`
- **Generated code (never edit manually):** `src/generated/game.c` and `game.h`

## Exact Build / Run / Test Commands

```bash
# Build comparison harness (most common)
cd <repo>/build
cmake --build . --target benefactor-harness -j$(nproc)

# Build standalone game executable
cmake --build . --target benefactor-pc -j$(nproc)

# Run headless harness (3 frames, 600 boot frames)
cd <repo>
bash run_harness_headless.sh 2>&1 | tee logs/harness_run.txt

# Quick filtered view
grep -E "WATCH|SNAP|DIFF|ok$|MATCH" logs/harness_run.txt

# One-shot capture + analysis
bash run_harness_and_analyze.sh

# Run interactive side-by-side PUAE | PC display
bash run_harness_interactive.sh

# Run standalone game (not harness)
./run_pc_game.sh --build

# Recompiler tests (run before and after any recomp.py change)
python3 tools/test_recomp.py

# Force regenerate game.c/game.h (or just delete them and rebuild)
python3 tools/recomp/recomp.py chip_ram_dump.bin --chip-dump \
  --out-c src/generated/game.c \
  --out-h src/generated/game.h
```

**Prerequisites:** GCC 15.2, CMake 3.31, SDL2 (`libsdl2-dev`), Python 3.14 + `capstone` (`pip install capstone`).

## Architecture & Boundaries

```
chip_ram_dump.bin (512 KB PUAE snapshot)
      │
      ▼
recomp.py  (capstone 5.0, offline)
      │
      ▼
src/generated/game.c  – one native C function per 68k subroutine
src/generated/game.h  – forward decls + dispatch table
      │
      ├── src/recomp/rt.c / rt.h  – memory routing, dispatch, M68KCtx
      └── src/recomp/hw.c / hw.h  – SDL2 hardware layer (OCS/CIA)
```

- `CMakeLists.txt` drives recompilation as a build step; deleting `game.c`/`game.h` triggers regeneration automatically.
- `benefactor-harness` embeds the full PUAE/libretro-uae emulator (vendor submodule) plus the PC port and compares them frame-by-frame.
- `benefactor-pc` is the standalone runtime; `run_pc_game.sh` is the canonical launcher.

## Comparison Harness Workflow (Always Verify First)

1. Build harness: `cd build && cmake --build . --target benefactor-harness -j$(nproc)`
2. Run: `bash run_harness_and_analyze.sh 2>&1 | tee logs/harness_run.txt`
3. If `PERFECT MATCH` → safe to port (replace a recompiled `gfn_` with native C).
4. If `DIFF` → the harness **automatically** prints `[auto-rc]` root cause inline — read that first.
   - `[auto-rc] CAUSE:` + `[auto-rc] FIX:` give the classification.
   - Never skip to fixing code without reading the root cause output.
   - Do not paper over divergences with hardcoded state or shortcuts.

**Auto root-cause output appears inline — no need to run Python scripts manually after a DIFF.**

**Harness evaluates three criteria per frame:**
1. `cop1lc` equality
2. Full copper list word content (`coplist[]` from `cop1lc`)
3. Rendered framebuffer pixels

**Cosmetic diffs (not harness failures):**
- `bplcon0` / `bpl1mod` register shadow values (timing artifacts)
- `palette[]` / `bplpt[]` snapshot values (PUAE frame-start vs PC frame-end)
- `audio[n].vol` (incomparable by design)

## Critical Conventions & Gotchas

- **ONE code path — no committed A/B toggles, dual implementations, or fallbacks.** Don't ship `if (g_use_new) new(); else old();`, env-gated `RECOMP_A`/`RECOMP_B`-style variants, a coroutine path *and* a host-driven path, or a native override that keeps the recompiled body around as a live alternative. Pick the approach, make it the single path, and DELETE the one you don't want (code, flags, env reads, dead branches). A/B toggles are fine *transiently while developing/validating a change in the working tree*, but they must be collapsed to one path before committing — a committed toggle is tech debt and a second thing to keep working. (This overrides the recomp-overrides "keep the recomp body for A/B" guidance: keep it only during the diff, then delete it.)
  - **The single permitted exception:** ONE global "overridden vs vanilla" switch, and only at the `rt_call` dispatch level — i.e. a master flag that makes `rt_call` either run native overrides or the plain recompiled body, for the WHOLE program at once. No per-feature/per-screen toggles. (Not implemented yet — do NOT add it preemptively; this just reserves the one allowed dual path.)
- **Prefer ONE struct for global/module state, not many separate top-level variables.** Group related globals into a single struct instance (the established pattern is `GameState g_state` in `game_state.h`, with legacy-name `#define g_foo (g_state.foo)` accessors). One struct means one place to reset/snapshot/serialise (savestates), clearer ownership, and no scattered `extern` sprawl. When adding new global state, extend an existing state struct or introduce a small dedicated one — don't drop loose `int g_thing;` / `int g_other;` at file scope. (Applies to runtime state in `rt.c` too, e.g. the continuation-stack/yield state.)
- **Hardware address routing:** `is_hw()` gates `$BFD000`, `$BFE000`, `$DFF000`. All other addresses are plain RAM.
- **A5 = `$00531C`**, **A6 = `$00DFF002`** (hardware base offset by 2). Defined in `pc_internal.h`.
- **Copper lists:** `$7BC8` = title screen; `$86CC` = gameplay. The game rebuilds them each frame.
- **Always use `w16` for copper value-words.** `w32` across a copper instruction boundary corrupts the adjacent register word.
- **Output paths are fixed** — use `logs/` for everything; never `/tmp/`.
- **`fprintf(stderr, ...)` is suppressed inside PUAE vendor code** — use `write(2, buf, n)` for debug prints in PUAE context.
- **PUAE/libretro-uae is compiled INTO the harness binary** (all sources in `vendor/libretro-uae/` are built as one TU). You can freely add tracing, watchpoints, or printf inside PUAE code — just rebuild the harness target. It is not a prebuilt library you cannot touch.
- **Do not delete `src/generated/game.c`** unless forcing a recompile.

## Native Override Pattern

Use this to replace a recompiled function with hand-written C:

```c
// In pc_overrides.c:
static void native_XXXXXX(M68KCtx *ctx) { /* C implementation */ }

// Register in pc_register_overrides():
rt_register_override(0xXXXXXXu, native_XXXXXX);
```

Macros in `pc_internal.h`: `r8/r16/r32(addr)`, `w8/w16/w32(addr,v)`, `hw_write16(reg,v)`, `call_fn(ctx, addr)`, `A5`, `A6`.

## Recompiler Workflow

- Fix `recomp.py`, never `generated/game.c`.
- Run `test_recomp.py` before and after changes (unit + artifact scopes must
  pass; `RECOMP_SLOW=1` adds the regen determinism / reproduces-committed tests).
- Regenerate, rebuild harness, verify with `run_harness_headless.sh`.

## Debugging Loop (Never Fix a "Likely" Cause)

```
Observe → Hypothesize → Test → Eliminate → Repeat
```

**Always find the root problem through observable evidence.** Use logs, watchpoints, Python scripts, and the harness. If a tool doesn't exist to make the observation, build it first.

**The core decision tree:**
1. Analyze the problem and find the root cause
2. Is it a recompiler bug? → Fix `recomp.py` (with a test)
3. Is it an engine/hardware behavior issue? → Add a PC native override
4. Are BOTH true? → Do BOTH: fix the recompiler AND create a PC native override

Never shortcut this. Never paper over divergences with hardcoded state.

## Where Knowledge Lives

Consult these before asking the user:

- `instructions/master-workflow.md` — master workflow, self-evolution rules, key facts
- `instructions/current-state.md` — active debugging state, current DIFFs, porting progress
- `instructions/scratchpad.md` — how to keep a debugging scratchpad to avoid circular reasoning
- `instructions/*.md` — per-topic deep dives (harness, recompiler, divergence diagnosis, overrides, ROM analysis)
- `skills/*.md` — action-oriented skills for ROM analysis, recompiler fixes, harness runs, divergence diagnosis, override creation, debug journaling
- `docs/` — reference docs: `build-and-run.md`, `codebase-layout.md`, `hardware-layer.md`, `recompiler-notes.md`, `capstone-m68k-facts.md`

**Keep `instructions/current-state.md` up to date** whenever harness status changes (new DIFF found, DIFF fixed, override added, root cause confirmed).

**Start a scratchpad at `session/scratchpad.md`** when a divergence is not obvious after 10 minutes or when exploring parameter spaces.
