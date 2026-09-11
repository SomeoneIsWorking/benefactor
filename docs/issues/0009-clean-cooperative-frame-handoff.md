# 0009 — Cooperative Frame Handoff and Scanline Wait Recognition

## Why

In the native/interpreter hybrid architecture, the 68000 CPU thread executes guest code while the host main thread presents frames, mixes audio, and handles platform input.

Previously, `hw_step_register_beam()` attempted to trigger `g_hw_vblank_yield()` during register accesses whenever the beam cycle calculation crossed a PAL frame boundary (141,648 cycles). This caused preemptions mid-instruction and mid-operation (such as between blitter register setups, or mid-copper list construction), corrupting custom chip register states. To mitigate those corruptions, a series of compensatory hacks were introduced:
- `hw_boundary_hold()` and `HW_BOUNDARY_HOLD_MAX = 2` to defer yields when mid-work.
- `s_boundary_owed` and replay loops in `pc_step_threaded` (`hw_present_paused_frame()`) to artificially re-present held frames so the game wouldn't run fast.
- `s_presented_beam_frame` filters to drop re-entrant presents.

Additionally, steady gameplay in Benefactor synchronises on scanlines by polling `VHPOSR` (`cmpi.b #$3B, $6(a6); bne.s $577130` in the main loop, and `#$3A` in level setup at `$578472`), rather than polling vertical-blank bit 8 in `VPOSR`. Because `src/port/wait_idiom.h` only recognised `VPOSR` bit 8 loops, gameplay frame waits were not converted to native yields. Furthermore, overlay loaders and `skip_intro` did not register wait idioms for the title, gameplay, or credits overlays upon decrunching.

## What changed

### 1. Extended wait idiom recognition (`src/port/wait_idiom.h`)
- Added recognition for `PC_WAIT_SCANLINE`:
  `0x0C2E` (`cmpi.b #line, $6(a6)`), displacement `0x0006`, followed by backward branch `bne.s`.
- Added recognition for displaced blitter poll:
  `0x082E` (`btst #6, $2(a6)`), displacement `0x0002`, followed by `bne.s` (Form 2 with `a6 = $DFF000`).
- Added support for `VPOSR` bit 8 polls with displacement `0x0005` (`a6 = $DFF000`).

### 2. Native wait dispatch (`src/port/overrides/wait_idioms.c`)
- Handled `PC_WAIT_SCANLINE` identically to `PC_WAIT_FRAME` by calling `hw_vblank_wait()`, releasing the guest thread at clean frame boundaries and presenting to the host.
- Added comprehensive unit tests in `tests/test_wait_idiom.c` covering the new scanline, displaced blitter, and VPOSR forms.

### 3. Comprehensive overlay idiom registration
- Registered wait idioms whenever overlay images are activated:
  - `native_overlay_loader` (`BENEFACTOR_IMAGE_TITLE`)
  - `native_overlay_load_d0` (`BENEFACTOR_IMAGE_GAMEPLAY`)
  - `pc_request_credits_start` (`BENEFACTOR_IMAGE_CREDITS`)
  - `pc_init_from_disk` when `skip_intro` is active (`BENEFACTOR_IMAGE_TITLE`)
  - `pc_request_cold_restart` (`BENEFACTOR_IMAGE_TITLE`)

### 4. Clean cooperative frame handoff
- Removed mid-instruction yields from `hw_step_register_beam()`. The function now solely updates scanline registers (`s_scanline`) and beam frames based on consumed cycles for guest register reads without preempting the execution context.
- Removed compensatory boundary-holding mechanisms: `hw_boundary_hold`, `hw_boundary_release`, and `hw_boundary_take_owed` now no-op cleanly.
- Removed `s_presented_beam_frame` and the owed-frame replay loop in `pc_step_threaded`.
- Connected `s_frame_watchdog_limit` in `hw_present_body` so unattended benchmark and test runs terminate deterministically upon reaching their limit.

### 5. Linkage and POSIX conformance
- Moved C++ headers (`<cstdio>`, `port/frame_accounting.h`) outside `extern "C"` blocks in `src/port/control/control_server.cpp` and `src/port/debug/debugger.cpp`.
- Defined `_POSIX_C_SOURCE 200809L` in `src/port/lockstep_digest.h` to declare `fdopen` under strict C11.

## Verification

- `CC=clang uv run --frozen python -m tools.verify` passes all 58 repository checks with zero warnings or errors.
- Headless runs (`./build/benefactor-pc --headless Disk.1 Disk.2 Disk.3 Disk.4`) with `BENEFACTOR_LIMIT=20` cleanly boot to poster and complete without hangs.
- Direct-to-gameplay runs (`./build/benefactor-pc --headless --level 1 Disk.1 Disk.2 Disk.3 Disk.4`) execute gameplay smoothly without watchdog timeouts, illegal instructions, or copper/blitter corruption.
- `./run.sh` builds and launches the target without requiring additional arguments.
