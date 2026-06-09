# Session handoff — renderer port (2026-06-09)

Working tree is clean; everything below is committed & pushed to `main`. Build with
`TMPDIR=scratch/tmp ./scripts/build.sh` (TMPDIR off /tmp is required — tmpfs quota).

## What this session landed (newest first)

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

- **Lever-pull single-frame X glitch — BENREN ONLY (open).** When a Marry Man pulls a lever,
  for one frame the lever (a list-A object) is drawn at the **wrong X** — displaced left/up
  of the Marry Man. **Vanilla renders it correctly; only BenRen shows it**, so it is a real
  BenRen bug (not faithful). It is the `native_wsobj` pass; the object is captured at
  `$57D8D0` (`native_objdraw_capture`, worldX = `ctx->D[0]`). Hypothesis: our captured
  `worldX` diverges from where the engine/blitter actually placed the lever for that
  transition frame (capture-timing, or the lever's draw uses a different X than D0 during the
  pull). Repro: the savestate in `logs/savestate.bin` shows it at frame 41 of a no-input
  resume (`load; pc 41; fbw …`, 960-wide benren) — and because the resume is frozen mid-pull,
  it's **persistent** in that render (frames 40–42 identical), so it's deterministic to debug.
  Isolate object pixels by diffing a normal frame vs `WS_NOOBJ`. Next: trace the lever's D0 at
  `$57D8D0` vs where Vanilla (`native_render_frame`, reads the blitter-drawn page) puts it,
  and find why BenRen's worldX is off for that frame.
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
