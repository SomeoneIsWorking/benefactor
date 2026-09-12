# Benefactor implementation notes

`AGENTS.md` is the canonical working agreement. For scope, state, and ownership,
consult the documents it names. `docs/oracle.md` describes the retired static
product used as a reference for behavioral comparison.

## Native override boundary

Native overrides enter in the middle of interpreted 68000 execution. Every
body must explicitly complete its guest boundary:

| Native body | Register with | Completion |
| --- | --- | --- |
| Wraps a guest routine | `rt_register_override` / `_gp` | `rt_call_original` / `rt_call_original_subroutine` |
| Replaces an RTS-terminated subroutine | `rt_register_replacement` / `_gp` | Adapter completes the RTS |
| Tail-jumps, including into a loaded image | Either | `rt_jump` |
| Unwinds to host screen ownership | Either | `rt_exit_to_host` |

The executor fails closed if a native override returns without completing that
boundary. A `$6100` or `$4EB9` entry opcode was a call and owes a return;
`$4EF8` or `$60xx` was a jump. See `src/runtime/guest_runtime.h` for the
current API and image-qualified registration variants.

Native replacements own proven guest wait idioms, blitter/beam polls, and
host-service boundaries. The original interpreted body remains available for
comparison where an override wraps it. Do not use an override to hide an
unknown CPU-semantic defect.

## Guest time and presentation

- `rt_get_guest_cycles()` is the clock. A PAL beam line is 454 cycles; a frame
  is 312 lines or 141,648 cycles. Sample the beam on every custom-chip access,
  including blitter polls, not only position-register reads.
- Guest time never rolls back. If an interrupt does not reach RTE and restores
  CPU state, carry `elapsed_cycles` across the restore.
- Crossing a beam-frame boundary raises a pending boundary in
  `src/engine/hw_beam.c`. The game's own wait loops, recognized from the
  player's image by `src/port/wait_idiom.h`, land it. Cap the hold at one frame.
  A cycle-budget cutoff may park the guest midway through a draw or blit.
- Present only at a beam read and at most once per beam frame. The game flow
  can park for host presentation; an interrupt on the host thread must present
  and pace in place. `hw_present_frame` declines a duplicate request.
- Do not park between the first `BLTxxx` write and `BLTSIZE`: interrupt code
  could then overwrite the shared blitter registers. Keep the boundary pending.
- Charge blits to guest time with `rt_add_guest_cycles`: one bus cycle per
  enabled DMA channel per word, two 68000 cycles per bus cycle, or two per
  pixel in line mode. `WaitBlit` loops have no other clock.

The derivation and regressions are in
`docs/issues/0007-interpreter-boundaries-and-beam-time.md` and
`docs/issues/0008-reference-product-differential.md`.

## Debugging and behavioral comparison

Do not rebuild an executable while `tools/oracle_diff.py` measures it; the
comparison may run a different binary halfway through. Its frame signature
(`src/port/frame_signature.h`) checks palette and per-channel audio sample,
period, and volume alongside screen/frame counts. Matching counts alone do not
show that fades and music advanced.

- Set `BENEFACTOR_LOG_LEVEL=debug` for named guest-call exits: reason, PC,
  instruction count, image, and entry (`execute`, `interrupt`, or
  `call-original`).
- `pc_trap_vector_execution` traps wild jumps at `$000000` before executing
  vector-table zeroes; it records the guest stack and retired-instruction ring.
- `rt_insn_ring_snapshot` / `rt_insn_ring_entries` hold the last 256 guest PCs
  and opcodes. Use `/trace` on the control server or
  `pc_log_retired_instructions` for a dump.
- The watchdog reports the guest PC, active call, last hardware-register read,
  `cop1lc`, and retired tail. `src/port/guest_profile.h` samples hot PCs by
  owner at every custom-chip access and reports them at screen end.
- `src/port/frame_accounting.h` and `/state` split guest cycles by game flow,
  level-3, and level-6 interrupt, with per-owner peaks and beam/wait/present
  counts. An owner far above 141,648 cycles indicates a runaway frame. Add
  fields through `benefactor::diag::FrameAccounting` and account by named owner.
- `/break?at=3732` stops before that guest instruction; `/breaks`, `/cpu`,
  `/mem`, and `/fb.ppm` inspect the held state. `/resume` continues, and
  `/step?frames=N` advances N frames. Keep the hold inside the game thread:
  returning a breakpoint exit to game flow would let it continue or shut down.
- `BENEFACTOR_HTTP=<port>` enables the Lucent-backed control server in
  `src/port/control/`. Its `/` page is interactive; `/state`, `/cpu`, `/mem`,
  `/poke`, `/hold`, `/press?fire=1&frames=4`, `/pause`, `/resume`,
  `/step?frames=N`, `/fb.ppm`, `/trace`, `/recent`, `/save`, and `/load`
  expose live control and inspection. Its server thread can accept `/resume`
  while the game is held.
- `BENEFACTOR_PRESSES=7300:8,7420:8,7560:8` supplies frame-indexed fire
  input for reproducible menu and gameplay comparison with `oracle_diff --play`.
- For headless interaction, launch the built `Benefactor` with `--headless`
  and `--disk Disk.1 Disk.2 Disk.3`, set `BENEFACTOR_HTTP`, then use the
  control endpoints while it runs.

## Build and verify

```sh
BENEFACTOR_AMIGAPORT_DIR=../shared/amigaport cmake -S . -B build/run -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/run --target benefactor_product --parallel
uv run --frozen python -m tools.verify
```

`./run.sh` is the player launcher. The adjacent `shared/amigaport` checkout
must match the ref pinned in `.github/workflows/release.yml`. The current
product refuses to build or launch until the runtime adapter exists, as
`AGENTS.md` records.
