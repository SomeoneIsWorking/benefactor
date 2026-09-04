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
Benefactor executor adapter --------> shared/amigaport 68000 interpreter
      |                                      |
      |                                      +-- maintained CPU execution owner
      +-- image-aware interception
          |                 |
          |                 +-- native override owners
          +-- OCS/CIA, frame, disk, interrupt service owners
                                  |
             render | audio | input | UI | platform/package

separate diagnostic target: same state/memory seam + independent PUAE oracle
```

The current modules are migration inputs. Intended framework/adapter locations
below describe ownership without asserting implementation state.

## Ownership table

| Subsystem | Responsibility | Current/target location | Entry point | Deep doc |
| --- | --- | --- | --- | --- |
| Product composition | Construct configuration, disk/image, host subsystems, overrides, and one gameplay executor | target app composition module | future product entry point | `docs/migration.md` |
| Typed configuration | Own environment, JSON, test-session, and default precedence; expose typed accessors so consumers never read environment variables | `src/port/config.c`, `src/port/config.h` | `pc_config_load`, `pc_cfg_int`, `pc_cfg_bool`, `pc_cfg_string` | `AGENTS.md` |
| Project paths | Discover the checkout and resolve typed project-local scratch roots without machine-specific paths | `src/port/project_paths.c`, `src/port/project_paths.h`, `tools/paths.py` | `pc_project_path`, typed Python path constants | `instructions/harness.md` |
| Process logging | One configurable sink/filter/format boundary, with global `log_level`, category thresholds, thread-local test capture, and a narrow C sink for future Lucent composition | `src/common/log.c`, `src/common/log.h` | `benefactor_log_configure`, `benefactor_log_write` | `AGENTS.md` |
| Disk and image ownership | Validate disks; load/decompress/relocate main, title, gameplay, and credits; assign generation identity | `src/engine/disk_boot.c`, `src/engine/overlay_load.c` | `overlay_load_main`, `overlay_load_title`, `overlay_load_gameplay`, `overlay_load_credits` | `docs/migration.md` |
| Benefactor executor adapter | Connect complete `amigaport` CPU state to title memory, services, overrides, exits, and invalidation | `src/runtime/guest_runtime.h`, target adapter implementation | image-qualified execute/call interface | `docs/migration.md` |
| 68000 interpreter | Full PC/SR/exception/cycle state and a maintained CPU execution owner behind typed memory/service callbacks | intended sibling `shared/amigaport` repository | `amigaport` product API | `docs/migration.md` |
| Guest memory and Amiga devices | Checked runtime-memory access plus OCS/CIA registers, blitter, audio channels, frame and interrupt services | target Benefactor executor adapter, `src/engine/hw.c`, `src/engine/hw_audio.c`, `src/engine/hw_blitter.c` | adapter memory and service callbacks | `docs/hardware-layer.md` |
| Runtime interception | Image-aware overrides, scoped original calls, service callbacks, and bounded exits | target CPU adapter plus `src/port/overrides/` | executor dispatch callback | `docs/migration.md` |
| Native game behavior | Deliberate title replacements and enhancements grouped by subject | `src/port/overrides/` | `src/port/overrides/register.c` | `docs/re-frontier.md` |
| Game lifecycle | Title/gameplay state transitions, frame coordination, pause and save/load orchestration | `src/port/game_loop.c`, `src/port/port.h` | port lifecycle API | `docs/re-frontier.md` |
| Rendering | Faithful/native scene construction, Vulkan/SDL presentation, effects, engine-view boundary | `src/render/` | `native_renderer.h`, `present_backend.h` | `instructions/rendering-overhaul-plan.md` |
| Input and actions | Keyboard/controller action mapping and device lifecycle | `src/port/input.c`, `src/port/input.h` | logical action API | `AGENTS.md` |
| Host UI | Pause, options, level selector, HUD, and touch presentation; edits config but does not own it | `src/port/pause_menu.c`, `src/port/level_select_ui.c`, `src/port/hud_icons.c`, `src/port/touch_controls.cpp` | port UI calls | `README.md` |
| Platform integration | Android JNI/activity handoff and package metadata | `src/platform/`, `platforms/` | platform bridge | `README.md` |
| Android application | Player disk import, Activity lifecycle, touch UI, and package resources | `platforms/android/` | Android Activity and Gradle package | `README.md` |
| Differential oracle | PUAE comparison, input driving, state/frame capture, trace, first-divergence reporting, and its single stable scratch activity | `src/harness/`, `vendor/libretro-uae/` | harness executable, REPL, `harness_artifact_path` | `instructions/harness.md` |
| Build, launch, packaging | Locked provisioning/build policy, slim launcher, AppImage and Android assembly | `CMakeLists.txt`, `bootstrap.py`, `tools/`, `platforms/` | `run.sh` -> `bootstrap.py` | `docs/migration.md` |
| Source policy | Scan all retained first-party product/test source and reject retired static paths/interfaces/vocabulary, direct diagnostic-harness product coupling, process-stream output outside logging, environment reads outside configuration, shell tooling, new oversized modules, and growth of frozen monoliths | `tools/source_policy.py`, `tests/test_source_policy.py` | `tools/source_policy.py` | `AGENTS.md` |
| Disk identity | Validate the exact three player-owned images without extracting content | `tools/disk_identity.py`, `tools/verify_disks.py` | `validate_disk_set` | `README.md` |
| Reverse engineering | Binary facts, gameplay/audio maps, claims, issues, and frontier | `instructions/`, `docs/issues/`, `docs/re-frontier.md` | project information tools | `docs/re-frontier.md` |

## Where does new work go?

| Change | Owner |
| --- | --- |
| 68000 registers, SR/flags, exceptions, instruction semantics, or execution core | `shared/amigaport` |
| Benefactor memory/image conversion, interception, bounded calls, or invalidation notification | Benefactor executor adapter |
| Disk identity, loader behavior, or image generation | disk/image owner |
| Title address, image-qualified override, or scoped original-call registration | native override registry |
| OCS/CIA, blitter, audio-device, or frame-service behavior | existing engine hardware owner |
| Scene construction, widescreen, renderer effects, or presentation | `src/render/` |
| Persistent option or environment/CLI precedence | typed configuration owner |
| Project-local recurring scratch path or harness artifact | project path owner plus `src/harness/artifacts.c` |
| Settings presentation for an existing option | host UI owner |
| Diagnostic output routing | Lucent-backed logging owner |
| Repeatable runtime scenario or differential comparison | `src/harness/` and its driving tools |
| Binary-derived fact or native replacement grounding | `docs/re-frontier.md` or one issue according to consumer |

## Source tree

```text
benefactor/
├── src/
│   ├── main.c        preserved host composition input; not a current product
│   ├── runtime/      image-qualified Benefactor/amigaport adapter contract
│   ├── engine/       memory, disk/image, OCS/CIA, blitter, audio
│   ├── port/         lifecycle, input, UI, configuration, native overrides
│   ├── render/       scene construction and SDL/Vulkan presentation
│   ├── platform/     Android/platform bridge
│   ├── common/       shared public value contracts
│   └── harness/      PUAE differential and interactive diagnostics
├── platforms/        package/platform composition
├── tools/            modular Python launch, identity, policy, package, and RE tooling
├── tests/            focused Python tests for shipping tooling and policy
├── instructions/     durable recovered title and subsystem facts
├── docs/             intent, state, ownership, migration, issues, and RE frontier
└── vendor/           third-party oracle source
```
