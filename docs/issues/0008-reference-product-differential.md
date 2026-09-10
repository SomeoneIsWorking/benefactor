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

## The frame boundary was being dropped 84% of the time (fixed 2026-09-10)

`0007` says never to take a frame boundary in the middle of a blit's register
sequence, and the first implementation of that did it at the boundary: a sticky
flag set by any `BLTxxx` write and cleared only by `BLTSIZE`, with the boundary
refused while it was set. Measured over 1247 frames of the intro:

    beam crossings          2,419,895
    left pending (blit)     2,036,806      -> 84% of them
    taken by the game flow         34
    host iterations                67

A deferred boundary does not update `s_beam_frame`, so every later custom-chip
access re-detects the same crossing — which is why the crossing count is in the
millions. The guest blits continuously, so the flag was effectively always set,
the game flow almost never parked, and the interrupt deliveries it gates
starved.

The hazard is the interrupt writing the SHARED blitter registers, not the
boundary itself. So the boundary is never refused any more; instead the vector
delivery lends the interrupt its own copy of `$040..$074` and hands the flow's
back afterwards (`hw_blit_regs_save` / `_restore`, used by `coro_call_vector`).
The interrupt's own blits still run — they complete at their `BLTSIZE`, inside
the saved window.

After: `pending_blit` is 0, and on the title screen the flow parks 1519 times
for 1521 boundaries — one host iteration per displayed frame, against 34 in
1248 before. The intro crawl is unchanged at 6279 frames against the
reference's 6290.

### Diagnostics added

`off the game flow` was doing double duty and hid this: it counted the host's
own renderer reading custom registers as if it were interrupt work. `/state`
now splits the crossings three ways — `beam.by_flow` / `by_irq` / `by_host` —
and reports `beam.pending_blit` with the `BLTxxx` register that set the flag,
plus `exec.on_game_thread` / `exec.owner` for who is running right now.

## Fixed: no music during the intro crawl

The intro's music player is `$3160 -> $0055A0`, delivered as the level-6 vector.
It is called once per host iteration, but a single delivery is short in
INSTRUCTIONS (never above 20,000, measured) while charging enough BLIT time for
the beam to cross ~18-25 frames inside it. So the player advances roughly once
per 20 frames and channels 1-3 never receive a period or a volume
(`vol=[39,0,0,0] per=[320,0,0,0]` throughout the crawl).

Two things were tried and rejected, both measured:

- **Answering VPOSR with a V8 that flips per read off the game flow**, so the
  guest's one-frame wait costs nothing inside an interrupt (the reference
  translator's transformation, `emitter.py: is_vposr_btst`). The wait pair does
  then fall through in one read each, but the delivery count did not move
  (45 per 447 frames, against 42 per 397 before) — the deliveries are not long
  because of VPOSR spinning, they are long because of charged blit time.
- **Delivering the vector once per DISPLAYED frame** rather than once per host
  iteration, catching up on whatever the last iteration showed. The catch-up
  computed `due=1` on 37 of 38 iterations, so the frame counter read at the
  delivery site does not advance the way the presented-frame count does. That
  discrepancy is the next thing to understand.

### What it actually was, and the fix

The blit time was real, but it was being spent inside a loop the host had
already satisfied. The intro's blitter wait is `btst #6,(a6) ; bne.s self` —
BBUSY. Our blitter completes inside the `BLTSIZE` write, so BBUSY is never
observed set: the loop exits on its first pass on merit, but every pass costs
guest cycles, and during the intro those cycles are charged to the level-6
timer interrupt. One delivery therefore held the beam for ~20 frames.

The retired translator never had this problem because it recognised the idiom
offline and emitted `hw_blitter_sync()` in its place (`tools/recomp/emitter.py`:
`is_bltbusy_btst`, `is_fire_wait_tst`, `is_vposr_btst`). `src/port/overrides/
wait_idioms.c` does the same at image-load time: it scans the freshly loaded
code for the exact encodings and registers a native override on the first
instruction of each match, which performs the host wait and resumes the guest
past the idiom. It is hooked into all three image loads (main, title,
gameplay).

Result: `beam.by_irq` fell from 11,004,903 crossings to 3,829, all four Paula
channels get a period and a volume during the crawl (`vol=[64,3,3,64]
per=[320,285,214,314]`), and the crawl itself measures 6290 frames against the
reference's 6290 — an exact match, where it had been 6279.

### What NOT to fold: the VPOSR frame wait

The translator also folded `btst #0,$3(a6)` + `beq self` + the same + `bne self`
into `hw_vblank_wait()`. Do not copy that one. The translator had no beam
model: its host call WAS the frame clock. Ours derives the beam from consumed
guest cycles, so the guest's own spin is what carries the crawl from one frame
to the next. Folding it away was measured: the crawl ended after 38 displayed
frames instead of 6290, and `beam.by_irq` went to 11 million because the
interrupt could no longer be paced by anything.

### A rule for every override that waits

**Read `rt_get_pc()` before the wait, never after.** A wait parks the game
thread; the host then delivers an interrupt on the same register file, and
`rt_get_pc()` afterwards points wherever that interrupt finished. Resuming from
it sent the guest into low memory and tripped the `$150` loader hand-off, which
restarted the game straight into gameplay at boot.

Two more things were found and left alone deliberately:

- **An access outside the decoded address space stops guest execution here,
  where a 68000 would float the bus and carry on** (the reference sends
  anything outside RAM to `hw_read16`, which answers 0). It only showed up
  while the VPOSR wait was folded and a5 was consequently wrong, so with the
  fold gone there is no evidence it is needed; leaving the fault in place keeps
  a real out-of-bounds access loud.
- **A misaligned word/long read was reported as `Unmapped`.** That one is
  fixed: the 68000 takes an address error there, and the executor already maps
  `MemoryFault::Misaligned` to `ExceptionVector::AddressError`.

### Note on `rt_call`

`Executor::call` sets the PC and runs — it pushes no return address. So
`rt_call` is only valid for an entry point whose return address the guest's own
stack already holds, or for code that never returns. Entering an RTS-terminated
leaf such as `$0055A0` with it makes that RTS pop whatever happened to be on
the stack (observed: a return to `$000000`). The intro's timer leaves must be
reached the way hardware reaches them, through the `$3160` wrapper's RTE.

## Still open: the screen after the crawl waits for fire

At the end of the crawl the game reaches `cop1lc = $0077C0` and stays there.
The reference gives that screen 191 frames — alternating `$0077C0`/`$0078F0`,
so it is animating — and then moves on to `$007770`/`$0091D0` by itself.

This predates the wait-idiom work: a build of the parent commit stalls in the
same place. What it is has now been pinned down.

**It is a wait for the fire button, and only the timeout is missing.** Driven
headless and left for 150 seconds it does not move; one `/input?fire=1` and it
runs straight on through `$0091D0` → `$007770` → `$008182` → `$0081D2`, which
is the reference's own later sequence. So the screen itself, and everything
after it, works — the game is simply never told the wait is over, where the
reference ends it after 191 frames without any input.

Three measurements to start from:

- **The game flow retires almost nothing per frame on this screen.**
  `cycles.flow` is 140,992, which is the frame `hw_vblank_wait` charges and
  essentially nothing else, and the retired-instruction ring holds only
  `$0031F6 btst` / `$0031FC bne` — the wait-until-V8-clears half of the frame
  wait at `$0031EE`. So the loop this screen sits in is the frame wait itself,
  polling for a fire press that the reference stops needing after 191 frames.
- **The level-6 delivery is small here**: `/state` reports `cycles.irq6 = 356`,
  about forty instructions, against an `irq6_max` of 20,185,094 earlier in the
  run. Whether that is correct for this screen or a handler being cut short is
  not yet established — it is the first thing to settle, because the timeout
  that ends this screen is the kind of per-frame counter such a handler owns.
- **The vector has settled on `$0055A0` itself.** `/recent` shows nothing but
  `0055A0` calls, and that routine is RTS-terminated (it runs to the `rts` at
  `$005890`; only `$0058C2` runs to an `rte`, at `$005918`), while we deliver
  every installed vector with `rt_call_interrupt`, which pushes an exception
  frame and stops at the FIRST rte. Worth checking against the reference,
  which reaches the same code as a plain call that runs to its `rts`.

Note that `rt_call` is not the answer on its own: `Executor::call` sets the PC
and runs, pushing no return address, so the `rts` pops whatever the game flow
left on its stack (observed: a return to `$000000`). Delivering a handler that
may end either way needs a return marker the executor can recognise —
hardware's own condition is "the stack pointer is back above the frame we
pushed".
