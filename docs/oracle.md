# The oracle: the retired static-recompiler product as a reference

This port is an interpreter: it reads the player's own disks and interprets
every 68000 instruction that native code does not own. It replaced an offline
68000-to-C translator ("static recompilation") whose generated corpus was
deleted, and which `tools/source_policy.py` fails the build for reinstating.

That retired product still has one job: it played the game correctly. So it is
kept as an **oracle** — a reference the interpreter is measured against, never
shipped, never built into the product, and never a source of code.

## Where it is

The oracle is the `oracle` branch of this repository, at the last commit where
the static recompiler worked. `tools/oracle_diff.py` names that commit:

```python
REFERENCE_COMMIT = "028be16"
```

`tools/oracle_diff.py --setup` checks that commit out into a worktree
(`~/repo/benefactor-oracle` by default) and builds it. The build regenerates its
corpus from *your* disks; nothing derived from the game is in the branch, and
none of it may be committed anywhere (see `AGENTS.md`).

The branch exists so the reference cannot be lost to a history rewrite or a
stale local worktree. Treat it as read-only: fixes go to `main`.

## What it is compared on

Frame counts alone are not enough. A change can match the reference's frame
counts while the melody stalls or a fade freezes, and one shipped that way once
(`docs/issues/0008-reference-product-differential.md`). So the diff reports two
tables:

- **Per screen, how many frames each product held it.** A screen is identified
  by the copper list(s) it alternates (`cop1lc`), and it ends where the *next*
  screen begins — not at its own last copper write, because a screen that holds
  still writes nothing while it is displayed.
- **Per screen, what it played and showed** — the frame signature
  (`src/port/frame_signature.h`): the palette hashed from the copper list, and
  each audio channel's sample pointer, period and volume. Reported as counts of
  frames on which each changed.

Both products must emit byte-identical instrument lines. The interpreter has the
instrument in its own source; the reference gets it by patch, from
`REFERENCE_EDITS` in `tools/oracle_diff.py`, which fails loudly if the text it
rewrites has moved.

## Using it

```bash
uv run --frozen python -m tools.oracle_diff --setup
uv run --frozen python -m tools.oracle_diff --play 7300:8,7420:8,7560:8 --reuse-reference
```

- The run is bounded by **the game's own progress**, not by a stopwatch.
  `--until COP1LC` (default `003484`, gameplay) stops each product once that
  screen has been up for `--settle` frames. `--timeout` is only a backstop
  against a hang; a run that hits it says so and its table is incomplete.
- Each product is run **unpaced** (`no_pace`), so the same frame sequence
  arrives as fast as the host can produce it. Nothing about the game's timeline
  depends on wall-clock — the beam is derived from consumed guest cycles — so
  this changes the duration, not the result. Reaching gameplay took seven
  minutes paced and about half a minute unpaced.
- `--play FRAME:HELD,...` drives **both** products with the same frame-indexed
  fire timeline. Without it neither product ever leaves the attract loop, so
  gameplay is never compared. Menu presses have to land on the same *frame* in
  both; wall-clock input cannot promise that.
- `--reuse-reference` skips re-running the reference. It is a fixed commit, so
  its timeline only changes when the instrument does.

There used to be a `--seconds`, and the caller had to guess a wall-clock
duration long enough to contain the frames they wanted. Reaching gameplay meant
`--seconds 420`, so every experiment cost seven minutes, and a guess that came
in short produced a table with `never shown` in it rather than an error.

Two traps the tool now handles, both of which produced convincing wrong tables:

- It **snapshots the executable** before running it. A rebuild part-way through
  a measurement silently replaces the program being measured (measured once: a
  6290-frame screen reported as 1391).
- It measures a screen **to the next screen's first frame**. Measuring to the
  last copper write reported the boot logo as 3 frames against 3 and called a
  sevenfold burst a perfect match.

## Frame-by-frame lockstep

The diff above answers "which screen went wrong". `tools/lockstep.py` answers
"which frame", by driving the two products **in step**: each stops at the end of
every presented frame and reports what its guest memory hashes to, and neither
is allowed to start the next frame until both have reported and the two reports
have been compared. The first frame they disagree on is the frame the
divergence was BORN on, and the run stops there with both products still parked
— so the whole 8 MB can be dumped from each and diffed to the byte.

```bash
uv run --frozen python -m tools.lockstep --play 7300:8,7420:8,7560:8
```

The protocol is four lines of text over a pipe pair, so a product speaks it in
about a hundred lines of C — `src/port/lockstep.c` here, and the same header
(`src/port/lockstep_digest.h`) injected into the reference by
`tools/oracle_diff.py`, so both hash the same bytes with the same function.
Useful options:

- `--start N` compares from frame N on (both still run in step before it), to
  step past a divergence already understood.
- `--ignore ADDR-ADDR,...` leaves addresses out of the hash. The exclusion
  happens where the bytes are hashed, in each product, driven by one spec given
  to both — which is what lets a four-byte vector be left out without losing
  the 32K region around it.
- `--dump-at FRAME,...` snapshots both memories and carries on, for when a
  divergence is already complete by the time it is noticed.
- `--compare-everything` turns the recorded structural differences back on.
- `--no-realign` stops it dropping the frames in `REALIGNMENTS` — frames this
  product shows that the reference never does. Leaving one in step means every
  frame after it is compared against the wrong partner and reports the same
  single difference for the rest of the run.

**How far it reaches.** The two products are frame-exact from the boot logo to
the poster — 7157 frames byte-identical, with the screen sequence matching
exactly — and the poster handover itself lands one frame apart. That earlier
claim here, that they could NOT be made frame-exact past the poster because a
blit costs this product guest time, was wrong in its conclusion: the blit cost
is right, and what was wrong was ending the frame at the cycle boundary instead
of at the guest's own wait. `src/engine/hw_beam.c` and `src/port/wait_idiom.h`
fix that; `docs/issues/0008` has the measurements.

**What is deliberately not compared**, each with the measurement behind it, is
in `STRUCTURAL_DIFFERENCES`, `STRUCTURAL_FIELDS` and `REALIGNMENTS` at the top
of the tool:
the level-6 autovector `$78` (the interpreter's handlers rewrite it, the
reference's `$78` never moves), the guest stack (the reference pushes a return
address only during gameplay), the port's scratch at `$700000`, the audio DMA
enables and the `AUDxLC` sample pointers (both written by handlers the
reference reaches by hand, in one breath, where this product reaches them on
the game's own sub-frame timer). `REALIGNMENTS` holds whole FRAMES rather than
state, for a frame one product shows that the other never does. Its one entry
was the intro → poster handover, and it is now a historical note rather than a
live exclusion: the handover was fixed at the source (`src/engine/hw_beam.c`),
so `--no-realign` is the honest way to run the tool today. Nothing goes on any
of those lists on a hunch, and nothing stays on one after the thing it hid is
fixed.

## What the oracle is not

It is an approximation of the real machine, not the machine. It cannot take
asynchronous interrupts at all, so it calls the interrupt service routines
itself, at addresses chosen by hand — on the intro it calls the music driver
leaf `$0055A0` and the audio-shadow copy `$0058C2` once a frame, and never runs
`$005892`, the handler that acknowledges and re-arms the CIA-B timer.

The interpreter delivers whatever the game installed at the vector, and the
game's own handlers chain by rewriting `$78`, so it runs all three. Where the
two disagree, the oracle is the *starting* evidence, not the last word: the
question is always what the real hardware would do. The game programs CIA-B
timer A with a latch of 384 against a 709379 Hz E-clock — about 37 deliveries
per PAL frame — which is why every link of a chain certainly runs within one
frame.

So: a difference from the oracle is a lead worth chasing, and a match is strong
evidence. Neither is a proof on its own.
