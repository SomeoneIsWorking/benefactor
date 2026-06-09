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

1. **CPU rasterizer** (`scene_rasterize`) — reproduces today's frame **byte-identical**.
   This proves the draw list is lossless: the precondition for any GPU backend to be correct.
2. **Vulkan** — upload each `idx` as an R8 texture + the palette LUT; draw each quad as a
   textured triangle pair; fragment shader: sample index → `discard` if cookie && index==0 →
   else `pal_rows[gl_FragCoord.y][index]`. A real per-sprite GPU draw.
3. **SDL per-sprite** — same list, `SDL_RenderCopy` per quad (so SDL is also real per-sprite).
4. Windowed present renders the list straight into the swapchain (deletes `vkCmdBlitImage`).

## Phasing & status

- [~] **P1 scene seam + CPU rasterizer**, gameplay passes emit quads, byte-identical headless.
      (objects → player → char → static → tiles → banner, converted one at a time, each verified)
      - DONE: `render/scene.{h,c}` (draw list + index-bitmap arena + per-row palette LUT +
        CPU rasterizer). Wired into both targets.
      - DONE + VERIFIED: **list-A objects** (`native_wsobj_compose`) emit index quads,
        rasterized into s_objlayer. Byte-identical at 960×282 over logs/savestate.bin
        (cmp logs/fbw_ref.bin vs logs/fbw_new.bin = 0 bytes differ).
      - NEXT: player → char → static → tilemap → banner, same one-at-a-time verify.
        Verify recipe: build HEAD ref binary (stash the seam edits) and the new binary;
        `printf 'load <abs>/logs/savestate.bin\npc 1\nfbw <tag>\nq\n' | BENEFACTOR_SKIP_PUAE=1
        BENEFACTOR_WIDESCREEN=960 ./build/benefactor-harness Disk.1 Disk.2 Disk.3`; `cmp`.
- [ ] **P2 Vulkan consumer** of the list, verified vs CPU reference by readback.
- [ ] **P3 SDL per-sprite consumer.**
- [ ] **P4 windowed present** renders the list (kill the blit).
- [ ] **P5 per-character lighting** pass (the original motivation; now has per-sprite quads).

## Verification

Headless, no display: drive the harness to gameplay (`load logs/savestate.bin`, `pc 1`),
dump `s_out`, and diff. P1 target = byte-identical to the pre-refactor frame. P2+ target =
max channel diff 0 vs the CPU rasterizer (same as the existing `--vk-selftest`).

## Rules

- Faithful-first: P1 must not change a pixel. Enhancements (lighting, filtering) come after.
- No flatten-before-backend ever again. If a pass can't be expressed as a quad, RE it into
  one — do not fall back to "compose in software then blit".
