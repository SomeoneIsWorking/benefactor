---
id: 24
title: WASM disk start loses browser heartbeat
status: resolved
symptom: The Pages picker accepts a valid Disk.1-Disk.3 ZIP, then Chrome/WebLua loses its heartbeat when native startup enters pc_step.
state_items: S030,S023
tags: wasm,browser,pthreads,release
created: 2026-09-12
updated: 2026-09-12
---

## Root cause

The picker was not the failure: WebLua accepted a locally assembled ZIP with
the three authenticated disk files and the native bridge entered
`pc_init_from_disk`. The web target then used the desktop `pthread` frame
handoff without enabling Emscripten pthreads. `pc_step_threaded()` waited for a
game worker that could not run, blocking the browser event loop until the
DevTools heartbeat expired. A no-thread shortcut is not valid because native
fade and menu owners yield from inside their C call stacks.

## What was tried / dead ends

The earlier live package was tested with and without WebLua's GPU flag. Both
runs accepted the valid ZIP and then lost the Chrome heartbeat, ruling out a
GPU-only presentation failure. Replacing the handoff with a synchronous
single-thread call would not preserve the native C stack across the repeated
fade/menu waits, so it is not an acceptable fix.

## Resolution

The current source enables the Emscripten pthread pool and adds a bounded
same-origin service-worker isolation handshake for GitHub Pages. The worker
adds COOP, COEP, and CORP headers, the page reloads at most once after
registration, and the generated runtime loads only after
`crossOriginIsolated` is true. Hosted run `34708373281` built the pthread
package, and Pages run `34708732681` deployed it. A fresh WebLua session at
the live route accepted the authenticated three-disk ZIP, reached native boot,
continued past frame 500, and remained alive with `crossOriginIsolated=true`
and an active service worker. The live canvas was producing frames, so the
heartbeat loss was the missing web pthread/isolation contract rather than a
disk-picker failure.
