# 0008 — Differential testing against the retired reference product

## Why

"The intro crawl is too fast" is not a measurement. Neither is "it was working
fine before". Both were true, and neither said by how much, or which screens.

The product that ran the game before this one — the retired offline-translation
build, last on commit `028be16` — is the only artefact that knows the answer: it
ran the game correctly, and it can still be built from the player's own disks.
It is therefore the ORACLE. The interpreter is correct when it shows the same
screens for the same number of frames.

## What was restored, and what was not

The retired product is **not** restored into this tree, and must not be:
`tools/source_policy.py` fails the build if its paths, symbols or vocabulary come
back, and today's engine could not link it anyway — it was deleted along with
`src/engine/rt.c`, and `src/engine/hw.c` has been rewritten since.

Instead `tools/oracle_diff.py` checks that commit out into a sibling git worktree
(`../benefactor-oracle`), builds it there, and drives it as a BLACK BOX over the
same command line the shipping product takes. Nothing about it enters this
repository except the tool that runs it.

    uv run --frozen python -m tools.oracle_diff --setup   # build the reference
    uv run --frozen python -m tools.oracle_diff           # run both, diff

Setup regenerates the reference from your own `Disk.1/2/3` (about 90 seconds) and
builds it (about a minute). It links this repository's `vendor/libretro-uae`
submodule by symlink, because the worktree does not get submodule contents.

## What is compared

Both builds emit one line per screen change — `phase: frame=<n> cop1lc=<addr>` —
and the tool reduces each run to a list of screens with the frames each lasted.
A screen alternates between two copper lists (double buffering), so a phase ends
when a THIRD address appears.

Frames, not seconds. The reference is authoritative about the game's own frame
sequence, not about how fast a host runs it. Both builds are paced to PAL 50 Hz
headless — the reference paces only when it has a window, which the tool patches
at setup — so the two runs are directly comparable.

## First result (2026-09-10)

Reference vs interpreter, both from a cold boot:

| screen (cop1lc)  | reference | interpreter | ratio |
| ---------------- | --------: | ----------: | ----: |
| `007BC8/0086CC` (intro crawl) |  6290 |  1463 | 4.3x |
| `0077C0/0078F0`               |   191 |     2 |  95x |
| `007770/0091D0`               |   296 |    46 | 6.4x |
| `0081D2/00844A` (cover art / high scores) | 1891 | 1630 | 1.16x |

So after the blit-time work in `0007`, the crawl is still about **four times too
fast**, and the two short screens between the crawl and the demo loop are far
worse — one of them lasts two frames instead of 191. The demo-loop screens, which
are driven by the game's own vblank logic rather than by blitter waits, are
already close.

That is the shape of the remaining problem: everything the guest paces by waiting
on hardware still finishes too early, and everything it paces on the vertical
blank is about right.

## Caveats about the oracle

- The reference build **skips guest code it never translated** — it logs
  `rt-miss $ADDR` and continues, and aborts outright on a miss during gameplay
  (`$57EE6A` on the attract-mode demo). Its intro and menus are trustworthy; a
  divergence deep in gameplay may be the oracle's gap, not ours.
- It is an approximation of the hardware too, not the hardware. Where the two
  disagree and neither looks right, `vendor/libretro-uae` (the diagnostic
  emulator behind `benefactor-harness`) is the higher authority.

## Result (2026-09-10, after the VPOSR fix)

| screen (cop1lc)  | reference | interpreter |
| ---------------- | --------: | ----------: |
| `000000/007770`               |    3 |    5 |
| `007BC8/0086CC` (intro crawl) | 6290 | 6279 |
| `0077C0/0078F0`               |  191 |  127 |
| `007770/0091D0`               |  296 |  233 |
| `0081D2/00844A` (cover art)   | 4845 | 2967 |

The crawl now matches the reference to 0.2%. The cause was not pacing at all:
**OCS VPOSR (`$DFF004`) carries only LOF (bit 15) and V8 (bit 0)** — V7..V0 live
in VHPOSR's high byte. Returning the whole scanline in the low bits put V0 where
V8 belongs, so the intro's one-frame wait at `$00346E`

    btst #0,$3(a6) / beq.s  ; wait until V8 sets    (a6 = $DFF002)
    btst #0,$5(a6) / bne.s  ; wait until V8 clears

was satisfied by any two adjacent scanlines instead of once per frame — 312x too
often. See `src/engine/hw.c`, case `VPOSR`.

## What NOT to copy from the reference

The reference's intro interrupt branch does not deliver the installed vectors:

    /* the $3160 wrapper isn't recompiled, so call its leaf music driver
     * + the audio-shadow copy directly. */
    call_fn(&s_game_ctx, 0x0055A0u);
    call_fn(&s_game_ctx, 0x0058C2u);

That is a workaround for the retired translator's own missing translations, not
a description of the hardware, and copying it into the interpreter is wrong on
three counts, each measured:

- **`$0055A0` is not a leaf.** It is a tail-branch dispatcher (`bra.w $5EB0`,
  `bra.w $5812`) whose chain reaches `$5892`, which ends in RTE. Entered as a
  subroutine it returns into the game flow's own stack and hangs the boot
  spinning the one-frame wait at `$3732`; entered as an interrupt its RTS pops
  the exception frame's SR word as the high half of a return address and jumps
  to `$20000000` / `$27000000` (~2000 faults per run).
- **`$0058C2` needs no caller.** The level-6 handlers chain by rewriting their
  own vector: `$5892` ends with `addi.l #$30,$78(a0)` (a0 = 0), moving `$78` on
  to `$58C2`. The hardware reaches it through `$78` like the first handler.
- **Those bytes do not stay put.** Once a screen has been loaded over them,
  `$58C2` is somebody else's data — it read as `$FFFF` at frame 900 and trapped.

Delivering exactly what the game installed at `$6c`/`$78` gives 6279 crawl
frames and zero faults.

## What is still wrong: the interrupt is starved


The reference ran **one host iteration per displayed frame**: the game flow was
parked at its per-frame wait, the host presented, and it delivered the level-3
and level-6 vectors once each. One frame, one step of whatever the screen was
animating.

The interpreter does not. Measured over 610 presented frames of the intro crawl
(`/state`, new `irq_calls` counters):

    frames presented        610
    level-3 deliveries       21
    level-6 deliveries       21     -> one delivery per ~29 frames

Every one of those frames was presented from the beam path with the boundary
DECLINED (`beam.off_flow` is 100% of declines), i.e. from inside guest code
running on the host thread, where nothing can park. That matches
`cycles.irq6_max` of 32.2M cycles — 227 PAL frames inside a single delivery.

So the crawl's animation, which lives inside the level-6 handler, does not return
after one step: its own per-frame wait is satisfied immediately, so it runs the
whole sequence in one delivery, presenting frames as it goes. The frame boundary
never gets to drive the host iteration, and the interrupts starve.

The reference never had to solve this: the offline translator recognised the
guest's frame-wait idiom (a `btst #0,$3(a6)` / `btst #0,$5(a6)` pair spinning on
itself — see its `emitter.py`) and replaced the spin with a blocking one-frame
handoff. The interpreter executes the spin literally, and our beam model
satisfies it in about two scanlines instead of a frame.

Next: find that spin in the crawl's handler and give it real per-frame
semantics — a hardware-wait override of the kind `register.c` already uses for
the blitter wait at `$0031A0`.

## Where the remaining divergence comes from (measured 2026-09-10)

Over 10,225 presented frames of a plain boot:

    frames presented      10225
    level-3 deliveries      399
    level-6 deliveries      399     -> one delivery per ~25 frames
    beam boundaries taken   335 of 62,922 crossed (the rest declined off-flow)

One root cause explains both remaining symptoms:

- **No music during the crawl.** `$3160` -> `$0055A0` IS the music player, and
  it writes Paula directly (`lea $DFF000,a5` is its first instruction). It is
  simply called 399 times instead of 10,225, so channels 1-3 never get a
  period or a volume. Measured: `vol=[45,0,0,0] per=[320,0,0,0]` throughout.
- **The logo screens run short** (127/233 against 191/296).

The mechanism is the one this document already described: guest code reached
from the level-6 delivery runs on the MAIN thread, where `hw_vblank_wait()` is
a no-op and nothing can park, so its own one-frame wait is satisfied by the
beam immediately and the whole sequence runs inside a single delivery.

The fix is NOT to bypass the vector (see above). It is to give the frame wait
real per-frame semantics off the game flow, or to deliver level 6 on the game
thread where it can park. An earlier attempt — charging the remainder of the
beam frame in an off-flow `hw_vblank_wait()` — over-corrected badly (`0077C0`
and `0091D0` ballooned past 700 frames) and was reverted.
