# Codebase Layout

**This is the canonical map of the project. If anything here disagrees with the code,
the code wins — fix this doc in the same commit.** Read it first when you start a session.

The project is a **static recompilation + native hybrid port** of the Amiga game
*Benefactor* to PC. The original M68K code is recompiled to C (`engine/generated/`),
runs against a hand-written Amiga hardware model (`engine/`), and is progressively
replaced by hand-written native C (`port/overrides/`, `render/`). A differential
harness (`harness/`) checks the PC output against the PUAE emulator (the oracle).

## Top-level

```
<repo>/
  CLAUDE.md  AGENTS.md  README.md     # agent/human entry docs (read these first)
  CMakeLists.txt                      # build for all three executables
  Disk.1 Disk.2 Disk.3                # user-supplied game disks (gitignored)
  harness/                            # user-supplied Amiga Kickstart/WHDLoad ROMs
                                      #   (gitignored) — used by the PUAE oracle.
                                      #   NOTE: distinct from src/harness/ (code).
  src/                                # all port source — see below
  tools/                              # recompiler (tools/recomp/) + dump/analysis scripts
  scripts/                            # run_*.sh, build.sh, tools_seed_converge.sh
  docs/                               # this dir — reference docs
  instructions/                       # deeper RE notes + per-topic playbooks
  skills/                             # task playbooks (load when doing that task)
  vendor/libretro-uae/                # PUAE emulator source (the harness oracle)
  scratch/  logs/  build/             # gitignored work output
```

## `src/` — module map

```
src/
  main.c            # entry point: parse args, init, run the port

  port/             # THE NATIVE PC PORT — the code that drives the recompiled
                    # engine on PC. Game loop, state machine, input mapping,
                    # config, menus, savestate.
    game_loop.c       # per-frame step, thread handoff, state machine (the core)
    port.h            # public port API; port_internal.h = shared internals
    config.c  input.c  level_select_ui.c  pause_menu.c  http_debug.c
    overrides/        # hand-written C that REPLACES specific recompiled M68K
                      # functions (registered by dispatch address). register.c
                      # is the registrar; boot/copper/render/gameplay/audio/
                      # pickup/hw.c group overrides by subsystem.

  engine/           # THE RECOMPILED BENEFACTOR ENGINE + the Amiga hardware model
                    # it runs against. This is "the original game", as C.
    rt.c/.h           # M68K runtime: memory routing, the dispatch table, M68KCtx
    hw.c/.h           # Amiga custom-chip + chip-RAM model; frame present (SDL)
    hw_private.h  hw_audio.c  hw_blitter.c
    disk_boot.c  overlay_load.c   # the disk/overlay loaders (also used standalone
                                  # by tools/dump_banks.c to produce recomp inputs)
    generated/        # recompiled 68k -> C (game_*.c). GITIGNORED build artifact,
                      # regenerated from the disks by tools/regen.sh at configure.
                      # Do NOT hand-edit; change tools/recomp/ + regen instead.

  render/           # THE RENDERER SUBSYSTEM (the Vulkan home)
    native_renderer.c # copper-walking software renderer: reads the copper list +
                      # bitplanes from chip RAM, decodes to the framebuffer.
    engine_view.c/.h  # the de-hack "firewall": a typed, read-only snapshot of the
                      # engine state the wide renderer may read — and NOTHING else
                      # (no copper/displayed-output reverse-projection). See
                      # docs/working-agreements.md "No bandaids".

  common/           # shared headers: log.h, game_state.h

  harness/          # THE DIFFERENTIAL TEST HARNESS (code) — runs the PC port and
                    # PUAE in lockstep and compares. harness_*.c, the REPL
                    # (harness_main.c), trace.c, puae_*. (Links PUAE from vendor/.)
```

## What each module is — and is NOT (so it can't be misread)

- **`engine/` is the original game**, recompiled. It is NOT "the PC engine we wrote".
  When you see `gfn_*` / `game_*.c`, that's translated M68K — sacrosanct, regenerated.
- **`port/` is the PC wrapper** we own. It is NOT the recompiled game; it drives it
  and overrides pieces of it. New native behavior goes here (or `render/`), not in
  `engine/generated/`.
- **`render/` owns pixels.** The renderer reads engine state via `engine_view`; it
  must not curve-fit to displayed output. Vulkan/backends land here.
- **`harness/` (code) vs `harness/` (repo root, ROMs)** are two different things with
  the same name: `src/harness/` is the test harness; root `harness/` is the gitignored
  Kickstart/WHDLoad ROM assets the PUAE oracle boots from.

## Include convention

Every local include is **`src`-rooted by module** — `#include "engine/hw.h"`,
`"render/native_renderer.h"`, `"port/port.h"`, `"common/log.h"`. So an include line
names its owning module. `src` is the sole include root. (Exceptions: `engine/generated/`
files use same-dir `"game.h"`; the PUAE-side `harness/puae_snap_impl.c`, which is
`#include`d into vendor `custom.c`, uses relative `../` paths.)

## Build targets (CMakeLists.txt)

- `benefactor-pc` — the game (port + engine + render + generated).
- `benefactor-harness` — the differential PUAE-vs-PC compare tool + REPL.
- `benefactor-dumpbanks` — bootstrap-clean dumper (loaders only) that produces the
  recompiler's bank inputs from the disks; used by `tools/regen.sh`.

Build both with `scripts/build.sh` (the green-build gate — run before committing).
See `docs/build-and-run.md` for running, and `docs/working-agreements.md` for the
hard rules (verify-before-commit, no-bandaids, etc.).
