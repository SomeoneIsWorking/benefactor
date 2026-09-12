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
| Gameplay level-data dispatch | Reset per-level native presentation state, then run the retail stream dispatcher and its callback setup through the interpreter | `src/port/overrides/level_load.c`, `src/runtime/guest_runtime.cpp` | `native_level_load` → `rt_call` | `instructions/gameplay-engine-map.md` |
| Gameplay hand-off low-memory init | Reconstruct the retail `$150` loader body's low-memory setup (`$3e`/`$184` card sentinels, `$1E.w` mode/difficulty word) that every `$577000` entry path depends on | `src/engine/gameplay_handoff.c`, `src/engine/gameplay_handoff.h` | `gameplay_handoff_prepare_low_memory` from the `$150` override, direct-to-gameplay, and restart-reinit | `instructions/gameplay-engine-map.md` |
| Benefactor executor adapter | Connect complete `amigaport` CPU state to title memory, services, overrides, exits, and invalidation | `src/runtime/guest_runtime.cpp`, `src/runtime/guest_runtime.h` | image-qualified execute/call interface | `docs/migration.md` |
| 68000 interpreter | Full PC/SR/exception/cycle state and a maintained CPU execution owner behind typed memory/service callbacks | intended sibling `shared/amigaport` repository | `amigaport` product API | `docs/migration.md` |
| Guest execution diagnostics | Turn the interpreter's retired-instruction ring into log dumps and debug-server listings | `src/port/guest_trace.c`, `src/port/guest_trace.h`, `src/port/control/` | `rt_insn_ring_*` / `rt_recent_snapshot` | `docs/issues/0007-interpreter-boundaries-and-beam-time.md` |
| Guest memory and Amiga devices | Checked runtime-memory access plus OCS/CIA registers, blitter, audio channels, frame and interrupt services | target Benefactor executor adapter, `src/engine/hw.c`, `src/engine/hw_audio.c`, `src/engine/hw_blitter.c` | adapter memory and service callbacks | `docs/hardware-layer.md` |
| Runtime interception | Image-aware overrides, title-owned guest-call boundaries, scoped suppression, service callbacks, and bounded exits | `src/runtime/guest_runtime.cpp`, `src/runtime/guest_call_policy.*`, plus `src/port/overrides/` | executor dispatch callback and `rt_call` | `docs/issues/0014-title-owned-interpreter-call-policy.md` |
| Native game behavior | Deliberate title replacements and enhancements grouped by subject | `src/port/overrides/` | `src/port/overrides/register.c` | `docs/re-frontier.md` |
| Game lifecycle | Title/gameplay state transitions, frame coordination, interrupt delivery, pause and save/load orchestration | `src/port/game_loop.c`, `src/port/port.h` | port lifecycle API | `docs/re-frontier.md` |
| Savestates | The on-disk snapshot format and the one guest state it is legal to take or restore one from | `src/port/savestate.c` | `pc_savestate`, `pc_loadstate`, `pc_savestate_allowed` | `docs/re-frontier.md` |
| Frame accounting | Where a frame's guest time went, split by owner (game flow / vblank vector / timer vector) with peaks, plus wait and delivery counts; the source of `/state`'s numbers and the watchdog's report | `src/port/frame_accounting.cpp`, `src/port/frame_accounting.h` | `benefactor::diag::FrameAccounting`, `pc_account_owner`, `pc_frame_accounting` | `CLAUDE.md` (Debugging the interpreter) |
| Guest vector table | Name the 68000 exception vectors the frame loop reads, and do the big-endian read once | `src/port/guest_vectors.c`, `src/port/guest_vectors.h` | `guest_vector_handler`, `GuestVector` | `docs/issues/0008-reference-product-differential.md` |
| Level and world geometry | How many levels each world has, level ↔ (world, level-in-world), the level-name table, which banner is on screen | `src/port/level_layout.c`, `src/port/level_layout.h` | `pc_level_split`, `pc_levels_in_world`, `pc_is_title_card_displayed` | — |
| Guest debugger | Breakpoints that stop the guest at an address and hold the game there so its state can be read | `src/port/debug/debugger.cpp`, `src/port/debug/debugger.h` | `pc_debug_breakpoint_reached`, `rt_set_breakpoint` | `CLAUDE.md` (Debugging the interpreter) |
| Interactive control channel | Play the game over HTTP: buttons, timed presses, pause/step, CPU registers, framebuffer | `src/port/control/control_server.cpp`, `src/port/control/input_script.cpp` | `pc_control_server_start`, `pc_control_frame` | `CLAUDE.md` (Debugging the interpreter) |
| Static-recompiler oracle | The retired translator product, kept as the reference the interpreter is measured against | `tools/oracle_diff.py`, branch `oracle` | `oracle_diff --setup` / `--play` | `docs/oracle.md` |
| Guest beam waits | Charge recognized native and guest waits to the interpreter's cycle clock; yield at a completed game-flow wait | `src/engine/hw_beam.c`, `src/engine/hw_private.h` | `hw_vblank_wait`, `hw_beam_wait_below/_above/_scanline` | `docs/issues/0008` |
| Guest busy-waits | Recognising a custom-register poll in the player's own image and giving it a native owner, so the frame ends where the guest asks to wait | `src/port/wait_idiom.h`, `src/port/overrides/wait_idioms.c` | `pc_wait_idiom_at`, `pc_register_wait_idioms` | `docs/issues/0008` |
| Frame-by-frame lockstep | Drive both products a frame at a time, compare guest memory and the audio/video registers each frame, and stop on the FIRST disagreement with both parked | `tools/lockstep.py`, `src/port/lockstep.c`, `src/port/lockstep_digest.h` | `lockstep --play`, `pc_lockstep_frame` | `docs/oracle.md` |
| Rendering | Faithful/native scene construction, Vulkan/SDL presentation, effects, engine-view boundary | `src/render/` | `native_renderer.h`, `present_backend.h` | `instructions/rendering-overhaul-plan.md` |
| Input and actions | Keyboard/controller action mapping and device lifecycle | `src/port/input.c`, `src/port/input.h` | logical action API | `AGENTS.md` |
| Host UI | Pause, options, level selector, HUD, and touch presentation; edits config but does not own it | `src/port/pause_menu.c`, `src/port/level_select_ui.c`, `src/port/hud_icons.c`, `src/port/touch_controls.cpp` | port UI calls | `README.md` |
| Platform integration | Android JNI/activity handoff and package metadata | `src/platform/`, `platforms/` | platform bridge | `README.md` |
| Android application policy | Benefactor disk identity and promotion, touch-action meaning, orientation, and package resources; shared Activity lifecycle, SAF staging, and touch contacts belong to `shared/android-port` | `platforms/android/` consuming the sibling `shared/android-port` runtime | `BenefactorActivity` and Gradle package | `README.md` |
| Desktop disk setup | Native first-run picker and exact three-disk identity validation | `src/platform/desktop_setup.cpp` | `desktop_setup_disks` | `docs/project-goals.md` |
| Desktop disk-selection storage | Read the last committed paths, prepare ZIP imports in an inactive slot, and publish a complete set without losing the previous selection on failure | `src/platform/disk_selection_store.*` | `benefactor::platform::DiskSelectionStore` | `docs/project-state.md` |
| Browser disk setup | Validate player-selected disks and hand one committed set to the WASM runtime | `platforms/web/` | `benefactor-disks-validated` browser bridge event | `docs/project-state.md` |
| Differential oracle | PUAE comparison, input driving, state/frame capture, trace, first-divergence reporting, and its single stable scratch activity | `src/harness/`, `vendor/libretro-uae/` | harness executable, REPL, `harness_artifact_path` | `instructions/harness.md` |
| Standalone PUAE observation | Independently build the libretro core and record test-only video/audio/chip-memory frame traces without linking the game product | `src/harness/puae_oracle.cpp`, `src/harness/puae_options.*`, `tools/build_puae_oracle.py`, `vendor/libretro-uae/` | `tools.build_puae_oracle` → `build/puae-oracle/puae_oracle` | `instructions/harness.md` |
| Build, launch, packaging | Locked provisioning/build policy, slim launcher, AppImage and Android assembly | `CMakeLists.txt`, `bootstrap.py`, `tools/`, `platforms/` | `run.sh` -> `bootstrap.py` | `docs/migration.md` |
| Source policy | Scan all retained first-party product/test source and reject retired static paths/interfaces/vocabulary, direct diagnostic-harness product coupling, process-stream output outside logging, environment reads outside configuration, shell tooling, new oversized modules, and growth of frozen monoliths | `tools/source_policy.py`, `tests/test_source_policy.py` | `tools/source_policy.py` | `AGENTS.md` |
| Asset-free CI | Run the retained-source verifier from a clean full-history checkout without game disks or runtime inputs | `.github/workflows/source-policy.yml` | GitHub Actions -> `uv run --frozen python -m tools.verify` | `docs/project-state.md` |
| Release CI | Build platform packages, verify Android signer identity and Windows imports, publish qualified native tags through GitHub Releases, and provide the browser artifact to central Pages | `.github/workflows/release.yml`, `tools/build_*.py`, `tools/check_windows_imports.py`, `tools/publish_release.py`; browser publication remains in sibling `pages/` | tag/manual release workflow | `docs/project-goals.md` |
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
