---
id: C001
kind: claim
status: holds
created: 2026-09-12
tags: render,oracle
depends: src/render/present_sdl.c, platforms/web/index.html
---

## Claim

The current SDL and browser presentation paths preserve authored pixels and reproduce the oracle gameplay frame

## Evidence

Static-oracle AE=0 at 16:9 level-1 frame 600 and levels 10, 30, 60 frame 600; real SDL X11 window 1000x564 downsampled with point filtering is AE=0 against its composed 500x282 frame; full verifier passed.

## What would falsify it

a matched current-build window or browser capture differs from the composed frame or static oracle at the same gameplay checkpoint
