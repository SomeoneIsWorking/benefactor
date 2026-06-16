# Rendering overhaul — 3 renderers + BenRen VK per-sprite GPU renderer (2026-06-16)

User direction: three renderers in a RENDERING submenu, and a REAL Vulkan renderer
that draws the PC-owned sprites on the GPU (not a post-process of a flattened CPU
frame). Per-object shaders are the longer-term goal, which is why it must be a
per-sprite GPU renderer.

## The three renderers

| Menu      | `renderer` | `present` | What it does                                  |
|-----------|------------|-----------|-----------------------------------------------|
| VANILLA   | vanilla    | sdl       | Amiga copper/blitter byte-faithful render     |
| SOFTWARE  | benren     | sdl       | BenRen — CPU per-sprite composite (s_out)     |
| HARDWARE  | benren     | vulkan    | BenRen VK — GPU per-sprite renderer           |

Aspect ratio (STOCK 4:3 / 16:9 / AUTO → `widescreen_mode`) is its own RENDERING-page
row, independent of the renderer.

## Window is created ONCE — renderer switching never recreates it

The present (window-owning) backend is chosen at init: **Vulkan whenever available,
else SDL** (`hw_resolve_backend`). It owns the window for ALL three renderers. The
Software<->Hardware choice is NOT a backend swap — it's read live each frame by
`hw_scene_render_enabled()` (= renderer==benren && present==vulkan), which decides
whether hw_present_frame calls the backend's per-sprite `present_scene` (Hardware) or
the composite `present` of s_out (Software/Vanilla/overlays). So the pause-menu
RENDERER row switches instantly with no window/backend teardown. (`BENEFACTOR_RENDER`
still force-selects a backend.)

## BenRen VK — the per-sprite renderer (present_vulkan.c)

The Vulkan twin of `scene_sdl.c`. It consumes the existing BenRen `Scene` draw list
(render/scene.c — one quad per sprite/tile/banner, already built every frame) and
draws each quad on the GPU:
- A sprite ATLAS image: every quad is shelf-packed + its texels baked (per-output-row
  palette resolve, same as scene_sdl's atlas_pack_bake) into a 1024x2048 atlas,
  uploaded once per frame.
- One textured-quad pipeline (`shaders/quad.vert` + `quad.frag`, push constants per
  draw): dst rect in content px (vertex shader maps content->window), src rect in
  atlas UV, plus a flat-colour mode for the per-row void background. NEAREST sampler
  (pixel-art crisp). Discard transparent texels → painter's order composites exactly.
- Per frame: base bands (top border + HUD from the composed surface) → per-row void →
  world sprite quads (camera-projected `x - view_left`, scissor-clipped to camera
  columns + playfield rows) → screen/banner quads on top.
- `present()` (no scene) is the overlay/Software/Vanilla fallback: one fullscreen
  quad of s_out.
- Atlas overflow → falls back to the composite present (matches scene_sdl).

Because each sprite is its own draw with its own push constants, per-object shaders /
lighting hang off here next (the FxFrame from `set_effects` is already stored).

### Vulkan correctness details (learned the hard way)
- **Per-image `sem_done`**: a single render-done semaphore reused each frame trips
  `VUID-vkQueueSubmit-pSignalSemaphores-00067` (present may still hold the prev one).
  Fixed with one semaphore per swapchain image, indexed by the acquired image.
- **Dynamic resolution**: the swapchain tracks the window's live drawable size
  (`SDL_Vulkan_GetDrawableSize`), rebuilt when it changes (`vk_check_resize` each
  frame + OUT_OF_DATE). Required on Wayland, where surface `currentExtent` is
  "undefined" and we must supply the size — otherwise the compositor upscales (blur).
- Swapchain images are COLOR_ATTACHMENT (render pass), dynamic viewport+scissor so a
  resize rebuilds only swapchain+framebuffers, not the pipeline.

## Effects (FUTURE — not in this push)

The `fx_ambient` / `fx_torch` / `fx_charglow` cfg knobs + `native_fx_flags()` +
FxFrame publish + `set_effects` seam are all in place but NOT yet applied. The
EFFECTS submenu + the GPU lighting/glow pass are the next phase, done as part of the
per-sprite scene render (per-object) — NOT as a screen-space filter on a flattened
frame (that was rejected: it can't do per-object shaders). `shaders/effects.frag`
exists as a starting point but is not wired.

Also future: per-object material/shader ids on the scene quads; graphics remake
(blocked on commissioned art); per-torch level light sources (needs RE — today only
the player's projected position is a light).

## Files
- render/effects_frame.h (NEW) — FxFrame + flags + publisher/flags getters.
- render/shaders/quad.{vert,frag} (NEW) — per-sprite textured/flat quad.
- render/shaders/effects.frag (NEW, unwired) — for the future lighting pass.
- render/native_effects.{c,h} — now just `native_fx_flags()`.
- render/native_renderer.c — publishes the FxFrame (no CPU effect mutation).
- render/present_backend.h / present_sdl.c — `set_effects` hook (sdl = NULL).
- render/present_vulkan.c — BenRen VK per-sprite renderer (replaces the blit present).
- engine/hw.c — Vulkan-owns-the-window resolution + `hw_scene_render_enabled`.
- port/config.c — present + fx_* knobs.
- port/pause_menu.c — RENDERING submenu (RENDERER 3-way + ASPECT).
- CMakeLists.txt — compile quad.{vert,frag} + effects.frag → .inc.
