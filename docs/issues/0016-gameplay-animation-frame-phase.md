---
id: 16
title: Gameplay animation advances one frame before the static oracle
status: investigating
symptom: The level-1 GET READY handoff advances the first gameplay animation one frame early; the first SFX trigger and PCM then differ.
state_items: S005,S023
tags: audio,animation,interpreter,timing
created: 2026-09-12
updated: 2026-09-12
---

In a direct level-1 run with fire at frame 200, both products present the
playfield at frame 248. Guest memory differs by 91 bytes through frame 460
(gameplay offset +212), largely structural plus the beam-fed RNG state. At
frame 461 (+213), the interpreter's animation cursor at `$5804B8` is 4 while
the static oracle's is 2; the guest memory difference jumps to 1,999 bytes.
The interpreter calls the native SFX trigger for descriptor `$585A96` at frame
464; the oracle calls the same trigger at frame 465. PCM first differs inside
frame 464. The descriptor is accepted by both products.

The `$1E.w` mode word also differs (2 in the interpreter, 0 in the oracle's
direct-level developer path), but preserving 0 experimentally did not change
the frame-464 SFX trigger. The SFX rejection path is not reached in this
scenario. Both accepted and rejected native SFX now use the replacement
adapter's automatic RTS (issue 0019), so this frame-phase difference is not
evidence for that path.

The interpreter's wait trace identifies the boundary: the card's VPOSR wait at
`$57859E` presents frame 460 and resumes on frame 461, then the first gameplay
scanline poll at `$577130` completes **within frame 461** (`461->461`). The
interpreter therefore runs the first 23-subsystem gameplay pass immediately;
later `$577130` polls present once per frame. The normal loop branches back at
`$5771FA` and does not reach the VPOSR waits near `$577262`/`$57726A` (those
belong to another path). The static oracle instead implements VHPOSR as a
synthetic counter incremented by one line per read, yielding on its 312-line
wrap. That model places the first animation pass one displayed frame later.

The interpreter's guest-cycle beam and the verified static oracle's per-read
beam are different timing models. Use the static recomp as the behavioral
oracle, as requested, but do not insert a one-address yield at the banner
transition. The next discriminator is the title-owned clock/wait rule across
the card and first gameplay scanline poll; a correction must preserve steady
gameplay pacing too. The 20 ms audio phase difference remains open. PUAE is not
part of this investigation.
