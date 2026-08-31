# Project state

## Current focus

S005 is the current focus.

## Comparison baseline

The baseline is the unmodified 1994 Amiga release running under a conventional emulator, including
its original 320-pixel presentation, controls and jump behavior, password flow, floppy timing, and
Amiga startup sequence.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | The game boots and plays natively from player-supplied disks without an Amiga emulator in the shipping process | verified | — | G001 |
| S002 | A native pause and options interface applies and persists modern settings without editing configuration files | verified | S001 | G001 |
| S003 | Keyboard, hot-pluggable controllers, touch input, rebinding, and an optional alternate control scheme work in the shipping game | verified | S001 | G001 |
| S004 | AppImage and Android releases provide no-terminal disk setup without packaging game assets | verified | S001 | G001 |
| S005 | Recompiled M68K gameplay behavior is replaced by readable native engine subsystems | partial | S001 | G001 |
| S006 | A side-by-side PUAE comparison harness and runtime debugging tools can drive, inspect, capture, and compare execution | verified | S001 | G001 |
| S007 | Turbo, hyper, and hold-to-fast-forward increase game speed while music and sound effects remain at normal speed | verified | S001, S002 | G001 |
| S008 | An optional platformer-physics model adds variable-height jumping, air control, momentum, and tunable motion while preserving classic physics | verified | S001, S002 | G001 |
| S009 | A 60-level selector, completion progress, and level locks replace password entry | verified | S001, S002 | G001 |
| S010 | Selectable native software and hardware renderers add ambient-darkness and character drop-shadow effects while retaining the faithful renderer | verified | S001, S002 | G001 |
| S011 | True widescreen renders additional simulated world at 16:9, 21:9, or the live window width instead of stretching the original frame | verified | S001, S010 | G001 |
| S012 | Native one-frame boot bypasses the Amiga startup sequence, and direct disk loading removes multi-second floppy loading waits | verified | S001 | G001 |
| S013 | A free camera can detach from gameplay and pan in real time or while paused | verified | S001, S002, S011 | G001 |
| S014 | The original Easy, Normal, and Hard difficulty selector is restored and usable from the main menu | verified | S001 | G001 |
| S015 | Skip-intro, unlock-all-levels, and light-or-disabled fall-damage options provide cheats and accessibility controls | verified | S001, S002 | G001 |
| S016 | Pickup and interaction reach can be extended independently of the selected control scheme | verified | S001, S002 | G001 |
| S017 | Savestates, direct level entry, headless execution, frame profiling, and runtime probes support development and testing | verified | S001 | G001 |
| S018 | Player-facing save slots include names, timestamps, and screenshot previews | missing | S017 | G001 |
| S019 | Hold-to-rewind restores recent game states through a bounded savestate history | missing | S017 | G001 |

## Capability details

### S001 — Native playable product

Evidence: the documented `benefactor-pc` product boots directly from the three validated disk images,
reaches all 60 levels, and runs without PUAE, Kickstart, Workbench, or another Amiga emulator in the
shipping process.

### S002 — Native pause and options interface

Evidence: the in-game pause surface provides Resume, Options, Retry, Exit to main menu, and Quit, and
its Graphics, Controls, and Extra pages apply supported settings live and persist them to
`benefactor.json`.

### S003 — Native and alternate input

Evidence: keyboard, SDL controller, and Android touch paths share logical actions; controllers are
hot-pluggable; keyboard and controller bindings can be captured in game; and classic and alternate
Interact/Drop control policies can be selected independently per device.

### S004 — Packaged setup

Evidence: the AppImage selects a disk folder through a graphical first-run path and Android imports
the validated three-disk set into private storage; neither package contains the disks. Android touch
controls hide when a physical controller is connected and return when it disconnects.

### S005 — Native engine ownership

The renderer, blitter, audio, disk loading, decompression, menus, and selected game flows have native
owners while the gameplay bank still contains mechanically translated M68K functions.

Gap: replace the remaining recompiled gameplay behavior with readable, verified native subsystems.

### S006 — Differential harness and debug tools

Evidence: the side-by-side PUAE harness can step or drive both executions, compare framebuffers and
state, inspect scanlines and display windows, watch chip-memory access, locate readers and writers,
capture screenshots and raw frames, manipulate inputs, and exercise savestate and level-entry paths.

### S007 — Faster game speed with normal audio

Evidence: Game Speed exposes Normal, Turbo, and Hyper pacing, while hold-to-fast-forward runs at 5x;
the audio clock remains wall-time based so music and effects do not pitch up or accelerate.

### S008 — Alternate platformer physics

Evidence: Jump Physics switches live between Classic and Platformer. The native model owns rise and
fall, variable-height cuts, air steering, momentum, terminal velocity, trampoline hand-off, wall
feedback, carried-character trails, and the original animation and sound side effects.

### S009 — Level selection instead of passwords

Evidence: the main menu replaces Enter Password with a world-grouped selector for all 60 levels.
Completion is persisted, finished levels are marked, locked levels are hidden as question marks, and
the completion banner says Level Complete instead of showing the next password.

### S010 — Native renderers and special effects

Evidence: Graphics can switch among the faithful renderer, the native software renderer, and the
native Vulkan hardware renderer. The hardware path exposes live ambient-darkness and character
drop-shadow effects while the faithful path remains available for comparison.

### S011 — Unbounded-aspect widescreen

Evidence: 16:9, 21:9, and Auto modes re-derive level tiles, objects, characters, and merry men beyond
the original 320-pixel camera. Auto follows the window width at runtime; the result reveals additional
simulated world rather than stretching the original image.

### S012 — Native boot and no floppy waits

Evidence: launch enters the title flow in one frame without executing an Amiga ROM, Workbench, or
floppy startup sequence. Native disk reads and ATN decompression replace timed MFM/Paula access, so
the original multi-second Accessing waits do not become loading screens in the port.

### S013 — Free camera

Evidence: the Free Cam action detaches the widescreen camera for horizontal exploration, displays a
camera indicator, and supports both real-time and paused panning policies.

### S014 — Restored difficulty selection

Evidence: left and right on Play Game cycle Easy, Normal, and Hard through the original difficulty
state that was present but not wired into the port's menu flow.

### S015 — Cheats and accessibility

Evidence: the Extra options page can skip the intro, unlock every level, and select Vanilla, Light,
or None fall damage. Fall-damage scaling preserves the original landing animation, sound, and state
side effects.

### S016 — Configurable interaction reach

Evidence: the Controls page extends horizontal pickup and interaction reach without changing vertical
reach, and the setting applies to both classic and alternate control schemes.

### S017 — Savestates and runtime diagnostics

Evidence: the executable supports save/load state, direct `--level` entry, headless execution, a
frame-time profiler, framebuffer and scene probes, forced completion/game-over diagnostics, and the
interactive control paths used by the comparison harness.

### S018 — Player-facing save slots

Missing capability: the existing single diagnostic savestate has no player-facing slot browser,
names, timestamps, or framebuffer previews.

### S019 — Rewind

Missing capability: no bounded history of recent savestates or hold-to-rewind player action exists.
