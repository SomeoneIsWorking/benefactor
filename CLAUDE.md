# Benefactor Amiga → PC Port

See `AGENTS.md` for full project context, session protocol, build commands, architecture, and conventions.

## Start here

- **`docs/codebase-layout.md` — the canonical project/module map. Read it first.**
  (`src/port/` = native PC driver + overrides, `src/engine/` = recompiled M68K + Amiga
  HW model + `generated/`, `src/render/` = renderer, `src/harness/` = PUAE test harness.)
- **`docs/working-agreements.md` — the hard rules** (verify-before-commit via
  `scripts/build.sh`, never commit half a coupled change, no-bandaids/firewall,
  src-rooted includes, generated code is sacrosanct). Follow them.

## Current Direction (as of 2026-05-20)

**Moving away from PUAE emulation toward a native PC port.**

- The harness / PUAE comparison work is COMPLETE (4 frames match). Do NOT re-open it.
- Focus is now on the **native renderer** (`src/render/native_renderer.c`) and **native game loop** (`src/port/game_loop.c`).
- Goal: widescreen, 60fps, PC-native features — none of which are possible through PUAE emulation.

### Active Rendering Path
`hw_present_frame()` → `native_render_frame()` — reads copper list from chip RAM, renders directly.
The old `hw_render_frame()` / `hw_copper.c` path is **dead code** and must not be revived.

### What "native" means here
Replace recompiled M68K stubs with hand-written C that:
- Updates chip RAM (copper lists, BPL data) the same way the game did on hardware
- Does NOT emulate the CPU or blitter instruction-by-instruction
- Uses `g_chip[]` / `g_mem[]` directly (no `MR16`/`MW16` wrappers for known regions)

## Self-Evolution (mandatory)

The general self-evolving workflow is global (`~/.claude/CLAUDE.md`). Project-specific
destinations and lessons:

- **Debugging tooling fell short?** → EXPAND the harness/tools, don't work around by hand.
  GDB hits a wall here (only ~1 hardware watchpoint inserts in the threaded harness) — the
  per-frame `CHIPDIFF_TRACE` mode was built exactly because of that. Reach for new tooling
  before resigning to "I can't see it."
- **New confirmed fact** → `instructions/harness.md` (+ `store_memory`). **New override** →
  `instructions/current-state.md`. Update `AGENTS.md`/`CLAUDE.md` in the same commit when stale.

## Skills (load when needed)

| File | When to use |
|------|-------------|
| `skills/run-harness.md` | Running the comparison harness (historical reference) |
| `skills/diagnose-divergence.md` | Root-cause analysis of rendering divergence |
| `skills/recompiler.md` | Fixing mis-translated instructions, regenerating game.c |
| `skills/create-override.md` | Adding a native C override for a recompiled function |
| `skills/rom-analysis.md` | Disassembling the M68K binary, tracing writes to addresses |
| `skills/debug-journal.md` | Recording confirmed facts |

## Instructions (load when needed)

| File | Topic |
|------|-------|
| `instructions/current-state.md` | Port progress, active issues — **read at session start** |
| `instructions/master-workflow.md` | Master workflow, debugging loop, self-evolution rules |
| `instructions/harness.md` | Established facts, DO NOT RE-EXAMINE list |
| `instructions/recompiler.md` | Recompiler usage and improvement targets |
| `instructions/create-override.md` | Native override patterns and existing overrides table |
| `instructions/rom-analysis.md` | M68K disassembly and address tracing |
| `instructions/gameplay-engine-map.md` | Working RE map of the `$577000+` engine: per-frame loop subsystems, level setup, object/animation system, a5-relative state vars (incl. player movement/JUMP state machine + SFX engine). **Update as you native-port pieces.** |
| `instructions/audio-engine.md` | Full RE map of the gameplay AUDIO engine (replayer + SFX): vblank tick `$59BA7A`, SFX `$586xxx` (trigger `$58656E`, streamers `$586612`/`$58684C`), CIA output ISR `$59BF3E`/`$59BFA6`, shared state, + staged native-port plan. |
| `instructions/widescreen-plan.md` | Plan for widescreen gameplay (wider view than the 320px Amiga playfield); feasible because game logic is camera-independent. |

## Key Files

| File | Role |
|------|------|
| `src/render/native_renderer.c` | Copper-walking native renderer — the display path |
| `src/port/game_loop.c` | Native game loop (title + gameplay state machine) |
| `src/port/overrides/*.c` | Hand-written C replacements for M68K functions |
| `src/engine/generated/game.c` | Recompiled M68K → C (generated, do not hand-edit) |
| `tools/recomp/` | Recompiler (Python) — generates game.c from binary |

## Debugging Architecture (GDB-centric)

**Config env overrides are STRICTLY `BENEFACTOR_<JSON_KEY uppercased>`** (e.g.
`interact_extend` → `BENEFACTOR_INTERACT_EXTEND`); an unprefixed env var is SILENTLY
ignored and the JSON value stays in effect — so when A/B-testing a knob via env, a
misnamed "off" run is silently not a control (burned 2026-06-10). Prefer the REPL
`cfg <key> <val>` for runtime knob changes; it can't be misnamed.

**New debug logging must be REPL-gated, not env-gated**: a global flag toggled by a
REPL command (e.g. `pklog` for the pickup/interact wrapper), never a new
`getenv("X_LOG")`.

**Trace via the REPL, NOT env-gated logs.** Do investigation/tracing through the
interactive harness REPL (`./build/benefactor-harness ... Disk.*`) — add a REPL command
(in `src/harness/harness_main.c`) that prints the state you need (counts, captures,
pointers), drive to the scene, and read it live. Do NOT scatter `getenv("X_DBG")` +
`fprintf`/`GLOBAL_LOG` debug logs through the engine/renderer to chase a bug; they're
throwaway, noisy, and get committed by accident. A new inspection need → a new REPL
command, the same way `pcwatch`/`pcread`/`pal`/`fbw` were added. (Reinforces the existing
preference: use the REPL for step/inspect, avoid one-off env flags.) Pre-existing env
traces below are the documented exceptions, not a pattern to extend.

**No Python log analyzers.** All divergence diagnosis is done in GDB.

- `harness_on_diverge(frame, reason)` in `harness_compare.c` — `noinline`, set a breakpoint here
- GDB launch: `gdb -ex "break harness_on_diverge" -ex run --args ./build/benefactor-harness <args>`
- Trace ring buffer: `s_buf[TRACE_MAX_ENTRIES]` in `trace.h` — inspect with `p s_buf` in GDB
- Env-var opt-in traces: `RT_CALLS=1`, `RT_INSNS=1`, `RT_WATCH_A6=1`, `RT_WATCHDOG=N` (all via rt.c)
- PUAE-side: `BENEFACTOR_HW_TRACE=1` enables per-register-write logs
- **Per-frame both-sides chip diff:** `CHIPDIFF_TRACE=1` (+ optional `CHIPDIFF_REGION=lo-hi` hex) logs, each frame, the total + NEW (first-seen) diverging chip-RAM bytes with PC/PUAE values. Both sides are deterministic, so the earliest frame's NEW list is the root divergence. Use this instead of GDB watchpoints when you need to know WHEN/WHERE chip RAM first diverges (the threaded harness only inserts ~1 hardware watchpoint). CAVEAT: it compares POST-frame g_mem vs POST-frame PUAE, so BLITTER-written regions (BPL/glyph buffers $025334/$04F6F4/$070958+/$077E60+) show a benign 1-frame skew (PC's synchronous blitter draws a frame ahead — appears as PU=00 / PC=data). CPU-written regions (object tables, $67xx/$69xx, stack) are exact. For blitter regions, trust the harness BPL-CRC (prerender) comparison instead.
- **PUAE-side GDB:** PUAE chip RAM is at `chipmem_bank.baseaddr`; watch `*(unsigned char*)(chipmem_bank.baseaddr+OFF)` and read the M68K PC via `regs.pc` at the hit to find which original instruction PUAE ran. Use SEPARATE gdb runs for PC vs PUAE watchpoints (can't insert both).
- **PC-vs-PUAE instruction trace** (for "identical inputs, different output" recompiler bugs): build `-DBENEFACTOR_TRACE_INSNS=ON`, run with `BENEFACTOR_M68K_TRACE=1 BENEFACTOR_M68K_RANGE=lo-hi` → logs/{pc,puae}_insn_trace.txt (PC+regs per instruction). Align on a once-per-run marker, diff address+register columns. Found the RT_SET_NZ double-eval bug. See `skills/recompiler.md`.

**Harness output is now clean** — boot preamble + one line per frame. No per-word blitter spam.

## Known Issues / Next Steps

- **TODO: Widescreen** — render gameplay wider than the 320px Amiga playfield (16:9). Feasible because the game simulates objects camera-independently (verify: no screen-bound culling/spawn). Core obstacle: the engine blits only the visible ~320px of the tilemap, so off-screen world isn't in chip RAM at render time → needs a native wide tile renderer (+ native object draw in the margins). Full plan + phases in `instructions/widescreen-plan.md`.
- **TODO (cosmetic): suppress the vestigial password field** ("3MQLGPQLGP") beside the LEVEL SELECT menu item. Its renderer (`$003DAA`, found via pcwatch on the `$3D9F` buffer) is double-emitted (standalone fn + inline in `gfn_gp_003872`); the menu reaches it via the inline fall-through, so an entry-override doesn't catch it. Needs the bitmap-draw routine or a recompiler-level fix.

0. **Level-intro card hang + gameplay entry — FIXED (2026-05-29).** Native disk-boot flow (menu → level card → cavern) is now playable & stable (4500+ frames). Three things, all done the no-band-aid way:
   - **VPOSR vblank-wait hang:** the game's "wait one frame" idiom is a `btst #0,d(a6)` VPOSR frame-parity pair the recompiler folds into `hw_vblank_wait()`, but `is_vposr_btst` only matched `$3(a6)` (intro/title). The **gpl bank uses `$5(a6)`** (44×) → raw busy-spins → level-setup (`$5782B4`) spun VPOSR without yielding → hang at `$5784A0`. FIX: `disp in (3,5)` in `emitter.py` + gpl regen. Do NOT make the hw layer time-guess VPOSR reads (a runtime yield-per-read hack was tried, broke the fire path — can't tell a parity-wait from a delay loop).
   - **Stale IRQ vectors during load (`$3532`/`$5694`):** the load runs with interrupts masked, so vectors still holding the previous screen's handlers must not fire. Modeled the real condition: `coro_deliver_timer_irq`/`pc_music_tick` fire a level only when `hw_get_intena()` has master (0x4000) + that level's source bit (LVL6 EXTER 0x2000, LVL3 0x70). Replaced an earlier `rt_gpl_has_fn` bank-gate band-aid.
   - **PC/harness divergence:** `pc_step` is now the single complete per-frame advance (game + `pc_music_tick` + audio); `pc_run` just loops it and the harness STEP_PC calls the same. Removed `g_pc_music_external` (made the standalone deliver music differently → `$5694` crashed only the standalone). New in-game vector handlers `$57DF78`,`$57A386` added to `gpl_seeds.txt`.
   - **Test via headless `benefactor-harness` (REPL `fire` injects fire; `headed` opens window on demand) — NEVER the standalone `benefactor-pc` (opens an SDL window needing manual fire).** `fb_view.py` reads dumps at **352×282** (= `HW_DISPLAY_W/H`); wrong stride shears every row ("warp").

1. **BPL2MOD divergence — FIXED**. Root cause: `$0031A0`/`$00377A`/`$0074AA` were called per-frame in `S_TITLE`; they must only run once at title-loop ENTRY (state is captured in chip dump). Removing them from the per-frame loop fixed it. Frame 0 now MATCH; BPL2MOD no longer diverges.
2. **Hardware register snapshot divergences (cosmetic for rendering):**
   - `bplpt[4,5]`: PUAE=$001004, PC=$000000. BPL5/6PT registers not in chip dump; actual bitplane pointers are in copper list (which matches), so rendering is unaffected.
   - `COLOR21-31`: PUAE holds sprite colors set by game code directly; PC fallback uses grey ramp (no COLOR21-31 entries in gameplay copper list). Native renderer reads colors from copper list directly so rendering unaffected.
   - `audio[1/2/3].vol`: Paula audio shadow registers not initialized from chip dump.
3. **Recompiler output** — split into per-subsystem files (done). bset/bclr/bchg/btst on data registers now 32-bit (mod 32). `RT_SET_NZ_*` macros fixed to evaluate side-effecting `(an)+`/`-(an)` operands once (was double-eval → `tst.w (a0)+` ran twice; resolved the title object-animation divergence). Indirect-dispatch handlers auto-discovered via `tools/recomp/discover_indirect.py` → `discovered.py`. Further: emit cleaner loops, remove dead patterns.
   - **gpl discovery is now ONE unified scan (2026-06-02).** `scanner.scan_program` is a single recursive worklist: one block scanner (`_scan_func`) + one edge rule (`_edge_targets`) + region superset, draining to a fixpoint. The edge rule **mirrors the emitter exactly** (a target registers iff the emitter renders it `rt_jump`/`rt_call`: calls always; `jmp` always incl. abs-long `$X.l`; `bra`/`Bcc` only when escaping the source function) so scanner∩emitter can't disagree. Targets register at their EXACT start even if interior (double-emit = bloat, not bug). `recomp.py` has a **build-time completeness gate** that ABORTS the regen on any unregistered in-region literal target — a leak fails the build, not the player's weekly quota. Replaced seed-by-miss + the old prologue/gap-fill/Phase-1/Phase-2 pass stack. Don't reintroduce per-miss seeding of discoverable targets; seeds are for genuine choke points only. **Build needs `TMPDIR` off `/tmp`** (tmpfs quota) → `scratch/tmp`.
4. **Native renderer pixel diff** — 9.8% pixel mismatch vs PUAE. Investigate after hardware snapshot issues are resolved.
5. **Harness is now DETERMINISTIC (both sides).** PUAE's live boot drifted ~1 frame run-to-run (host-timed disk load); FIXED by freezing/restoring a full save-state (`logs/puae_sync.state`; `BENEFACTOR_REFREEZE=1` to refresh). PUAE is now bit-exact. Compare `logs/chip_pc.bin` vs `logs/chip_puae.bin` directly after `FBDUMP_FRAME=N`.
7. **Blitter A-shift carry — FIXED (2026-05-25).** The recompiled blitter (`hw_blitter.c`) PERSISTED the A-shift register (`s_shift_carry`) across blits on the assumption "real OCS carries the A register across blits." That assumption is FALSE per PUAE's source: `blitter_start_init()` (`blitter.c:2003`) does `blt_info.bltaold = 0` at **every** blit start — the A register is reset per blit. The persisted carry corrupted the title-text shear compositing (the visible doubled/garbled glyphs), compounding through the double-buffer ping-pong. FIX: reset the carry to 0 per blit (now the default; `BLIT_CARRY=1` re-enables the old behaviour for testing). Result: chip RAM now matches PUAE for **251+ frames** (was 31) and the title text renders cleanly. Residual fb diff is the renderer's starfield/sprite/palette handling (separate, see #4), NOT the text.
6. **Frame-0 parity sync — FIXED (2026-05-25).** The dump was captured at the cop1lc-change point ($7BC8/$86CC), which is MID-iteration (~$003752). PC's `pc_step()` then ran a full fresh iteration while PUAE's post-sync `retro_run` completed the partial one **plus two more** (instruction trace: PUAE ran the title body `$41A4` **3×** in "frame 0" vs PC's 1×), permanently offsetting the double-buffer toggle parity. FIX: a CPU sync breakpoint in `newcpu.c` (`g_benefactor_sync_pc`/`_skip`/`_hit`, fires in `m68k_run_2_020`, sets `libretro_frame_end` so `m68k_go` returns cleanly) lets `harness_puae.c` advance PUAE to the once-per-iteration boundary **`$003742`** (`tst.w $41A2`, just before cop1lc is selected — NOT `$003732`, the wait-vblank spin) and dump there. Both sides now begin an identical full iteration; frame 0 MATCHES. `puae_run_to_loop_top(skip)` is the helper.
