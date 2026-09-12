---
id: 25
title: SDL web renderer cannot find the canvas
status: resolved
symptom: After pthread-enabled WASM startup, SDL Emscripten createContext receives null because the HTML canvas id is benefactor-canvas instead of canvas.
state_items: S030
tags: wasm,browser,sdl,render
created: 2026-09-12
updated: 2026-09-12
---

## Root cause

The SDL3 Emscripten video backend's default window path resolves the browser
canvas through the conventional `#canvas` element. Benefactor supplied a
semantic `#benefactor-canvas` id, so the renderer reached
`createContext(null)` after native startup and failed before its first frame.
This was exposed only after issue 0024 allowed the pthread-backed game flow to
reach the renderer.

## What was tried / dead ends

The failure was not a HiDPI filter or a missing disk asset: the live browser
reported `crossOriginIsolated=true`, accepted the authenticated ZIP, and logged
the SDL fallback before the null-canvas exception. Renaming the canvas is the
SDL platform contract; adding a second hidden canvas would create two possible
presentation owners and is not acceptable.

## Resolution

The browser package now uses `id="canvas"` and keeps the pixelated CSS rule on
that SDL-owned element. The focused release-builder gate checks the required
canvas contract. Hosted run `34708373281` and Pages deployment
`34708732681` contain the fix. The live WebLua run accepted the authenticated
ZIP, created a `704x564` SDL canvas with `image-rendering: pixelated`, and
captured rendered gameplay without the previous null-canvas exception.
