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

1. **Marry Men (rescued creatures) invisible.** **MOSTLY FIXED (2026-06-05)** — they now
   render (cleanly, no ghosts/outline) in the central view; remaining: margin cull (see end).
   The Marry Man = the short red-shirt figure inside the **cage** (grey
   two-pillar cell with a cross on the right pillar), NOT the teleporter (the green figure in
   the bottom-left grey structure is the PLAYER — do not chase him). On L1 the cage sits at
   ~worldX 67, page dpt `$02C338`. CONFIRMED VIA ABLATION (REPL `blitskip <fn>` drops every
   blit from a routine): blitskip-ing **`57D6C4`** empties the cage → he is drawn by the
   cookie-cut CHARACTER executor `57D6C4`. blitskip-ing every other candidate (`57DB16`/
   `57DA40`/`57DA88` list-B, `57D56C`, the `57D282` restore) left him intact → the doc's
   old "list-B / multi-tile" guess was WRONG. He is a **character we never captured** (user's
   call), distinct from the player (`$57A666`) and enemies (already done).
   - His blit: `con0=AFCA` (A-shift 10), 2×9, **DATA(bpt)=`$010xxx`, MASK(apt)=`$013xxx`**,
     `bmod=-2`, animated (4 frames, bpt +`$5A`/frame), 5 planes at dpt step `$2A0C`.
   - WHY uncaptured: the char capture hooks the `$57D3F4` builder; EVERY char it captures has
     data `$05xxxx`, his is `$01xxxx` → `$57D3F4` never fires for him. Three builders feed the
     executor `57D6C4`: `$57D3F4` (captured), `$57D5AA`, `$57D6F2` (`$57D5AA`/`$57D6F2` are the
     low-level blit-issue helpers — `btst d4,(a6)` wait-blitter + reg-stream + trigger — so the
     worldX/Y is only in their indirectly-dispatched object handler, not in clean regs there).
   - The executor `$57D6C4` is a blitter-QUEUE PLAYER (reads a descriptor stream from A0,
     issues all 5 planes in ONE entry → an override on it fires once/char, but params are in
     the queue not registers). PAGE dpt is camera-INDEPENDENT (page = full level width), so
     dpt→worldX/worldY is derivable (page base `$02B3EC`/`$038628`, row stride `$2E`=46 → L1
     marry man dpt `$02C338` decodes to worldY≈85, worldX≈48+ASH, matches the cage).
   - FIX (`native_wsmissedchar_compose` in native_renderer.c, called from
     `native_render_wide_bg` after the other captures): his blit is ALREADY recorded in the
     blit-capture (`hw_blit_capture`, con0=AFCA passes the filter). We project the cookie-cut
     char blits the OTHER capture systems don't draw, into `s_objlayer`, using the SAME
     camera-independent page→world projection the (dead) `s_pg` list used
     (`worldX = cam16 + xrel*8 + ASH`, `xrel = (dpt - displayed_BPL_ptr) % rowstride`). DEDUP
     is by gfx IDENTITY — skip any blit whose MASK ptr matches a `$57D3F4`-captured char or the
     player (already drawn by their own pass) — NEVER by src RANGE (which is per-level). Opaque
     (list-A) and bg-restore blits are excluded (mask==0). Runs only for wide output
     (`ow>352`) or the WS_CMP gate, so default 352 is byte-untouched. VERIFIED (L1, 960px):
     marry man now renders in the cage; pre-fix vs post-fix wide frame diff = 49px in ONE 16×9
     region (his figure) and nothing else → no enemy/object doubling, dedup correct. worldX
     ≈63-79 (cage ~67). `scratch/screenshots/mm_full.png`.
   - FOLLOW-UP FIXES (2026-06-05, after user testing): the first cut re-drew sprites the
     other passes already draw (walker/key/mushrooms ghosted) and had a red outline.
     - **Dedup by gfx IDENTITY (src ptr), not mask/position.** The mask-based dedup let
       ANIMATED captured sprites through (their derived capture-mask ≠ the blit's apt, and
       both change per anim frame) → ghosts. The blit's SRC ptr == the captured sprite's
       data (verified: walker blit src=$064ADA == its captured `chr` data=$064ADA), and is
       WRAP-INDEPENDENT. Dedup blit.src against wsobj/wschar/player gfx, position as a
       secondary anim-skew catch. Result: 0 missed-char draws on L9 (all captured), 1 on L1
       (the marry man). REPL `wsmc` lists what the pass drew (NOT an env log).
     - **Red blinking outline = colour-0 drawn opaque.** Cookie-cut colour 0 must be
       TRANSPARENT (`if (ci)`, like native_objlayer_from_capture); drawing it as pal[0]
       painted the silhouette in COLOR0 (dark red in the cave palette).
     - **Wrap to the opposite screen side** (page is a 368px CIRCULAR buffer → dpt→world is
       only correct mod 368): guard `delta>=0` and `orow` within the playfield, so a
       wrapped/off-screen image isn't drawn as a phantom on the far side.
   - STILL OPEN (marry men): they are CULLED in the wide margins the same way the torches
     were (#4) — but via their own char draw path (drawn per-frame from the main loop
     $5770F8 → $57D6C4, NOT the object walker $57D79A, so the #4 walker-cull widening does
     NOT cover them). When the wide camera scrolls a marry man into the margin the engine
     stops blitting him → not in the blit-capture → can't be drawn. Needs his char-path cull
     found + widened (same shape as #4). He renders correctly in the central view meanwhile.
   - Scroll-tracking + wrap guard are logically derived (couldn't drive the camera to scroll
     in-harness to empirically test); projection formula is identical to the proven bg/object
     projection. Diagnostic tools
     added: REPL `blitskip <fn>` (ablate a routine's blits), `vren` (faithful `hw_render_frame`
     of current g_mem — note: that legacy renderer is now mostly dead/black, not a usable oracle;
     use PUAE).
   - Verify method: PUAE (`pugoto 1`) vs native at 352, lockstep `both N`, crop the cage
     (`scratch/screenshots/cage_van.png` shows him, `cage_nat.png` is the page-faithful native
     render which ALSO shows him — but the WIDE output `fbw`/`s_out` does NOT; that gap is the
     bug). NOTE: object src-range classification is PER-LEVEL — classify by ROUTINE, never by
     magic src range.

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
