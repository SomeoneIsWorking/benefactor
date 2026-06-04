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
   queue-build path as issue #2. NEXT: RE the `$57D81C` path + how the object queue is
   built; capture worldX/Y/gfx pre-clip and draw natively. NOTE: object src-range
   classification is PER-LEVEL (L1 gfx in `$05xxxx`, L9 in `$06xxxx`) — classify by
   ROUTINE, never by magic src range.

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

3. **GET READY / GAME OVER banners invisible.** OPEN. Art spec fully RE'd & VERIFIED
   (cookie-cut, DATA=`$A49A` MASK=`$BDCC`, w16 h43 bmod=-2 afwm=FFFF alwm=0000, plane
   stride `$50A`, camera-relative SCREEN position; routine `$578974`, four banner
   variants `578860/57889C/5788DE/57892E`). BLOCKER: one-shot draw lifetime — it persists
   in the page until cleared; native needs a "banner active" state flag (not yet located)
   to know when to draw the overlay. NEXT: find the GET-READY/GAME-OVER state flag, draw
   the captured banner centered while active.

4. **Decorations (torches) culled where vanilla culls them.** OPEN. Decoration sprites
   inherit the engine's 320px clip instead of showing in the wide margins. NEXT: capture
   them BEFORE the engine's camera clip (same pre-clip principle as the walker), draw
   across the wide view.

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

### Widescreen — DONE / NOT-A-BUG (verified this push series)
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
