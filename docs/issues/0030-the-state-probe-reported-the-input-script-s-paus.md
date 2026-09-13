---
id: 30
title: The state probe reported the input script's pause, not the pause menu
status: resolved
symptom: /state's paused field read the harness input script, and adding a field beside it silently misaligned the JSON
state_items: S017
tags: diagnostics,control,input
created: 2026-09-14
updated: 2026-09-14
---

## Root cause

Two faults in one probe, and together they cost an afternoon of wrong
conclusions about the pause menu:

* `/state`'s `paused` was `InputScript::instance().paused()` — whether the
  *harness input script* is playing — while everyone reading it, including this
  port's own touch verification, meant the game's pause menu. `/menu` is the
  route that toggles the menu and reports `pc_pause_active()`; `/pause` and
  `/resume` belong to the script. A probe whose name means the wrong thing is
  worse than no probe: it answered "the menu is closed" every time it was asked.
* Adding a field beside it (`script_paused`) for the script left the `formatted`
  argument list one short, so `%d` for the new field consumed the *next*
  argument and every later value in the JSON shifted by one. The numbers stayed
  plausible, so the misalignment showed up only as "the pause menu closes
  immediately", which is not a thing the code can do: `s_paused` is written to 0
  in exactly three places, all inside `pc_pause_tick`, and that path logs now.

## Resolution

`paused` is `pc_pause_active()` again — the game's own pause menu — and the
script's state is the separately named `script_paused`, with the argument list
matching the format string. Verified by measurement rather than by reading the
JSON: `/menu` then `/state` now reports `paused 1` and stays there, and on the
emulator the pause control opens the PAUSED panel and closing it returns
`paused 0`.

`/state` still has no test seam: it is one long `formatted(...)` call that only
ships with real disks, so a future mismatch will again present as a truthful
field going wrong. Worth splitting into per-section builders the next time it is
touched.
