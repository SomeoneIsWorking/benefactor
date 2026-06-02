# Benefactor PC Port — Current State

**Update this file whenever state changes. Not a journal — just the current truth.**

---

## Execution model (2026-06-02): game thread + SDL main thread

The recompiled game runs on its **own OS thread**; the SDL **main thread** paces
frames. They hand off via a condvar (`s_hand_cv`/`s_turn` in `pc.c`) so EXACTLY
ONE runs at a time — no data races on `s_game_ctx`/`g_mem`, deterministic.

- A frame wait is a **plain blocking call**: the emitter folds VPOSR/vblank →
  `hw_vblank_wait()`, fire → `hw_wait_fire()`, blitter → `hw_blitter_sync()`.
  `hw_vblank_wait()` calls `g_hw_vblank_yield` = `game_thread_yield()` which parks
  the game thread until the host releases it (one frame).
- `pc_step_threaded()` (main): release the game thread one frame → it parks at its
  next wait → present (SDL, on main) + deliver IRQ (`coro_deliver_timer_irq`,
  runs on main while the game is parked) + audio.
- Restart-at-entry (`pc_cps_start_at` for exit-to-menu/$3330, direct-gameplay/
  $577000, `$150`+`g_enter_gameplay`) = cooperatively stop the old game thread +
  spawn a fresh one at the new entry.

This **REPLACES the CPS continuation transform** (commit 56e0f53), which broke the
game (diverged in the first frames). The CPS revert: emitter emits straight-line
blocking C (no resume switch / `rt_cont_push` / `g_rt.yield`); `rt.c` lost the
continuation runtime (`g_rt`/`rt_cont_*`); `pc.c` lost `pc_step_cps`. It is a 1:1
re-expression of the ucontext coroutine (commit 5a7717a) that was working, using
real threads + a mutex per the user's request.

**VERIFIED via headless harness:** boots → intro crawl renders/scrolls →
title/screens transition on fire → `goto 1` (thread restart) → level card
($003914) → cavern ($003484) "GET READY!" renders, engine running, **0 rt-misses,
no watchdog**. `benefactor-pc` links pthread.

Regen all banks with `TMPDIR=scratch/tmp` (main/gp/gpl; credits has no CPS, no
input dump — leave it). The `g_rt`-referencing banks were main+gp+gpl.

---

## Harness Status

**Current status: PERFECT MATCH — frames 1–34 match**.

Last confirmed harness run: `bash run_harness.sh --frames 40` with:
- `Boot frame 0: MATCH` ✓
- Frames 1–34: all `ok` ✓
- Frame 35: `DIFF (BPL data CRC)` — buf_a staging area `$0718BA` (1 bit), not in active copper list BPL

### BPL CRC Comparison
PC's blitter is synchronous (completes instantly); PUAE's is async (may span retro_run boundaries).
Result: PC's POST-frame chip RAM is 1 frame ahead of PUAE's at glyph frames.

Fix: compare PUAE post-frame CRC against **PC pre-render CRC** (captured before game code runs
each frame). This correctly aligns the "display state" for both sides.
- `frames_differ()` no longer checks bpl_data_crc (would use wrong PC post-frame value)
- `harness_main.c` computes `pc_bpl_crc = s_pc_prerender_bpl_crc ? s_pc_prerender_bpl_crc : c.bpl_data_crc`

### Frame 35 Divergence (next issue)
At CF35, the pre-render CRC differs: PC=$E5A648DA, PUAE=$733819ED.
- Diff byte: `$0718BA` (buf_a base), 1 bit: PC=$FF, PUAE=$7F
- Cause: blitter A-shift carry accumulated from frame 33's buf_b glyph blits differs between
  PC (synchronous, carry=$FFFC) and PUAE (async, carry≈$3F80)
- **Not currently displayed**: copper list at CF35 points BPL2 to `$077E60` (buf_b), not buf_a
- All copper-list-pointed BPL regions MATCH at CF35
- Will matter when game transitions to gameplay and `$070958` becomes active BPL2

### Match Definition
1. `cop1lc` — copper list base address (checked in `frames_differ()`)
2. `coplist[]` — full copper list chip RAM content (checked in `frames_differ()`)
3. `palette[]` — same palette at frame-end (checked in `frames_differ()` for frames > 0)
4. BPL data CRC — PC pre-render vs PUAE post-frame (checked in `harness_main.c` as `bpl_crc_diff`)

Excluded:
- `bpl_data_crc` inside `frames_differ()` — removed; uses wrong PC post-frame value
- `audio[n].vol` — fundamentally incomparable (Paula DMA state vs register shadow)
- `bplpt[0-5]`, `bplcon0`, `bpl1mod/bpl2mod` — snapshot timing artifacts; coplist is authoritative

### Informational-only Diffs (not bugs)
| Location | Explanation |
|----------|-------------|
| `bplpt[0,1,2]` PUAE=$025334/48/5C PC=$04F6F4/7E60/F71C | Timing artifact: $7BC8 copper list has BPL1PT=$025334 before WAIT line=90, then $04F6F4 after. PUAE snapshots at frame-start, PC at frame-end. Same copper list. |
| `palette[]` 15 entries differ | Same timing artifact: copper list writes COLOR01=$0110 before line=90 WAIT, then $0248 after. Both PUAE and PC execute same copper instructions. |

---

## $7BC8 Copper List Structure (Title Screen)

The $7BC8 copper list has 3 sections active at different scanlines:

1. **Lines < 44** (before WAIT $2C11): background/border setup, COLOR registers, BPL1PT=$025334
2. **Lines 44-89** (after WAIT $2C11): display window, main title content  
3. **Lines 90+** (after WAIT $5A11 at $7CAC): different BPL pointers ($04F6F4) and palette (COLOR01=$0248 etc.)

The "final" value after full copper execution is from section 3. PUAE snapshots at section 1/2.

---

## Native Overrides in Place (pc_overrides.c)

| Address | Function | Notes |
|---------|----------|-------|
| `$0030C2` | `native_hw_wait` | No-op |
| `$0031A0` | `native_blitter_wait_clear` | Fully native — direct hw_write32/hw_write16 (no delegation) |
| `$003488` | `native_game_frame` | Delegates + copper static rebuild |
| `$003818` | `native_sprite_table_init` | No-op (broken recompiled) |
| `$0040B6`–`$004236` | render wrappers | All delegate to recompiled |
| `$00405C` | `native_text_sprite_render` | Delegates to recompiled; overwrites $7BC8 BPLPTRs with screen-0 values |
| `$0041A4` | `native_sprite_blitter_setup` | Delegates + copper static rebuild |
| `$0055A0` | `native_timer_interrupt` | Delegates to recompiled |
| `$0074AA` | `native_boot_anim_iterator` | Delegates with iteration guard |
| helper | `native_rebuild_copper_static` | Restores BPLPTRs + palette + mod lane in both $86CC and $7BC8 copper lists |
| helper | `TRACE_CHIP_MEMSET` | Traced chip-RAM scrub wrapper used for timer scratch resets |

---

## RESOLVED: gp-overlay TITLE/cover-art ($0081D2) bottom-half + leaderboard reflection (2026-05-28)

After the car demo the flow reaches the **title/cover-art screen** cop1lc=**$0081D2**, rendered by the **gameplay-OVERLAY bank** (gp; its $345A sets the copper). With fire OFF it is an attract-mode cycle: cover art → right-panel "BENEFACTOR" title wipe → credits scroll → **leaderboard/high-score screen** cop1lc=**$00844A** (water-reflection effect).

### Bug 1 (FIXED, commit 3d0fa98): cover-art bottom half never drew
Symptom: only the TOP ~128 logical rows of the cover art drew; bottom stayed black.
**Root cause was the BLITTER, not the gp reveal logic** (my earlier "reveal index doesn't advance" theory was WRONG — the $47B0/$4534 reveal routines animate the RIGHT-panel title text, not the cover art). The cover art is drawn by gfn_gp_003330: it loads BLTAPT=$975A / BLTDPT=$49000 once ($3414/$341C) then triggers BLTSIZE=$C00D **twice** ($3424, $3430), relying on OCS auto-advancing the channel pointers so the 2nd blit continues into the bottom half ($50800+). `hw_do_blit()` advanced its *local* apt/bpt/cpt/dpt but never wrote them back to the BLTxPT shadow registers, so the 2nd blit restarted at $49000 → bottom half all zeros. FIX: write the advanced pointers back to s_regs at the end of hw_do_blit (`src/recomp/hw_blitter.c`). Verified: full art + title now match the PUAE reference (`logs/cov_pu_s81_5.png`); no intro/credits regression.

### Bug 2 (FIXED): leaderboard ($00844A) water-reflection animation missing on PC
The reflection is a copper-driven per-scanline effect: ~78 per-line re-points of BPL2PT ($00E4/$00E6) + BPLCON1 ($0102) horizontal wobble + COLOR09 ($0192) shimmer (confirmed in PC's copper — it animates frame-to-frame). The native renderer (`native_renderer.c`) capped BPL-pointer anchors at `MAX_ANCHORS=16`, silently dropping reflection lines past ~16 → only the top of the mirror tracked, rest was static/wrong. FIX: raise `MAX_ANCHORS` to `HW_DISPLAY_H + 8` (one re-point per scanline). Reflection now renders fully and ripples.

### Tooling notes for reaching these screens in the REPL
`REPL=1 PC_DISK_BOOT=1 ./build/benefactor-harness <kick> <whdload> <disk1..3>`. Reach the title: `f 1; g 250; f 0` (fire advances the intro, then release — fire SKIPS the title, so observe with fire OFF). gp bank a5=$511E. Title reveal counters: bottom $4330 (a5-$dee), top $4332 (a5-$dec), toggle $34C8 (a5-$1c56). Leaderboard $00844A appears after ~2400 fire-off frames (`sp 2450`). PUAE cannot be driven to these screens in-harness (it dwells indefinitely at the $976 overlay-load screen, ~1s/frame) — use the saved `logs/cov_pu_s81_*.png`/`fb_puae_s81_*.bin` references instead.

### Menu freeze — FIXED (2026-05-28, two recompiler bugs)
Pressing fire on the title transitions to the **menu** (cop1lc=**$008302**, set at `$394A` in gfn_gp_003872). The menu setup calls the recompiled **ATN! decruncher `$3700`** (src $1339A "ATN!" $C800 → dest $49000), which spun forever and froze the whole app. Two emitter bugs (both fixed in `tools/recomp/emitter.py`, see "Confirmed Fixes Applied"):
1. **byte/word add/sub flags**: result truncated before the flag macro → carry/X always 0; byte ops used 16-bit N/Z/V. Broke the decruncher's `add.b d3,d3`→`bcc` bit-reader.
2. **`move X,-(An)` missing predecrement**: `wr()` only special-cased `(An)+`; `-(An)` wrote to `A[n]-sz` without decrementing `A[n]`. The decruncher's `move.b -(a3),-(a4)` literal copy left `a4` frozen → never terminated.
Now the menu reaches and renders (left "BENEFACTOR" panel, green textured bg, beach scene, a "NORMAL" option). Deterministic intro comparison still a PERFECT MATCH; cover-art/leaderboard unaffected.
**gp-bank regen gotcha:** must use the FULL `tools/recomp/gp_seeds.txt` seed list (`--seed "$(cat …/gp_seeds.txt)"`), NOT just `3330` — the extra seeds are indirectly-reached entries; `3330` alone drops ~16 of 44 functions. (CMakeLists comment now documents this.)

### Menu → gameplay (NEXT PHASE — input model mapped, not yet launching)
The menu ($008302) main loop is `gfn_gp_003872` @ `$3960` (hw_vblank_wait top). Per-frame it reads input via `jsr $3BAA(=a5-$1574)` → d0, then `$39B8 btst #5,d0`:
- **bit5 clear** → `$39BE`: `tst $2be2(a5)`; if nonzero loop, else fade ($7C22) + `jmp $33E2` (back toward title/attract).
- **bit5 set** → `$39D0`: navigation keyed on selection `-$18be(a5)`(=$3860) and sub-flag `-$15b6(a5)`(=$3B68). With sel==0 & -$15b6==0 it falls to `$39F2`: the **start-game** path — palette fade, `jsr a5-$b22`, then sets low-mem timers `$1c.w=$bf`, `$20.w=1`, and runs a timer/flag-gated menu-out animation ($3A26–$3A6C) comparing `$1c`/`$20` vs `$bf`/`$3c` and testing flag `$38.w`.

Empirical (harness REPL): idling the menu with NO fire stays rendered indefinitely (attract timer `$2be2`=900 does NOT decrement on PC — likely the gp timer ISR `$53A2` should decrement it; not yet). A *held fire* turns the screen **black** but does NOT reach the start path (`$1c`/`$20`/`$38` stay 0, sel stays 0) — so the harness's `hw_set_fire`/$bfe001 injection is NOT what `$3BAA` decodes as bit5. The menu's real input source ($3BAA / `$1e.w` = keyboard/CIA buffer) needs mapping before the menu can be driven to launch. **The standalone game (`run_pc_game.sh`) with a real keyboard/joystick may navigate it even though the harness can't inject the right input.** NEXT: RE `$3BAA` (a5-$1574) to learn what bit5 is and where `$1e.w` is populated; ensure the gp timer ISR `$53A2` updates `$2be2`/`$1c`/`$20`; then the start path + gameplay/level entry.
- **Menu text**: a menu label renders as garbled "3MOLGPOLGP" (may be a scramble-in animation, or a remaining text-routine issue — unverified).
- **Title ($0081D2) music — tempo FIXED, volume residual OPEN.**
  - **FIXED (tempo, commit e7e45f0):** the gp music engine is the level-6 (CIA-B Timer A) ISR `$3544`→`$53A2`; on HW it fires several times/frame but `coro_deliver_timer_irq` delivered it once/frame → ~3× slow. Fix: live audio now QUEUE/push mode (`hw_audio_open` callback=NULL); `pc_run` renders each frame's 441 samples in `GP_MUSIC_TICKS=3` chunks, calling `pc_music_tick()` (delivers the $78/v6 ISR) before each chunk so the song advances at full tempo AND each sub-frame note is rendered. `g_pc_music_external` gates the v6 delivery out of `coro_deliver_timer_irq`. N=3 (not 7) — N=7 played right notes ~2.3× too fast. Harness BOOT_DISK has the same chunked render under `GP_MUS_MULT`.
  - **FIXED (left "break", commit 7ea079a):** Paula is hard-panned (ch0/3 L, ch1/2 R); PUAE's default `sound_stereo_separation=7` blends ~19% across sides. `hw_audio_mix` now applies that blend (`(l*26+r*6)/32`) so a resting left channel no longer collapses.
  - **OPEN (volumes ~3× low):** PC's engine writes channel volumes far below PUAE — confirmed via `PUAE_VOL_TRACE` (added to vendor audio.c `AUDxVOL`) vs PC `BENEFACTOR_AUD_TRACE`: PUAE ch0/ch3 reach **vol=64** (800+ writes), PC reaches **~17-22** (ch3 mostly $16). **Rate-independent** (identical at GP_MUS_MULT=3 and 18) → NOT a timer-rate issue, a **computation divergence** in the music engine. Localized to the volume written from the voice struct (`voice+12` → `AUDxVOL` at $5450) set by the note/effect handlers (jump table at $571c; instrument table base `lea $6a2c(pc)` = **$C16A**, 32-byte entries). Could not pinpoint the exact diverging instruction by reading (dense Protracker-style engine). NEXT: instruction-level PC-vs-PUAE trace (`-DBENEFACTOR_TRACE_INSNS=ON`, `RT_INSNS`/`BENEFACTOR_M68K_TRACE` + `BENEFACTOR_M68K_RANGE`, `BENEFACTOR_M68K_FRAMES`) over the $53A2 player → diff the voice-vol computation (same technique that found the RT_SET_NZ bug). Tooling: `AUDCH_ONLY` isolates one Paula channel; lockstep `STEP()` dumps PC PCM frame-aligned with PUAE under AUDIODUMP.
- Audio: the leaderboard/other-screen audio not separately investigated.

## Confirmed Fixes Applied

- **Blitter: byte-odd channel pointers not word-aligned — FIXED (2026-05-28)** (`hw_blitter.c`, where `apt/bpt/cpt/dpt` are read from `BLT*PT`). The OCS blitter is WORD-addressed — it ignores bit 0 of the channel pointers (chip-RAM DMA is word-granular). The PC blitter honored a byte-odd pointer, writing the data 8px off. The title-car bob ($91D0) computes `dpt = $73680 + (carX>>3)` — a *byte* offset that is odd whenever `carX>>3` is odd — and puts the fine sub-word X in `BLTCON1` BSH (0-15). On hardware the odd byte is dropped and BSH supplies the fine position, so X = carX (smooth). On PC the odd byte shifted the car 8px on those frames → the car LURCHED (smooth −10px/frame on PUAE became −18,−3,−18,−3… on PC, i.e. the "car animation stutter"). FIX: mask bit 0 off all four pointers (`& ~1u`). Verified: car buffer `$73680-$77000` is now **0-diff (bit-exact)** vs PUAE on every frame (was oscillating 2200/0/0), leading-edge motion is smooth −10→−6/frame matching PUAE; title ($86CC) unchanged. ROOT-CAUSED with the lockstep REPL: `dc` of the car buffer with PUAE advanced +1 frame (`su 1`) showed the offset oscillating 0/2200 (matched 2 of 3 frames, jumped the 3rd) → not a phase/render issue but a position bug on the odd-`carX>>3` frames. NOTE: a native motion-player reimplementation of `gfn_game_frame` was tried first and reverted — the recompiled handler + word-aligned blitter is bit-exact, so no native rewrite was needed; the motion table lives at `a5-$2082` (64-word ease-in/center-pause/ease-out X path), phase at `a5-$2084` (+2/frame), cell toggle `a5-$2088 ^= $f1e`.

- **Blitter: B-channel not shifted in ASCENDING mode — FIXED (2026-05-27)** (`hw_blitter.c`, the `else` branch of the per-word shift). The ascending path shifted A by ASH (`a = ((prev_a<<16)|a_masked) >> a_shift`) but set `b = b_raw` (UNSHIFTED), while the descending path shifted B correctly. The car-demo bob ($91D0) is an ascending cookie-cut (con0=`$XFCA`, con1=`$X000`): A=mask, B=data, both want the same sub-pixel shift X. With B unshifted, the mask and data misaligned by X; since X cycles 0–15 every frame (the moving car's fine position), the car garbled DIFFERENTLY each frame → looked "stuttery / glitchy / incorrect" in motion (driver flickered in/out, edges fringed). Fix: `b = ((prev_b<<16)|b_raw) >> b_shift` (mirrors A, and the descending path). Verified frame-by-frame: PC car now matches PUAE (e.g. f707/f710 zoomed identical modulo the constant ~2px capture offset); title screen unaffected (only blits with `b_shift != 0` change; `b_shift==0` is identity). FALSIFIES the line-85 claim "car renders clean" and the line-89 note "the B-shift lead was wrong" — those were about the GREEN-BOX symptom (con1 mode, fixed by `ror`); the B-channel-shift bug was a SEPARATE, still-present defect that the green-px-count check didn't catch. Diagnosed by: (1) per-frame state vars `-$2088/-$2084(a5)` MATCH PUAE, (2) blit params MATCH PUAE once you account for PUAE logging pointers POST-blit vs PC PRE-blit (the apparent "one cell off" apt was `BLTAMOD×height`), (3) `dc` of the car buffer `$73680-$77000` showed PC `80 00` where PUAE had `87 FC` — PC losing the bits a right-shift spills into the next word.

- **Recompiler: `ror`/`rol` implemented — fixes the car-demo green box** (`tools/recomp/emitter.py`). The recompiler emitted `ror`/`rol` as empty comments (`/* ror: ... */`) — total no-ops. The car-demo draw (`game_frame` $3604) does `ror.w #4, d2` to move the bob's sub-pixel shift into BLTCON1 bits 15-12 (BSH). With ror a no-op, the shift stayed in bits 0-3, where it set the **descending/fill** bits of BLTCON1 → the blitter ran the car blit in the wrong mode → a bright-green ($1F1) box + duplicated fragment around the car as it scrolled in. ROOT-CAUSED by comparing PC vs PUAE BLTCON1 in the blit traces (`BLIT_TRACE_ALL=1 BLIT_TRACE_DIR=<abs>`): PUAE con1=`$1000/$2000/...` (BSH in bits 12-15), PC con1=`$0001/$0006/$000E...` (shift in low bits → desc/fill). Fix: implement `ror`/`rol` (8/16/32-bit, register or memory dest, C/N/Z flags). Verified: car renders clean at entrance (380) and center (440), 0 green px vs PUAE 0; title still matches frames 1-31; tests 51/51. This was the LAST comment-only no-op in the generated code (`grep '/\* ror\|/\* rol\|UNK:'` now empty). NOTE: the blitter also gained correct **descending mode** (validated by the `BLIT_SELFTEST` oracle — descending copy + cookie-cut both PASS); the car blits are now ascending (correct con1) so it isn't exercised by them, but other descending blits are now handled.

- **Recompiler: `Scc`/`st`/`sf` implemented** (`tools/recomp/emitter.py`). The recompiler had NO handler for `Scc` (set-byte-on-condition), so it emitted `/* UNK: st.b ... */` no-ops. This silently broke the car-demo screen ($91D0, `gfn_game_frame` $3488): `$35DA st.b -$2002(a5)` and `$35E6 st.b -$2001(a5)` set the screen's exit flags when the animation phase `-$2084(a5)` hits `$3e`/`$7e`. Un-set → the exit check at `$3652` always looped back → the car demo **never ended** (car kept re-blitting → corruption). Fix: emit `MW8(ea, 0xFF)` for `st`, `0x00` for `sf`, `(RT_CC_x?0xFF:0x00)` for conditional `Scc` (data-reg dest sets low byte). This was the **last `UNK` instruction in the entire generated codebase** — `grep 'UNK:' src/generated/*.c` is now empty. Verified: car demo advances $91D0→$7770 and the exit flags ($331A/$331B) become $FF, matching PUAE's "car leaves → fade → next screen". Tests 51/51 pass. ALSO fixed the CMake regen dependency: `recompile_game` now depends on emitter.py/scanner.py/helpers.py (it only listed entries.py, so emitter edits didn't trigger regeneration).

- **(RESOLVED) car-demo green box + right-edge fragment** — was the `ror`/`rol` no-op above (BLTCON1 BSH shift landed in the desc/fill bits). Fixed by implementing `ror`/`rol`. The earlier "1-frame timing offset" and "B-shift" leads were both wrong; the real cause was found by diffing PC vs PUAE BLTCON1 in the blit traces.

- **Harness determinism save-state missing** (`logs/puae_sync.state`): not present, so the PUAE side live-boots non-deterministically and the auto-comparison frame-0 lands on either double-buffer half ($7BC8 vs $86CC). Root: `harness_puae.c` writes the state to the RELATIVE path `logs/puae_sync.state`, but PUAE chdir's away from the repo root, so the write silently fails (same CWD bug as the blit trace). FIX NEEDED: resolve the state path (and trace paths) to absolute at startup before PUAE chdir's. This is independent of the car-demo work.

- **Blitter does NOT implement descending mode** (`hw_blitter.c`): `desc = (bltcon1>>1)&1` is read but unused — the blit always processes ascending (apt/dpt += per row). Latent bug for any DDOWN blit; the car bob is ascending so unaffected here, but worth fixing.

## Debugging tools: live HTTP server + savestate render

- **HTTP debug server** (`src/pc_http_debug.c`, opt-in via `BENEFACTOR_HTTP=<port>`):
  runs a localhost-only thread in `benefactor-pc` so you can inspect the game WHILE
  someone plays. Use it with `BENEFACTOR_HTTP=8080 ./run_pc_game.sh` (env passes
  through). Endpoints (GET): `/state` (level, cop1lc, bank flags, `$57FEB8` player
  block), `/mem?addr=HEX&len=N`, `/poke?addr=HEX&val=HEX`, `/fb.ppm`, `/fb.bin`
  (raw ARGB8888 352×282 → `tools/fb_view.py png`). Zero overhead when the env var
  is unset.
- **harness REPL `loadmem <file>` + `render`:** `loadmem` loads g_state+g_mem from a
  savestate WITHOUT resuming the parked game thread (works across binaries — the
  Amiga memory map is binary-independent); `render` re-renders s_fb straight from
  g_mem with the snapshot delay bypassed. Together they reproduce ANY saved scene
  exactly for inspection. NOTE: `render` MUST force `g_native_render_delay=0` — the
  default render path reads a 1-frame snapshot ring (`s_snap_ring`), which after a
  bare `loadmem` (no stepping) still holds the pre-load frames → shows the OLD scene.

## Known bug: savestate LOAD crashes (mid-gameplay)

Loading a mid-gameplay savestate from a fresh boot crashes (user-reported). Root
cause is the STOPGAP documented on `pc_savestate` (pc.c): the save captures
`g_state` (incl. the M68K `game_ctx`) + `g_mem` but NOT the game THREAD's C call
stack, so the resume can't continue the thread at its suspended point. The thread
model can't serialise the C stack. Proper fix direction: on load, RESPAWN the game
thread at a clean per-frame re-entry (e.g. main-loop top `$577114`) with the loaded
`g_state`/`g_mem`, the same way `goto`/direct-gameplay respawn via `pc_cps_start_at`
— the engine re-reads all live state (player/objects/camera) from `g_mem` (a5-rel +
chip RAM) each frame, so it should continue the saved scene. NOT yet implemented.

## Chandelier chains — FIXED (blitter LINE mode implemented)

Root cause: the game draws the chains with the blitter's **LINE mode** (BLTCON1
bit0), and `hw_do_blit` only ever did area copy — it ignored bit0, so every chain
line-draw was mistreated as an area blit → no chain. Found by the PUAE oracle
(`pugoto 35` + `puloadmem` teleport): PUAE issues LINE-mode blits (con1=0001 etc.)
from engine routine `$57DE3C` for the chains; PC issued none. FIX: implemented
`hw_do_line()` (Bresenham, octant/accumulator logic ported 1:1 from PUAE
`blitter.c` actually_do_blit line path) + area **FILL** mode (inclusive/exclusive,
PUAE `build_blitfilltable`). Both gated by BLTCON1 bits, so normal area blits are
untouched (no intro/title regression). Verified: the orange diagonal chain now
draws in PC at the chamber, matching the PUAE reference.

LESSON (per user): audit which blitter MODES the game uses vs what our blitter
implements (env `BLT_MODES` on the PUAE side logs con0/con1 incl. LINE/fill) —
don't chase one hypothesis at a time. My first "ruled out line mode" was WRONG
because I tested at level *start* where the chain is off-screen; the oracle at the
actual chamber settled it. Candidate next: the **gameover-screen cursor** image is
missing — possibly another mode/feature our blitter or sprite path lacks.

## OPEN: wrong SFX plays sometimes (jump grunt) — needs music/SFX code RE

Repro: compare harness, level 1, past GET READY, hold fire+left → repeated jumps
+ grunt SFX; PC sometimes plays a wrong/different sound. Tools added: `SFX_TRACE=1`
(logs/sfx_pc.txt: ch/AUDxLC/len/per/vol on each audio-DMA enable) + REPL `audlc`
(PC vs PUAE per-channel sample ptr).

RESOLVED separation (2026-06-02): SFX has its OWN state, separate from music —
`$57fe4e.b` pending flag + `$57fe50` active descriptor (sample ptr+params),
triggered by `$58656E` (`$775c(a5)`) and streamed by `$586612`. So you CAN isolate
SFX: diff `$57fe50` PC-vs-PUAE on the same jump, NO music confound. Full SFX-engine
map in `instructions/gameplay-engine-map.md` ("SFX engine"). Jump grunt sample =
`$5B0BE4`. NEXT: drive PUAE (pugoto) + PC (goto) to a jump, compare `$57fe50`.

OLD DEAD END (channel-snapshot path, do not repeat): per-channel AUDxLC snapshot
diffing can NOT separate SFX from music (music retriggers every frame + PC/PUAE
phase skew). `$07AE94` etc. were MUSIC samples, not the grunt (there is no idle
grunt). Use the `$57fe50` SFX-descriptor path above instead.

GAMEPLAY AUDIO ARCHITECTURE (RE'd 2026-06-02, verified via SFX_TRACE+fn column):
The gameplay (gpl bank) audio is a COMBINED music+SFX player whose hardware output
is a single CIA-B timer ISR, delivered via `pc_music_tick` (the `$78`/LVL6 vector):
  - `$59BF3E` — DMA-RETRIGGER stage. Reads an enable-mask (`$59d240`) + per-channel
    "new sample ready" flags (`$59cf1c` ch0, `$59cf1d` ch1, bit1 of `$57fea5`), then
    does the classic disable-then-enable DMACON write (`$dff096`) to retrigger the
    ready channels. EVERY DMACON audio-enable (music note-on AND SFX) goes through
    here — confirmed: 100% of SFX_TRACE `fn=` are `$59BF3E`.
  - `$59BFA6` — HARDWARE-COPY stage. Copies precomputed AUDxLC/AUDxLEN from a buffer
    at `$59d14e` into Paula `$a0/$b0/$c0/$d0(a5)` for the ready channels.
CONSEQUENCE (corrects the old plan): there is NO separate "SFX writes Paula
directly" path. SFX is injected into the SAME player's voice table; an ISR-level
kill-switch (BENEFACTOR_MUTE_MUSIC / REPL `mute`, gating `pc_music_tick`) mutes BOTH
music AND SFX — VERIFIED: 0 DMACON enables after the mute frame, jumps included.
NOTE: `$07AE94` (len `$004C`) is NOT an "idle grunt" (per user: there is no idle
grunt) — it's present at baseline = a MUSIC sample. The grunt sample is still
UNIDENTIFIED; derive it empirically by fire-correlation (sample that recurs right
after each fire press across many jumps), NOT by trusting the old "idle" label.

TOOLS: `mute [0|1]` REPL + `BENEFACTOR_MUTE_MUSIC` env (pc.c `g_mute_music`, gates
`pc_music_tick`); SFX_TRACE now logs `fn=` (g_rt_last_call) per DMACON-enable.

NEXT (to actually isolate SFX): intercept UPSTREAM, not at the ISR. Find (a) the
music-DECODE routine that fills the voice table / sets `$59d240` + ready-flags for
the music channels, and (b) the SFX-TRIGGER the jump code calls to inject the grunt
into a voice. Then suppress only one side's per-channel writes. Lead: pcwatch
`$59d240` (enable mask) and the per-channel sample slots during a single jump to see
which fn (non-ISR) writes the grunt vs the music. Old (title-bank) refs, may differ
in gpl: instrument table `$C16A`, note/effect jump table `$571C`, voice-vol→AUDxVOL
`$5450`, title player `$53A2`. This RE also lays groundwork for the FEATURE:
independent music/SFX volume. Then instruction-trace the trigger PC vs PUAE for
grunt-selection (same technique as the RT_SET_NZ bug).

## OPEN: gameover-screen cursor missing = native hardware-SPRITE rendering unimplemented

The CONTINUE/GAME OVER menu (cop1lc=$003914 after death; reach it in PUAE via
`pugoto 3` → walk right into the water of "Keep Your Feet Dry", or `pc`-side
`gameover`) shows a white selector dot left of CONTINUE in PUAE but NOT in PC.
Root cause: the gameover copper activates **hardware sprite 0** (SPR0PT=$0000EE2E,
sprite palette COLOR16-31 at copper $1A0-$1BE), but `native_render_frame`
(`native_renderer.c`) has **ZERO** sprite code — it never composites hardware
sprites. So the cursor (and any other hardware-sprite graphic) is missing. NOT a
blitter issue (the chains were; this is separate). FIX (not yet done, user
checkpointed): implement hardware-sprite compositing in the native renderer —
read SPRxPT from the copper (anchor them like BPL pointers), decode SPRxPOS/CTL
for X/Y + attach, read the 2 sprite bitplanes → COLOR16-31, composite at the
playfield priority. Verify against the PUAE oracle; guard the deterministic intro.

## Debugging tool: interactive REPL

`PC_REPL=1 ./build/benefactor-harness <kick> <whdload> <disk1..3>` boots the native disk coroutine and drops to a stdin REPL — step the flow and inspect chip RAM live without recompiling per probe. Commands: `s [n]` step, `g <frame>` run-to-frame, `u <addr> <val> [w|l]` step-until-memory-equals, `f <0|1>` hold fire/start, `p <addr> [n]` peek bytes, `w <addr>` peek word, `c` frame+cop1lc, `fb <path>` dump framebuffer, `q` quit. This replaced the env-var recompile cycle for the car-demo root-cause.

- **Logo palette fades restored** (`pc_overrides_boot.c` `native_boot_anim_iterator`, $0074AA = `$218e(a5)`, a5=$531C). This routine is the game's palette-fade iterator: the recompiled screen handlers ($3218/$31C2/$366A) call it ONCE per fade and expect it to BLOCK for the whole ramp (original $74AA waits `(delay+1)` vblank frames per pass via the `btst #0,$3(a6)` toggle + `dbra`, steps every R/G/B nibble one step toward target, repeats `outer` times → `outer*(delay+1)` ≈ 16*2 = 32 frames). The native override had been written for the OLD per-frame native-title design (one step per call, vblank delay skipped), so when the coroutine flow calls it once it collapsed the whole fade into a single frame — logo fade-ins/outs were invisible and pressing fire appeared to skip everything instantly. Fix: faithfully wait `(delay+1)` frames per pass with `hw_vblank_wait()` (which yields through the game coroutine, same as the recompiled hold loops). Verified: PC now matches PUAE frame-exact through the post-title logo screens — same screen transitions ($78F0 f=171, $77C0 f=237, $91D0 f=323 on both sides) and identical COLOR ramp each frame (black→full→black). LESSON: native overrides written for the dead per-frame native-title loop can silently break under the recompiled coroutine flow, which calls them once and expects original blocking semantics.

- **Copper WAIT next-frame guard** (`hw_copper.c` `copper_pos_reached`): the Amiga copper 8-bit vertical counter wraps at 255. After line 255, any WAIT with `vp < 128` where `(cmp_line - vp) > 127` is a "next-frame" wait that should only fire after the counter wraps. Without this guard, WAIT(vp=1..43) fired immediately at line 255 (cmp=255 > 1), causing BPLCON0=$0200 to execute at line 255 (out_y=228 with top_border=17), rendering star-field lines 228–255 as black. The guard defers these WAITs to lines 257–299 where cmp_line properly equals vp. Fix: `if (cmp_line > vp && (cmp_line - vp) > 127) return 0;` in `copper_pos_reached`.

---

## Key Facts (Do Not Re-Examine)

- **gfn_blitter_wait_clear** ($0031A0): zeros $070930–$07F930 (61440 bytes). PUAE ran this during BOOT. PC must NOT run it again (sync dump is already post-init state). `call_fn(0x0031A0)` was removed from `if (s_first)` block in pc.c.
- **Blitter fill** `bltcon0=$19F0` overwrites `$8720–$872C` with `$FFFF` each frame. `native_sprite_blitter_setup` restores BPLCON0/1/2 afterwards.
- **Counter at $67D2**: timer interrupt (`gfn_0055A0`) decrements this. When 0, runs full palette computation. Starts at 7 in sync dump (so first palette compute is frame 7).
- **Palette animation data** at A6=MR32($69DC)=$06231A. First byte=$80 (bit7 set = negative path in timer interrupt).
- **A5 = $DFF000** always. **A6 = $DFF002** in some contexts (hardware base offset).
- **coplist[] in frames_differ()** is chip RAM bytes from cop1lc — correct timing-independent comparison.
- **palette[] and bplpt[] shadow registers** in hw_get_snap() reflect end-of-frame state, NOT PUAE's beginning-of-next-frame state — do not compare directly.

---

## Current Confirmed Findings

- `run_pc_game.sh` now launches `benefactor-pc` with `chip_ram_dump.bin`; the `benefactor-pc` target itself was switched to the `pc.c` runtime path (old `recomp_main.c` target path removed) so standalone and harness PC execution share one runtime.
- `$0091D0` gameplay copper corruption is fixed: PC blitter wrote `$0120 -> $99B9` at frame 0 because gameplay-copper protection ended at `< $91D0`. Expanding to `< $91D4` removed boot-frame coplist DIFF (`Boot frame 0: MATCH`).
- **Frame-sequence regression FIXED:** `native_sprite_blitter_setup` (`pc_overrides_copper.c`) was skipping the original `$0041A4` `not.w -$117A(a5)` toggle that controls title/gameplay selection via `$41A2`. This caused PC to stay on gameplay copper (`$86CC`) at frame 1 while PUAE correctly switched to title (`$7BC8`). Fix: added the `$41A2` toggle to `native_sprite_blitter_setup`. Harness now shows `Frame 1: PIXEL DIFF (copper matches)` with all BPL ranges MATCH.
- Disabling `$0041A4/$00405C` in PC removes the large bitplane-write divergence (BPL ranges become MATCH), but leaves a smaller pixel DIFF and PUAE-only state updates (`$0042FC..`, `$0069F1..`, `$006A27..`, `$07FFA2..`). This confirms call-sequence mismatch is central but requires a sequence-correct replacement, not a blind skip.
- The first differing bitplane bytes are written by the **PC blitter**, not by CPU `MW16/MW32` stores. Harness trace now shows `[PC BLT]` at the first diff addresses in BPL1/BPL2/BPL3/BPL4.
- At the first-diff addresses, PC blitter math is internally consistent (`out` matches `A` with `C=0` for `bltcon0=$0BFA`), so the immediate mismatch is upstream source-buffer content (`A` words) rather than the final store itself.
- Upstream source words feeding BPL2 first diff (`$070934`) come from prior PC blits into `$06942x` (e.g. `$06942C=$B4FC`, `bltsize=$06A8`, `apt≈$061EA4`, `bltcon0=$0BFA`); no CPU writes were observed to those source windows.
- PUAE writes zeros at the BPL2 diff window (`$070934..$070944`) while PC writes nonzero (`$B4FC...`), confirming divergence in the blit data stream before pixel conversion.
- The blitter passes hitting the live bitplanes use `bltcon0=$0BFA`, `$FBFA`, and `$19F0` with destinations inside the title-screen bitplane ranges.
- Concrete fix point is now isolated in `gfn_sprite_blitter_setup` (`$0041A4`) loop: PC emits a unique `BLTSIZE=$7D60` (PUAE never emits this size in the compared window), then immediately writes first bad word at `$070934` (`[BLT_PROBE_DIFF]`).
- At that bad launch, PC writes `BLTDPT=$0007030C` and enters blitter with `d2=$7D60`, `a1=$0007030C`, and `a2=$00004218` (`[BLTDPT_LIVE_PC]`), which means `(a2)+` has advanced outside the intended `$4166..$4178` word table and is feeding an invalid height/size into `$56(a6)`.
- Root cause confirmed: runtime flag macros in `recomp/rt.h` used internal locals named `_d/_s/_r`, colliding with generated compare blocks that also declare `_d/_s`; this produced undefined flag results at `$004212 cmpa.l` and made `$004218 bne` take when `a2==$417A`.
- Applied fix: renamed macro internals to collision-proof identifiers (`__rtf_*`) in `RT_ADD_FLAGS_*` and `RT_SUB_FLAGS_*`; this removed the bad `BLTSIZE=$7D60` / `BLTDPT=$07030C` launch entirely.
- Post-fix behavior: PUAE and PC blit-launch streams now match exactly after the known startup-only PUAE launch (`$0031BA`, `BLTSIZE=$803C`, `dpt=$070930`) is skipped.
- The current runtime still executes blits synchronously on `BLTSIZE` writes. A first deferred-blit experiment did **not** remove or move the frame-1 diff, so "simple late completion" is not yet proven as the full root cause.
- The generated `st.b -$1cb4(a5)` at `$0052CA` is still unimplemented, but its target byte `$003668` stays `00` in boot / PUAE / PC snapshots for this frame and is **not** the controlling difference here.
- In PC frame 0, timer interrupt path `$0055A0` writes `$0069DC=$00DFF002` (`[STATE_CPU32]`), while sync/PUAE keep `$0069DC=$000622DA`. This pointer feeds `movea.l $69dc(pc),a6` in the same routine and is upstream of `$0069F0-$006A70` state-table updates.
- Sprite base pointers are not the missing display surface for the current frame-1 diff: harness snapshots now capture `sprpt[8]` on both sides, and the failing 2-frame run reports `[sprite-ptrs] all 8 match` while the `6.7%` pixel diff remains.
- Disabling PC sprite drawing (`PC_DISABLE_SPRITES=1`) leaves the frame-1 diff unchanged (`5499`, same first pixel), so active sprite overlay is not the controlling path.
- Renderer mapping knobs move the pixel diff while PC chip RAM remains byte-identical (`cmp logs/harness_pc_after_frame_baseline.bin logs/harness_pc_after_frame_tuned.bin` returns equal):
	- default (`TOP=17`, `X=56`) => `5499` diff
	- `TOP=16`, `X=50` => `5072` diff
	This confirms the remaining issue is primarily render interpretation/alignment, not missing game-state mutation.
- Decode-mode sweep results: `BENEFACTOR_LORES_X2=1` worsens diff; `BENEFACTOR_WAIT_AT_FETCH` is negligible; `BENEFACTOR_ADVANCE_BEFORE=1` has non-monotonic interaction with x/y bias and is not a standalone fix.
- Additional renderer probes falsified: forcing hires downscale had no effect; doubling computed DDF fetch width made divergence much worse (`11.9%`).
- New artifact/tooling: harness now dumps framebuffer binaries on pixel diff (`logs/harness_puae_fb_diff.bin`, `logs/harness_pc_fb_diff.bin`), and `tools/analyze_fb_alignment.py` reports shift sensitivity; current baseline best overlap shift is `dx=8, dy=5` with residual `4810/78312` (still high), ruling out a pure constant translation fix.
- Row-vs-mode correlation (`logs/fb_mode_correlation.txt`) localizes the residual by active `BPLCON0` segment: `$4200` contributes `3569` diff pixels, `$3600` contributes `1926`, `$0200` contributes only `4`. This shifts focus to renderer semantics for those two display modes, not global state divergence.
- Mode-specific mapping probes confirm the mismatch is segment-dependent, not a single global offset:
	- `BENEFACTOR_XDELTA_4200=-6` (default global settings) reduces diff `5499 -> 5257`.
	- Combining `BENEFACTOR_TOP_BORDER=16` with `BENEFACTOR_XDELTA_4200=-6` reduces further to `5048`.
	- Additional `$3600`-only delta keeps reducing the metric (e.g. `BENEFACTOR_XDELTA_3600=20` gives `4971`), indicating unresolved mode-transition mapping rather than a physically-correct single constant.
- Important falsification: enabling `BENEFACTOR_LORES_X2=1` worsens the diff across broad top/x sweeps (typically `~8.3–8.8%`), so the residual is not solved by simple lores pixel-doubling.
- Additional falsifications/low-impact checks:
	- `$4200`-only fetch-width doubling probe (`BENEFACTOR_FETCH_X2_4200=1`) worsens to `7537` (`9.2%`) and is rejected.
	- Approximate dual-playfield decode probe (`BENEFACTOR_DUALPF_DECODE=1`) only reduces `5499 -> 5490` (all in `$3600` segment), so it is not the primary cause.
	- One-line `BPLCON0` transition-delay probe (`BENEFACTOR_DELAY_BPLCON0=1`) worsens slightly (`5507`) and is rejected.
- New reusable analyzer `tools/analyze_fb_mode_offsets.py` emits per-row best-dx and per-mode totals from log + dumps. Current baseline report (`logs/fb_mode_offsets_report.txt`) shows:
	- `BPLCON0=$4200` dominates (`3569` diff px) with row best-dx centered around `~+5`.
	- `BPLCON0=$3600` is secondary (`1926` diff px) and mostly near `dx=0` except transition bands.

- Newly confirmed high-signal branch: line-phase correction is transition-local, not global. With `BENEFACTOR_TOP_BORDER=17`, `BENEFACTOR_XDELTA_4200=-6`, and `BENEFACTOR_XDELTA_3600=10`:
	- Global `BENEFACTOR_BPL_LINE_OFFSET_4200=1` gives `5013` diff.
	- Windowed `BENEFACTOR_BPL_LINE_OFFSET_4200=1` over early rows only (`START=0`, `END=55/58`) improves to `4911` diff.
	- Refinement found `END=59` improves slightly further to `4902`.
	- Extending past transition (`END>=60`) regresses immediately back to `5013`.
	This isolates a specific `BPLCON0=$4200` transition-band phase mismatch around `y≈56..60`.
- Re-sweeping `$3600` x-delta on top of that transition window keeps reducing the scalar metric (`4911 -> 4482` by `XDELTA_3600=80`), which is now treated as a non-physical alignment/gaming signal rather than a credible final fix. Use this as evidence that post-transition `$3600` mapping still lacks a semantic model; do not promote large fixed x-shifts as solutions.
- Falsified branch: odd/even-row-specific `$4200` x-origin correction (`BENEFACTOR_XDELTA_4200_ODD`) does not beat the non-parity baseline and was removed.
- Safety check: default runtime remains unchanged after adding the windowed probe hooks; with no env vars, harness still reports `5499` diff and first mismatch `(120,17)`.
- New single-row isolation: the sharp `END=59 -> END=60` regression is entirely row `y=60` (`+111` mismatched pixels); no other row changes.
- New pointer-sync falsification: `BPL_SYNC` tracing shows no `s_bplptr` resyncs near `y=60`; pointer resyncs occur at line 90 / `out_y=63` (the known mode switch point), so the y60 anomaly is not caused by mid-band pointer rewrite.
- New plane-mask probe: applying the `$4200` line-offset only to planes 0/1 (`MASK=3`) slightly improves over all-planes (`5002` vs `5013` for full window), and combined with `SKIP_Y=60` reaches `4891` (best probe metric so far). This is still probe-only and not a semantic fix.
- Additional row-level characterization of the `END=59 -> END=60` jump:
	- `PC(end59)->PC(end60)` changes exactly `147` pixels, all on `y=60`, across spans roughly `x=116..271` (with small gaps).
	- All changed pixels are regressions versus PUAE (`better=0`, `worse_or_still_wrong=147`).
	- Reproduces under reduced tuning (`XDELTA_4200=-6` only): still exactly `+111` on `y=60`.
- Stronger plane attribution from mask sweep logs:
	- The y60 blowup (`count=160`) appears whenever plane-1 offseting is enabled (e.g. masks `2`, `3`, `10`, `15`).
	- Masks without plane-1 offseting keep y60 low (`~18..49`) but worsen total elsewhere, so this is not a complete fix.
	- Current best probe metric is now `4887` with `MASK=11` (planes `0/1/3`) plus `SKIP_Y=60` on full-window settings (`END=63`) under `TOP=17`, `XDELTA_4200=-6`, `XDELTA_3600=10`.
- Beam-line-local refinement confirms the transition anomaly is tightly localized:
	- With `SKIP_Y` disabled (`-1`), `MASK=11`, and full 4200 window (`END=63`), only `SKIP_LINE=43` changes behavior; neighboring lines (`41,42,44,45`) do not.
	- At `SKIP_LINE=43`, removing line-offset from plane-1 on that line (`SKIP_LINE_MASK=2`, or `10`) drops diff to `4857` (best current measured metric) and reduces `y=60` row count to `19`.
	- Removing offset from all masked planes on that line (`SKIP_LINE_MASK=15`) gives `4887` with `y=60=49`.
	- This indicates the high-impact correction is specifically "no 4200 line-offset for plane 1 at transition line index 43."
- Top-border cross-check: the `SKIP_LINE=43` fix is highly effective under the working `TOP=17` branch but not under `TOP=16` (which remains worse overall), so all active narrowing remains anchored to the `TOP=17` mapping branch.
- Semantic candidate added in renderer probe path: `BENEFACTOR_RULE_4200_PLANE1_TRANSITION=1` now applies the discovered rule directly (`mode $4200`, line index `43`: suppress plane-1 line-offset). Under the same base tuning (`TOP=17`, `XDELTA_4200=-6`, `XDELTA_3600=10`, `BPL_LINE_OFFSET_4200=1`, `MASK=11`, no skip-line probes), this reproduces `4857` and `y=60=19`; with the rule off, it returns to `4998` and `y=60=160`.
- Safety re-check after semantic-candidate code addition: default run still unchanged (`5499`, first mismatch `(120,17)`).
- Promotion applied: the `$4200` plane-1 transition rule is now enabled by default in `hw_copper.c` (still env-overridable via `BENEFACTOR_RULE_4200_PLANE1_TRANSITION=0`). Verification:
	- Default harness run remains unchanged at `5499` with first mismatch `(120,17)`.
	- Tuned branch now gets `4857`/`y=60=19` without explicitly setting the rule env var.
	- Explicitly disabling the rule (`...=0`) reverts the same tuned branch to `4998`/`y=60=160`.
- New `$3600` semantic branch probe: extra x-origin adjustment gated by `BPL2MOD<0` (`BENEFACTOR_XDELTA_3600_FFD8`) added to test phase-aware mapping.
	- Signal is real but modest: examples include `BASE=8, EXTRA=6 -> 4857` and `BASE=8, EXTRA=8 -> 4852`.
	- This branch improves over low-global-offset settings (`~4902`) but does not yet beat aggressive global-overfit values from earlier sweeps.
	- Interpretation: `$3600` mapping likely has per-phase structure (not pure constant shift), but this single-gate model is incomplete.
- Safety re-check after adding the `$3600_FFD8` probe: default run still unchanged (`5499`, first mismatch `(120,17)`).
- Expanded `$3600` two-phase probe now available: `BENEFACTOR_XDELTA_3600_ZERO` (applies when `BPL2MOD==0`) plus `BENEFACTOR_XDELTA_3600_FFD8` (applies when `BPL2MOD<0`), with global `XDELTA_3600` settable independently.
- Two-phase sweep outcome (with `XDELTA_3600=0`) still shows monotonic improvement as both phase offsets increase, e.g.:
	- `Z=0,F=0 -> 4902`
	- `Z=12,F=20 -> 4833`
	- `Z=20,F=20 -> 4808`
	This mirrors prior global-overfit behavior and does not reveal a stable semantic optimum.
- Ablation indicates global and phase-specific knobs overlap strongly:
	- `base8,extra0 -> 4869`
	- `base0,extra8 -> 4889`
	- `base8,extra8 -> 4852`
	So current phase gates are not yet capturing a physically distinct rule; they mostly act as additive alignment pressure.
- Safety re-check after the two-phase additions: default run remains unchanged (`5499`, first mismatch `(120,17)`).
- New analysis entrypoint: `run_harness_and_analyze.sh` runs one harness capture and then both root-cause analyzers on the same logs.
- New non-offset `$3600` probes added and evaluated:
	- Pointer-phase (source-row) probes: `BENEFACTOR_BPL_LINE_OFFSET_3600`, `_ZERO`, `_FFD8`.
	- Decode-window probe: `BENEFACTOR_FETCH_WORDS_BIAS_3600`.
- Non-offset probe results are strongly falsifying:
	- Any non-zero `BPL_LINE_OFFSET_3600` (global or per-phase) significantly worsens diff; best is exactly zero (`4902`).
	- `FETCH_WORDS_BIAS_3600` is best at `0` (`4902`); both negative and positive biases are worse.
	- Coarse two-phase grid over `BPL_LINE_OFFSET_3600_ZERO` and `_FFD8` (including negatives) confirms best at `(0,0)`; no hidden mixed-sign optimum found.
	This rules out simple pointer-row and fetch-width mismodels for `$3600` in the current renderer branch.
- Additional timing falsifications under current `$4200` rule branch:
	- `BENEFACTOR_ADVANCE_BEFORE=1` degrades heavily (`~5282+`), so current per-line pointer advance order is not the controlling mismatch.
	- `BENEFACTOR_WAIT_AT_FETCH=1` is neutral-to-worse (`4902 -> 4910`).
	- `BENEFACTOR_DUALPF_DECODE=1` gives only tiny gain (`4902 -> 4893`, nine rows with `-1` each), low impact.
- Additional copper timing probe (`BENEFACTOR_COPPER_CUR_HP`) tested fixed horizontal beam positions for WAIT comparison:
	- Extreme low hpos (`0x00`) degrades badly (`5380`).
	- Broad mid/high range (`0x70..0xFE`) is effectively flat (`~4902`), with only tiny variation (`4910` at low-mid values).
	This suggests the remaining residual is not primarily due to the current single-cur_hp simplification, except for obviously wrong low-hpos choices.
- ROM-side clue captured from generated code: `gfn_sprite_playfield_setup` (`$00377A`, core loop `$00378A..$0037FE`) explicitly emits copper words containing `BPL2MOD` payloads `#$10a0000` and `#$10affd8` while building the late playfield section. This confirms the `$3600` phase pattern is intentional game data generation, reinforcing that the mismatch is in render interpretation/timing rather than missing copper construction.
- New per-line `$3600` trace (`BENEFACTOR_TRACE_3600_LINES`) correlates row diffs with renderer inputs. Under the current non-overfit branch (`TOP=17`, `XDELTA_4200=-6`, `XDELTA_3600=0`, `$4200` rule enabled), the highest-diff `$3600` rows are mixed across both `BPL2MOD=$0000` and `$FFD8`; no single phase dominates the worst rows.
- Corrected phase comparison for `XDELTA_3600=12` versus base `0` shows broad, weakly phase-selective improvement rather than a semantic signature:
	- `BPL2MOD=$0000`: `976 -> 947` (`-29`)
	- `BPL2MOD=$FFD8`: `893 -> 873` (`-20`)
	- Largest row deltas are small (`-5`, `-4`, then mostly `+/-1..2`) and spread across both phases.
	Interpretation: current `$3600` x-offset gains behave like broad alignment pressure, not a clean phase-specific renderer correction.
- New per-pixel `$3600` trace (`BENEFACTOR_TRACE_3600_PIXELS_Y/X0/X1`) confirms the `XDELTA_3600=12` branch is a literal horizontal resampling shift on representative rows, not a decode change:
	- On `y=116` (`BPL2MOD=$0000`), the base run samples nonzero plane bits (`$04/$05`, `cidx=4/5`) around `x≈224..235`; `x12` shifts `sx` left by 12 so those same output pixels sample zeros instead and become correct. The same `$04/$05` island then reappears at later `x≈239..246`, creating new errors.
	- On `y=93` (`BPL2MOD=$FFD8`), the same pattern repeats: `x12` turns some early wrong `$04/$05` pixels into zeros, but moves that nonzero island to later x positions, trading one set of row-local errors for another.
	- This is strong evidence that `$3600` x-origin tuning is purely translational; it does not reveal the missing renderer semantics.
- Direct ROM decode of `gfn_sprite_playfield_setup` (`$00378A`) shows the `$3600` phase table is dense, mostly alternating `BPL2MOD=$0000/$FFD8` line by line; there are no large phase blocks to explain the residual as a coarse row scheduling issue.
- Cheap timing check falsified: rendering `$3600` with the previous line's `BPL2MOD` (`BENEFACTOR_3600_USE_PREV_BPL2MOD=1`) leaves the diff unchanged (`4902 / 81920`, same first mismatch).
- Dual-playfield decode clarifies but does not fix the representative wrong islands: on rows `y=116` and `y=93`, enabling `BENEFACTOR_DUALPF_DECODE=1` simply renumbers the sampled islands from `cidx=4/5` to `pf1=2/3`, while those pixels remain wrong versus PUAE.
- Crucial narrowing fact: the representative wrong islands are plane bits `$04/$05`, i.e. planes `0` and `2` only (`BPL1/BPL3` in Amiga numbering). Those planes advance with `BPL1MOD`, not the alternating `BPL2MOD` schedule emitted by `gfn_sprite_playfield_setup`. The `$3600` `BPL2MOD` table is therefore a region marker, but it does not directly explain the sampled bad pixels.
- Gameplay copper list sanity check: `BPLCON1` is written once to `$0000` at `$8724`; there is no split-nibble horizontal scroll programmed here. The residual is not coming from ignored per-playfield `BPLCON1` scroll.
- Important correction from copper decode: `BPL1MOD` is not constant in gameplay. At `$87C0` the copper switches `BPL1MOD` from `$003C` to `$0028`, and the traced `$3600` `BPL1/BPL3` bases then advance by `+80` bytes per line, matching `bytes_per_line (40) + BPL1MOD (40)` exactly. The current renderer is following that odd-plane modulo change consistently.
- New targeted probe: shifting only the `$3600` `pf1` sampling path (`BENEFACTOR_PF1_SHIFT_3600`) improves the harness materially without touching `pf2`.
	- `PF1_SHIFT=0`: `4902 / 81920`
	- `PF1_SHIFT=-8`: `4869 / 81920`
	- `PF1_SHIFT=-12`: `4853 / 81920`
	- `PF1_SHIFT=-16`: `4834 / 81920`
	This beats the base branch and matches/exceeds earlier global x-shift gains, which strongly suggests the residual lives in the `pf1` fetch alignment rather than the whole renderer.
- Per-pixel comparison shows the `PF1_SHIFT=-16` probe is still translational, but specifically for `pf1`: on representative rows (`y=116`, `y=93`) it turns the early wrong `$04/$05` islands into zeros at the correct pixels, then reintroduces the same kind of islands about 16 pixels later. Interpreted semantically, the remaining hypothesis is no longer "global x-origin is wrong"; it is "`pf1` is sampled with a constant ~2-byte / 16-pixel horizontal misalignment in `$3600` mode."
- Follow-up base-pointer probe shows the effect is tied to the `pf1` fetch base as well, but the sign is opposite of the first guess. Global `BENEFACTOR_PF1_BASE_BYTES_3600` results:
	- `+1`: `4869 / 81920`
	- `+2`: `4858 / 81920`
	- `+3`: `4830 / 81920`
	- negative values (`-1`, `-2`, `-3`) are clearly worse (`5044`, `5233`, `5335`).
- Per-pixel validation for `PF1_BASE_BYTES_3600=3` shows the same structural behavior as the earlier `PF1_SHIFT` probe: it fixes representative early wrong islands on `y=116` and `y=93`, but creates new wrong `$04/$05` islands elsewhere on the same rows. This is still a translational `pf1`-local effect, not yet the missing semantic rule.
- Phase split of the `pf1` base-byte probe indicates both `$3600` phases are involved, with somewhat stronger leverage on `BPL2MOD=$0000` rows:
	- `BENEFACTOR_PF1_BASE_BYTES_3600_FFD8=3`: `4875 / 81920`
	- `BENEFACTOR_PF1_BASE_BYTES_3600_ZERO=3`: `4857 / 81920`
	- global `BENEFACTOR_PF1_BASE_BYTES_3600=3`: `4830 / 81920`
	Interpretation: the live issue is not confined to one `BPL2MOD` phase, though `$0000` rows respond a bit more.
- Coarse row-bucket analysis weakens the section-entry hypothesis: the `PF1_BASE_BYTES_3600=3` gains are spread through the middle of the `$3600` region, not concentrated near the first few `$3600` rows. Biggest improvement buckets are `y=96..127` and `y=128..159`, so a one-time section-start correction is unlikely to be the whole explanation.
- Fetch-start warm-up probe falsified: applying the `pf1` byte offset only to the first few fetched bytes of each `$3600` line (`BENEFACTOR_PF1_WINDOW_BASE_BYTES_3600=3`, byte windows `<=1..5`) is neutral or worse (`4940`, `4947`, `4940`, `4911`, `4912`). The missing semantics are not limited to the first few fetched bytes of each line.
- `pf1` per-line advance probe falsified: adjusting only the odd-plane-perceived `pf1` line step in `$3600` (`BENEFACTOR_PF1_STEP_BIAS_3600=-2..2`) is neutral or worse (`4912`, `5007`, `4902`, `5024`, `4946`). The residual is therefore not explained by a small error in the per-line `BPL1/BPL3` pointer advance.
- UAE reference readback: the bundled UAE renderer treats playfield-local delay as a first-class concept (`delay1`, `delay2` from `firstword_bplcon1`), which is consistent with the current narrowing, but this gameplay copper list still programs `BPLCON1=$0000`, so there is no immediate direct register bit to port from there.
- Plane-specific probe falsified the "single plane" version of the hypothesis. In `$3600`, shifting only plane 0 or only plane 2 by the best current byte-space offset is much worse, while shifting both together reproduces the earlier `pf1` improvement:
	- base: `4902 / 81920`
	- `BENEFACTOR_PLANE0_BASE_BYTES_3600=3`: `5437 / 81920`
	- `BENEFACTOR_PLANE2_BASE_BYTES_3600=3`: `5422 / 81920`
	- both plane 0 and plane 2 at `+3`: `4830 / 81920`
	Conclusion: the residual does not belong to one pf1 constituent plane; it belongs to the shared `pf1` path.
- Useful numeric consistency check: the best current shared-`pf1` byte-space probe is `+3` bytes. In the current 320-pixel output path, that corresponds to roughly the same directional movement as the earlier `XDELTA_3600=12` translational improvement, which reinforces that both probes are exposing the same shared-`pf1` horizontal offset rather than unrelated effects.
- Additional UAE reference fact: `compute_toscr_delay()` in the bundled UAE `custom.c` applies a fetch-start-dependent `delayoffset` on top of `BPLCON1`-derived per-playfield delays. That keeps the live semantic space on playfield-local fetch delay/packing, even though the current Benefactor copper list does not encode a nonzero `BPLCON1` delay directly.
- Stronger UAE readback: with `BPLCON1=0`, the bundled UAE path gives identical shifter delay to both oddeven groups (`delay1=delay2=0`, `toscr_delay_shifter[0]==toscr_delay_shifter[1]`), and the delay consumers (`do_delays_*`) use those equal values symmetrically. So raw `BPLCON1`/`compute_toscr_delay()` shifter delay cannot by itself create the observed `pf1`-only offset in this Benefactor scene.
- That narrows the remaining semantic space further: the missing rule is more likely in how the combined `pf1` path is packed, selected, or emitted after fetch, not in a simple per-playfield delay register interpretation.
- New best probe branch so far: a slight phase asymmetry on the shared `pf1` base-byte offset improves beyond the previous global `+3` branch:
	- previous best: `BENEFACTOR_PF1_BASE_BYTES_3600=3` -> `4830 / 81920`
	- new best: `BENEFACTOR_PF1_BASE_BYTES_3600_ZERO=3` and `BENEFACTOR_PF1_BASE_BYTES_3600_FFD8=4` -> `4804 / 81920`
	- nearby asymmetric checks are weaker (`z3f2=4844`, `z2f3=4844`, `z4f3=4812`).
- Phase totals against base for the new `z3f4` branch show improvement in both phases, with larger gain on `$0000` but nontrivial gain on `$FFD8` too:
	- `$0000`: `976 -> 918` (`-58`)
	- `$FFD8`: `893 -> 853` (`-40`)
	So the branch is not phase-exclusive; it is still shared-`pf1` behavior with modest phase asymmetry.
- Representative row comparison (`y=116`, `y=93`) versus the previous global `+3` branch indicates this new branch still behaves like offset-like trade, but now with different row/phase selectivity (for example `y=116` changes while `y=93` remains unchanged relative to global `+3`). Keep this as a stronger narrowing clue, not as a semantic endpoint.
- Safety re-check after all non-offset sweeps: default run still unchanged (`5499`, first mismatch `(120,17)`).
- Safety re-check after the latest probes: default run still unchanged (`5499`, first mismatch `(120,17)`).

- New control-snapshot diagnostics (added to `FrameState` + harness compare) show no direct `BPLCON1/2`, `DIW*`, or `DDF*` mismatches at the failing frame; remaining divergence is in renderer interpretation, not obvious control-register value mismatch.
- Bitplane snapshot timing probe (`BENEFACTOR_BPL_SNAP_OFFSET`) is high-sensitivity: offsets `+1/+2` materially move the metric (e.g. baseline-only `5499 -> 5462/5305`; with mode offsets best seen around `5046`), reinforcing that line-start timing/modeling is a key unresolved component.

## Next Tasks

1. **Fix timer/animation state divergence** — `$0067D3`, `$0069F1`–`$006A6F` drift across frames causes frame 3+ copper list content divergence (DDF and BPL pointer instructions differ). Trace why PC's timer counter decrements at a different rate than PUAE's. Likely in `native_timer_interrupt` / `gfn_0055A0` path.
2. Once frame 3 matches: run with `--frames 10` to see if divergence reappears later.
3. Retire stale probe env vars from before the star-field fix (old `$3600` x-offset sweeps etc. are no longer relevant to current divergence).

---

## Debugging Hints

- **Never compare palette hardware registers across PUAE and PC** — they're read at different beam positions. Always compare coplist chip RAM words.
- **$7BC8 copper list has split-screen palette trick**: COLOR01=$0110 before line 90, $0248 after. bplpt and palette snapshot diffs are ALL from this. They're NOT bugs.
- Current diagnostics in `hw_blitter.c` / `harness_compare.c` dump `[BPL_BLT_ROW]` and `[bpl-trace]` data for the live bitplane ranges. Use those before adding new probes.
- **`w32` across copper instruction boundary corrupts adjacent reg-word** — always use `w16` for copper val-words.
- **`native_rebuild_copper_static` hardcodes values** — any value it writes must match PUAE's chip RAM exactly. Mismatches are immediate coplist DIFFs.
- **`fprintf(stderr,...)` is suppressed inside PUAE vendor code** — use `write(2, buf, n)` for debug prints in PUAE context.
