# Remaining Issues

Living list of open issues. Keep it honest: update status, record dead ends, delete
when truly fixed+verified. Details for the widescreen items live in
[[widescreen-plan]]; this is the index + current status. Verify any "fixed" claim on
real data (REPL `wscap`/`fbw`, screenshots) before crossing it off.

Last updated: 2026-06-09.

---

## A. Widescreen native renderer — gameplay sprites/objects

The native wide renderer (`native_render_wide_bg`) composes the playfield from the
tilemap + per-routine sprite captures, ignoring the engine page. Anything drawn only
into the page by a path we DON'T capture is invisible. Full routine map +
verified specs in [[widescreen-plan]] "Phase 4 — COMPLETE sprite-routine MAP".

1. **Marry Men (rescued creatures).** **MACHINE FIXED 2026-06-09 (carried still TODO).** The
   FREED marry man's whole animation machine — idle/excited/turn/walk/ladder-climb (and, by
   construction, gap-jump/teleport) — now renders. ROOT CAUSE was a band-aid **handler-range
   gate** (`a1 in $57C112..$57C2D0`) on the build-entry capture: pose handlers live in TWO
   discontiguous regions (`$57BA74..$57C61E` AND `$584Exx`), so the guessed range dropped
   every pose outside it → turning (`$584E8C`), climbing (`$584EB6`/`$584EC2`/`$584EBA`),
   gap-jump, teleport went invisible. FIX = **delete the gate**: `$57B19E` (red) / `$57B856`
   (blind) are marry-man-only BY CONSTRUCTION (all 49 `jmp -$3c74(a5)` sites + the `$584Exx`
   pose handlers funnel through them; nothing else does), so hooking them IS the identification
   — no filter. VERIFIED (harness L9, logs/savestate.bin): re-derived gfx == the engine's own
   BlitRec for walk frame 5 (`$00F0BC`) AND climb frame 54 (`$0101BA`); capture count stays
   clean (1), `wsstatic drawn=1` through turn→walk→climb. Use the `blits` + `wsmm` (re-derived)
   REPL cmds to re-validate any pose.

   **STILL TODO — CARRIED / held-by-Ben** (separate path, `wsmm`=0): the held MM is a distinct
   cookie-cut blit (`src=$0129CE mask=$013A42`, h=8, `$01xxxx` creature pool — `blits` b22..b26
   while held) drawn alongside the player, captured by NONE of wsmm/wschar/wsobj/wsplayer. Needs
   its draw routine found (`blitskip`/`g_rt_last_call` at that blit) and hooked like
   `native_char_capture`. It renders correctly in the engine's own 352 view; only the wide
   margins lose it.

   **ENTITY TAXONOMY (verified live, harness L9 + logs/savestate.bin, 2026-06-09 — do NOT
   re-confuse these):**
   - **The L9 `wschar` chr0 ($05C922 @x105) / chr1 ($05E37E @x64) are UNRELATED stationary
     background characters, and `wsobj` is level objects — NEITHER is the marry man.** Stop
     tracking the MM by them.
   - **MM drop = FIRE alone** (Down+Fire = *item* drop). Clears `$10AC` bit `$4000`.
   - Harness PC-only drive: `BENEFACTOR_SKIP_PUAE=1`; `--level 9` lands on the card — hold
     fire ~250 frames to reach the cavern; or `load`+`pc 1` on logs/savestate.bin.

   The caged Marry Man = the short red-shirt figure inside the **cage** (grey
   two-pillar cell with a cross on the right pillar), NOT the teleporter (the green figure in
   the bottom-left grey structure is the PLAYER — do not chase him).

   **CORRECTED RE (the prior notes here were WRONG — marked falsified below).** He is NOT a
   character drawn by a "non-$57D3F4 builder", and the fix is NOT a page-blit reverse-
   projection. He is a **static-placement OBJECT**:
   - Placement records live at **`$5A4562`** (6 words/record, clean WORLD coords:
     `+2 worldX.w`, `+4 worldY.w`; `+0` type). On L1 the marry man is record [0]:
     **worldX=66, worldY=85**.
   - The per-frame object compositor **`$57B0B4`** (per-record loop re-entry **`$57B0EE`** →
     common build `$57B19E`; a heavyweight tilemap-collision/dirty-rect routine) reads each
     record and builds a cookie-cut blit descriptor into the **object-only queue `$5A39EC`**
     (24-byte descriptors: `+0 data(B,5pl)`, `+4 mask(A,1pl)`, `+8 con0/con1`, `+C BLTAMOD/
     BLTDMOD`, `+10 dst`, `+14 BLTALWM.w`, `+16 BLTSIZE.w`=0 terminates).
   - Executor **`$57D6C4`** (via `$57D56C`, which also plays queue `$5A371C` through `$57D5AA`)
     plays the queue. The marry man's descriptor on L1: data=`$010052`, mask=`$0131F6`,
     con0=`$AFCA` (cookie-cut, ASH=10), BMOD=-2, dst=`$2C339` (row 85), BLTSIZE=`$0242`
     → w=2,h=9, displayed 16×9 (BLTALWM=0 kills the spillover word). His gfx is in the
     `$01xxxx` pool (a shared low-mem creature gfx), distinct from per-frame chars (`$05xxxx`,
     captured by `$57D3F4`). That is why `$57D3F4` never sees him.
   - The **FREED/walking** marry man (cage opened) becomes a NORMAL `$05xxxx` character in the
     `$10e6(a5)` char list, drawn per-frame via `$57D3F4` → ALREADY captured & rendered.

   **ARCHITECTURE — build-entry capture (single source), `native_wsstatic_compose` +
   pc_overrides_gameplay.c.** The earlier "in-view from queue `$5A39EC` / off-view from placement
   record `$5A4562`, paired by worldY" DUAL-source design is RETIRED. The override now hooks the
   compositor's two build entries — `$57B19E` (red, `native_build_red`) and `$57B856` (blind,
   `native_build_blind`), both reached per-record from the handler `$57C13A` — and snapshots
   `{worldX=d1, worldY=d2, frame=d3, flags=d4, variant}` for EVERY Marry Man each frame, in view
   AND culled off-view (the engine resolves the gfx for all records before the draw cull; verified
   the build is reached for off-view ones). `native_wsstatic_compose` then re-blits each captured
   record at its true world X across the wide view: `frame2 = frame + ($55 if !d4.bit1)` →
   `gfx=$4a72(a5)+frame2*8` → `{data_off+$EEFA(+$4C38 blind), mask_off+$12E7E(+$4C38 blind), yoff,
   BLTSIZE}`, cookie-cut, BMOD=-2, left=worldX-8, top=clamp(worldY,$D7)+yoff. Because d3 is captured
   AT THE BUILD ENTRY (after `$57C13A`'s per-pose sub-handler has overwritten it), the captured
   frame already carries the full animation — there is NO table replay here. No X-dedup, no learned
   offset, no persistence cache, no record-walking. Runs only for `ow>352` (default 352 untouched).

   **RESOLVED 2026-06-09 — blind variant gfx = red + $4C38 (a CONSTANT). The earlier "per-level"
   claim was WRONG (falsified).** Variants selected by record **type bit7** (initial handler ptr)
   and, at draw time, by `tst.b d0; bmi $57b856` (d0<0 → blind): painted/RED (`$57BA74`→build
   `$57B19E`/emit `$57B4F6`) and blind/GRAY (`$57BBF8`→build `$57B856`). VERIFIED: `$57B856`'s emit
   is byte-identical to red's — same `$4a72(a5)` descriptor, same `(frame[+$55 if !d4.bit1])*8`
   index — except it adds `$13B32`/`$17AB6` (data/mask) where red adds `$EEFA`/`$12E7E`. Both
   deltas == **`$4C38`** (immediates `game_gpl_0.c:53632/53638` vs `:27974/27980`). So
   `blind_data = red_data + $4C38`, `blind_mask = red_mask + $4C38` — a hardcoded constant, fine to
   use. The "+$6AB0 on L11" mismeasurement was a SEPARATE mechanism: `$57B856` special-cases
   resolved frame `$3a` (gated `$10aa(a5)>=$2c` + `$57FEB8`/`$10ad(a5)` parity) to a fixed gfx page
   `#$585138` (`game_gpl_0.c:52981`). Full model in [[gameplay-engine-map]] "$57B0B4 internals".
   **OFF-VIEW gaps — ALL RESOLVED & VERIFIED 2026-06-09 (harness L9, REPL `wsmm`).** The
   build-entry capture gives the engine's own values, so facing/animation/variant come for free:
   - **VARIANT** = RESOLVED (blind = red + `$4C38`, above).
   - **FACING** = RESOLVED. `d4` bit1 is captured and applied (`frame2 += $55` when clear). The
     off-view blind Marry Man on L9 carried `d4=$0042` (bit1 set ⇒ no `+$55`), the engine's own value.
   - **ANIMATION** = RESOLVED. The old "idle-only" symptom was the SUPERSEDED record-walking
     resolver replaying only `$57C13A`'s top table. The current code captures `d3` AT THE BUILD
     ENTRY, after the per-pose sub-handler (`$526a`/`$545a(a5)` → `$57C194`/…) has set it — so the
     captured frame is the full per-pose frame, nothing to replay. VERIFIED: L9 had two Marry Men,
     red @worldX 322 and BLIND @worldX 1474 (far off the ~320px narrow view); both captured, both
     animating (frame cycled 50→53), both with engine facing+variant. (Untested edge: a pose whose
     sub-handler never reaches the build — it'd be absent both on- and off-screen, matching vanilla.)
   So the off-view Marry Man is DONE — no large per-level port, no facing/anim replay needed. The
   `$57B19E`/`$57B2B8` terrain-collision pass is NOT needed for DRAW (it feeds the player-overlap
   hardware sprite + collision, not the blitter gfx). Every value derives from engine state; the
   `$4C38` is a VERIFIED instruction-stream constant, not a learned delta.

   **FALSIFIED prior claims (do NOT re-chase):**
   - ~~"drawn by a non-$57D3F4 BUILDER feeding $57D6C4 ($57D5AA/$57D6F2)"~~ — he's an OBJECT
     built by `$57B0B4` into queue `$5A39EC`; `$57D5AA`/`$57D6C4` are the queue EXECUTORS.
   - ~~`native_wsmissedchar_compose` page-blit reverse-projection~~, ~~persistence cache~~,
     ~~`$57B0EE` builder hook~~, ~~learned variant offset~~ — ALL removed; all were hacks that
     produced ghosting / frozen anim / wrap phantoms / DUPLICATION. See [[feedback_no_hacks_re_first]].
   The real DECORATIONS (torches/teleporter/enemies, `$06xxxx`) go through the object WALKER →
   `$57D8D0` path (cull widened + anim captured in #4) — already handled, not part of this list.

2. **GET READY: all objects/characters missing.** **NOT REPRODUCED 2026-06-09 — likely
   FIXED; needs user confirmation in normal play.** The player being absent during the
   banner is CORRECT — it teleports in after the banner (do NOT chase it). The reported bug
   was that everything ELSE (objects, characters, decorations) was also missing, because the
   per-frame object loop `$57D79A` supposedly didn't run during GET READY (`wscap` →
   `wsobj=wschar=0` the whole time). **This symptom no longer occurs in the harness.**
   Verified (goto L1, `BENEFACTOR_WIDESCREEN=680`, per-frame `wscap`): from the first
   gameplay frame the walker HAS run (`objwalk=1`) and objects are captured+drawn
   (`wsobj=11 wschar=4`), and a frame dump during the banner shows "BENEFACTOR / GET READY!"
   WITH the terrain, objects and characters fully rendered behind it
   (`scratch/screenshots/b2.png`). So the "walker never runs / queue empty during GET READY"
   condition is gone — presumably resolved by the object-capture + banner work in the
   2026-06-05..09 push series. NEXT: have the user confirm it's fixed in normal play
   (full intro→PLAY GAME start, not `goto`); if it still shows there, the gap is a
   goto-vs-normal-start difference in when the setup object-build runs, not a missing
   render path. (Earlier hypothesis — a SETUP queue-build bypassing `$57D8D0`/`$57D3F4`,
   retain-last-capture tried+reverted — kept here for history.)

3. **GET READY / GAME OVER banners.** FIXED (2026-06-05). Box + teleport animation + text
   composite as a centered top UI overlay (`native_wsbanner_overlay`), drawn while the
   banner is active (lifetime via the objwalk latch). Art spec: cookie-cut, DATA=`$A49A`
   MASK=`$BDCC`, w16 h43 bmod=-2 afwm=FFFF alwm=0000, plane stride `$50A`; routine
   `$578974`, variants `578860/57889C/5788DE/57892E`. KEY FIX: the box is blitted with a
   **10px blitter A-shift** (BLTCON0 `$AFCA` → ASH nibble `$A`=10); the anim (`$09F0`,
   ASH 0) and text (`$57892E`, a CPU byte-writer, no shift) are NOT. We draw the box at
   its calibrated visual left, so the children (placed by their page-offset delta from the
   box dest) must subtract the box A-shift — else they land 10px right (the figure sat low
   in the circle, needing an ugly rectangular "hole fill" bandaid to hide the gap). With
   the shift applied: anim fills the circle hole itself (no fill, no circle-bg blit),
   verified dx=0/dy=0 cross-correlation + 99.87% bannercmp vs vanilla. The A-shift is read
   from the box con0 immediate (`$5789A6`), not hardcoded. REPL: `bpos` prints the
   captured offsets + derived placement.

4. **`$06xxxx` objects culled ~3 tiles past the window (torches, teleporter, enemies).**
   **FIXED — list-A path (2026-06-05).** The cull is a per-object camera-window test in the
   object-list WALKER. Two override-widened windows (margin 0 at default 352 → vanilla;
   `(out_w-320)/2` per side in widescreen):
   - **`$57D7BC` → `native_objstep`** (pc_overrides_gameplay.c): re-implements one walker
     iteration ($57D7BC..$57D812), widening the MAIN cull ($30/$170). `native_objwalk`
     ($57D79A) now ports the setup then `rt_jump($57D7BC)` so the override catches obj 1.
   - **`$57D8B4` → `native_objstep_b`**: the walker routes objects with a non-zero anim
     nibble (`and.w -$c(a1),d2; bne $57D8B4`) to a SECOND, separate window ($30/**$1b0**).
     The $06xxxx ANIMATED objects (torches/teleporter/enemies) go through HERE — this is
     why widening only $57D804 wasn't enough; static $05xxxx decorations (anim=0 → $57D7BC
     path) already persisted. Widen $1b0 the same way. (The RE below missed this second
     cull; found by tracing the torch's dispatch path — it never reached the $57D804 cull.)
   VERIFIED (L9, 960px): torch (worldX 912: $060xxx tall + $067xxx base + $05BCB4 deco) now
   stays in `wsobjs` at cam 961 (the exact frame it dropped before) out to cam 1043
   (screenX −131); 26 objs vs 22 pre-fix. Also verified at 640px. REGRESSION: at default 352
   the `wsobjs` capture is byte-IDENTICAL to the pre-fix build (diffed). Wide screenshot:
   `scratch/screenshots/ws_torch_fixed_cam961.png`. Mechanism: $57D7BC/$57D8B4 overrides
   leave the Amiga stack exactly as the recompiled fall-through (one a0 pushed → DISPATCH
   $57D816/$57D8CA which push a2-a4; SKIP $57D8A8/$57D8F2 pop a0). The recompiled handlers,
   $57D816/$57D8CA dispatch and $57D8D0 draw are UNCHANGED.
   STILL OPEN: the MULTI-TILE path ($57D81C twin cull `cmpi.w #$150` at $57D826) — that's
   issue #1 (Marry Men), a separate change; not needed for the list-A torch/teleporter/enemy
   fix (none of the L9 torch objects take the multi-tile branch — verified).

   --- original RE (kept for reference) ---
   ROOT CAUSE NAILED to a single camera pixel (2026-06-05, L9, BENEFACTOR_WIDESCREEN=960).
   Held right, dumped every frame f70..f140 + its `wsobjs` list; user pinpointed the torch
   present at f92, gone at f93 (cam 960→961, ONE pixel). Diff of the two object lists: the
   torch = two list-A objects at **worldX 912** — `src=$060FA2` (16×32, the tall rock+stick+
   flame) and `src=$067DC6` (16×4) — both drop the instant screenX goes −48 → −49. A third
   object at the SAME worldX 912, `src=$05BCB4` (16×9), SURVIVES; so does obj1 `src=$0590D4`
   at worldX 336 / screenX −625. So the cull is **per-object-TYPE, not a screen clip**:
   `$06xxxx` (real object gfx) handlers cull when `screenX>>4 < -3` (object's left tile ≥4
   cols left of the camera) BEFORE reaching `$57D8D0`; `$05xxxx` (decoration gfx) objects
   are NOT culled and persist far off-screen (we already capture them at `$57D8D0` entry,
   pre-clip, so they show across the margin). That's why list-A "mostly works" in the margin
   but enemies/torches pop out ~3 tiles in.
   This is the SAME family as #1 (Marry Men) — `$06xxxx` sprites gated by a per-object cull
   upstream of the draw choke. FIX OPTIONS: (a) find & WIDEN the per-handler cull constant
   (`screenX>>4 < -3`) so the engine draws further into the margin and we capture it at
   `$57D8D0`/`$57D3F4` (clean if the cull is shared/few sites); (b) read object worldX/Y +
   gfx from the object struct at the walker `$57D79A` per-object point, bypassing handler
   culls entirely. RULED OUT (despawn): the active object-pointer list `$1162(a5)`=`$57FF74`
   is byte-identical at f92 vs f93 — the object STAYS in the list, the walker still iterates
   it; only its handler skips the `$57D8D0` draw when `screenX>>4 < -3`. So the cull is in
   the `$06xxxx` object HANDLER (dispatched `jmp (a1,d2)`), upstream of `$57D8D0`, before its
   draw jump.
   **CULL LOCATED (2026-06-05) — it's a SHARED window test in the walker `$57D79A`, not
   per-handler.** At `$57D804`: `move.w (a0),d1` (object worldX) → `addi.w #$30,d1` (+48) →
   `sub.w $57FDBA,d1` (−camera) → `cmpi.w #$170,d1` (vs 368) → `bhi $57D8A8` (UNSIGNED > 368 →
   skip, don't `jmp (a1)` the handler). So an object dispatches only when `screenX ∈
   [-$30, $170-$30]` = `[-48, +320]`. Verified to the pixel: torch worldX 912 — f92 cam 960 →
   912+48−960=0 ≤368 drawn; f93 cam 961 → −1 = 65535 >368 skipped. The `$57D81C` multi-tile
   path has the twin `cmpi.w #$150` at `$57D826`. ($05xxxx decorations come through a
   different path with no such test, which is why they persist.)
   **FIX (override, NOT recompiler):** widen `$30` (left margin) and `$170` (window width)
   so the engine dispatches objects across the wide view; they reach `$57D8D0`, where we
   capture at ENTRY, while `$57D8D0`'s own internal clip writes skip-descriptors for off-page
   parts → the engine's page blit can't overflow. e.g. for a 960px view (320 margin/side):
   `$30`→`$140` (320), `$170`→`$3C0` (960). The walker is recompiled (constants are C
   literals) — apply via the EXISTING `native_objwalk` override of `$57D79A`: re-implement
   the per-object visibility/dispatch loop natively with the wide window, still dispatching
   the recompiled per-object handlers (`jmp (a1)` → `rt_jump`). This is a BEHAVIOR change for
   a PC-native feature → an override, NOT a recompiler edit (do NOT emit different immediates
   from the recompiler — that mistranslates the ROM and breaks the oracle diff; see
   [[create-override]] "the boundary"). Do NOT multi-pass the walker (it has per-frame side
   effects: queue build, anim/timer advance `$78e0(a5)`) — re-implement it ONCE.
   Tools added: REPL `wsobjs` (obj/char lists + screenX + data src),
   `view_left` in `wsobjs`, `BENEFACTOR_WIDESCREEN` up to 960 (`HW_OUT_MAX`). NOTE: needs
   ≥640px to observe — at 480 the sprite leaves the true wide edge before the cull triggers.

5. **Vanilla (non-widescreen / 352) playfield EDGES show the tile-column render toggles.**
   OPEN. On real HW the display window (DIWSTRT/DIWSTOP) hides the partially-drawn edge
   columns the engine over-fetches for smooth scroll (columns popping visible/invisible
   at the left/right edge). Our native renderer extends the view too far at the edges and
   shows those column toggles → the playfield edge looks wrong in the default 352 path
   (and the `wsdiff` margin==0 compare path). This is the VANILLA rendering, NOT the wide
   camera (which is correct). FIX: clip the 352 playfield decode to the proper DIW. NOTE:
   a DIW clip was implemented then REVERTED earlier because it pulled the WIDE view back
   to ~320 (defeats widescreen) — so apply it ONLY to the margin==0 / 352 path, never to
   the wide (margin>0) path. See [[widescreen-plan]] "DIW over-fetch clip" history.

6. **Line-blitter graphics (chandelier ropes) — PARTLY DONE (2026-06-09).** The ropes now
   render natively: `native_wsrope_compose` (native_renderer.c) reads the engine's own
   per-frame line-segment list at **$5ABB5E** ($5ABB5E.w = count-1, $FFFF=empty; then
   {x0,y0,x1,y1} s16 WORLD coords per segment, confirmed live via `pcwatch`) and draws each
   as a Bresenham line into s_objlayer (world X / screen Y = pf_top+worldY), so the main
   composite loop maps + clips them like every sprite. Colour = the rope's main brown
   (palette index 19 / $9C5521). Verified: ropes draw on/off (WS_NOROPE) over the user's
   chandelier savestate. **REMAINING:** (a) the engine's emitter ($57DCD4) clips each segment
   to the vanilla window [cam,cam+0x170] and culls chandeliers fully outside it → ropes are
   MISSING for chandeliers visible only in the wide margins. A capture of the pre-clip
   endpoints at $57DCD4 was tried but $57DCD4 is **double-emitted** (standalone gfn + inlined
   into gfn_gpl_57DCC4) so the entry override fires unreliably (same class as the removed
   $57B0EE hook) — needs a recompiler de-dup or a reimplemented emitter. (b) the blitter
   draws a textured 2-tone rope (palette 17/19/20/22); we render a solid 1px line.
   Below: the original RE of the blitter draw path, for the eventual full port.

   The engine draws chain/rope graphics with
   the blitter in LINE mode; `hw_do_blit` renders them ([[project_blitter_line_fill]]) into
   the engine PAGE, which the native wide renderer ignores → invisible in widescreen.
   **DRAW SUBSYSTEM RE'd:** routine `$57DD42` (gpl bank): `a0=$5ABB5E` = a per-frame line-
   SEGMENT list = a `$544e`-marked word, then a `count` word, then `count × {x0,y0,x1,y1}`
   in **WORLD coords** (s16); `a1=$5A1D18` row→page table, `d6=$67e(a5)` page base. Loop
   `$57DD92` per segment: sort endpoints, dx=`$2A0C`-strided per plane, set BLTCON1 LINE mode
   (`$42(a6)`), BLTADAT=`$FFFF8000`, AMOD/BMOD=`$2e`; executor tail `$57DE3C` issues the
   octant line blit per plane (the `ori.l #$f00000,d0` on a later plane sets the colour bits).
   `$5ABB5E` is EMPTY (count=0, `FFFF` marker) until the chandelier is active (verified on L31
   start) → it's a dynamic per-frame buffer, NOT static level data. **PORT (when worth it):**
   read `$5ABB5E` live each frame (marker/count/segments), draw each `{x0,y0,x1,y1}` natively
   (Bresenham) at world coords across the wide view — clean endpoints, NO page-wrap. OPEN: the
   exact line COLOUR (which plane(s) the line writes; the per-plane `$57DE3C` loop). Same
   "draw from a clean per-frame list" shape as the Marry Men (`native_wsmm_compose`).

### Widescreen — DONE / NOT-A-BUG (verified this push series)
- **Pause menu / overlays Z-order** — FIXED (2026-06-05): the pause/level-select/toast
  overlays were drawn into `s_fb` before the wide compose, so `native_render_wide_bg`
  overpainted them. Now drawn into `s_out` AFTER compose, width-aware (`pc_overlay_set_dims`)
  so they center/dim the full wide view.
- Native tilemap background (per-level row stride), wide camera clamp/center.
- **Wide camera is GREAT** (user-confirmed) — NOT an issue. The `wsdiff` edge-band
  exclusion (`WSDIFF_EDGE`=32px/side) exists because the COMPARE path (native@352 vs
  vanilla) diverges near edges due to a VANILLA engine camera quirk, not a native bug.
  Don't "fix" the native edge camera to match vanilla there.
- Player draw (`$57A666`), damage-blink black silhouette.
- Cookie-cut characters/walkers (`$57D3F4`, no doubling).
- `$57D8D0` list-A objects (platforms, pickups, ladders, box).

---

## B. Other known native-port issues (pre-existing, see instructions/current-state.md)

5. **Native hardware-SPRITE rendering unimplemented (no CONFIRMED reachable use yet).**
   `native_render_frame` has zero hardware-sprite (SPRxPT) compositing code — distinct
   from the blitter object draws above. The previously-cited example (gameover CONTINUE
   cursor = hardware sprite 0) is MOOT: the PC port intentionally bypasses the GAME OVER
   SCREEN (vanilla = banner → game-over screen → CONTINUE → level card; the port drops
   the screen and goes banner → level card directly, see [[project_gameover_transition]]),
   so that cursor never appears. It's unknown whether any other currently-reachable screen
   uses hardware sprites (gameplay copper has SPRxPT all 0). LOW priority / unverified
   until a real hardware-sprite use is observed; audit the copper SPRxPT across screens
   before building this.

6. **Vestigial password field beside LEVEL SELECT** ("3MQLGPQLGP", renderer `$003DAA`,
   double-emitted). Cosmetic; needs the bitmap-draw routine or a recompiler-level fix.

7. **Audio: PC full-mix ~2× quieter than PUAE** (master/mix gain, affects music+SFX
   equally). Separate from the (fixed) SFX-drop bug. Music replayer (`$59BA7A`+) still
   recompiled, not yet native-owned.

8. **Final-level win path crashes** — different code path; NOT a recompiler/dispatch
   miss. Don't force-win the final level as a coverage test. ([[project_final_level_win_path]])
