# Widescreen — plan

## Progress (2026-06-03)

- **Phase 1 (camera-independence): VERIFIED enough to proceed.** The pickup work
  (`PICKUP_SCAN`) showed off-screen objects' handlers ARE dispatched every frame
  (items at screen-Y 432, dy=-400 from the player), and the object list is the full
  level set loaded up front — no scroll-keyed spawn/cull. The object-loop walker
  (`$57D7BC`) iterates the whole list with no screen gate. The rigorous "off-screen
  enemy behaves identically to PUAE" check is deferred to Phase 4 (when margins show
  them). Treat as confirmed; revisit if a margin enemy misbehaves.
- **Phase 2 (widen surface + pillarbox): DONE.** Output width is runtime-configurable
  via `BENEFACTOR_WIDESCREEN=<px>` (or `=1` → 480; default 352 = unchanged). The
  engine still renders the 4:3 playfield into `s_fb` (352); `hw_compose_output()`
  composites it CENTERED into a wider `s_out` (cap `HW_OUT_MAX`=640) with black
  pillarbox margins, presented via the widened SDL texture/window. Harness path
  (`s_fb`/`hw_get_framebuffer`, PUAE compare) is untouched; `hw_get_output_framebuffer`
  + REPL `fbw [tag]` capture the wide surface. Verified: gameplay + menu both
  pillarbox correctly at 480. Margins are black for now — Phase 3 fills the gameplay
  margins with native tiles; for non-gameplay screens pillarbox is the FINAL look.
- **Next: Phase 3** — RE the tilemap + camera X, native-draw background tiles into the
  gameplay L/R margins, seam onto the engine's center.

---


Goal: render the gameplay at a wider aspect (e.g. 16:9 ≈ 480–576 px wide) instead of
the Amiga's 320 px playfield, showing more of the level left/right. PC-native feature,
in line with the port's direction (own the rendering; don't emulate the hardware).

## Why this is feasible (and the one thing to verify)

The game's simulation is **camera-independent**: objects/enemies update and events fire
regardless of what's on screen — there are no "activate when near the viewport" or
"cull when off-screen" triggers (user is nearly certain; **verify before building**).
That means a wider view shows *more of an already-correct simulation* — we don't have
to fork or re-time game logic.

**Verification step (do first):** grep the gpl engine for screen-bound comparisons that
gate behaviour — object-update loops that skip when an object's X is outside the
visible window, spawn/despawn keyed to scroll position, or AI that wakes on proximity to
the screen edge. Concretely:
- Look in the per-frame object loop (`$5770F8`-class main loop, the object table walkers
  in [[gameplay-engine-map]]) for compares against the scroll position / `$1C`/`$2C`
  scroll counters or a fixed 320/0x140 window.
- Empirically: widen the view (Phase 2 below) and watch whether enemies that were just
  off the old edge behave identically to PUAE when they enter the old 320px window.
If a few such checks exist, they can usually be patched to use the *wider* window; if
many do, widescreen gets much harder — hence verify first.

## The core obstacle

The native renderer (`native_renderer.c`) walks the copper list and decodes bitplanes
per-scanline — but it can only show what's **in the bitplane buffer**. The gameplay
engine blits only the visible ~320 px (+1 tile column of margin for smooth scroll) of
the scrolling tilemap into a double-buffered bitplane buffer (ptrs at `a5+$67A/$67E`,
swapped each frame; scroll via BPL pointers + BPLCON1 fine-scroll + BPL2MOD). The
off-screen world to the left/right is simply **not blitted** anywhere. So widescreen
cannot just "read more columns" — that data doesn't exist in chip RAM at render time.

Two things must each become wider than the game makes them:
1. **Background** (the scrolling tilemap) — only screen-width is blitted.
2. **Blitter objects** drawn into the playfield buffer — only on-screen ones are drawn.
(Hardware sprites are different — see below — and are the easy part.)

## Current rendering pipeline (baseline)

- `hw_present_frame()` → `native_render_frame()` reads the copper from chip RAM, builds a
  per-scanline `ScanState` (BPLCON0/1, BPL ptrs+mods, DDFSTRT/DDFSTOP, colors), then
  decodes each scanline into the 352×282 ARGB framebuffer (`HW_DISPLAY_W/H` in `hw.h`).
- Playfield width comes from DDFSTRT/DDFSTOP (`DDF_TO_X`); dual-playfield, ≤6 bitplanes.
- Output size is fixed at compile time: `HW_DISPLAY_W=352`, `HW_DISPLAY_H=282`.

## Approaches

**A. Widen the engine's blit (shortcut, fights the recompiled engine).** Override the
scroll/tile-blit setup so the game blits a wider region into a wider bitplane buffer
(wider DDFSTOP, modulos, double-buffer allocation), then render that. Pro: reuses the
game's exact tile blit. Con: deep surgery in recompiled scroll code; buffer sizes,
BPL2MOD, DIW/DDF, page-flip all have to widen consistently; high risk of subtle scroll
corruption. Good for a quick experiment, poor as the final design.

**B. Native wide background renderer (recommended target).** Read the level tilemap +
tile graphics + camera/scroll position from chip RAM and draw the **full wide view**
ourselves, bypassing the narrow bitplane buffer for the background. Composite game
objects on top. Pro: full control of width; doesn't fight the engine; matches the port's
"own the rendering" direction. Con: must RE the tilemap format, tile-graphics layout,
parallax layers, and the exact camera/scroll position (sub-pixel/fine-scroll).

**C. Hybrid (good first milestone).** Keep the engine's blitted ~320px center exactly as
today; native-render only the **extended left/right margins** from the tilemap, and seam
them onto the center. Less native rendering than B, gives immediate widescreen, and the
center stays pixel-identical to the original. Migrate toward full B if the seam or
parallax is fiddly.

Recommendation: **C → B.** Start hybrid (center untouched, native margins), then absorb
the center into the native renderer once the tile path is proven.

## Per-subsystem work

- **Background tiles:** RE the level tilemap (map array + tile-graphics base) and the
  camera X (scroll position; the `$1C`/`$2C` counters + `a5+$10D8` accumulator look
  scroll-related — confirm). Native-draw tiles for the wider column range. Handle the
  parallax/starfield background layer (PF1) at its own scroll rate.
- **Hardware sprites (easy):** the renderer already has sprite-register access (currently
  parked). At wider view, sprites previously clipped at the 320 edge become visible —
  just widen the sprite X clip to the new bounds. No new data needed (sprite pointers/pos
  are live registers).
- **Blitter objects (player, enemies, pickups):** these are drawn into the playfield
  buffer by the engine, so off-screen ones in the new margins aren't drawn. With B/C,
  draw them natively from the object tables (positions are simulated and live — the
  camera-independence point). Reuse the object-table walk from [[gameplay-engine-map]].
- **HUD / status panel** (bottom bar): it's UI, not world. Keep it at native width
  centered, or extend its art to the new width. Decide per aesthetics; centered is safe.
- **Non-gameplay screens** (intro crawl, logos, car, poster, menu, level card, credits):
  these are full-screen 4:3 art, not tile-scrolled. **Pillarbox** them (render 4:3
  centered on the wider surface) — don't try to widen fixed art. Cheap and correct.
- **Output/aspect:** make `HW_DISPLAY_W` configurable (e.g. a build/runtime option),
  widen the SDL texture/window, keep `HW_DISPLAY_H`. Letterbox/pillarbox at the chosen
  aspect. Keep a 4:3 fallback.

## Phased rollout

1. **Verify camera-independence** (above). Gate the whole effort on this.
2. **Widen the surface + pillarbox everything.** `HW_DISPLAY_W` configurable; all current
   screens render 4:3 centered. No new content yet — proves the plumbing/aspect/window.
3. **Hybrid background margins (C).** RE the tilemap + camera X; native-draw background
   tiles in the L/R margins; seam onto the engine's center. Gameplay shows wider terrain.
4. **Objects/sprites in the margins.** Widen sprite clip; native-draw blitter objects from
   the object tables in the margins. Enemies/pickups appear in the widened view.
5. **(Optional) Full native background (B).** Absorb the center into the native tile
   renderer; retire the dependency on the engine's narrow blit for display.

## Open questions to resolve during implementation

- Exact tilemap format + tile-graphics base in chip RAM, and the camera/scroll X (fine +
  coarse). Start from the scroll subsystem rows in [[gameplay-engine-map]].
- Parallax layer count and per-layer scroll rate (PF1 vs PF2 in dual-playfield).
- Any screen-bound object culling/activation (the verification step).
- HUD width treatment (centered vs extended art) — product decision.
- Vertical: this plan widens horizontally only; vertical 16:9 would need the same tilemap
  treatment top/bottom and is out of scope unless wanted.
