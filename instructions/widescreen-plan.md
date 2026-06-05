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

### Phase 3 RE — tile decode FULLY CRACKED & VERIFIED (2026-06-04)

The standalone reference renderer (`scratch/ws_render.py`, reads a `dumpall` g_mem dump)
now reproduces the real cave EXACTLY at 480px wide (`scratch/screenshots/ws_final.png` vs
`ws_real_fb.png`: matching dirt, platforms, two ladders, green ledges, red pickups, dark
bg). **COMPLETE, VERIFIED tile-render spec:**

- **Tile SELECTION:** `gfx = read_long($5A539E + (mapword & 0xFFFE))`. The earlier `&0xFE`
  was THE bug — the draw does `moveq #$fe,d0` (= `$FFFFFFFE`) then `and.w (a1,d2),d0`, so
  the mask is `0xFFFE` (clears only bit 0), and the FULL 16-bit map word (e.g. `0x05EC`,
  `0x0320`) is the table byte-offset. The `$5A539E` table is NOT uniformly strided — always
  use the lookup, never a `$5D902A + n*$A0` formula.
- **Tile GRAPHICS format:** 160 bytes, **5-plane PLANE-MAJOR**: `plane p, row r` word =
  `gfx + p*32 + r*2`, 16 rows × 1 word (16×16px, 5bpp). (Row-interleaved was wrong.)
- **PALETTE is PER-SCANLINE (copper).** The playfield region (copper VP ~39–231 / display
  lines ~12–204) sets COLOR16-31 to the cave browns (`FFFFFF BB8855 AA6633 995522 884422
  553311 442200 331100 ...`). Parsing the copper to its END grabs the *HUD* palette
  (greens/cyan at VP 255+ wrapped) — wrong. `native_renderer.c::walk_copper` ALREADY builds
  the correct per-scanline `st->palette`, so the C port gets this for free. `g_state.palette`
  is a grey/tan FALLBACK ramp, not the displayed colors.
- Indexing (base `$552A0`, row-major, row stride `$F4`=244B, `col = worldX>>4`, ~14 rows)
  unchanged & still harness-verified.

New harness REPL cmd `pal` prints the live 32-entry palette (committed). Reference renderer
+ gameplay dump (`scratch/bin/gmem_gameplay.bin`, cam=961) + `scratch/screenshots/ws_*.png`
are the verification harness — gate any render claim on a clean reference render.

**Phase 3 — native playfield render (engine↔bitplane relation RE'd & PC-owned).**
CORRECTION to the earlier hybrid: the engine-center + native-margins approach was a
FAKE (two renderers glued → seam + tuning constants); REJECTED by the user. The
engine composes the WHOLE playfield as blitter A→D copies into the double-buffer pages
($2B3EC/$38628): tile columns (scroll $57C72A) + object sprites (executor $57DB34,
descriptors {src,mod,dst,size} × 5 planes step $2A0C, BLTCON0=$09F0 straight copy).
That whole relation is now RE'd. native_render_wide_bg now composes the ENTIRE playfield
width itself from the tilemap (engine bitplane NOT used for the playfield) — ONE renderer,
no seam, no alignment constants (worldX derived per-scanline from copper x_off/scroll1 +
camera, exactly the engine's own mapping). Verified seamless at 480 (ws_onerenderer.png).
NEXT: native OBJECTS via the RE'd $57DB34 descriptors (recover world pos from dst, draw
sprite from src across the wide buffer) — then the player/enemies render too and native
becomes the sole gameplay renderer at all widths. (was: hybrid margins) Implemented in
`native_renderer.c::native_render_wide_bg(out, ow, margin)`, called from
`hw.c::hw_compose_output` whenever the output is wider than 352 (gameplay only, gated on
`s_cur_cop1lc==$003484`). It fills the L/R output margins (+ the engine's thin side borders)
with native tiles decoded from the tilemap, keeping the engine's center playfield (objects/
player) intact. Reads tilemap/gfx straight from `g_mem` (gfx >512KB, past the `chip_r16`
window); reuses `s_scan[y].palette` (per-scanline copper palette built by `walk_copper`).
Verified at `BENEFACTOR_WIDESCREEN=480` (`scratch/screenshots/ws_app.png`): cave extends
seamlessly both sides, center keeps player/items/ladders, HUD centered, seams continuous.
Default 352 path + harness `s_fb` compare path untouched (only `s_out` margins change).
Seam alignment tunables (env, defaults work for L9): `BENEFACTOR_WS_ANCHOR` (4, s_out x of
worldX==camera, rel. margin), `_PFL`/`_PFR` (4/340, engine playfield L/R x to seam up to),
`_TOP`/`_ROWS` (12/14, playfield top line + row count).

**→ NEXT (Phase 4): native objects/sprites in the margins.** The center has live objects
(engine), but the margins only show terrain — enemies/pickups/player off the old 320 edge
aren't drawn there. Native-draw them from the object tables (positions are simulated &
live, camera-independent — see [[gameplay-engine-map]] object walker `$57D7BC`). Also widen
the hardware-sprite clip. Then verify a 2nd world (per-level `$5A539E`/`$5D9xxx` stability)
and any parallax/background 2nd layer.

### Phase 4 RE — object draw path FULLY DECODED (2026-06-04)

REJECTED the last session's "page display-list" (`native_objlayer_ingest`/`project`,
`s_pg`): it captured RAW hardware blits and reverse-classified them by MAGIC src ranges
(`0x045000`/`0x054000`/`0x060000`), reverse-derived world pos from `dst-bplpt`, and had to
re-implement every blitter quirk (phantom ASH columns, per-plane FILL) → endless patches.
That violates [[feedback_render_full_native_not_hybrid]]. DELETE it; render objects from the
engine's own resolved per-object draw values instead.

**The choke point = `$57D8D0`** (`gfn_gpl_57D8D0`, a registered/overridable fn). EVERY
"normal" object handler (`$59AC38` etc., the per-type animation-script VM) runs first
(advancing the frame even off-screen — camera-independent) then `jmp -$1542(a5)` = `$57D8D0`.
So at `$57D8D0` ENTRY the engine has each object's resolved values, BEFORE the camera-clip
throws off-screen ones away. Live regs at entry:
- **`d0` = object world X**, **`d1` = object world Y** (screen row; game doesn't scroll Y).
- **`d5` = per-type anim gfx offset** = `animTable[frame]` (the `move.w table(pc,d5),d5` in
  the `$59ACxx` handler; table is per object type, e.g. `$59AC6C`).
- **`a1`** = object handler ptr. Descriptor fields (after `lea -$10(a1)`): `MOD=MR32(a1-$10)`,
  `SIZE=MR16(a1-$C)` = `(height<<6)|width_words`, `objGfxBase=MR32(a1-$A)`.
- **gfx src = `objGfxBase + d5`** (5-plane sprite, plane stride `$2A0C`, w words × h rows).
- dst (engine only) = `pageBase($57F490) + $5A1D18[worldY*2] + worldX/8`. `$5A1D18` = row→page
  byte-offset table. We DON'T need dst: native draws at screen `(worldX - camera, worldY)`.

**The camera-clip IS the camera-dependence** (`$57D8D0`): `screenX = d0 - $57FDBA`; off-left
or `objTile >= (camera+$160=352)>>4` → emits a no-blit sentinel and SKIPS the object. This is
the ONLY thing to bypass for widescreen — capture unclipped, clip to the WIDE buffer instead.

Also: **special multi-tile path `$57D81C`** (high-bit objects, `(a1) bmi`) — a manual
`move.w (a2)+,(a3)` tile blit with strides `$2E`/`$29DE`, ALSO camera-clipped (`cmpi.w
#$150,d1` at `$57D826`); and a **masked path** (`$57fed8` flag) using a B (mask) channel.
Both secondary; the normal `$57D8D0` path covers most objects (player/enemies/pickups).

**Plan:** native override of `$57D8D0` → capture `{worldX=d0, worldY=d1, src=objGfxBase+d5,
mod, w=SIZE&$3F, h=SIZE>>6, masked}` into a native per-frame list, then super-call the recomp
body (vanilla center unaffected). Native renderer draws bg (tilemap, done) + this object list
at `(worldX-camera, worldY)` across the wide buffer — ONE renderer, no magic, no blit-replay.
Verify wide=0 == vanilla (sprite decode must match the engine's `BLTCON0=$09F0` straight copy
/ cookie-cut for masked). Source spec verified by disasm of `logs/savestate.bin` (L9, file
off `0x5D0+addr`); live dump `scratch/bin/gm_obj.bin` (cam=881, playerX=1056, level 9).

### Phase 4 — DONE so far + the no-choke-point reality (2026-06-04)

- **DONE: one continuous bg + `$57D8D0` objects.** The bg loop's centre-clip
  (`in_margin`/`width_px`, commit `ba229bc`) was deleted — it was a center+margins SEAM added
  to chase byte-exact wide=0 parity (vanilla clips its playfield to the data-fetch window, so
  matching it forced the split). Now every output px → one absolute world X → tilemap, edge to
  edge, clamped only at level edges. `$57D8D0` capture (`native_objdraw_capture` /
  `native_objlayer_from_capture`) draws the objects it covers (platforms, box, ladders, items).
  USER-CONFIRMED 'best result so far'.
- **NO single object-draw choke point.** `BLIT_LOG=1` + group-by-`fn=` (g_rt_last_call) shows
  gameplay sprites are blitted by ~8 SCATTERED routines, each a per-category loop doing 5-plane
  blits into the double-buffer pages ($02xxxx/$03xxxx), each with its OWN clip:
  - `57C79E` bg tiles (skip — we render tilemap). `57DB5E` = `$57DB34` list-A executor (the
    `$57D8D0` objects we already capture). `57DA40` list-B executor.
  - `57A666` cookie-cut sprites (`con0=8FCA`, mask=A ch, minterm CA); `57A88A`/`57D282` opaque
    (`con0=09F0`, D=A); `57D6C4`/`57D688` cookie-cut; `57DB16`; `578974` GET READY banner (5
    blits); `578B94` (low-mem src → HUD/text?). NOT hw sprites (copper SPRxPT all 0).
  - **Characters (player+walkers), banner, pause, some deco are drawn by these OTHER routines**
    → not in the `$57D8D0` capture → invisible/early-culled in the native render.
- **DECISION (user): per-routine unclipped capture.** Hook each sprite-draw routine at its
  per-sprite point BEFORE its 352 clip, capture `{src, mask, worldX, worldY, w, h, mode}`,
  draw natively across the wide view (faithful, gives margin objects too; no magic ranges, no
  blit-replay quirk-chasing). Capturing at the blit level (`hw_do_blit`) gives only CLIPPED
  on-screen sprites (no margins) — must intercept before each routine's clip. RE each routine.
- **KNOWN BUG: native camera alignment is off by a few px toward the L/R EDGES.** So native
  compares skip an edge band: `wsdiff` excludes `WSDIFF_EDGE` px (default 32) per side. Fix the
  edge camera math separately (cam16/x_off/scroll1 vs $57FDBA); don't tune objects to it.

#### Per-routine RE progress

- **`$57A666` = PLAYER draw — DONE & PORTED (2026-06-04).** Reads player block `$10a6(a5)`
  (`$57FEB8`): `movem.w (a4),d1-d4` → d1=worldX, d2=worldY, d3=anim idx, d4=state. Clip:
  `d0=worldX-$fa8(a5)` (camera); `cmpi.w #$150` → cull if off-screen. **Player is
  camera-centered so it is NEVER in the margins** — it was invisible only because the native
  renderer neither reads the page nor captured this blit. Sprite (cookie-cut, `BLTCON0=$XFCA`
  = minterm `$CA`, `D = A?B:C`, ASH=worldX&15):
  - **A ch = MASK = `$52AA0` + frameoff**, single plane reused for all 5 planes (internal
    row stride `$28`); `BLTALWM=$0000` → word1 killed → sprite is **16px wide**.
  - **B ch = DATA = `$19E02` + frameoff**, 5-plane, **plane stride `$2800`** (manual
    `lea $2800(a0)`), row stride `$28`, word0.
  - **CORRECTION:** an earlier note here had A/B reversed (it called `$52AA0` the gfx). The
    minterm `$CA` cookie-cut means **A = mask, B = data**, and the `movem.l a0/a2-a3,(BLTBPTH)`
    confirms B=`$19E02`(data), A=`$52AA0`(mask). Verified by `scratch/ws_player.py` decoding
    several poses to clean ASCII characters.
  - frameoff = `MR16($2286(a5) + animidx)` (+$14 if `$7(a4)`=`$57FEBF` bit1 set). xoff =
    `s16(MR16($23E2(a5)+animidx))` (neg+2 if state bit1); gfx left X = worldX-8+xoff. yoff =
    `s16(MR16($253E(a5)+animidx))`; top row = clamp(worldY,$D8)-8+yoff, floored 0.
  - size `$402` → w=2 words (read), h=16; final displayed sprite = 16px × 16, 5bpp.
  - **PORTED:** `native_player_capture` (`$57A666`, pc_overrides_gameplay.c) captures
    `{wxleft, wytop, dbase, mbase}` then super-calls; `native_wsplayer_compose`
    (native_renderer.c) cookie-cuts it into `s_objlayer` at absolute world X. ⚠️ the table
    addresses MUST be `A5 + disp` (let the compiler add) — hand hex-arithmetic of them was
    the bug that made the player "fly all over" (wrong table base → garbage frameoff/xoff).
### Phase 4 — COMPLETE sprite-routine MAP (2026-06-04, verified via BLIT_LOG grouped by fn=)

The remaining widescreen issues (Marry Men / banners / decorations / damage-blink invisible)
all trace to this: `native_render_wide_bg` rebuilds the playfield from the tilemap and IGNORES
the engine's page, so anything only-in-the-page that I don't separately CAPTURE is lost. Full
map of every gameplay sprite-draw routine (`BLIT_LOG=1`, classify by `fn=`/`con0`/`apt` src):

| fn | con0 | what | src (apt) | captured? |
|----|------|------|-----------|-----------|
| `57C79E` | 09F0 | BG TILE scroll-draw | tilegfx | N/A — native tilemap |
| `57DB5E` | 09F0 | **list-A** object exec (opaque) | $06xxxx | ✓ via `$57D8D0` |
| `57D6C4` | 0FCA | small cookie-cut chars | $06xxxx | ✓ via `$57D3F4` |
| `57D688` | XFCA | walkers/enemies (cookie-cut) | $06xxxx | ✓ via `$57D3F4` |
| `57A666` | 8FCA | PLAYER | $19E02 | ✓ via `$57A666` |
| `57D282` | 09F0 | **bg-RESTORE** under small chars | $04/$05xxxx | SKIP (native redraws bg) |
| `57A88A` | 09F0 | **bg-RESTORE** under player-size | $04/$05xxxx | SKIP |
| `57DB16` | 09F0 | **list-B** object exec (opaque, w1 h4) | $06xxxx | ✗ NOT captured |
| `57DA40` | 09F0 | **list-B** object exec (opaque, w1 h4) | $06xxxx | ✗ NOT captured |
| `57DA88` | 09F0 | list-B small | $06xxxx | ✗ |
| `578974` | AFCA | GET READY banner (one-shot, 5 blits) | $A49A | ✗ (see below) |
| `578B94` | 09F0 | HUD/text (low-mem src, 8 blits) | low | ✗ (HUD is centered, low pri) |

**bg-restore vs real object = the apt (A=src) high byte.** Opaque (`con0=09F0`, D=A): src
`$04/$05xxxx` = the bg-SAVE buffer (the engine restores old bg before redrawing a sprite —
SKIP, we redraw bg fresh); src `$06xxxx` = real object gfx (DRAW). amod=$42 (=66) on the
restores (full page row stride), amod=0 on the $06 object draws.

**Marry Men (the caged rescue creatures) = static-placement OBJECT — FIXED (2026-06-05).**
The earlier "= list-B (`57DB16`/`57DA40`)" guess was WRONG (FALSIFIED). The CAGED marry man is
not a character or a list-A/B object: he is a placement record at **`$5A4562`** (clean world
coords) processed per-frame by the object compositor **`$57B0B4`** (per-record re-entry
`$57B0EE`), which builds a cookie-cut descriptor into the object-only queue **`$5A39EC`**,
played by executor **`$57D6C4`** (via `$57D56C`). His gfx is `$01xxxx` (shared low-mem creature
pool), so the `$05xxxx` char builder `$57D3F4` never sees him. The FREED/walking marry man
becomes a normal `$05xxxx` char (`$10e6(a5)` list → `$57D3F4`) and is already captured. FIX:
`native_wsstatic_compose` walks `$5A39EC` + `native_staticobj_capture` (`$57B0EE`) captures the
true worldX/worldY; see remaining-issues.md #1. (The page-blit reverse-projection
`native_wsmissedchar_compose` that briefly "fixed" this was the rejected `s_pg` approach and is
DELETED — it caused the ghosting/red-outline/opposite-side phantoms.)

**GET READY / GAME OVER banners (`578974` etc.)** — full art spec VERIFIED: cookie-cut
`con0=$AFCA`, **DATA(B)=$A49A, MASK(A)=$BDCC** (single mask, all 5 planes), w=16 h=43 BMOD=-2,
afwm=FFFF **alwm=0000** → eff width rowstride/2=15 words, plane stride h*(w*2+bmod)=$50A. Dest
camera-relative → FIXED SCREEN pos (UI). Y row offset = `MR16($5A1DB8)` (=$E60) into page
(/$2e=46 → row ~80). FOUR banner routines `578860/57889C/5788DE/57892E` (GET READY / GAME OVER
/ etc.), state-machine dispatched, drawn ONCE (not per-frame). THE OPEN PROBLEM = lifetime:
one-shot draw persists in the page until scrolled over; native ignores the page so needs an
explicit "draw overlay while banner active" signal (a GET-READY/GAME-OVER state flag — not yet
located). The OLD page-display-list architecture (s_pg) handled this and was superseded; bring
back a minimal screen-overlay lifetime for banners, gated on the real state flag.

**Decorations (torches) culled where vanilla culls — FIXED (2026-06-05, list-A).** The cull is
a per-object camera-window test in the WALKER, in TWO places (the RE first found only one):
- `$57D804..$57D812` (main, $30/$170) — most objects. Widened in override `native_objstep`
  ($57D7BC). `native_objwalk` ($57D79A) now ports the 6-instr setup then `rt_jump($57D7BC)`
  so the override catches the first object (the recomp $57D79A inline-falls-through to $57D7BC).
- `$57D8B4..$57D8C8` (animated, $30/**$1b0**) — objects with a non-zero anim nibble go here via
  `and.w -$c(a1),d2; bne $57D8B4`. The **$06xxxx torches/teleporter/enemies take THIS path**;
  widening only the main cull left them popping out. Widened in override `native_objstep_b`.
Both widen by `(out_w-320)/2` per side, **0 at the default 352** (gated `ow>HW_DISPLAY_W`) → the
352 `wsobjs` capture is byte-identical to pre-fix (verified by diff). Each override leaves the
Amiga stack exactly as the recomp fall-through (one a0 pushed; DISPATCH $57D816/$57D8CA push
a2-a4, SKIP $57D8A8/$57D8F2 pop a0); recomp handlers + $57D8D0 draw unchanged. Verified L9 960px:
torch at worldX 912 stays out to screenX −131 (was culled at −49). Multi-tile path ($57D826
`cmpi.w #$150`) is still narrow → issue #1 (Marry Men), separate.

**Damage-blink — DONE & PORTED (2026-06-04).** The blink is NOT a skipped draw and NOT a
palette flash — the engine draws the player EVERY frame, alternating normal and a solid BLACK
silhouette (user: "turns all black and normal"). RE of `$57A666`: `d4` = player state word
`$57FEBE`; if flag byte `$57FEBF` bit7 set (invincible, sets on damage, clears when the counter
hits 0 at `$57A6A8 bclr #7`), the draw takes the NORMAL cookie-cut path (`$57A73E`, con0 minterm
`$CA`) only when bit2 of the invincibility counter `$f9f(a5)`=`$57FDB1` is set, else the BLACK
path `$57A7E6` (con0 minterm `$0A`, fills the mask silhouette with colour 0). The counter
decrements each frame, so bit2 gives 4 frames normal / 4 frames black. Repro: level 4, hold
right → fall → fall damage. PORTED: `native_player_capture` computes `black = (fbyte&0x80) &&
!(MR8($57FDB1)&4)`; `native_wsplayer_compose` fills the mask with colour 0 (pal[0]) when black.
Verified in-app (scratch/screenshots/pfx13.png = black silhouette, pfx norm = green). NB the
player capture is double-buffered (promoted at `native_objwalk`) so the blink lags 1 frame like
the object capture — imperceptible. (The earlier "skipped draw → stale ghost" guess was WRONG.)

- **TODO remaining routines (each: find pre-clip point, capture `{src,mask,worldX,worldY,w,h,
  mode}` unclipped, draw natively):** line-blitter chains (#6); list-B (`57DB16`/`57DA40`)
  opaque objects IF any prove visible-but-missing (the caged Marry Men, once thought list-B,
  are actually the static-placement `$5A39EC` queue — DONE via `native_wsstatic_compose`).

Resolved the 2D layout empirically with the running harness (`--level 9` + `rungame`,
then `joy 0 0 0 1` to scroll right + `pcread 552A0-56400` to log tilemap reads; the read
offsets decode cleanly as (row,col)). The scroll/redraw `$57C72A` confirmed running live
(via rt wrappers — `pcwatch 57FDBA` shows 2 camera writes/frame from `fn=$57C72A`).

**TILEMAP — complete spec (everything needed to render natively):**
- **Base `$552A0`** (fixed work-buffer addr; per-level CONTENTS, stable address). Live
  bytes match the L9 savestate exactly.
- **ROW-MAJOR. Row stride is PER-LEVEL — do NOT hardcode `$F4`.** `$F4`=244B (122 cols)
  is LEVEL 9's stride; narrow levels are smaller (L1/L2 = **64B = 32 cols**). Hardcoding
  `$F4` made every non-L9 level decode the wrong rows → a whole wrong level (worst on
  narrow levels). **Read it at runtime from the row-offset/phase table `$57F4BC`:
  `entry[k].d2adj = k * rowstride` ({d2adj.w, a3adj.w} × 16), so `rowstride =
  abs(MR16($57F4BC + 4))`** (entry[1].d2adj). Confirmed by disasm of the tile-draw loop
  `$57C850` (`add.w (a0,d3),d2` steps the tilemap index by the phase table) and by dumping
  `$57F4BC` on L1 vs L9 (64 vs 244). Map height ≈ **14–16 playfield rows** (rows 0–13
  terrain, ~14–16 solid floor, row 17+ = out of playfield).
- **Index: `word @ $552A0 + row*$F4 + col*2`**, where **`col = worldX>>4` ABSOLUTELY**
  (col N covers world px [N*16, N*16+16); verified: camera_tile 56 + 21-col lead → col 78).
- **Entry word**: low byte `& $FE` selects the tile; high byte = collision/layer ATTR
  (0x00/0x03/0x04/0x05/0x0B seen — ignore for rendering).
- **Tile graphics**: `gfx_ptr = read_long($5A539E + (word & $FE))`
  = `$5D902A + ((word & $FE)/4) * $A0`. (`$5A539E` table + `$5D902A` gfx base both
  STABLE addrs, per-level data.) Each tile = **`$A0`=160 bytes, PLANE-MAJOR: 5 planes ×
  16 rows × 1 word** (16×16px, 5bpp).
- **Camera** = `word @ $57FDBA` (= a5+$0fa8, a5=$57EE12), **SIGNED** (can be negative near
  the left edge, e.g. -16 on L1). It is stored as `$fa8 = d2 - $10`, where `d2` is the
  value the engine clamps to `[$107a-$90, $107c-$100]`. So the **clamped displayed-left
  world X = `$57FDBA + 16`** (= d2). A negative `$57FDBA` is NOT an unclamped camera —
  it's the clamped 0 minus the engine's fixed 16 offset; always use `+16`. **Level edges**
  = `[$57FE8C - $90, $57FE8E - $100]` (a5+$107a/$107c); L9 = `[0, 1392]`, L1 = `[0, 160]`.
- **Wide camera (implemented in `native_render_wide_bg`, margin>0 path):** worldX maps 1:1
  to output x. If `level_w <= ow` → CENTER the level (symmetric black margins); else FOLLOW
  the player (`eng_left - (ow-320)/2`) clamped to `[level_lo, level_hi-ow]` so the view never
  reveals past an edge. Reading the tilemap by absolute worldX means no page coarse/fine
  hysteresis → no tearing; smooth scroll is the camera's fine bits. The margin==0 COMPARE
  path keeps the old engine-aligned mapping (cam16/x_off/scroll1) so `wsdiff` stays valid.
- **Palette**: from the copper list (native_renderer already parses COLORxx).

**→ Native wide renderer (decode now FULLY VERIFIED — see the 2026-06-04 cracked spec
above; note the tile SELECTION mask is `0xFFFE` not `0xFE`):** for the wide camera window,
for each visible world column `C = worldX>>4` (clamped to level edges) and each playfield
row `r` in 0..~13: `w = MR16($552A0 + r*$F4 + C*2)`; `gfx = MR32($5A539E + (w & $FE))`;
decode the 16×16×5bpp plane-major tile at `gfx` with the copper palette; place at screen
`(C*16 - camera_x, playfield_top + r*16)`. The engine's per-column incremental redraw
(phase table `$57F4BC`, double-buffer, the 21-col lead) is an OPTIMIZATION we do NOT need
to replicate — we read the full map directly. Fine scroll = `camera_x & 15`.
NOTE the masked-tile path (`$57CA80`, when `$10c4(a5)` set) handles transparent/overlay
tiles — base terrain uses the `& $FE` gfx; revisit transparency if margins look wrong.

REMAINING for Phase 3/4 (not blockers for a first wide background): confirm exact
playfield row count + top Y (DIW); per-level stability of `$5A539E`/`$5D902A` (verify on a
2nd world); parallax/background layer (if any 2nd playfield); then Phase 4 native objects.

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

### Phase 4 — character (walker/enemy) draw: DONE & PORTED (2026-06-04)

Native-drawn in the wide view (single walker, no doubling), VERIFIED in-app at
`BENEFACTOR_WIDESCREEN=480` (`scratch/screenshots/ws_char9fix.png`). Implemented as
`native_char_capture` ($57D3F4 override, pc_overrides_gameplay.c) + `native_wschar_compose`
(native_renderer.c). The earlier reverted attempt had TWO bugs, both from hand-arithmetic
instead of reading ground truth:

- **Choke point = `$57D3F4`** (descriptor BUILDER). Each object's per-type handler
  tail-jumps here (continuation after `jmp (a1,d0.w)` dispatch at `$57D3F0`), so it's hit
  ONCE PER CHARACTER with resolved values live, BEFORE the `[cam, cam+$180]` clip. It builds
  6-long descriptors into TWO queues consumed by executors `$57D6C4` (small chars) and
  `$57D688` (the w=3 walkers). Capture at entry, super-call. Regs: `d0`=worldX, `d1`=worldY,
  `d5`=anim frame offset, `a1`=descriptor.
- **Descriptor `a1`:** maskoff=`MR16(a1+0)`; BMOD=`(int16)MR16(a1+2)` (modulos hi word);
  SIZE=`MR16(a1+8)` → w=`&$3F` words, h=`>>6`; gfxBase=`MR32(a1+$A)`.
- **BUG 1 (was "DATA plane decode OPEN") — DATA base = gfxBase + 5·d5, not +d5.** The builder
  does `add.l d5,d1` (data=base+d5) then `add.w d5,d5`(×2) twice + `add.l d5,d1` again →
  base + 5·d5 (×5 for the 5 planes); MASK uses base+d5 (×1) + maskoff. The reverted attempt
  used base+d5 (×1) for DATA → scattered checkerboard noise. **DATA plane stride = h·rowstride**
  (the B-channel HARDWARE auto-advance per blit; the executor steps only the DEST by `$2A0C`/
  plane). VERIFIED against the real blits (`BLIT_LOG` fn=$57D688): bpt steps `061EB6→061F0A→…`
  = `$54` = 21·4 = h·rowstride; apt(mask)=`066C86` constant across all 5 planes.
- **BUG 2 (the "doubling" the user caught) — displayed width = rowstride/2 words, NOT w.** The
  blit reads `w`=3 words/row but row-ADVANCES only `rowstride` = `w*2+BMOD` = 4 bytes (=2 words);
  the spillover 3rd word (the fine-shift guard) is killed by `BLTALWM=$0000` (verified in the
  blit log: walkers afwm=FFFF **alwm=0000**; small chars w=2/bmod=0 afwm=alwm=FFFF). Rendering
  `w`=3 words drew that masked spillover as a doubled second body. `rowstride/2` gives the true
  packed/displayed width in BOTH cases (walker 4/2=2, small char 4/2=2). The data is packed at
  `rowstride` bytes/row.
- **Row stride (both DATA & MASK, since AMOD=BMOD) = `w*2+BMOD` = 4** on the L9 walker.

LESSON (per the user): don't hand-derive sprite bounds/strides — READ them from the actual
blits. `BLIT_LOG=1` now also logs `afwm`/`alwm`; group by `fn=` (g_rt_last_call) to find which
executor draws what and read the real per-plane bpt step (= plane stride), apt-constancy,
size, modulos, and word masks. The player ($57A666) is a SEPARATE path (B manually re-pointed,
plane stride `$2800` ≠ h·rowstride) handled by `native_wsplayer_compose`.

TODO remaining routines (other sprite loops in the margins): `$57A88A`/`$57D282` (opaque
`con0=09F0`), `$57D6C4`-fed small chars share the path (already captured), `$578974` GET READY
banner, `$578B94` HUD/text. Each: capture pre-clip at its builder, draw via the same
rowstride-derived decode.
