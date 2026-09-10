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
uv run --frozen python -m tools.oracle_diff --seconds 300
uv run --frozen python -m tools.oracle_diff --seconds 420 \
    --play 7300:8,7420:8,7560:8 --reuse-reference
```

- `--play FRAME:HELD,...` drives **both** products with the same frame-indexed
  fire timeline. Without it neither product ever leaves the attract loop, so
  gameplay is never compared. Menu presses have to land on the same *frame* in
  both; wall-clock input cannot promise that.
- `--reuse-reference` skips re-running the reference. It is a fixed commit, so
  its timeline only changes when the instrument does.

Two traps the tool now handles, both of which produced convincing wrong tables:

- It **snapshots the executable** before running it. A rebuild part-way through
  a measurement silently replaces the program being measured (measured once: a
  6290-frame screen reported as 1391).
- It measures a screen **to the next screen's first frame**. Measuring to the
  last copper write reported the boot logo as 3 frames against 3 and called a
  sevenfold burst a perfect match.

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
