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
* Title entry through the level-select panel: the override applied `$20.w` once at
  the `$150` hand-off, and successive completions advanced the index
  30 → 31 → 32 → 33 with no replay.

Sending the win flag *before* the level's start card has been dismissed does replay
the level, because the flag is consumed while the game is still on the card — that
is an artifact of the probe, not of the transition.

## Open questions

Which route the reported session took (title CONTINUE, level select, a restored
savestate, or the level card), and whether `$20.w` had already been advanced. A
savestate or in-game Load restores an older `$20.w`, which would produce exactly
one replay followed by correct progress. Needs the reproduction's entry route
before changing anything: both entries tested here are correct, so a change now
would be a guess.
