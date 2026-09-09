---
id: 7
title: Native overrides hung the interpreter and the beam ran on read counts
status: resolved
symptom: Pressing fire never reached gameplay — a frame never finished and the watchdog killed the app; the intro also ran tens of times too fast.
state_items: S005,S020,S021
tags: interpreter,overrides,timing,diagnostics
created: 2026-09-09
updated: 2026-09-09
---

Three defects, all from the same root: behaviour the retired static translator
supplied implicitly, which the interpreter requires a native owner to state.

CAUSE 1 — unterminated native replacements. Under the translator, a native
override function returning WAS the guest subroutine returning. Under the
interpreter the guest PC is architectural state: a replacement that neither
called the original, jumped, nor consumed the guest return leaves the PC on its
own address, so the executor re-enters it forever. `$0031A0` (blitter wait and
frame init) hung the first frame of every boot this way; `$58656E` (SFX
trigger), the platformer physics family, the menu cursor/difficulty/glyph
routines and `$0074AA` had the same defect on their own paths.

CAUSE 2 — image replacement unwound an intentional transition. `$6D714` loads
the title overlay, activates the new image and jumps into it. The executor ends
a run whose image was replaced, so the whole cold start unwound and the app
stopped the moment fire was pressed. A native continuation IS the transition, so
it now re-authorizes the run for the image the override chose.

CAUSE 3 — the beam was counted, not timed. `hw_step_register_beam` advanced one
scanline per VPOSR/VHPOSR READ, so a guest busy-wait retired almost instantly
and the game ran far more logic per displayed frame than hardware would (the
visible symptom: the intro crawl). Worse, whichever thread crossed the frame
boundary consumed it, and interrupt handlers run on the host thread — the
presenter saw 5 frames for every 139 the guest ran.

FIX:
- `shared/amigaport` fails closed with `UnterminatedNativeOverride` when an
  override returns with the PC still on its own address, re-authorizes the run
  on a native continuation, and honours an explicit `hand_off_to_host` exit.
- `rt_register_replacement`/`_gp` register a native body that wholly replaces a
  guest subroutine; the adapter completes its RTS when the body left the
  boundary untouched. `rt_exit_to_host` states a deliberate unwind (the `$150`
  gameplay hand-off, the `$59C5B0` game-over restart).
- The beam is derived from the guest's own consumed 68000 cycles (454 per PAL
  line, 312 lines per frame) through `rt_get_guest_cycles`, and only the game
  flow may consume a frame boundary; a boundary crossed inside an interrupt
  stays pending for the game flow's next register read.

DIAGNOSTICS ADDED (the reason this was findable at all): a per-instruction
retired-PC/opcode ring in the executor, exposed as `rt_insn_ring_snapshot` /
`rt_insn_ring_entries`; a live `rt_get_pc`; recent guest call targets; a named
reason for EVERY guest-call exit; the unterminated-override report naming the
instruction that entered it; `pc_log_retired_instructions` on an unexpected flow
return; watchdog output carrying guest PC plus the retired tail; and `/trace`
plus `instructions`, `guest_cycles`, `fps` and frame-time sections on `/state`.

NOT THE CAUSE: guest addresses did not move. Every override was entered at its
original address, from the instruction the retail image branches with — the
`$0031A0` report reads `entered from $00366A opcode=$6100 return=$0000366E`, a
retail BSR to that exact address.

VERIFIED headless with Disk.1-Disk.3: boot reaches the attract sequence and the
menu unaided, fire enters level 1, held right and fire+up move the player
(`[32,166]` → `[230,154]`), 6300+ frames with zero runtime errors, ~4.8 ms of
host time per gameplay frame. Windowed: a steady 50 fps with the intro, high
scores, level card and gameplay all rendering. `tools/verify.py` green in both
repositories; `amigaport` ctest green including three new executor tests.

OPEN: the guard is the safety net for any override not yet classified —
`rt_register_override` still fails closed rather than guessing a boundary, so a
path reached for the first time reports its address instead of hanging. A
windowed run also ended cleanly (`SDL_EVENT_QUIT`, no fault) after about a
minute unattended; not reproduced headless and not diagnosed.

## Follow-up (2026-09-09): the intro crawl

The crawl still jumped instead of scrolling. Four separate faults, each found by
a measurement rather than a guess:

1. **DMACONR reported the wrong bits.** BBUSY is bit 14 and BZERO is bit 13; the
   read put BZERO at 14. The classic `btst #6,$DFF002` WaitBlit therefore saw
   the blitter busy once after every blit, and nothing could ever observe BZERO.

2. **The beam was sampled only on VPOSR/VHPOSR reads.** The crawl syncs on the
   blitter and drives its blits register by register without reading the beam,
   so it never crossed a frame boundary and the watchdog killed the frame. The
   beam is now sampled on every custom-chip access, read or write.

3. **Interrupts rewound guest time.** `Executor::call_interrupt` restores the
   saved CPU state when the handler does not reach its RTE — including
   `elapsed_cycles`. The host derives the beam from that counter, so the clock
   froze during an interrupt and then jumped. `elapsed_cycles` is now carried
   across the restore (amigaport `9379082`+).

4. **The crawl's animation loop lives inside the level-6 handler**, which runs
   on the host thread where the per-frame wait cannot park anything. One
   delivery ran up to 14M cycles — 99 PAL frames — of crawl in a single host
   frame, and was then cut off and rolled back by the interpreter's 1M
   instruction budget: the whole thing discarded, then jumped. A frame boundary
   crossed on the host thread now presents and paces in place, so a frame
   waited for inside an interrupt is a frame the viewer sees. To keep the two
   present paths from pacing twice per guest frame (which halved the speed to
   21 fps), `hw_present_frame` refuses a beam frame it has already shown.

Also fixed: `$0052A4` (`native_post_blit_handler`) reimplements its routine
outright and was registered as a wrapper, so it never completed its guest
boundary — the executor's fail-closed guard reported it by address.

DIAGNOSTICS ADDED: `src/port/frame_accounting.{c,h}` — per-owner guest cycles
for the last host iteration (game flow / level-3 / level-6 / present / whole
iteration) each with a PEAK, beam boundaries crossed/taken/declined, per-frame
waits reached/refused/parked, and presents/re-entrant. All on `/state`; the beam
counters and the guest owner are in the watchdog report too. The peaks are the
point: a runaway iteration is invisible to a sampler, which only ever reads what
the previous short iteration left behind.

VERIFIED: windowed, the guest advances 7,120,665 cycles/s against a PAL ideal of
7,082,400 (0.5%); a steady ~50 fps through the full intro crawl, high scores and
into level 1 with zero runtime errors; the crawl scrolls its text legibly frame
by frame instead of jumping.
