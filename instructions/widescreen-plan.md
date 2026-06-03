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
- **Phase 3 IN PROGRESS (2026-06-03).** Decision: **full native background (approach B)**
  (user picked it over hybrid). NOTE the catch: the engine blits tiles AND objects into
  the SAME single-playfield 5-plane buffer, so a *pure* B that shows objects needs native
  object drawing too (Phase 4) — until then the practical first result is C-shaped (native
  background everywhere + engine's center buffer for objects). The native background
  renderer is the shared foundation; build it first.
  - **Camera X = `$57FDBA` (a5+$0FA8)** — screen-left world coord, engine-clamped to the
    level's real edges (this is "camera stops where a normal camera would"). camera_tile =
    `$0FA8`>>4, fine = `$0FA8`&15. Found via `scratch/camhunt.py` (differential scan).
    `scratch/camedge.py` drives to edges to read the clamp range (= level edges) — but
    walking LEFT off the start-area edge on the test save kills the player; drive RIGHT.
  - **Background scroll mechanism (from `BLIT_LOG`):** NOT a tall-column blit. The terrain
    is drawn as **16×16 tile cells** — `w=1 h=16` blits (~114/frame while scrolling), one
    per plane, sourced from tile graphics, dest = the playfield double-buffer. As the camera
    crosses a 16px boundary a new column of cells is blitted in. So: the `w=1 h=16` blit
    SOURCES are tile-graphics addresses; the per-column sequence of sources encodes the
    tilemap. Playfield buffers via `$67A/$67E/$682(a5)` (= `$57F48C/$57F490/$57F494`),
    dest regions seen: $02/$03/$04xxxx; tile-gfx sources in $04/$05/$06xxxx.
  - **Blit chain (from a clean BLIT_LOG, w=1 h=16):** the cells come in PAIRS with a
    consistent **plane stride $2A0C** (=10764 ≈ 336×256/8 = one full-screen planar plane):
    `src=$02xxxx → dst=$046xxxx`, then `src=$046xxxx → dst=$039xxxx`. The five cells of a
    column step by $2A0C (the 5 bitplanes). So $02/$03/$04xxxx are full-screen PLANAR
    bitmaps (plane stride $2A0C), and the "tile" draw is really a planar buffer→buffer copy
    chain ($02→$04→$03), i.e. the engine composes/scrolls between off-screen pages, NOT a
    simple tilemap→screen blit. CONSEQUENCE: the tilemap array + tile-graphics base are NOT
    directly the blit sources here — need to RE the level-setup ($5782B4) that builds the
    $02xxxx source bitmap, OR the scroll code that picks columns. Buffers via
    `$67A/$67E/$682(a5)`.
  - **Page roles (read on the level-9 save):** `$67A(a5)=$67E(a5)=$038628` → the DISPLAY
    double-buffer page (region $03 = the blit chain's final dest). `$682(a5)=$5A251C` →
    a pointer into the gameplay overlay's LEVEL DATA ($5Axxxx) — prime candidate for the
    tilemap / level structure to decode. (The $02/$04xxxx pages are the compose/scroll
    work buffers.)
  - **Level-data pointers (read on the level-9 save, a5+$67A region $57F48C..):**
    `$67A/$67E(a5)` = the two display pages `$0002B3EC` / `$00038628` (regions $02/$03,
    swap each frame). `$682(a5)=$5A251C`, then `$57F498=$5A268C`, `$57F49C=$5ABB5E`,
    `$57F4A0=$5ABD3E` — four $5Axxxx LEVEL-DATA structures (tilemap / tile-gfx / object
    list / palette?, TBD). Dump of $5A251C (8-byte records): `000012DD 002A0242 / 000006D8
    00280303 / 000014D6 002A0242 / 000014D8 002A0802 / 0 002A0802 ...` — a coord-ish long
    + a `$2A0xxx` long per record (the $2Axxxx region is the decompressed level data; note
    plane stride was also $2A0C). Camera `$0FA8` confirmed again here = $04CB while
    playerX=1402 (screenX 175).
  - **Next concrete steps (next session, fresh context):** (1) Decode the four $5Axxxx
    structs (which is the tilemap array, which the tile-graphics base, which the width) —
    cross-ref with the level-setup $5782B4 that fills them, and with the planar source page
    ($02xxxx) the scroll copies from. (2) Find the level-setup code that fills
    the source bitmap from level data → that reveals the real tilemap + tile graphics +
    level width. (3) Either render natively from the tilemap (true B) OR — simpler given
    the planar pages — if a page already holds MORE than 320px of decoded terrain, decode
    the wider region straight from that page (camera `$0FA8` picks the window) and clamp to
    level edges. Verify whether any page is wider than the screen first.
  - **DIW over-fetch clip (committed? NO — in tree):** `native_renderer.c` now clips the
    bitplane decode to DIWSTRT/DIWSTOP (`fb_x = H-123`), removing the engine's ~16px
    smooth-scroll over-fetch garbage (the original screenshot bug). Correct HW behavior +
    a clean 320 seam edge; but under full-B the background won't come from the bitplane
    decode at all, so this may be superseded. Keep for now.
  - **DIW clip REVERTED (2026-06-04):** discarded the uncommitted clip change — clamping
    to DIWSTRT/DIWSTOP pulls the view back to the ~320px display window, the exact thing
    widescreen must EXCEED. Dead end. Kill the right-edge garbage by rendering real tiles +
    stopping the camera at level edges, not by clipping.

### Phase 3 RE — tile rendering DECODED (2026-06-04)

Disassembled from `logs/savestate.bin` (level-9 save; g_mem at file offset `0x5D0`, so
chip addr A = file `0x5D0+A`; disasm via `python3 tools/disasm2.py logs/savestate.bin
$((0x5D0+ADDR)) ADDR LEN`). Verified the actual scroll + tile-draw path. a5 = `$57EE12`.

- **Scroll/redraw + camera clamp + copper-BPL build = `$57C79E`** (entered mid-routine
  from the scroll orchestrator). Camera math:
  - `d2` = candidate screen-left world X (player-tracking, then `subi.w #$9f`).
  - **Level edges (the camera clamp):** `min_cam = $107a(a5) - 0x90`, `max_cam =
    $107c(a5) - 0x100`. On the L9 save: `$107a=144`, `$107c=1648` → camera ∈ **[0, 1392]**.
    `$107a`/`$107c` are the per-level world bounds — read these for the wide-camera clamp.
  - `move.w d2,$fa8(a5)` then `subi.w #$10` → camera_x. fine = `~d2 & 0xf` → BPLCON1
    (copper at `$3572`/`$3576`); coarse `d2>>4` → BPL ptr byte offset `(d2>>4)*2` into the
    `$2B3EC` page; 5 BPL ptrs written to copper at `$3576..` each +`$2A0C` (plane stride).
  - **Confirms: the `$2B3EC` display page is only screen-width + margin (row stride `$2e`
    = 46 bytes = 368px; plane stride `$2A0C` = 46×234). It is a WRAP buffer — off-screen
    terrain is NOT present. So approach B (native tilemap render) is mandatory; no
    read-wider-from-page shortcut.**
- **Per-cell tile draw = `$57C72A`** (one 16×16×5-plane tile, fully unrolled, CPU
  `move.w (a4)+` + blitter copy into BOTH double-buffer pages; alt blitter-only path at
  `$57CA80` when `$10c4(a5)` set — masked/transparent tiles). It reads the source pointers:
  - `a1 = $552A0` = **TILEMAP** (indexed by `d2 ≈ (camera>>4)*2 + 0x2a + phase`).
    `tileval = $552A0[d2]; idx = tileval & 0xFE`.
  - `a4 = $5A539E` = **tile-graphics POINTER TABLE**; `gfx = *(long*)($5A539E + idx)`.
    Entries are contiguous `$5D902A + n*$A0` (each tile gfx = `$A0`=160 bytes). On L9 save:
    `[0]=$5D902A,[1]=$5D90CA,...` (+$A0 each).
  - `a0 = $57F4BC` = **phase table**, 16 entries × {d2_adjust.w, a3_adjust.w} (one per
    fine-scroll phase), TWO halves selected by `sign($694(a5))` (scroll direction;
    `lea $40(a0),a0` picks the 2nd half).
  - **Tile gfx format (from the unrolled writes — `(a4)+`→rows at `$2e` stride, then
    `adda d6,a3` per plane): PLANE-MAJOR, 5 planes × 16 rows × 1 word = 80 words = 160B.**
- **Decompressor `$5782B4` → `$59DC02`**: a generic stream decompressor (magic
  `"=SB="`=`$3D53423D`, chunk `$4000`) that decompresses level data from the `$5Axxxx`
  region into the display page `$2B3EC`. Not the tilemap source per se.

**Native wide background — what's now known vs. still needed:**
- KNOWN: camera (`$0fa8`), level-edge clamp (`$107a`/`$107c`), tile size (16×16×5bpp,
  160B plane-major), tile-gfx table (`$5A539E[idx]`, `idx=mapval&0xFE`), palette (copper).
- STILL NEEDED (next pass — use the RUNNING harness to correlate, not more static disasm
  of unrolled code): the **tilemap 2D layout** — the column stride in `$552A0` and the
  vertical indexing (the `$57C72A` cell-draw is called per-cell by a vertical loop in its
  caller; find that loop OR derive the stride empirically by dumping `$552A0` against the
  visible screen at a known camera). Hypothesis: column-major, `d2` walks columns; vertical
  via a loop that steps `d2` by a row count. Confirm before building the renderer.

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
