---
id: 27
title: Unused screen area rendered white under the touch overlay
status: resolved
symptom: Letterbox bars beside the game frame showed the touch overlay's pale fill colour (242,247,250) instead of black
state_items: S011
tags: render,touch,present
created: 2026-09-14
updated: 2026-09-14
---

## Root cause

`src/render/present_sdl.c` cleared the renderer with a bare `SDL_RenderClear`, so
the clear used whatever colour the renderer was last set to. The touch overlay
draws its glyphs through the same renderer in the same frame, and its light
`#F2F7FA` fill is the last colour set — so every area the game frame does not cover
inherited that fill, and the bars appeared white.

Measured with `adb exec-out screencap`: the same four corner samples read
`srgba(242,247,250,1)` during gameplay with the overlay visible, exactly the
overlay's fill colour, while a pre-overlay phone capture had black bars.

## What was tried / dead ends

Nothing to try: the leftover colour and the bar colour were identical, which
identifies the owner. Treating it as a scene/copper problem would have been the
wrong owner — the playfield itself was correct.

## Resolution

The presenter now owns an explicit opaque black clear (`sdl_clear_frame`) and both
present paths use it, so the background the game frame does not cover is a
presentation decision rather than a side effect of the last pass. The same corner
samples on the emulator, in gameplay with the overlay drawn, now read
`srgba(0,0,0,1)`.
