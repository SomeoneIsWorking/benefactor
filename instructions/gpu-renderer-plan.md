# GPU renderer plan — making the renderer a *renderer*, not a software blit

## The problem (2026-06-09)

The "Vulkan backend" (`src/render/present_vulkan.c`) is **not a renderer**. The whole
gameplay frame is composed in software by `native_render_wide_bg()` into one ARGB surface
(`s_out`), then the backend just uploads that surface as one texture and draws it
(`vkCmdBlitImage` for the window, a fullscreen-quad sampler for the self-test). SDL and
Vulkan are therefore byte-identical blits — "render each sprite via SDL or Vulkan" is
impossible by construction, because every sprite is already flattened into one image
before any backend code runs.

The user's actual ask (multiple sessions): **each sprite is a drawable that the SDL or
Vulkan backend renders itself**, so it "functions like a real PC game" — and per-character
lighting has per-sprite quads to attach to.

## Two renderer modes (2026-06-09) — Vanilla | BenRen

The frame renderer is now a named, configurable mode (the `renderer` cfg knob;
`pc_render_mode()` in `port/config.c`). This is the FRAME renderer — distinct from the
*present backend* (`BENEFACTOR_RENDER=sdl|vulkan`), which is only how the finished surface
reaches the screen.

- **Vanilla** — `native_render_frame` (copper-walk, bitplane, Amiga-blit-faithful, 352px).
- **BenRen** — the sprite-based draw-list renderer below (no Amiga blit). **Owns widescreen.**

The old third option, "WSRend" (the per-pixel widescreen compositor `native_render_wide_bg`),
is **dropped as a mode** — it was just BenRen mid-construction (a draw list that's still
flattened on the CPU). It keeps the function name for now but is the BenRen compose; the work
below removes its per-pixel rasterizer in favour of per-sprite consumers.

Selection: `renderer=vanilla|benren` (ENV `BENEFACTOR_RENDERER`, REPL `cfg renderer <v>`, or
JSON). Unset = AUTO: BenRen when a widescreen width is requested, else Vanilla (so existing
`BENEFACTOR_WIDESCREEN` users keep the wide render). `BENEFACTOR_WS_CMP` still forces the
BenRen compose at 352 for the vanilla-vs-benren pixel diff.

## Why it's feasible (the RE is already done)

Every gameplay drawable is *already* an enumerated descriptor at the compose functions in
`native_renderer.c`:

| pass | source | descriptor |
|------|--------|-----------|
| tilemap bg     | `$552A0` map + `$5A539E` gfx + camera | 16×16 5-plane tile grid |
| list-A objects | `native_wsobj_*`    ($57D8D0 capture) | cookie-cut 5-plane sprite |
| player         | `native_wsplayer_*` ($57A666 capture) | 5-plane data + 1-plane mask |
| characters     | `native_wschar_*`   ($57D3F4 capture) | 5-plane data + 1-plane mask |
| static / MM    | `native_wsbuild_*`  ($57B19E/$57B856) | gfx-table sprite |
| banner         | `native_wsbanner_*` ($578974 capture) | box + tel-anim + text |

Nothing more needs reverse-engineering. The descriptors are flattened too early; the fix
is to move the flatten point *past* the backend boundary.

## Architecture — a draw list (scene) the backend consumes

`render/scene.{h,c}` defines the seam:

- `Scene`: a per-frame draw list + an index-bitmap arena + a per-scanline palette table
  `pal_rows[H][32]` (this engine's palette is **per-scanline** via the copper; the GPU path
  carries it as a per-row LUT so colour effects stay exact — no single-constant-palette
  shortcut).
- `Quad { int x,y,w,h; const uint8_t *idx; int stride; uint8_t cookie; }` — `idx` is a
  colour-INDEX bitmap (0..31), `cookie`=1 means index 0 is transparent. NO resolved ARGB in
  the quad; the consumer does the palette lookup. This is exactly what a GPU sampler does.

Consumers (each verified headless via offscreen readback — the dev box display is off):

1. **CPU rasterizer** (`scene_composite_argb`) — reproduces today's frame **byte-identical**.
   This proves the draw list is lossless: the precondition for any GPU backend to be correct.
2. **SDL per-sprite** (do first) — same list, `SDL_RenderCopy` per quad (so SDL is a real
   per-sprite renderer). This is the user's priority: get SDL fully set before Vulkan.
3. **Vulkan** — upload each `idx` as an R8 texture + the palette LUT; draw each quad as a
   textured triangle pair; fragment shader: sample index → `discard` if cookie && index==0 →
   else `pal_rows[gl_FragCoord.y][index]`. A real per-sprite GPU draw.
4. Windowed present renders the list straight into the swapchain (deletes `vkCmdBlitImage`).

## Phasing & status

- [~] **P1 scene seam + CPU rasterizer**, gameplay passes emit quads, byte-identical headless.
      (objects → player → char → static → tiles → banner, converted one at a time, each verified)
      - DONE: `render/scene.{h,c}` (draw list + index-bitmap arena + per-row palette LUT +
        CPU rasterizer). Wired into both targets.
      - DONE + VERIFIED: **list-A objects** (`native_wsobj_compose`) emit index quads,
        rasterized into s_objlayer. Byte-identical at 960×282 over logs/savestate.bin
        (cmp logs/fbw_ref.bin vs logs/fbw_new.bin = 0 bytes differ).
      - DONE (seam, CPU-rasterized): player, char, static now emit index quads too
        (the per-pass `s_objlayer` writes were replaced by `scene_add_quad` + a single
        `scene_composite_argb`). NEXT on the list: **tilemap bg + banner** (still per-pixel
        in `native_render_wide_bg`), so the WHOLE frame is a draw list.
        Verify recipe: build HEAD ref binary (stash the seam edits) and the new binary;
        `printf 'load <abs>/logs/savestate.bin\npc 1\nfbw <tag>\nq\n' | BENEFACTOR_SKIP_PUAE=1
        BENEFACTOR_RENDERER=benren ./build/benefactor-harness Disk.1 Disk.2 Disk.3`; `cmp`.
- [~] **P2 SDL per-sprite consumer** (user: "get SDL fully set in first").
      - DONE + VERIFIED: `render/scene_sdl.c` — `scene_draw_sdl()` draws each quad as its own
        SDL texture via `SDL_RenderCopy` (real per-sprite, not a blit). SDL2 has no fragment
        shader, so the per-row palette is CPU-resolved when baking each quad's texture
        (transparent -> alpha 0; drawn -> opaque 0xFF|RGB). `scene_sdl_selftest()` diffs it vs
        the CPU rasterizer via a software renderer + readback (display off). Harness `scenesdl`:
        34 quads @ 2560x282 = **0 px differ, BYTE-IDENTICAL** over logs/savestate.bin.
      - DONE (P2b/P4, 2026-06-10): **windowed per-sprite present.** The whole frame is on the
        list (tiles 2502cbf; ropes + banner 2044cb3 — banner as SCREEN-space quads via
        `SCENE_SPACE_SCREEN` + `scene_composite_screen_argb`; the Scene carries the camera
        view: `view_left` + world clip). `scene_draw_sdl_window()` draws the window frame
        per-sprite: black clear (void) + non-scene rows (top border/HUD) from `s_out` as the
        base + world quads camera-projected (`x - view_left`, clipped to the world range) +
        banner quads on top. Wired as the optional `present_scene` backend hook (SDL only);
        `hw_present_frame` routes to it for a FRESH BenRen scene with no PC overlay active
        (pause/level-select/toast read-modify `s_out`, so those frames fall back to the blit —
        port the overlays to quads to remove the fallback). GATE: harness `scenewin` renders
        the windowed frame offscreen and byte-diffs vs `s_out` — 0 px on the lever savestate
        (435 quads) and a level-entry banner frame (440 quads). NOTE: the CPU composite still
        runs every frame (s_out stays the reference for harness dumps/overlays); skipping it
        when the per-sprite path presents is a later optimization. Quad textures are also
        re-baked per frame — cache by `idx` pointer when perf matters.
- [SHELVED 2026-06-10] **P3 Vulkan consumer** of the list (the per-character-lighting path —
      SDL2 can't shade, Vulkan can). User decision: bigger priorities; the present_scene seam
      and the draw list are ready for it whenever it resumes.
- [x] **P4 windowed present** — done via the SDL per-sprite path above (the Vulkan
      `vkCmdBlitImage` variant is shelved with P3).
- [ ] **P5 per-character lighting** pass (the original motivation; needs P3 or an SDL-feasible
      approximation, e.g. per-quad colour modulation — SDL_SetTextureColorMod is per-sprite).

## Verification

Headless, no display: drive the harness to gameplay (`load logs/savestate.bin`, `pc 1`),
dump `s_out`, and diff. P1 target = byte-identical to the pre-refactor frame. P2+ target =
max channel diff 0 vs the CPU rasterizer (same as the existing `--vk-selftest`).

## Rules

- Faithful-first: P1 must not change a pixel. Enhancements (lighting, filtering) come after.
- No flatten-before-backend ever again. If a pass can't be expressed as a quad, RE it into
  one — do not fall back to "compose in software then blit".
