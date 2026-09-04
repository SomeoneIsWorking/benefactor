# Project state

This is the factual capability inventory for the intended Benefactor
native/dynarec product. Goals are in `docs/project-goals.md`, execution order
and gates in `docs/migration.md`, and atomic work in `docs/issues/`.

## Comparison baseline

The user-facing baseline is the unmodified 1994 Amiga release under a
conventional emulator: 320-pixel presentation, original controls and jump
behavior, password flow, floppy timing, and Amiga startup.

The implementation baseline is this repository's native host plus
offline-generated 68000-to-C execution. That execution method is retired and
must not be regenerated, built, or run for new evidence. Its durable binary,
native-subsystem, and PUAE-harness facts define the frontier that the intended
native/dynarec product must re-establish independently.

## Current focus

S005 is the current focus: establish `shared/amigaport` and integrate its
runtime 68000 dynarec without disturbing the existing native host owners.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | The complete game boots, transitions among all runtime images, and plays all 60 levels through the intended product | partial | S005, S023 | G001, G002 |
| S002 | Native pause/options apply and persist modern settings in game | verified | — | G002 |
| S003 | Keyboard, hot-pluggable controllers, touch, rebinding, and alternate controls share logical actions | verified | — | G002 |
| S004 | AppImage and Android provide no-terminal disk setup without packaged game assets | partial | S022, S023 | G003 |
| S005 | Native owners plus `shared/amigaport` execute every non-native 68000 path directly from authenticated runtime images | missing | S020 | G001 |
| S006 | PUAE differential and interactive tools can drive, inspect, capture, and compare the port | verified | — | G001 |
| S007 | Turbo, hyper, and hold-to-fast-forward change gameplay pace while audio remains at normal speed | verified | — | G002 |
| S008 | Optional platformer physics provides variable jump, air control, momentum, and tunable motion while classic physics remains | verified | — | G002 |
| S009 | A 60-level selector, completion progress, and locks replace password entry | verified | — | G002 |
| S010 | Faithful, native software, and Vulkan renderers provide selectable lighting and shadow effects | verified | — | G002 |
| S011 | 16:9, 21:9, and live-window widescreen reveal additional simulated world rather than stretching | verified | S010 | G002 |
| S012 | Native boot and disk/decompression owners remove Amiga startup and floppy waits | verified | — | G002 |
| S013 | Free camera pans in real time or while paused | verified | S011 | G002 |
| S014 | Easy, Normal, and Hard selection is restored in the main menu | verified | — | G002 |
| S015 | Skip intro, unlock all levels, and reduced/disabled fall damage are configurable | verified | — | G002 |
| S016 | Pickup and interaction reach can be extended independently of control scheme | verified | — | G002 |
| S017 | Savestates, direct level entry, headless driving, profiling, and runtime probes support development | partial | S020 | G002 |
| S018 | Player-facing save slots provide names, timestamps, and screenshot previews | missing | S017 | G002 |
| S019 | Hold-to-rewind restores recent states from a bounded history | missing | S017, S020 | G002 |
| S020 | `shared/amigaport` supplies complete 68000 PC/SR/exception/cycle state and a qualified runtime dynarec | missing | — | G001 |
| S021 | Image-generation-aware cache, overrides, and scoped original calls work across all four address-reusing images | missing | S005 | G001, G003 |
| S022 | Gameplay build/link/selector surfaces contain no interpreter, generated guest body, or fallback | missing | S005 | G001, G003 |
| S023 | Representative interactive gameplay conforms through native/dynarec execution on each released host | missing | S005, S021, S022 | G001, G002, G003 |
| S024 | Offline translator, generated corpus/dispatcher, generation-only seeds, and static-only tests are absent | missing | S023 | G001, G003 |

## Capability details

### S001 — Complete game flow

The pre-migration native host has documented routes through the three disks,
all 60 levels, title/gameplay transitions, game over, and ending/credits.

Gap: that frontier has not been re-established through the intended
native/dynarec product; the old generated-C runs are not current product
evidence.

### S002 — Native pause and options

Evidence: the in-game pause UI provides Resume, Options, Retry, Exit to main
menu, and Quit; supported Graphics, Controls, and Extra settings apply live and
persist.

### S003 — Unified input

Evidence: keyboard, SDL controller, and Android touch feed logical actions;
controllers hot-plug, bindings are captured in game, and classic/alternate
Interact and Drop policies are selectable per device.

### S004 — Packaged setup

The AppImage graphical flow selects a disk folder and Android imports a
validated three-disk set into private storage without packaging the disks.

Gap: clean-machine packages must be rebuilt around the native/dynarec gameplay
product and prove interpreter/generated-content absence.

### S005 — Native/dynarec execution

Missing capability: create and consume `shared/amigaport`, adapt the existing
memory, disk-image, OCS/CIA, interrupt, and override boundaries, and execute
every remaining guest path from live bytes through its dynarec. Issue #1.

### S006 — Independent oracle and control

Evidence: the PUAE harness can step or drive both executions, compare frames and
state, inspect display/chip memory, capture output, manipulate input, and reach
savestate and level-entry paths.

### S007 — Speed controls

Evidence: Normal, Turbo, Hyper, and 5x hold-to-fast-forward change gameplay
pacing while the audio clock remains wall-time based.

### S008 — Alternate physics

Evidence: Classic and Platformer policies are live-selectable; the native model
owns variable-height rise/fall, air steering, momentum, terminal velocity,
trampoline hand-off, collision feedback, animation, and sound side effects.

### S009 — Level selection

Evidence: the main menu exposes all 60 levels by world, persists completion,
marks finished levels, and enforces locks without password entry.

### S010 — Renderer choices and effects

Evidence: the running host switches among faithful, native software, and Vulkan
renderers and exposes ambient darkness and character drop shadows.

### S011 — True widescreen

Evidence: curated 16:9 and ultrawide captures plus native view/camera/object
routes show additional simulated level tiles and actors with preserved source
geometry rather than a stretched final frame.

### S012 — Native boot and loading

Evidence: the native disk/ATN owners enter title flow without Kickstart,
Workbench, or timed floppy I/O and preserve the loader/relocation effects.

### S013 — Free camera

Evidence: the Free Cam action detaches the widescreen camera, exposes an
indicator, pans horizontally, and supports running or paused policies.

### S014 — Difficulty selector

Evidence: left/right on Play Game cycles Easy, Normal, and Hard through the
original difficulty state.

### S015 — Cheats and accessibility

Evidence: Extra options can skip the intro, unlock levels, and select vanilla,
light, or no fall damage while retaining landing animation/sound/state effects.

### S016 — Interaction reach

Evidence: the Controls setting extends horizontal pickup/interaction windows
without changing vertical reach and applies to both control schemes.

### S017 — Runtime diagnostics

The existing host supports save/load state, direct level entry, headless
execution, frame profiling, framebuffer/scene probes, and interactive control.

Gap: savestate and CPU inspection currently encode the reduced generated-call
context and must move to complete `amigaport` state; diagnostics must then
exercise the shipping dynarec path.

### S018 — Save-slot UI

Missing capability: provide a player-facing slot browser with names,
timestamps, thumbnails, validation, and OS user-data storage.

### S019 — Rewind

Missing capability: provide a bounded recent-state history and hold-to-rewind
action over complete image-aware runtime state.

### S020 — Complete 68000 framework

Missing capability: `shared/amigaport` must own full architectural PC and SR,
all register/supervisor/interrupt state, exception frames/vectors, timing,
decoder/lowering, host backends, executable memory, cache lifetime, and a
separately linkable test interpreter. Issue #2.

### S021 — Four-image runtime identity

Missing capability: key translation and override decisions by main/title/
gameplay/credits image generation plus address, invalidate on load/restore, and
prove enabled/disabled/scoped-original behavior without recursion. Issue #3.

### S022 — Product composition excludes old engines

Missing capability: build/link/selector audits must prove gameplay contains no
interpreter, interpreter-backed helper, generated guest function, static
dispatcher, or fallback route.

### S023 — Representative conformance

Missing capability: pass the bounded interactive cavern scenario and four-image
transition checks in `docs/migration.md`, including complete CPU, memory,
exception/interrupt, timing, service, audio, frame, and host-performance
evidence. Issue #4.

### S024 — Static pipeline removed

Missing capability: after S023 passes, delete the offline translator, generated
corpus and build rules, static dispatcher, generation-only seeds, static-only
tests, and stale methodology in one milestone without a compatibility mode.
