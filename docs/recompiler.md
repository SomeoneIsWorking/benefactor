# Regenerating the Recompiled C

The repo ships pre-generated C from the M68K binary (under `src/generated/`), so a normal build doesn't need any of this. You only need this workflow if you're changing the recompiler (`tools/recomp/`), adding seed entry points, or rebuilding from a fresh extraction of the game's chip RAM.

## The three banks

The recompiler runs **three separate times** to produce three banks of generated C. Each bank corresponds to one game phase and has its own input dump, code base, and `a5` register base. The runtime (`src/recomp/rt.c`) switches between them based on whether the game is in the intro, the title/menu overlay, or the gameplay overlay.

| Bank | Source dump | Code base | a5 base | Generated files | Used during |
|------|-------------|-----------|---------|-----------------|-------------|
| **intro** (default) | `chip_ram_dump.bin` | `$3000` | `$76000` | `src/generated/game_*.c` | Cold boot, intro, logos, title |
| **gp** (title/menu) | `logs/chip_flow_256_0081D2.bin` | `$3000` | `$511E` | `src/generated/game_gp_*.c` | Title screen, menu, level intro card |
| **gpl** (gameplay engine) | `logs/gmem_after_load.bin` | `$3000` | `$57EE12` | `src/generated/game_gpl_*.c` | In-cavern gameplay |

## Producing each input dump

### `chip_ram_dump.bin` (intro bank)

This is the PUAE chip RAM (512 KB) captured at the deterministic sync point during boot. The script runs the harness once and copies the dump out:

```bash
python3 tools/extract_chipram_from_roms.py --build
```

That produces `chip_ram_dump.bin` at the repo root.

### `logs/gmem_after_load.bin` (gpl bank)

This is the full 8 MB process memory captured the moment the game's gameplay overlay finishes loading. The native override `native_overlay_loader` (`src/pc_overrides_boot.c`) writes this file automatically when the `$150` overlay-load entry fires with `d0=0`.

To produce it, run the standalone PC port and progress until the gameplay overlay loads:

```bash
./run_pc_game.sh --build
# Hold fire through intro / title / menu. When the cavern transition fires,
# the console prints:
#   [overlay-loader] $150 d0=0: loaded; dumped logs/gmem_after_load.bin
# Once you see that line you can quit.
```

### `logs/chip_flow_256_0081D2.bin` (gp bank)

Currently a one-off historical artifact — there is no automated regen path for this one yet. CMake's comment puts it plainly: *"no auto-regen until a canonical gameplay image artifact is checked in"*. You only need to regenerate this if you're working on the title/menu bank specifically. Skip this section unless you are.

## Running the recompiler

After you have the input dump(s):

### Intro bank (CMake target)

If `chip_ram_dump.bin` exists at the repo root, CMake exposes a target:

```bash
cmake --build build --target recompile_game
```

Equivalent direct invocation:

```bash
python3 tools/recomp/recomp.py chip_ram_dump.bin --chip-dump --out-dir src/generated
```

### gp bank

```bash
python3 tools/recomp/recomp.py logs/chip_flow_256_0081D2.bin --chip-dump \
    --base 3000 --code-size 46000 --areg 5=511E --bank gp \
    --seed "$(cat tools/recomp/gp_seeds.txt)" \
    --out-dir src/generated
```

### gpl bank

```bash
python3 tools/recomp/recomp.py logs/gmem_after_load.bin --chip-dump \
    --base 3000 --code-size 5A0000 --areg 5=57EE12 --bank gpl \
    --seed "$(cat tools/recomp/gpl_seeds.txt)" \
    --out-dir src/generated
```

The `--seed` lists hold entry points reached via indirect dispatch — jump tables, handler pointers, computed jumps — that static descent from the bank's entry can't find on its own. When the runtime hits an unseen indirect-dispatch target, it logs `[rt-miss] $XXXXXX`; append that address to the relevant seed file and rerun.

## Recovering object handlers from a post-mortem dump

When the standalone hits an `[rt-miss]` in the gameplay loop, the strict-abort handler dumps `g_mem` to `logs/pc_freeze.bin`. The live object structures for the current level (plus the spawn list for the *next* level) are sitting in chip RAM at that moment, and the dispatcher arithmetic that the runtime missed can be replayed statically:

```bash
./run_pc_game.sh                                          # runs until rt-miss
python3 tools/recomp/discover_object_handlers.py          # show new bodies
python3 tools/recomp/discover_object_handlers.py --append # append to gpl_seeds.txt
cmake --build build --target recompile_game               # (if chip_ram_dump.bin present)
# or run the gpl regen command shown above
cmake --build build -j"$(nproc)"
```

The walker scans the `$57F000..$580000` work-area window for struct pointers into the gpl code range, tries both dispatcher arithmetics (`struct+mo` and `struct+mo+2` — different struct families use one or the other), and accepts the one whose target starts with the standard `movem.w (a0),...` / `movem.l <regs>,-(sp)` entry preamble. That recovers every object body the level needs in one shot, not one miss at a time.

## Seed convergence loop (legacy)

The `tools_seed_converge.sh` script automates seed-discovery for the gpl bank by iterating regen → build → scripted gameplay → append → repeat. It used `RT_ALLOW_MISS=1` to harvest misses in batches; **do not run gameplay with that flag** — a skipped miss leaves the caller in a corrupt state and every downstream watchdog/freeze becomes a meaningless symptom of the corruption, not a real bug. The post-mortem walker above is the strict-mode replacement.

## Rebuilding after a regen

```bash
cmake --build build --target benefactor-pc benefactor-harness -j"$(nproc)"
```

## Internals

- `tools/recomp/recomp.py` is the CLI entry. It dispatches into `scanner.py` (iterative constant-target descent over the disassembly) and `emitter.py` (Capstone-disassembled M68K → C).
- `tools/recomp/entries.py` defines fixed entry points (per bank).
- `tools/recomp/emitter.py` is where to fix mistranslated instructions — folds for hardware busy-waits (`is_vposr_btst`, `is_bltbusy_btst`), the `Scc`/`st` patterns, etc. live here.
- The recompiler emits `RT_TRACE_INSNS`-guarded `rt_trace_insn(addr, mnem, ctx)` calls. Configure CMake with `-DBENEFACTOR_TRACE_INSNS=ON` to turn them on; this enables the watchdog's last-instruction ring and per-instruction trace files for diffing against PUAE.

See `instructions/recompiler.md` for the higher-level "improve the recompiler vs write a native override" guidance.
