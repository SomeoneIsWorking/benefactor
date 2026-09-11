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

### The frame signature: what the screen played and showed

Frame counts alone let a regression through. A change was landed that matched
the reference screen for screen and still broke the crawl music, the logo fades
and gameplay (reverted in `02fd7f3`) — because a frame count cannot see a
melody or a fade.

Both builds therefore also emit, once per CHANGE:

```
sig: frame=<n> pal=<hash> alc=<AUD0..3 sample pointer> aper=… avol=… adma=…
```

`src/port/frame_signature.c` in this product; the same code injected into the
reference's `hw_present_frame` by `REFERENCE_EDITS`. The tool counts, per
screen, the frames on which

- a channel's **sample pointer moved** — the tune advancing,
- the **palette changed** — a fade ramping,
- a channel's **volume changed** — the mixer moving,

and reports reference vs interpreter for each. A stalled tune, a frozen fade
and a runaway one all show up here and nowhere else. Any timing change must be
green on BOTH tables before it is landed.

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

## Found and fixed: the crawl had no music because a wrapper ran past an RTE

**The cause was a legacy native override, not timing.** `$0055A0` is the
level-6 timer leaf — the music player. It was wrapped by
`native_timer_interrupt`, which existed back when the host called it directly
and needed to control how often it ran. The guest's own vector wrapper
(`$003160`) already does `bsr $55A0` once per delivery, exactly as the
reference product does, so the override was a second caller — and a harmful
one. `rt_call_original` runs the guest **without stopping at an RTE**, and
`$55A0`'s chain ends at one (`$005892`). Past that RTE the run carried on into
the code the interrupt had interrupted — the crawl's own outer loop at
`$003732` — until the million-instruction budget ended it, at which point the
whole run rolled back.

Measured before and after, over 1498 frames:

| | before | after |
| --- | --- | --- |
| level-6 deliveries | 81 | 1529 (once per frame) |
| worst single delivery | 17,635,874 cycles (124 frames) | 4,284 cycles |
| Paula volumes | `45,0,0,0` | `32,30,64,26` |
| Paula periods | `320,0,0,0` | `428,157,428,339` |
| crawl loop attributed to | the level-6 vector | the game flow |

The fix is to delete the wrapper. A native owner for the timer has to be
entered AS the vector, so the delivery's own RTE boundary applies to it; a
wrapper around a routine the guest is already calling does not get that
boundary. `src/port/overrides/render.c` carries the warning in place of the
code.

### How it was found, after two instruments failed

The per-screen frame counts said the crawl was correct (6290 frames, matching).
A one-shot register snapshot said audio channel 0 was playing. Both were true
and both were useless. What found it:

1. **The frame signature** (above) turned "the music doesn't progress" into
   259 tune advances in the reference against 2 in ours.
2. **The hot-PC profiler** (`src/port/guest_profile.h`) attributed guest time
   per owner and named `$003732 82%` under the level-6 vector — the game
   flow's own frame wait, running inside a timer interrupt. The
   retired-instruction ring could not show this: it is one global list, so it
   showed the loop without saying whose cycles it was spending.
3. **The guest-call exit log, extended to name the ENTRY** — `entry=…` in
   `guest call exit:` — which separated "the flow's own slice ran a million
   instructions" from "a wrapper's `call_original` ran a million
   instructions". They print identically otherwise, and they want opposite
   fixes. That line named `call-original($0055A0)` and the search was over.

Each of the three was built because the previous one could not see the fault.
Build the instrument before the fix.

### Measured and rejected: folding the blitter wait idiom

The first diagnosis was that the crawl's `btst #6,(a6) ; bne self` blitter
poll was burning the interrupt's time, and a fold of it was landed and reverted
(`92282f7`, `02fd7f3`). Re-measured narrowly afterwards: folding it in the
main image (14 sites) changed **nothing at all** — music 2/259 and volume
54/3027, identical to not folding it. The idiom was never the fault. Do not
re-land it without a measurement that moves.

## Attempted and reverted: no music during the intro crawl

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

Measured headless, that worked: `beam.by_irq` fell from 11,004,903 crossings
to 3,829, all four Paula channels got a period and a volume during the crawl
(`vol=[64,3,3,64] per=[320,285,214,314]`), and the crawl measured 6290 frames
against the reference's 6290 — exact, where it had been 6279.

**It was reverted anyway.** Played rather than measured, it broke three things
the frame counts and the register dump could not see: the crawl music does not
progress normally, the logo fades are wrong, and gameplay is broken. Frame
counts per screen and a snapshot of the Paula registers are not enough
evidence to land a change to the guest's own timing — what the screen and the
speaker actually do has to be checked too.

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

## The comparison now reaches gameplay

Every table above stops at the attract loop, because neither product was ever
told to press anything: the reference build understood a single
`BENEFACTOR_PRESS=<frame>`, and one press only gets as far as the main menu.
So "gameplay is broken" had no measurement behind it at all — the screen the
tables called `$0081D2` is the credits scroller, not the game.

Reaching gameplay takes three presses (credits → menu → level intro → playing),
and the comparison only means anything if both products press on the *same
frame* — driving them over the control channel cannot promise that, because two
HTTP round trips are wall-clock and the products do not run at the same speed.

So both now parse a frame-indexed fire timeline,
`BENEFACTOR_PRESSES=7300:8,7420:8,7560:8` (frame:frames-held):

- the interpreter product in `src/engine/hw_testrun.c`;
- the reference through one more `REFERENCE_EDITS` entry in
  `tools/oracle_diff.py`, the same way the frame signature is injected.

`tools/oracle_diff.py --play 7300:8,7420:8,7560:8` sets it on both. Measured in
the interpreter product, those frames are deterministic: credits at 7161, main
menu at 7334, the level running at 7612.

That the frames can be fixed at all is a consequence of the vblank charge — the
four screens before the credits now match the reference exactly, so a frame
number means the same thing in both products up to that point.

## Found and fixed: the intro music ran at half speed because only one link of the level-6 chain was delivered

With the frames matching exactly, one column still did not: the tune advanced
145 times over the 6290-frame crawl where the reference advanced 259, and the
channel volumes moved 869 times against 3028.

The intro's level-6 handlers **chain by rewriting their own vector**. Delivering
the `$78` vector once per displayed frame therefore ran a *different* link each
frame, so the music driver ran on every other frame:

| link | what it does | instructions |
| --- | --- | --- |
| `$003160` | saves registers, `BSR $0055A0` (the music driver), clears INTREQ, RTE | 83 |
| `$005892` | acknowledges CIA-B, re-arms timer A (`CRA=$19`), writes DMACON | 9 |
| `$0058C2` | copies the audio shadow into Paula — `AUD0LC`, `AUD0LEN`, … | 17 |

Once per frame was never a rate the game asked for. It programs CIA-B timer A
with a latch of **384** against the 709379 Hz E-clock — about **37 deliveries
per PAL frame** — so on real hardware every link of the chain certainly runs
within one frame. (That latch is now reported at debug level, once per change,
from the CRA write: `ciab: timer A cra=$19 latch=384 -> 36.95 per PAL frame`.)

So `coro_deliver_timer_irq` now delivers the whole chain, each **distinct**
vector at most once per frame. Both halves of that sentence were measured:

| | crawl music | crawl volume | logo volume | `$0077C0` volume |
| --- | --- | --- | --- | --- |
| reference | 259 | 3028 | 22 | 131 |
| one link per frame | 145 | 869 | 1 | 102 |
| walk until the vector stops moving | 525 | 3039 | 22 | 250 |
| **each distinct link once** | 251 (ch0) | **3028** | **22** | **131** |

The middle row is the trap: the chain is a *ring* — `$3160` moves `$78` on and
`$58C2` moves it back — so walking it until the vector stops changing goes round
twice and runs the driver twice a frame.

Volumes, palette fades and frame counts are now identical to the reference, and
so is the melody: channel 0's pointer sequence is the reference's sequence
offset by one entry (`06594E, 064446, 064DE0, 064446, …`), 251 changes against
229, with the same gap distribution (commonest gaps 8, 48, 16, 24 frames in both).

### Still open: channels 1-3 are re-pointed more often than the reference

| channel | reference changes | interpreter changes | distinct samples |
| --- | --- | --- | --- |
| 0 (melody) | 229 | 251 | 7 vs 9 |
| 1 | 7 | 315 | 4 vs 4 |
| 2 | 39 | 203 | 9 vs 11 |
| 3 | 17 | 267 | 8 vs 11 |

Nearly the same small set of samples, revisited far more often, and on a regular
period (channel 3's commonest gaps are 1 and 7 frames) rather than at random —
so this looks like rhythm the reference is not playing, not spurious restarts.
Two reasons to think the interpreter may be the more faithful one here, which is
why this is recorded rather than "fixed":

- the reference **never runs `$005892`** at all, and cannot: a static recompiler
  takes no asynchronous interrupts, so it calls `$0055A0` and `$0058C2` by hand
  and skips the handler whose job is to acknowledge and re-arm a timer it does
  not have. Seven pointer changes on channel 1 in 6290 frames is one every ~15
  seconds, which is not obviously a bass line.
- the volumes match to the exact event count (3028 against 3028). A channel
  being restarted spuriously would show up there first.

Deciding this needs listening, not counting. See `docs/oracle.md` on why a
difference from the oracle is a lead and not a verdict.

#### The obvious explanation is ruled out

The tempting answer was `$005892`: it is the one handler we deliver and the
oracle cannot, so anything we do extra should come from there. It does not.

With the audio trace naming the guest PC of every register write that actually
changes a value, the channel 1-3 pointer writes come overwhelmingly from
`$0059BC` (owner 6). A breakpoint there, held at the instant of the hit, gives
the real guest stack:

```
/break?at=59BC
  {"stopped":{"at":"0059BC","pc":"0059BC","frame":36,...}}
/cpu   -> a7 = $0007FFAA
/mem?addr=07FFAA -> 00005640 00003168 00025384 00000057
```

`$003168` is the return address inside `$003160`, immediately after its
`BSR $0055A0`. So the chain is `$003160 -> $0055A0 -> ... -> $005640 ->
$0059BC`: the writes come from **the music driver, which the oracle calls
too**, not from the extra handler. Both products run that driver once per
frame.

So the difference is not "we run something the oracle does not". It is that the
same driver, run the same number of times, reaches a different decision — which
points at the state it reads (the CIA-B timer having been acknowledged and
re-armed, DMACON, or a counter one of the other links advances) rather than at
the delivery count. Recorded here rather than guessed at further.

#### A second delivery path exists, and it is not guarded like the first

Found while making the frame accounting an object: level 6 is delivered from
**two** places, and they are not the same.

| | `coro_deliver_timer_irq` | `pc_music_tick` |
| --- | --- | --- |
| when | once per presented frame, whole chain | the per-screen sub-frame rate |
| entry | `GUEST_ENTRY_RTE` | `GUEST_ENTRY_RTE` (identical) |
| owner recorded | `PC_OWNER_LEVEL6_TIMER` | **nothing** — attributed to the flow |
| blitter registers lent | yes | **no** |

The entry is identical, so this is not a wrong-return question. Two things do
differ, and both matter:

- **The accounting lies.** Cycles burned by the sub-frame music deliveries are
  charged to the game flow, and those deliveries are not counted at all. So
  `irq6` under-reports and `flow` over-reports by however much the music
  player costs — on a screen driven from inside an interrupt, that is most of
  the frame. Every earlier reading of this instrument on the intro screens was
  wrong in that direction.
- **The blitter guard is missing on the busier path.** The frame-loop path
  lends the interrupt a copy of the blitter registers precisely because the
  registers are one shared set and a delivery landing between a `BLTxxx` write
  and `BLTSIZE` merges two blits into one runaway blit (see issue 0007). The
  path that delivers *more often* does not do that.

Whether the missing guard is what makes the same driver reach a different
decision is untested — that is the open question above, and this is a lead, not
a verdict. But the accounting error is not a lead, it is a fault in the
instrument, and it has to be fixed before any further reading of `irq6` against
`flow` means anything.

### Stop guessing where: run the two products in step

The tables above compare two runs that have already finished, so every reading
is a symptom hundreds of frames downstream of its cause. `tools/lockstep.py`
(documented in `docs/oracle.md`) removes the guess: both products stop at the
end of every presented frame, report a digest of guest memory plus what they
are playing and showing, and neither starts the next frame until the two
reports have been compared. The run stops on the FIRST disagreement, with both
products still parked on that frame, and dumps and byte-diffs both memories.

Walking the divergence forward with it found two faults in this product, both
fixed:

- **Title-menu overrides fired in the intro.** `$003872`, `$0039D0`, `$0049B6`,
  `$003C5A`/`$003C6E`/`$003C88`/`$003C9A`, `$003700` and `$003DAA` were
  registered for every image. The intro reuses those guest addresses, so the
  menu's native bodies ran against the crawl and corrupted its text. They are
  now registered for the TITLE image only (`rt_register_override_title` /
  `rt_register_replacement_title`).
- **Per-frame interrupts were delivered for frames nobody saw.** `hw_present_frame`
  declines a beam frame it has already shown and says so only by leaving the
  frame counter alone, so an iteration that presented nothing still delivered a
  level-3 tick. One extra delivery on frame 34 put the intro's volume ramp a
  frame ahead (60 -> 64 against 60 -> 63) and its tick counter ahead for the
  whole run. `pc_step_threaded` now delivers only when the frame counter moved.

A fifth was established the same way, and it is a whole FRAME rather than a
field, so it gets its own list (`REALIGNMENTS` in `tools/lockstep.py`):

- **Frame 7160, the intro → poster handover.** `$0033A6` points `COP1LC` at
  `$008182`; the poster then runs two blits and patches the six bitplane
  pointers into that same list at `$0082BC` before `$00345A` points `COP1LC` at
  the finished `$0081D2`. A blit costs this product guest time (issue 0007), so
  the beam boundary falls between the `BLTSIZE` write at `$003424` and the
  `BBUSY` poll at `$00342A`, and the half-built list — no bitplane pointers in
  it — is shown for one frame. The reference charges nothing for a blit, so its
  whole pass lands inside one frame and it never shows `$008182`.

  Measured with a breakpoint on `$00345A`: at that instant the frame counter
  says 3 and `COP1LC` is already `$008182`, and the retired-instruction ring
  shows the level-3 and level-6 handlers running between `$003424` and
  `$00342A` — which is the host presenting. Real hardware takes that time, so
  this product is the faithful one; the extra frame is real and stays. What the
  tool does is stop comparing it against the wrong partner: every frame after
  it was a frame out of step, so the same single difference was reported again
  and again and gameplay could never be reached. `--no-realign` turns the drop
  off.

### The frame boundary was the whole story

Every difference above and below traces to one thing, and it took the whole of
this investigation to see it. The oracle had **no cycle model**. Its frames
began and ended only at the host waits its translator had put in place of the
guest's busy-wait loops. This product derives the beam from consumed guest time
— which it must, because the game polls VPOSR and that has to read honestly —
and it also ENDED the frame there, wherever a frame's budget ran out.

A blit costs this product four tenths of a frame. So the boundary landed
between the `BLTSIZE` write at `$003424` and the `BBUSY` poll at `$00342A` that
waits for that blit, in the middle of the poster rebuilding its copper list:
its first displayed frame was `$008182` with the six bitplane pointers at
`$0082BC` still null, where the oracle showed the finished `$0081D2`. Every
per-frame counter after it was a tick out, `$0065D4` first, and its countdown
then started the title music a frame early.

The fix is to separate the two questions the boundary was answering:

- Crossing the beam boundary **raises** it (`src/engine/hw_beam.c`).
- The game flow reaching one of **its own waits** lands it.

For the second half to mean anything, this port has to know where the guest
waits. `src/port/wait_idiom.h` recognises the shape — two instructions, a read
of one custom register and a conditional branch straight back to it, no body —
and `src/port/overrides/wait_idioms.c` scans the player's own decrunched image
at load time and registers a native owner at each one: the VPOSR-bit-8 pair
becomes `hw_vblank_wait`, each half becomes `hw_beam_wait_below`/`_above`, and
the `BBUSY` poll becomes nothing at all, this port's blitter being synchronous.
The boot image has 11 frame waits, 11 halves and 14 blitter polls. Registration
is by address, so a pattern matching bytes the guest never executes is inert.

Measured frame by frame after the change: **the screen sequence matches the
oracle exactly** — no `$008182`, no `$0033E0`, no extra screen pair — and 7157
frames are byte-identical, and the poster handover lands on **frame 7160, the
same frame as the oracle**.

Two things remain, both inside the handover and both measured:

- The title overlay's decrunch lands a frame early here — at 7158 against the
  oracle's 7159, 2292 bytes of the title music driver at `$005F34`-`$006DFF`.
  The load itself is native in both products, so this is when the guest reaches
  it, not how long it takes.
- The poster's own fade then runs fast for its first frames: at 7161 its
  palette is two steps on here (`6DAB8C1E` against `E4C64D45`), with the
  copper's colour block at `$008240`-`$0082B9` differing to match. The guest is
  completing more of its frame-wait iterations per PRESENTED frame than the
  oracle does, which points at a wait that produced no present — `hw_present_frame`
  declining because the interrupt path had already shown that beam frame. That
  is the next thing to measure and it is not yet explained.

The hold cap is **2**, and it was measured rather than argued. A cap of 1 reads
better — a second held boundary puts two frames of guest work into one
presented frame — but the poster's rebuild does not fit inside one held frame,
and at 1 the whole fault comes back: frame 7160 shows `$008182` with null
bitplane pointers again and 23548 bytes of the 8 MB differ. At 2 that frame is
gone. Raising it further changes nothing measured.

Four differences were established as the *oracle's* limits instead,Four differences were established as the *oracle's* limits instead, and are
recorded with their evidence at the top of `tools/lockstep.py` rather than
"fixed" toward the reference: the level-6 autovector `$78`, the guest stack,
the audio DMA enables, and the `AUDxLC` sample pointers.
