---
id: 26
title: Level complete stalled the frame: the guest's fire poll was not a wait idiom
status: resolved
symptom: Beating level 1 (or /complete) left the frame unfinished; the watchdog reported PC $005772DC spinning and exited 2
state_items: S001
tags: gameplay,wait,watchdog,input
created: 2026-09-14
updated: 2026-09-14
---

## Root cause

The level-complete sequence ends in the banner's "press fire to continue" wait at
`$5772D6`:

```
5772D6  tst.b $bfe001.l     ; CIA-A PRA bit 7 = /FIR1, the fire button (active low)
5772DC  bmi.b $5772d6       ; loop while the bit reads high, i.e. while fire is up
```

Two instructions, no body — but it polls the **input**, not a custom register, so
`src/port/wait_idiom.h` did not recognise it. The recogniser only knew the beam
(`VPOSR`), the scanline (`VHPOSR`) and the blitter (`DMACONR`) polls, all of which
the host can satisfy from its own frame model. A joystick poll cannot be: this port
latches the player's input once per frame, in `hw_set_joystick`/`apply_bound_input`,
so a spin that never returns to the frame driver can never observe the press. The
guest looped forever inside one frame, the beam never advanced, and
`src/engine/hw_watchdog.c` reported the unfinished frame and called `_exit(2)`.

Measured on the phone as `dumpsys activity exit-info` `reason=1 (EXIT_SELF)
status=2` and reproduced headlessly: the report named `pc $005772DC`,
`call $0057901E` (the runtime's hardware-read site) and `last hw read $00BFE000`,
with the retired-instruction ring alternating `$005772DC`/`$005772D6`.

## What was tried / dead ends

An offline scan of the decrunched gameplay image found **exactly one** such poll,
and only in the `tst.b`/`bmi` form (no `btst` form and no CIA-B poll), so the
recogniser accepts exactly that shape and nothing broader. Registering the poll by
address (`/complete` on the control channel sets the same win flag the game's own
teleport-out sets) made the stall reproducible in seconds without a play-through.

Earlier attempts to read the stall from the device went nowhere: the product's
logger writes to stderr, and an Android application has no console, so the fatal
report never reached logcat (fixed separately by `shared/android-port`'s
stdio→logcat redirect).

## Resolution

`PC_WAIT_FIRE` was added to the wait-idiom recogniser: a `tst.b $BFE001` followed by
a short `bmi`/`bpl` branch straight back to it, with `wait_pressed` recording the
polarity (`bmi` waits for a press, `bpl` for a release). Its native body calls
`hw_wait_fire(wait_pressed)`, which yields one frame per test until the button
state changes, and then `rt_continue_original` — so the guest's own `tst`/`bmi`
pair still sets the flags and takes the branch, rather than the port jumping over
it. `tests/test_wait_idiom.c` covers both polarities with the image's real bytes
plus the negative shapes (branch not back to the test, a body between the two
instructions, another register, `beq` on the port byte, and an 8-byte read at the
end of memory).

Verified on the desktop product: the banner holds with frames advancing and no
watchdog report, fire at each prompt continues the sequence, and level 1 completes
into a running level 2 (`$20.w` 1→2 at the win's `addq.w #1,$20.w`, the level card
loads the differing level, and frames keep advancing afterwards). Before the fix
the same run died with the watchdog report above.
