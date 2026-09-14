---
id: 28
title: Level progress replays the played level once after a level-select entry
status: investigating
symptom: A first level completion replayed the same level, and only the second completion advanced (user-observed on PC)
state_items: S001
tags: gameplay,progression,level
created: 2026-09-14
updated: 2026-09-14
---

## What is known

Reported while playing on PC: level select → first level → beat it → the first
level played again → beat it again → level 2 correctly. The freeze that followed
the same transition is issue 0026 and is fixed; this is the level *index*, not the
stall.

The guest's progression is visible: the per-level main loop's win branch runs
`$5771FE: addq.w #1,$20.w` (the requested level), the level card at `$57840A`
compares `$20.w` with `$2E.w` (the level whose data is loaded) and only reloads the
level when they differ, then `$57846C: move.w $20.w,$2E.w`. So a replay means the
card was reached with `$20.w` equal to (or reset to) the level already loaded.

## What was measured, and did NOT reproduce it

Two entries were driven to a *steady* level (PC `$577130`, `cop1lc = $003484`) and
then completed with `/complete` (the same win flag the game's own teleport-out
sets), pressing fire at each prompt like a player:

* Direct entry (`--level 1`): the breakpoint trace shows `$20.w` 1 → 2 at the win
  increment, the card's compare seeing 2 ≠ 1, the level data reload, and
  `$2E.w` = 2 afterwards; the game then runs level 2.
* Title entry (no `--level`): successive completions advanced the index with no
  replay. The index was NOT 1 — a later probe measured 27, 30, and 33 on three
  runs, and the cause of those numbers is a probe artifact, established below.

Sending the win flag *before* the level's start card has been dismissed does replay
the level, because the flag is consumed while the game is still on the card — that
is an artifact of the probe, not of the transition.

## Also negative: reporting a completion and then playing on

A probe that reports `/complete` and then taps fire for a while (a headless
`--level 1` run) advances `$2E.w` from its idle `0xFF00` to `1` and reloads the
copper list (`cop1lc` `003914` → `003484`) while `level` stays `1`, which is the
level's own geometry, not a replay. Nothing in that path re-runs level 1. Third
entry tested with the same result, so the route matters and is still unknown.

## The high level indices were the probe typing a password

Re-run one press at a time through the title (`scratch/probe_title_entry.py`),
reading `/state` and `$20.w` after each press:

* No presses at all: stays on the title, `$20.w` = 0.
* Three presses: still on the title/password screen, `$20.w` = 0.
* The fourth press: gameplay at `level = 33`, `$20.w = 33`.

Nothing on the host writes that value *in this run*. `pc_set_start_level` is
called only by the `--level` option and the harness; `g_pc_start_level` stays 0
without either, and the `$150` hand-off only touches `$20.w` when it is greater
than zero.

A harness-driven probe is the other way a high index appears, and it must not be
mistaken for the guest: the earlier `probe_natural.py 1` runs logged
`[level-select] start level := 30 (applied at $150 hand-off)`, so their 30 and 33
were the *probe's own* override. Anyone reading a surprising level from a probe
should grep its log for `level-select` before concluding anything about the game. A minimal config
holding just `skip_intro` reproduces level 33, so no OPTIONS knob is involved
either: the *guest's* title code set it, which is what the password screen does
while it is live. Blind fire presses are password characters, and the differing
numbers across runs (27, 30, 33) are differing typed strings.

So none of the high indices were evidence about progression, and no host path
pre-advances the level. The transition itself was already shown correct at
arbitrary indices.

## Open questions

Which level the reported session's level card showed when it started, and which
route it took (title CONTINUE, level select, a restored
savestate, or the level card), and whether `$20.w` had already been advanced. A
savestate or in-game Load restores an older `$20.w`, which would produce exactly
one replay followed by correct progress. Needs the reproduction's entry route
before changing anything: both entries tested here are correct, so a change now
would be a guess.
