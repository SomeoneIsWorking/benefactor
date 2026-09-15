# Benefactor implementation notes

`AGENTS.md` is the canonical working agreement. For scope, state, and ownership,
consult the documents it names. `docs/oracle.md` describes the retired static
product used as a reference for behavioral comparison.

## Native override boundary

Native overrides enter in the middle of interpreted 68000 execution. Every
body must explicitly complete its guest boundary:

| Native body | Register with | Completion |
| --- | --- | --- |
| Wraps a guest routine | `rt_register_override` / `_gp` | `rt_call(ctx, ctx->image, its address)` suppresses only the active override |
| Replaces an RTS-terminated subroutine | `rt_register_replacement` / `_gp` | Adapter completes the RTS |
| Tail-jumps, including into a loaded image | Either | `rt_jump` |
| Unwinds to host screen ownership | Either | `rt_exit_to_host` |

The executor fails closed if an outer native override returns without
completing that boundary. Benefactor's `GuestCallPolicy` distinguishes
`JSR`/`BSR` calls from `JMP`/`BRA` tail transfers for nested `rt_call`; it does
not treat `BSR` as an ordinary branch. Calls and continuations must carry the
active image kind and generation. The adapter rejects stale image tokens, and
the host rebinds its borrowed context before interrupt delivery because a
parked game flow may have loaded a new overlay. See
`src/runtime/guest_runtime.h` for the current API.

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
  instruction count, image, and entry (`execute` or `interrupt`).
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
- `/state`'s `frame` is the frames SHOWN. The `frames` object beside it splits
  the three counts that used to be one integer: `presented` (shown), `game`
  (produced by the guest, shown or not) and `beam` (guest beam-frame boundaries,
  the only one that moves during bring-up). Fast-forward samples the display at
  PAL and skips the rest, so `game` runs ahead of `presented` — and
  `BENEFACTOR_PRESSES` is indexed by `game`, so a press timeline means the same
  amount of game at any speed. `src/port/frame_accounting.h` owns both counts.
- `/state`'s `level` is `$20.w`, the level the game has *requested*; the level whose
  data is actually loaded is `$2E.w`, visible as the copper list changing
  (`cop1lc`). Reading `level` as "the level being played" makes a normal transition
  look wrong: it changes at the win increment, one card before the reload.
- A hold is readable, not guessed: `/state` reports `script_paused:1` and a
  frozen `frame` while a breakpoint holds the game, and the log says
  `breakpoint $XXXXXX reached ... holding`. Clearing breakpoints does NOT
  release the hold — `/resume` does. Measured: three separate probes pressed
  fire for a minute against a held game and reported a frozen frame counter.
- A breakpoint that lands inside a recognised wait idiom (`$577130` and its
  siblings in `src/port/wait_idiom.h`) re-hits the same address on every
  `/step`: the idiom's whole job is to park there until the watched condition
  changes, so single-stepping it never leaves. Use `/resume`, and set the
  breakpoint at the *instruction after* the loop rather than inside it. A probe
  that appears frozen on one PC is reporting the wait, not a hang. The control
  route `/update?tag=…|error=…` reports an update-check result by hand so its
  panel renderings can be driven without waiting for a release.
- `./run.sh` opens the control channel itself, on 8613 or the next free port
  above it, and records it in `build/control-port`; `uv run --frozen python -m
  tools.control_port` prints the URL of the game running now. A live report is
  the only chance to look at what is being reported, so the channel is open by
  default rather than needing a restart. `BENEFACTOR_HTTP=<port>` names a
  different port and `BENEFACTOR_HTTP=0` (or `off`) closes the channel; a
  `--http N` passed through `run.sh` decides it directly. Any other launch of
  the binary needs `BENEFACTOR_HTTP` or `--http` to enable the Lucent-backed
  control server in `src/port/control/`. Its `/` page is interactive; `/state`, `/cpu`, `/mem`,
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

`tools.verify` runs re-harness's two C++ gates. The configuration audit always
runs; the ownership scan needs a configured build, because it reads the real
compile commands, and it says so rather than passing when there is none. The
sites the C boundary still holds are named with their reasons in
`tools/cpp_ownership_accepted.txt` — anything not on that list fails, and a
listed site that stops occurring fails too, so the list shrinks as C modules
move to C++ instead of quietly outliving them.

`./run.sh` is the player launcher, and it resolves the shared trees first:
`tools/shared_checkouts.py` reads the revisions `.github/workflows/release.yml`
pins and gives CMake a path for amigaport, lucent, setup-ui and RmlUi (and a
Freetype checkout when the host has no font engine). A checkout that is already
there is used; one that is clean and only behind the pin is fast-forwarded onto
it; one carrying its own work is used untouched and reported; nothing at all is
checked out at the pin under `shared/` or `dependencies/`. `BENEFACTOR_*_DIR`
and `SETUP_UI_*_DIR` override any of them.

The cmake invocation above builds against whatever the adjacent checkouts hold,
so it needs those to match the pinned refs itself — `uv run --frozen python -m
tools.shared_checkouts` prints the paths it resolves and aligns them.
