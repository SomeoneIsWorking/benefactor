# Remaining Issues

Living list of open issues. Keep it honest: update status, record dead ends, delete
when truly fixed+verified. Details for the widescreen items live in
[[widescreen-plan]]; this is the index + current status. Verify any "fixed" claim on
real data (REPL `wscap`/`fbw`, screenshots) before crossing it off.

Last updated: 2026-06-04.

---

## A. Widescreen native renderer — gameplay sprites/objects

The native wide renderer (`native_render_wide_bg`) composes the playfield from the
tilemap + per-routine sprite captures, ignoring the engine page. Anything drawn only
into the page by a path we DON'T capture is invisible. Full routine map +
verified specs in [[widescreen-plan]] "Phase 4 — COMPLETE sprite-routine MAP".

1. **Marry Men (rescued creatures) invisible.** **MOSTLY FIXED (2026-06-05).** Render, don't
   duplicate, correct colour IN VIEW; remaining: off-view "blind" variant (see OPEN below).
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

   **ARCHITECTURE (`native_wsstatic_compose`, native_renderer.c).** The `$5A4562` placement
   list is Marry-Men-ONLY (records: +0 type[0=end], +2 worldX, +4 worldY, +$a LIVE anim cursor,
   +$c cached handler ptr ==`$57C13A`; stride 64). Each Marry Man is drawn EXACTLY ONCE, paired
   to the engine's per-frame queue `$5A39EC` by **worldY** (the page row from dst — it never
   scrolls/wraps, so the match is EXACT; pairing by X was the bug that caused scroll-dependent
   DUPLICATION, since dst→X is only correct mod the 368px page):
   - **in view** (a queue descriptor matches its row) → drawn from that descriptor = the engine's
     EXACT resolved gfx (correct frame AND variant — see the variant note below). Source of truth.
   - **off view** (no descriptor; engine culls at `$57b4dc` `[cam,cam+$160]`, and the 368px page
     can't hold the wide margin) → drawn from the placement record, natively resolving the LIVE
     animation frame: `frame=MR16($5d5a(a5)+cursor)` → `gfx=$4a72(a5)+frame*8` →
     `{data_off+$EEFA, mask_off+$12E7E, yoff, BLTSIZE}`, cookie-cut, BMOD=-2, left=worldX-8,
     top=clamp(worldY,$D7)+yoff. Verified numerically (L1 cursor $46→frame 50→data=$010052).
   An unmatched queue descriptor (far-side page wrap) is DROPPED, never drawn (no phantom). No
   X-dedup, no learned offset, no persistence cache. Runs only for `ow>352` (default 352 untouched).

   **OPEN — off-view "blind" (gray) variant (RE not finished; do NOT hack).** The Marry Man has
   two variants, selected by record **type bit7**: painted/RED (clear → sub-handler `$57BA74` →
   build `$57B19E` → `$4a72` gfx) and blind/GRAY (set, e.g. type=$82 → sub-handler `$57BBF8` →
   build **`$57B856`**, a TERRAIN-gfx path using `$5a5d9c`/`$55ba(a5)`/`$5042(a5)`/`$5a211c`).
   DECODED (user L9 savestate): the blind sprite is the SAME SHAPE as red, recoloured into the
   gray palette band (colour indices ~$18–$1D vs red's ~$11–$18), and its gfx is exactly
   **red + $4C38** (BOTH data $010052→$014C8A and mask $0131F6→$017E2E) — i.e. a parallel gray
   sprite sheet $4C38 after the red one. The native off-view resolver only does the RED path, so
   an off-view BLIND Marry Man renders RED; IN VIEW it is correct (the queue carries the engine's
   exact gray gfx). To FINISH PROPERLY: RE `$57B856`'s gfx resolution so the blind data/mask are
   derived from the record off-view (replay of its index math does NOT yet reproduce $014C8A, so
   it isn't understood). Do NOT hardcode the $4C38 offset (magic constant) or re-learn the delta
   from the queue (both are the hacks already rejected — see [[feedback_no_hacks_re_first]]).

   **FALSIFIED prior claims (do NOT re-chase):**
   - ~~"drawn by a non-$57D3F4 BUILDER feeding $57D6C4 ($57D5AA/$57D6F2)"~~ — he's an OBJECT
     built by `$57B0B4` into queue `$5A39EC`; `$57D5AA`/`$57D6C4` are the queue EXECUTORS.
   - ~~`native_wsmissedchar_compose` page-blit reverse-projection~~, ~~persistence cache~~,
     ~~`$57B0EE` builder hook~~, ~~learned variant offset~~ — ALL removed; all were hacks that
     produced ghosting / frozen anim / wrap phantoms / DUPLICATION. See [[feedback_no_hacks_re_first]].
   The real DECORATIONS (torches/teleporter/enemies, `$06xxxx`) go through the object WALKER →
   `$57D8D0` path (cull widened + anim captured in #4) — already handled, not part of this list.

2. **GET READY: all objects/characters missing (everything EXCEPT the player should be
   VISIBLE).** OPEN. The player being absent during the banner is CORRECT — it teleports
   in after the banner (user-confirmed; do NOT chase it, even though `wscap` may report
   the player captured — it still doesn't render during GET READY, which is right). The
   bug is that everything ELSE (objects, characters, decorations) is also missing.
   During GET READY the per-frame object loop `$57D79A` does NOT run, and it doesn't build
   the object queue until AFTER GET READY (`wscap` → `wsobj=wschar=0` the whole time).
   Vanilla shows the objects because the executors re-blit a queue built ONCE by a SETUP
   path that bypasses the `$57D8D0`/`$57D3F4` choke points. Retain-last-capture did NOT
   help (no non-empty capture exists before GET READY ends — tried + reverted). NEXT:
   find/hook the setup queue-build path (likely shared with issue #1).

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

6. **Line-blitter graphics (chandelier chains) not rendered natively.** OPEN (RE'd
   2026-06-05; port-ready, but unverifiable in-harness — needs the chandelier chamber,
   which the player can't be reliably driven to). The engine draws chain/rope graphics with
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
