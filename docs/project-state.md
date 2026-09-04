# Project state

This is the factual capability inventory for the intended Benefactor
native/interpreter product. Goals are in `docs/project-goals.md`, execution order
and gates in `docs/migration.md`, and atomic work in `docs/issues/`.

## Comparison baseline

The user-facing baseline is the unmodified 1994 Amiga release under a
conventional emulator: 320-pixel presentation, original controls and jump
behavior, password flow, floppy timing, and Amiga startup.

The implementation baseline is this repository's native host plus
offline-generated 68000-to-C execution. That execution method is retired and
must not be regenerated, built, or run for new evidence. Its durable binary,
native-subsystem, and PUAE-harness facts define the frontier that the intended
native/interpreter product must re-establish independently.

## Current focus

S005 is the current focus: establish `shared/amigaport` and integrate its
maintained 68000 interpreter without disturbing the existing native host owners.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | The complete game boots, transitions among all runtime images, and plays all 60 levels through the intended product | partial | S005, S023 | G001, G002 |
| S002 | Native pause/options apply and persist modern settings in game | partial | S005 | G002 |
| S003 | Keyboard, hot-pluggable controllers, touch, rebinding, and alternate controls share logical actions | partial | S005 | G002 |
| S004 | AppImage and Android provide no-terminal disk setup without packaged game assets | partial | S022, S023 | G003 |
| S005 | Native owners plus `shared/amigaport` execute every non-native 68000 path directly from authenticated runtime images | missing | S020 | G001 |
| S006 | PUAE differential scenarios and interactive controls are preserved as an independent oracle for the shipping interpreter | partial | S005 | G001 |
| S007 | Turbo, hyper, and hold-to-fast-forward change gameplay pace while audio remains at normal speed | partial | S005 | G002 |
| S008 | Optional platformer physics provides variable jump, air control, momentum, and tunable motion while classic physics remains | partial | S005 | G002 |
| S009 | A 60-level selector, completion progress, and locks replace password entry | partial | S005 | G002 |
| S010 | Faithful, native software, and Vulkan renderers provide selectable lighting and shadow effects | partial | S005 | G002 |
| S011 | 16:9, 21:9, and live-window widescreen reveal additional simulated world rather than stretching | partial | S005, S010 | G002 |
| S012 | Native boot and disk/decompression owners remove Amiga startup and floppy waits | partial | S005 | G002 |
| S013 | Free camera pans in real time or while paused | partial | S005, S011 | G002 |
| S014 | Easy, Normal, and Hard selection is restored in the main menu | partial | S005 | G002 |
| S015 | Skip intro, unlock all levels, and reduced/disabled fall damage are configurable | partial | S005 | G002 |
| S016 | Pickup and interaction reach can be extended independently of control scheme | partial | S005 | G002 |
| S017 | Savestates, direct level entry, headless driving, profiling, and runtime probes support development | partial | S020 | G002 |
| S018 | Player-facing save slots provide names, timestamps, and screenshot previews | missing | S017 | G002 |
| S019 | Hold-to-rewind restores recent states from a bounded history | missing | S017, S020 | G002 |
| S020 | `shared/amigaport` supplies complete 68000 PC/SR/exception/cycle state and a maintained interpreter owner | missing | — | G001 |
| S021 | Image-generation-aware execution, overrides, and scoped original calls work across all four address-reusing images | missing | S005 | G001, G003 |
| S022 | Gameplay uses one shared interpreter CPU owner and contains no generated/static execution or direct diagnostic-emulator dependency | missing | S005 | G001, G003 |
| S023 | Representative interactive gameplay conforms and meets performance gates through native/interpreter execution on x86-64, Apple Silicon macOS, and Android arm64-v8a | missing | S005, S021, S022 | G001, G002, G003 |
| S024 | Offline translator, generated corpus/dispatcher, generation-only seeds, and static-only tests are absent | verified | — | G001, G003 |
| S025 | Asset-free source-policy CI runs from a full-history checkout | partial | Hosted run `33887566417` reached the verifier but exposed an unpinned clang-format major-version mismatch; the locked formatter fix is pending hosted verification; runtime platform jobs wait for S005 and shared/amigaport | G003 |

## Capability details

For S002-S016, “Evidence” describes retained native implementation and the
last pre-migration observations. Each remains partial until the same behavior
is exercised through the native/interpreter product; deleted static execution is
not current verification.

### S001 — Complete game flow

The pre-migration native host has documented routes through the three disks,
all 60 levels, title/gameplay transitions, game over, and ending/credits.

Gap: that frontier has not been re-established through the intended
native/interpreter product; pre-migration runs are not current product
evidence.

### S002 — Native pause and options

Evidence: the in-game pause UI provides Resume, Options, Retry, Exit to main
menu, and Quit; supported Graphics, Controls, and Extra settings apply live and
persist.

Gap: re-exercise and verify the retained owner through S005's interpreter product.

### S003 — Unified input

Evidence: keyboard, SDL controller, and Android touch feed logical actions;
controllers hot-plug, bindings are captured in game, and classic/alternate
Interact and Drop policies are selectable per device.

Gap: re-exercise and verify the retained owner through S005's interpreter product.

### S004 — Packaged setup

Android imports a validated three-disk set into private storage without
packaging the disks. The retired AppImage shell setup flow is no longer a
shipping implementation.

Gap: implement desktop first-run disk selection in the native product, then
rebuild both clean-machine packages around the native/interpreter gameplay
product and prove generated-content plus direct-emulator absence.

### S005 — Native/interpreter execution

Missing capability: create and consume `shared/amigaport`, adapt the existing
memory, disk-image, OCS/CIA, interrupt, and override boundaries, and execute
every remaining guest path from live bytes through its maintained interpreter. Issue #1.

### S006 — Independent oracle and control

The PUAE source, scenario descriptions, comparison code, and interactive
control vocabulary are preserved independently from the removed static product.

Gap: recompose them as a separately built oracle against the shipping interpreter;
the former mixed PUAE/static executable was deleted with the static build.

### S007 — Speed controls

Evidence: Normal, Turbo, Hyper, and 5x hold-to-fast-forward change gameplay
pacing while the audio clock remains wall-time based.

Gap: re-exercise and verify pacing and audio through S005's interpreter product.

### S008 — Alternate physics

Evidence: Classic and Platformer policies are live-selectable; the native model
owns variable-height rise/fall, air steering, momentum, terminal velocity,
trampoline hand-off, collision feedback, animation, and sound side effects.

Gap: re-exercise and verify both policies through S005's interpreter product.

### S009 — Level selection

Evidence: the main menu exposes all 60 levels by world, persists completion,
marks finished levels, and enforces locks without password entry.

Gap: re-exercise and verify the retained flow through S005's interpreter product.

### S010 — Renderer choices and effects

Evidence: the running host switches among faithful, native software, and Vulkan
renderers and exposes ambient darkness and character drop shadows.

Gap: rebuild and verify every renderer through S005's interpreter product.

### S011 — True widescreen

Evidence: curated 16:9 and ultrawide captures plus native view/camera/object
routes show additional simulated level tiles and actors with preserved source
geometry rather than a stretched final frame.

Gap: reproduce that evidence through S005's interpreter product.

### S012 — Native boot and loading

Evidence: the native disk/ATN owners enter title flow without Kickstart,
Workbench, or timed floppy I/O and preserve the loader/relocation effects.

Gap: connect those retained owners to S005's runtime memory/image boundary and
verify the product path.

### S013 — Free camera

Evidence: the Free Cam action detaches the widescreen camera, exposes an
indicator, pans horizontally, and supports running or paused policies.

Gap: re-exercise and verify the retained owner through S005's interpreter product.

### S014 — Difficulty selector

Evidence: left/right on Play Game cycles Easy, Normal, and Hard through the
original difficulty state.

Gap: re-exercise and verify the retained menu behavior through S005.

### S015 — Cheats and accessibility

Evidence: Extra options can skip the intro, unlock levels, and select vanilla,
light, or no fall damage while retaining landing animation/sound/state effects.

Gap: re-exercise and verify each retained option through S005's interpreter product.

### S016 — Interaction reach

Evidence: the Controls setting extends horizontal pickup/interaction windows
without changing vertical reach and applies to both control schemes.

Gap: re-exercise and verify the retained behavior through S005's interpreter product.

### S017 — Runtime diagnostics

The existing host supports save/load state, direct level entry, headless
execution, frame profiling, framebuffer/scene probes, and interactive control.

Gap: savestate and CPU inspection still need serialization through the complete
`amigaport` CPU state; diagnostics must then exercise the shipping interpreter
path.

### S018 — Save-slot UI

Missing capability: provide a player-facing slot browser with names,
timestamps, thumbnails, validation, and OS user-data storage.

### S019 — Rewind

Missing capability: provide a bounded recent-state history and hold-to-rewind
action over complete image-aware runtime state.

### S020 — Complete 68000 framework

Missing capability: `shared/amigaport` must own full architectural PC and SR,
all register/supervisor/interrupt state, exception frames/vectors, timing,
instruction semantics, executable-image access, bounded execution, and a
maintained interpreter implementation behind the shared API. Issue #2.

### S021 — Four-image runtime identity

Missing capability: key active execution and override decisions by main/title/
gameplay/credits image generation plus address, replace identity on load/restore, and
prove enabled/disabled/scoped-original behavior without recursion. Issue #3.

### S022 — Single interpreter-owner product composition

Missing capability: build/link audits must prove gameplay contains no generated
guest function, static dispatcher, direct diagnostic-PUAE dependency, or second
CPU owner. The shipping interpreter must enter only through `shared/amigaport`.

### S023 — Representative conformance

Missing capability: pass the bounded interactive cavern scenario and four-image
transition checks in `docs/migration.md`, including complete CPU, memory,
exception/interrupt, timing, service, audio, frame, and host-performance
evidence. Issue #4.

### S024 — Static pipeline removed

Evidence: `tools/recomp/`, `src/engine/generated/`, `src/engine/rt.c`, static
generation/build scripts, generated-symbol calls, and the old delayed-removal
methodology are absent. `tools/source_policy.py` rejects their return.

### S025 — Asset-free CI

Hosted run `33887566417` checked out full Git history with read-only repository
permissions and pinned action revisions, installed no game disks, and reached the
retained-source verifier. It failed because the workflow installed
clang-format 18 from the runner while the maintainer verification used
clang-format 22; the versions parse several block-scope pointer declarations
differently despite the same style file.

Gap: the formatter is now pinned in the locked Python tool environment and
pointer alignment is explicit, but that repair has not yet passed a hosted
run. Runtime platform jobs wait for S005 and the shared `amigaport` adapter.
Windows, macOS, and Android product jobs are not claimed yet: the runtime
adapter is unavailable, so those jobs would be policy-only duplicates rather
than platform build or package boundaries.
