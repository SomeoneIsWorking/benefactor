# Skill: Debug Journal

Record a confirmed Benefactor debugging fact using `store_memory` so it is never re-examined. Use AFTER every debugging conclusion: root cause confirmed, override added and verified, or recompiler bug fixed.

> **Self-evolution:** If you extend or build a tool that changes this skill's procedure, update this file in the same step.

## Purpose
Persist confirmed facts across sessions using the `store_memory` tool. Call it **frequently** — after every confirmed root cause, every override added, every recompiler fix.

**DO NOT create markdown journal files. They bloat context. Use `store_memory` only.**

## When to invoke (immediately after any of these)
- A chip RAM address has a confirmed purpose
- A recompiled function's behavior is fully understood
- A native override is added and verified by the harness
- A recompiler bug is confirmed and fixed
- A snap/frame-hook sequence fact is established
- Any significant narrowing step produces observable evidence

## Procedure

Call `store_memory` with:
- `subject`: one of `blitter behavior`, `copper list layout`, `native overrides`, `recompiler bugs`, `harness timing`, `project workflow`, `harness methodology`, `harness boot`
- `fact`: single concise statement (include addresses, values, function names). Max 200 chars.
- `citations`: file + line number, or harness output snippet, that proves the fact
- `reason`: why this prevents future re-examination (2-3 sentences)

**Also update `instructions/current-state.md`** whenever:
- A DIFF is resolved or a new one discovered
- An override is added, changed, or removed
- A root cause is confirmed
- Debugging hints or lessons are learned
- Harness architecture or timing is better understood

This file holds all active state — current DIFFs, porting status, debugging hints. It is not a journal; keep it as a current snapshot, not a history.

Then, **if the fact belongs in the always-loaded summary**, add a DO NOT RE-EXAMINE entry to `instructions/harness.md`.

If an override was added or changed, update the overrides table in `instructions/create-override.md`.

## Anti-Patterns
- Do NOT store hypotheses — only confirmed, verified facts.
- Do NOT create markdown files for journaling.
- Do NOT batch facts into one store_memory call — one call per fact.
- Do NOT re-store facts already in `instructions/harness.md` (they load automatically).
