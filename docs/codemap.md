# Benefactor port — codemap

This map owns responsibility and placement only. Capability state belongs in
`docs/project-state.md`, product intent in `docs/project-goals.md`, execution
order in `docs/migration.md`, and evidence in the issue and RE-frontier records.

## Architecture

```text
player-owned Disk.1-Disk.3
          |
disk/image owner: identity, load, relocation, generation token
          |
Benefactor executor adapter --------> shared/amigaport 68000 dynarec
      |                                      |
      |                                      +-- live translated blocks/cache
      +-- image-aware interception
          |                 |
          |                 +-- native override owners
          +-- OCS/CIA, frame, disk, interrupt service owners
                                  |
             render | audio | input | UI | platform/package

separate diagnostic target: same state/memory seam + test interpreter or PUAE
```

The current modules are migration inputs. Intended framework/adapter locations
below describe ownership without asserting implementation state.

## Ownership table

| Subsystem | Responsibility | Current/target location | Entry point | Deep doc |
| --- | --- | --- | --- | --- |
| Product composition | Construct configuration, disk/image, host subsystems, overrides, and one gameplay executor | `src/main.c`, target app composition module | `main` | `docs/migration.md` |
| Typed configuration | Parse CLI, environment/`.env`, files, defaults, and precedence once; publish immutable typed values | `src/port/config.c`, `src/port/config.h` | configuration load boundary | `AGENTS.md` |
| Process logging | One configurable Lucent-backed sink/filter/format boundary | `src/common/log.h`, target logging module | injected logger interface | `AGENTS.md` |
| Disk and image ownership | Validate disks; load/decompress/relocate main, title, gameplay, and credits; assign generation identity | `src/engine/disk_boot.c`, `src/engine/overlay_load.c` | `overlay_load_main`, `overlay_load_title`, `overlay_load_gameplay`, `overlay_load_credits` | `docs/migration.md` |
| Benefactor executor adapter | Connect complete `amigaport` CPU state to title memory, services, overrides, exits, and invalidation | target CPU adapter module | one bounded execute/call interface | `docs/migration.md` |
| 68000 CPU and dynarec | Full PC/SR/exception/cycle state, decode/lowering, host backends, executable memory, block cache | intended sibling shared/amigaport repository | `amigaport` product API | `../../shared/jit-common/docs/migration.md` |
| Test interpreter | Independent diagnostic execution over the canonical CPU/memory contract; never a gameplay dependency | intended sibling shared/amigaport repository plus test composition | separate diagnostic target | `docs/migration.md` |
| Guest memory and Amiga devices | Checked RAM access, OCS/CIA registers, blitter, audio channels, frame and interrupt services | `src/engine/rt.c`, `src/engine/hw.c`, `src/engine/hw_audio.c`, `src/engine/hw_blitter.c` | memory/service callbacks | `docs/hardware-layer.md` |
| Runtime interception | Image-aware overrides, scoped original calls, service callbacks, and bounded exits | target CPU adapter plus `src/port/overrides/` | executor dispatch callback | `docs/migration.md` |
| Native game behavior | Deliberate title replacements and enhancements grouped by subject | `src/port/overrides/` | `src/port/overrides/register.c` | `docs/re-frontier.md` |
| Game lifecycle | Title/gameplay state transitions, frame coordination, pause and save/load orchestration | `src/port/game_loop.c`, `src/port/port.h` | port lifecycle API | `docs/re-frontier.md` |
| Rendering | Faithful/native scene construction, Vulkan/SDL presentation, effects, engine-view boundary | `src/render/` | `native_renderer.h`, `present_backend.h` | `instructions/rendering-overhaul-plan.md` |
| Input and actions | Keyboard/controller action mapping and device lifecycle | `src/port/input.c`, `src/port/input.h` | logical action API | `AGENTS.md` |
| Host UI | Pause, options, level selector, HUD, and touch presentation; edits config but does not own it | `src/port/pause_menu.c`, `src/port/level_select_ui.c`, `src/port/hud_icons.c`, `src/port/touch_controls.cpp` | port UI calls | `README.md` |
| Platform integration | Android JNI/activity handoff and package metadata | `src/platform/`, `platforms/` | platform bridge | `README.md` |
| Differential oracle | PUAE comparison, input driving, state/frame capture, trace, and first-divergence reporting | `src/harness/`, `vendor/libretro-uae/` | harness executable and REPL | `instructions/harness.md` |
| Build, launch, packaging | Locked provisioning/build policy, slim launcher, AppImage and Android assembly | `CMakeLists.txt`, `tools/`, `platforms/` | target `run.sh` -> Python bootstrap | `docs/migration.md` |
| Reverse engineering | Binary facts, gameplay/audio maps, claims, issues, and frontier | `instructions/`, `docs/issues/`, `docs/re-frontier.md` | project information tools | `docs/re-frontier.md` |
| Legacy shell workflows | Existing build/run/harness/seed shell entry points pending launcher and tooling migration | `scripts/` | Do not extend; when touched, move the live behavior into the owning Python launcher/tool and delete the shell path | `docs/migration.md` |

## Where does new work go?

| Change | Owner |
| --- | --- |
| 68000 registers, SR/flags, exceptions, instruction semantics, emitter, or cache | `shared/amigaport` |
| Benefactor memory/image conversion, interception, bounded calls, or invalidation notification | Benefactor executor adapter |
| Disk identity, loader behavior, or image generation | disk/image owner |
| Title address, image-qualified override, or scoped original-call registration | native override registry |
| OCS/CIA, blitter, audio-device, or frame-service behavior | existing engine hardware owner |
| Scene construction, widescreen, renderer effects, or presentation | `src/render/` |
| Persistent option or environment/CLI precedence | typed configuration owner |
| Settings presentation for an existing option | host UI owner |
| Diagnostic output routing | Lucent-backed logging owner |
| Repeatable runtime scenario or differential comparison | `src/harness/` and its driving tools |
| Binary-derived fact or native replacement grounding | `docs/re-frontier.md` or one issue according to consumer |

## Source tree

```text
benefactor/
├── src/
│   ├── main.c        current composition entry
│   ├── engine/       memory, disk/image, OCS/CIA, blitter, audio; old static runtime
│   ├── port/         lifecycle, input, UI, configuration, native overrides
│   ├── render/       scene construction and SDL/Vulkan presentation
│   ├── platform/     Android/platform bridge
│   ├── common/       shared public value contracts
│   └── harness/      PUAE differential and interactive diagnostics
├── platforms/        package/platform composition
├── tools/            current build/RE utilities; static tooling removed at migration gate
├── scripts/          legacy shell workflows; retirement/migration target only
├── instructions/     durable recovered title and subsystem facts
├── docs/             intent, state, ownership, migration, issues, and RE frontier
└── vendor/           third-party oracle source
```
