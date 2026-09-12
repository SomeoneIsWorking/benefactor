---
id: 21
title: No-pace turbo confounded the level-1 oracle comparison
status: resolved
symptom: In a headless unpaced frame-480 capture, the interpreter retained GET READY while the static reference had entered gameplay.
state_items: S005,S023
tags: diagnostics,oracle,timing
created: 2026-09-12
updated: 2026-09-12
---

The interpreter checkout's ignored `benefactor.json` selected `game_speed=turbo`
(120%), while the separate static reference ran at normal speed. The diagnostic
also set `BENEFACTOR_NO_PACE=1`. At speeds above 100%, `hw_audio_frame_due()`
uses a real-time 20 ms clock to keep the music at normal tempo; removing frame
pacing lets hundreds of game frames pass before the card music advances. The
card's guest loop at `$57859E..$5785C4` waits for music state `$59CF29` to
change, so this test setup delayed the transition indefinitely in frame-count
terms. It was not evidence of a renderer or interpreter regression.

With `BENEFACTOR_GAME_SPEED=normal` set on both products, the interpreter clears
GET READY at the reference's transition and the frame-480 audio signature
matches exactly. A paced turbo run also clears the banner by frame 550. The
remaining one-frame gameplay animation discrepancy is tracked separately in
issue 0016; user-reported missing graphics and lines require their own
matched-configuration comparisons. No diagnostic emulator was used.
