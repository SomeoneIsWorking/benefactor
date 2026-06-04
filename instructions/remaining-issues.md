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

1. **Marry Men (rescued creatures) invisible.** OPEN. Repro: level 1, figures stand in
   doorways. They are NOT in any current capture (the human-sized things captured on L1
   were a door + a boulder). They go through an UNHOOKED walker path — likely the
   `$57D81C` multi-tile branch (a CPU `move.w` copy, NOT a blitter blit → absent from
   BLIT_LOG and the `$57D8D0`/`$57D3F4` choke captures) and/or the same setup
   queue-build path as issue #2. SHARES the root cause of #4: the char/list-B draw paths
   are gated by a per-object camera cull in the walker `$57D79A` (unlike `$57D8D0` list-A
   which we capture pre-clip). NEXT: hook the walker's per-object point before the cull;
   capture worldX/Y/gfx for ALL objects (on+off screen) and draw natively — one fix covers
   marry men + torches + teleporter (#4). NOTE: object src-range classification is PER-LEVEL
   (L1 gfx in `$05xxxx`, L9 in `$06xxxx`) — classify by ROUTINE, never by magic src range.

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

4. **Animated sprites culled at the vanilla 320 window (torches, teleporter, etc.).** OPEN.
   ROOT CAUSE NAILED (2026-06-05, L9 repro: torch flame at worldX≈902). The torch is a
   self-contained animated COOKIE-CUT sprite (rock+stick+flame, one blit `con0=4FCA
   apt=$066C86 3x21`) drawn via `$57D3F4`; the wall's "lit" look is baked into the BG tiles
   (stays). The sprite is captured into `s_wschar` ONLY while on-window: at cam 938 (torch
   36px left of the window) it still draws & shows in the wide margin; by cam 981 (79px
   left) the engine STOPS calling its draw → not captured → vanishes — even though it's at
   wide-screen x≈225 (well inside a 960px view). So unlike `$57D8D0` (list-A: the engine
   reaches the choke for off-screen objects too, writing a skip-descriptor INSIDE — we
   capture at entry, pre-clip, so list-A shows in margins), the `$57D3F4` char path and the
   list-B execs are GATED by a per-object camera cull UPSTREAM of the draw (in the walker
   `$57D79A`/object handler). This is the SAME root cause as #1 (Marry Men) and #2.
   FIX (one change covers torches + teleporter + marry men): hook the walker's per-object
   point BEFORE its cull, capture every object's {worldX, worldY, gfx, mask, w, h, con0}
   regardless of on/off-screen, draw natively across the wide view. Tools added: REPL
   `wsobjs` (dump captured obj/char lists + screenX), `view_left` in `wsobjs`,
   `BENEFACTOR_WIDESCREEN` now up to 960 (`HW_OUT_MAX`). NOTE: needs ≥640px to even observe
   the cull — at 480 the sprite leaves the true wide edge before the cull triggers.

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
