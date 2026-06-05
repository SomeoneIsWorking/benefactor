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

1. **Marry Men (rescued creatures) invisible.** **FIXED (2026-06-05, proper builder/queue
   capture).** The caged Marry Man = the short red-shirt figure inside the **cage** (grey
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

   **FIX (`native_wsstatic_compose` in native_renderer.c + `native_staticobj_capture`
   `$57B0EE` override).** Walk the object-only queue `$5A39EC`, decode each cookie-cut
   descriptor (mask 1-plane gates, data 5-plane plane-stride `h*rs`, transparent colour-0),
   place at the dst-projected world position. The builder override captures each record's TRUE
   worldX/worldY (clean, pre-cull) so the renderer can pair by worldY and snap a genuine 368px
   page-wrap to the true position. Double-buffer skew handled by projecting against the
   descriptor's OWN page base + the displayed coarse-scroll offset (the queue is built into the
   BACK buffer, so dst is usually in the other page than bp0). Runs only for wide output
   (`ow>352`) → default 352 byte-untouched. VERIFIED (L1, 960px,
   `scratch/screenshots/mm_crop960.png`): marry man renders cleanly in the cage, NO red
   outline, NO ghosting; one figure only. L9: queue empty there → pass inert (drawn=0), no
   doubling. REPL: `wsstatic` (= old `wsmc`) reports records-captured / queue scanned / drawn.

   **FALSIFIED prior claims (do NOT re-chase):**
   - ~~"drawn by a non-$57D3F4 BUILDER feeding $57D6C4 ($57D5AA/$57D6F2)"~~ — he's an OBJECT
     built by `$57B0B4` into queue `$5A39EC`; `$57D5AA`/`$57D6C4` are the queue EXECUTORS.
   - ~~`native_wsmissedchar_compose` page-blit reverse-projection (`hw_blit_capture`)~~ —
     DELETED. It re-drew EVERY page blit → ghosting (needed gfx-identity dedup), reverse-
     projected the 368px circular page → opposite-side wrap phantoms, and painted colour-0
     opaque → the blinking red outline. This was the rejected `s_pg` architecture re-added;
     the symptoms the user reported ("walker/key ghost", "duplicate on the other side", "red
     outline") were ALL artifacts of it. Replaced by the builder/queue capture above.

   **Margin cull — FIXED PROPERLY via live record resolution (2026-06-05).** The engine only
   builds a `$5A39EC` descriptor while the object is within `[cam, cam+$160]` (the cull at
   `$57b4dc`), and the 368px page can't hold a 960px view, so a caged Marry Man scrolled into
   the wide margin (or one never yet approached) had no descriptor → vanished. (An earlier
   persistence-cache attempt was a BANDAID: it froze the cached frame, so the animation stopped
   off-screen, and it could never show a never-approached Marry Man — both correctly called out.)
   FIX (`native_wsmm_compose` in native_renderer.c): resolve each Marry Man DIRECTLY from his
   placement record every frame and draw natively across the full wide view, independent of the
   engine's draw cull. RE of the compositor `$57B0B4`/handler `$57C13A`/build `$57B19E..$57B558`:
   - records at `$5A4562` (stride 64): +0 type (0=end), +2 worldX, +4 worldY, +$a LIVE anim
     cursor (the per-record loop advances+writes it back for EVERY record BEFORE the cull, so it
     ticks even off-screen), +$c cached draw-handler ptr (==`$57C13A` ⇒ Marry Man).
   - frame = `MR16($5d5a(a5) + cursor)`; gfx entry = `$4a72(a5) + frame*8` → {data_off, mask_off,
     yoff, BLTSIZE}; data = data_off+`$EEFA`, mask = mask_off+`$12E7E`; cookie-cut, BMOD=-2;
     left = worldX-8, top = clamp(worldY,$D7)+yoff. VERIFIED numerically on L1 (cursor $46 →
     frame 50 → data=$010052 mask=$0131F6 size=$0242) and in-app on L5 (960px): walking right,
     `wsstatic` shows the queue drain (scanned 2→0) while `marrymen` stays 3 (incl. one at worldX
     642 never approached), the cursor advances 86→92→98 (live anim), all 3 render across the
     view (`scratch/screenshots/l5mm_v.png`). Queue-walk dedups Marry Men by position; default
     352 untouched. REPL `wsstatic` reports scanned/drawn/marrymen.
   NOT an issue (verified): the `$5A4562` list is Marry-Men-ONLY — on L5 records [0..2] are the
   3 marry men (handler `$57C13A`), records [3..4] have invalid handler ptrs (`$00350008`) i.e.
   they are not drawable objects, just trailing data. The real DECORATIONS (torches/teleporter/
   enemies, `$06xxxx`) go through the object WALKER → `$57D8D0` path, whose cull was widened and
   whose animation is captured in #4 — they are already handled. So no further static-object
   handler port is needed for the visible gameplay sprites.

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

6. **Line-blitter graphics (chandelier chains) not rendered natively.** OPEN. The engine
   draws chain/rope graphics with the blitter in LINE mode (con1 bit0); the emulated
   blitter renders them (line+fill implemented in `hw_do_blit`, see
   [[project_blitter_line_fill]]), but they go into the engine PAGE, which the native wide
   renderer ignores → invisible in widescreen (visible as a diagonal line top-left in the
   L9 960px view). Same family as #1/#4: page-only draw not captured. FIX: capture the
   line-blit segments (endpoints/colour) before the page write and draw them natively, or
   fold into the unified pre-cull capture.

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
