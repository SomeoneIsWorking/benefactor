# Benefactor Amiga → PC port

`AGENTS.md` is the working agreement and takes precedence over this file. Read it,
then `docs/project-goals.md`, `docs/project-state.md`, `docs/codemap.md` and the
open items in `docs/issues/`.

## What this product is

Hand-written native code owns disk loading, Amiga services, rendering, audio,
input, UI and deliberately replaced game behaviour. **Every other 68000
instruction is interpreted** from the player's own disks by `shared/amigaport`.
There is no offline 68000-to-C translator, no generated corpus, no static
dispatcher — they were deleted, and `tools/source_policy.py` fails the build if
they come back in any form.

## Working on native overrides

An override is native code standing in the middle of interpreted guest
execution, so it must always say how the guest continues. Pick one:

| The native body… | Register with | Boundary |
| --- | --- | --- |
| wraps a guest routine (measures, widens, captures) | `rt_register_override` / `_gp` | `rt_call_original` / `rt_call_original_subroutine` |
| wholly replaces an RTS-terminated guest subroutine | `rt_register_replacement` / `_gp` | the adapter completes the RTS |
| tail-jumps somewhere else (incl. into a newly loaded image) | either | `rt_jump` |
| deliberately unwinds so the host takes over the screen | either | `rt_exit_to_host` |

Saying nothing is a bug, not a default: the executor fails closed with
`native override $ADDR returned without completing its guest boundary`, naming
the instruction that entered it (a `$6100`/`$4EB9`-class opcode means the guest
called it and it owes a return; `$4EF8`/`$60xx` means it was jumped to). Under
the retired translator a C function returning was the routine returning — that
assumption is what broke on the switch, so treat an override written before
2026-09-09 as unclassified until it has run.

## Timing

Guest time is 68000 cycles, from `rt_get_guest_cycles()`. The PAL beam is
derived from it (454 cycles per line, 312 lines per frame) — never from how many
times the guest read a register, and the beam is sampled on EVERY custom-chip
access, not only the position registers: the intro crawl syncs on the blitter
and would otherwise never cross a boundary.

Guest time never runs backwards. An interrupt whose handler does not reach its
RTE rolls the CPU state back, and `elapsed_cycles` is deliberately carried
across that restore — those cycles were really spent.

**Present exactly one frame per beam frame, whoever notices the boundary.** The
game flow parks at its boundary and the host presents; guest code inside an
interrupt runs on the host thread and cannot be parked, so it presents (and
paces) in place. Both asking to present in the same beam frame halved the
guest's speed, so `hw_present_frame` refuses a beam frame it has already shown.
Present only at a beam READ — any other access can land mid-draw, which cropped
crawl text mid-line.

**Never take a frame boundary in the middle of a blit's register sequence.** The
blitter registers are one shared set, so parking the game flow between the first
`BLTxxx` write and `BLTSIZE` lets the interrupt the host then delivers write the
same registers, and the two blits merge into one runaway blit. Leave the boundary
pending instead.

**A blit costs the guest time.** One bus cycle per enabled DMA channel per word,
two 68000 cycles a bus cycle (two per pixel in line mode), charged through
`rt_add_guest_cycles`. Code that paces itself on `WaitBlit` — the intro crawl —
has no other clock.

See `docs/issues/0007-interpreter-boundaries-and-beam-time.md`.

## Debugging the interpreter

Reach for these before adding a print:

- **Every guest-call exit is named** at debug level (`BENEFACTOR_LOG_LEVEL=debug`):
  `guest call exit: reason=… pc=… instructions=… image=…`.
- **A wild jump names itself.** Reaching the exception vector table traps on the
  FIRST instruction (`pc_trap_vector_execution`, registered on `$000000`), logs
  the guest stack around A7, and dumps the ring — before the ring is overwritten
  by the vector table's own zeros.
- **Retired-instruction ring** — the last 256 guest PCs with their opcodes.
  `rt_insn_ring_snapshot` / `rt_insn_ring_entries`, `pc_log_retired_instructions`
  for a log dump, `/trace` on the debug server for a live one. This is what
  turns "it hung" into an address.
- **Watchdog output** carries the guest PC, the active call, the last hardware
  register read, cop1lc and the retired tail.
- **Frame accounting** (`src/port/frame_accounting.h`, in `/state` and the
  watchdog): guest cycles for the last host iteration split by owner — the game
  flow, the level-3 vector, the level-6 vector — each with its PEAK, plus the
  beam boundaries crossed/taken/declined, the per-frame waits reached/refused/
  parked, and presents/re-entrant. One PAL frame is 141,648 cycles; an owner
  far above that is the fault. The peaks matter: a runaway iteration is
  invisible to a sampler, which only ever sees what the previous short
  iteration left behind. This is what found the crawl bug — `irq6_max` of 14M
  cycles (99 frames inside one interrupt delivery).
- **Debug HTTP server** (`BENEFACTOR_HTTP=<port>`): `/state` (frame, level,
  cop1lc, player block, retired instructions, guest cycles, fps, per-section
  frame times, the frame accounting above), `/mem`, `/poke`, `/input` (drive the
  game headless), `/fb.ppm`, `/trace`, `/recent`, `/save`, `/load`.
- **Drive it headless**: `./build/run/…/Benefactor --headless --disk Disk.1 Disk.2 Disk.3`
  with `BENEFACTOR_HTTP` set, then `curl "localhost:PORT/input?fire=1"`.

New inspection needs become one of these owners. Do not scatter `getenv`-gated
`fprintf` traces through the engine — `src/port/config.c` is the only module
that may read the environment and `src/common/log.c` the only one that may write
to a process stream.

## Build and verify

```
BENEFACTOR_AMIGAPORT_DIR=../shared/amigaport cmake -S . -B build/run -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/run --target benefactor_product --parallel
uv run --frozen python -m tools.verify
```

`./run.sh` is the player-facing launcher. `shared/amigaport` must be checked out
next to this repository at the ref pinned in `.github/workflows/release.yml`.
