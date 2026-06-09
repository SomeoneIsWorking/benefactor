# Session handoff — renderer port (2026-06-10)

Working tree is clean; everything below is committed & pushed to `main`. Build with
`TMPDIR=scratch/tmp ./scripts/build.sh` (TMPDIR off /tmp is required — tmpfs quota).

## What this session landed (newest first)

- `1e87a63` **Lever-pull X glitch FIXED** — BenRen now mirrors the engine's page
  persistence: `native_objdraw_capture` checks whether `$57D8D0` actually emitted a blit
  (a3/a6 queue advance) and, for an in-window object the engine dirty-rect-skipped, reuses
  the object's last-emitted entry instead of the transient capture. See "Open bugs" below.
- `2502cbf` **Tilemap background on the BenRen draw list.** The terrain was the last
  per-pixel loop in `native_render_wide_bg`; now `native_wstiles_compose` emits each
  visible tile as an opaque 16×16 colour-index quad, and the main loop is a pure camera
  projection (map world X → screen, clip to `[mincol,maxcol)`). **The whole gameplay frame
  is now one scene draw list** (tiles + objects + player + chars + static + ropes).
- `a474fb4` **Own object handler `$59AC38`** + `mpset` REPL poke (see "W6L2" below).
- `27a90e8` reverts `0ed54f9` (savestate low-RAM repair — rejected; we don't prop up broken
  savestates).
- `9713a62` / `ec1efbe` **Chandelier ropes** rendered natively (blitter LINE-mode), incl.
  un-culling them past the vanilla window (capture pre-clip at `$57DCD4`).
- `fdcdc87` **Widescreen object clip** — objects no longer leak into the hidden void.
- `bf612c5` / `748d11a` / `0152f8a` / `32f6e5f` **Renderer modes + scene draw list + SDL
  per-sprite consumer** (see below).

## Renderer architecture (current)

Two **frame-renderer** modes, selected by the `renderer` cfg knob (`pc_render_mode()` in
`port/config.c`; ENV `BENEFACTOR_RENDERER` / REPL `cfg renderer <v>` / JSON; unset = AUTO
= BenRen when widescreen, else Vanilla). This is distinct from the **present backend**
(`BENEFACTOR_RENDER=sdl|vulkan`, only how the finished surface reaches the screen).

- **Vanilla** = `native_render_frame` — copper-walk, bitplane, Amiga-blit-faithful, 352px.
- **BenRen** = `native_render_wide_bg` (`render/native_renderer.c`) — sprite-based, owns
  widescreen. Every pass emits **colour-index quads** to `s_scene` (`render/scene.{h,c}`):
  `native_wstiles_compose` (bg) → `native_wsobj_compose` → `native_wsplayer_compose` →
  `native_wschar_compose` → `native_wsstatic_compose` (Marry Men) → `scene_composite_argb`
  → `native_wsrope_compose`. Then the projection loop copies `s_objlayer` → output.
  - Sprites/objects are captured from the engine via overrides (`port/overrides/`,
    registered in `register.c`): objects `$57D8D0` (`native_objdraw_capture`, worldX=D0/
    worldY=D1), chars `$57D3F4`, player `$57A666`, Marry Men `$57B19E`/`$57B856`, ropes
    `$57DCD4` (pre-clip), banner `$578974`/…
  - "WSRend" (the old per-pixel widescreen path) was dropped — it WAS BenRen mid-build.

Consumers of the draw list:
1. **CPU rasterizer** `scene_composite_argb` — the reference; current runtime path.
2. **SDL per-sprite** `scene_draw_sdl` (`render/scene_sdl.c`) — draws each quad as its own
   SDL texture. Verified byte-identical to the CPU rasterizer via `scene_sdl_selftest`
   (harness `scenesdl`): **436 quads (tiles+sprites) @ 2560×282, 0 px differ**. SDL2 has no
   fragment shader, so the per-row palette is CPU-resolved when baking each quad's texture.
3. Vulkan present (`render/present_vulkan.c`) — still just a fullscreen-quad **blit** of the
   finished surface (offscreen self-test + windowed swapchain that falls back to SDL). Not a
   per-sprite consumer yet.

## Next steps (the rendering port)

Plan + phasing live in `instructions/gpu-renderer-plan.md`. Now that the whole frame is a
draw list and the SDL consumer reproduces it byte-identical, the next steps:

- **P4 windowed per-sprite present** — make BenRen's SDL backend draw the draw list straight
  to the window (background + per-sprite quads) instead of CPU-compositing into `s_objlayer`
  and blitting the whole surface. NOTE the seam: the scene/`s_objlayer` is **world-space**
  (absolute worldX, capped at `WS_LAYER_W`=2560); the final camera projection (worldX→screen
  + `[mincol,maxcol)` clip) happens in the projection loop, NOT in the consumer. A windowed
  per-sprite present needs that projection applied per-quad (a screen-space draw list, or the
  consumer applies the camera transform).
- **P3 Vulkan per-sprite consumer** of the same list (the per-character-lighting path; SDL2
  can't shade, Vulkan can) — verify vs CPU/SDL by offscreen readback.
- **P5 per-character lighting** (the original motivation).

## Open bugs / notes

- **Lever-pull single-frame X glitch — FIXED 2026-06-10.** Root cause was NOT a capture
  bug. The lever (a list-A object) has an embedded handler that, while a Marry Man pulls it,
  computes a transient garbage `worldX` (D0=2 vs the real 144) for exactly one frame.
  The engine's `$57D8D0` dirty-rect / incremental redraw **skips the blit** that frame
  (measured: real-blit queues a3/a6 do not advance → `realblit_bytes=0`), so the page —
  and Vanilla — keep the lever at its last-committed 144. BenRen has no page; it rebuilt the
  whole frame from captures and drew the transient X=2. **Fix** (`native_objdraw_capture`):
  measure whether the engine actually emitted a blit (a3 saved on the stack at the dispatch
  movem `$57D816/$57D8CA` → read `MR32(a7+4)` at `$57D8D0` entry; a6 read directly). If it
  skipped AND the object is inside the original 320px window (dirty-rect "unchanged", not an
  off-screen cull), reuse the object's **last-emitted** entry by identity (the descriptor
  ptr `ctx->A[1]`, now stored as `WsObj.id`) — i.e. mirror page persistence. Outside the
  window = a widescreen-margin sprite the engine never blits → draw at current (so margin
  sprites still animate). Verified: void ghost gone, lever renders at 144 matching Vanilla
  (`load; mpset 1890 200 w; pc 41; fbw …`).
- **W6L2 / corrupt-savestate handling (done, by design).** `$59AC38` is an object handler
  whose gate at `$59AC56` checks the loader low-RAM invariant (`$1890==$0200`). A stale
  savestate that bypassed the loader has `$1890=0`, the gate fails, and the engine bails to
  `jmp(a0=object)` → wild jump into object DATA (the bogus "rt_call: NO FUNCTION at $58081A").
  We **own** `$59AC38` (`native_obj_anim_59AC38`): delegates to the recompiled body when the
  gate passes, else prints a clear diagnostic + `abort()` in our code (broken savestates are
  expected to fail, just diagnosably — we do NOT repair them). Use `mpset 1890 0 w` to break /
  `mpset 1890 200 w` to fix for testing.
- **PUAE is down in-harness** (WHDLoad FS missing → `/tmp/WHDLoad/*` → segfault on boot).
  Not fixed (deprioritized). Blocks the PUAE oracle; use `goto N` / the savestate + the
  Vanilla path as the reference instead.

## Tooling added this session (harness REPL)

- `scenesdl` — verify the SDL per-sprite consumer == CPU rasterizer on the current BenRen
  scene (run after a benren gameplay frame).
- `scenepal <y>` — dump the BenRen per-scanline palette (`scene.pal_rows[y]`); the gameplay
  palette is per-row/copper so the old `pal` (g_state) is empty in-game.
- `mpset <hex addr> <hex val> [w|l]` — poke PC `g_mem` (big-endian) for bad-state testing.
- Pass toggles: `WS_NOTILES`, `WS_NOOBJ`, `WS_NOROPE`, `WS_NOBANNER` (env) isolate passes.

## Verify recipe (byte-identical gate for BenRen changes)

Must run in actual **gameplay** (`cop1lc=$003484`), not the level card (`$003914`, where the
wide path early-returns). `goto N` stops on the card; the savestate is in gameplay but its
`$1890=0` aborts on the handler — `mpset 1890 200 w` first. Build HEAD ref (stash the edit)
and the new binary, then:
`printf 'load <abs>/logs/savestate.bin\nmpset 1890 200 w\npc 1\nfbw <tag>\nq\n' |
 BENEFACTOR_SKIP_PUAE=1 BENEFACTOR_WIDESCREEN=960 BENEFACTOR_RENDERER=benren
 ./build/benefactor-harness Disk.1 Disk.2 Disk.3`; `cmp`. PNG via `scratch/fbw2png.py <bin> <png> 960`.
