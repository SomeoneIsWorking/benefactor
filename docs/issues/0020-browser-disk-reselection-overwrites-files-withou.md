---
id: 20
title: Browser disk reselection overwrites files without restarting game
status: resolved
symptom: After starting WASM gameplay, choosing another disk set rewrites Emscripten files while benefactor_web_start returns success without restarting the running game.
state_items: S031
tags: browser,disks,lifecycle
created: 2026-09-12
updated: 2026-09-12
---

## Root cause

The browser setup treated each chooser change as a fresh start, but the WASM
entry point is deliberately one-shot. A second validated selection replaced
`/Disk.1`–`/Disk.3` in Emscripten FS while the existing runtime kept executing.
The setup also published `lastValid` before filesystem writes and native start
succeeded, so a failed start looked committed.

## What was tried / dead ends

The shipping JavaScript was exercised through a Node VM with fake File,
Emscripten FS, and native-start boundaries. Before the change, the second
selection made a second start call and a failed start left `lastValid` set.

## Resolution

Make disk setup an explicit one-shot lifecycle: pre-commit validation failures
remain retryable; validation runs are mutually exclusive; filesystem/native
start failures and successful starts keep the chooser disabled and tell the
player to reload. Publish `lastValid` and the validated event only after the
native start succeeds. Five shipping-script regression cases cover success,
failed start, retryable validation, partial FS write, and overlapping selections.
