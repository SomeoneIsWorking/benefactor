# Working Agreements (hard rules for every agent)

These are the rules that keep this project correct and navigable. They live **in the
repo** (not in any one machine's assistant memory) on purpose: the work spans multiple
machines and every session starts cold, so the lessons must travel with the code.
Mirror of the spirit of the global principles, made concrete and enforceable here.

## 1. Verify before you commit — build BOTH targets from a clean configure

Run `scripts/build.sh` before pushing. It does a clean `cmake` configure + builds
`benefactor-pc` AND `benefactor-harness`. A green local single-target build is NOT
enough: this repo links the same sources into two executables, and the generated code
is regenerated at configure time.

**Why this rule exists:** a commit that built locally still broke the build on the
other machine because only one target / only the dirty working tree was exercised. The
clean two-target build is the gate that catches it.

## 2. Never commit half a coupled change; never stage a file that already had the user's edits

`git add <file>` stages the file's ENTIRE current content — including any in-flight work
that was already there. If you stage a caller but leave its definition (in another file)
uncommitted, HEAD fails to link on a clean checkout.

- Before staging, check `git status`: a file that was already modified at the start of
  your session is the user's territory. Use `git add -p`, or commit only files you
  exclusively created/own.
- A caller and its definition are ONE change — commit them together or not at all.

**Why this rule exists:** staging a whole file swept in the user's in-progress work,
landing the caller without the definition → undefined-symbol link error on the other
machine (arm64). See git history around the EngineView firewall commits.

## 3. No bandaids — derive from engine state, or reverse-engineer it; never curve-fit

This is a recompilation: there is a correct answer in the original engine for almost
everything. Do not paper over a symptom.

- **No magic constants / learned offsets / frozen caches / second sources of truth.** If
  output is misaligned, find the engine variable that holds the truth and read THAT.
- The renderer reads engine state through `render/engine_view` (the "firewall") and is
  denied access to the displayed copper output precisely so it can't reverse-project a
  value from its own output. Keep it that way: if you need a quantity the firewall
  doesn't expose, add a sourced accessor (cite the engine address) — don't reach around.
- If a real fix is genuinely too big right now, say so plainly and mark any stopgap
  in-code as `// STOPGAP: <proper fix> because <why>`. Don't slip a hack in as a fix.

## 4. The include convention is src-rooted by module

`#include "engine/hw.h"`, `"render/native_renderer.h"`, `"port/port.h"`, `"common/log.h"`.
`src` is the sole include root, so every include line names its module. New files follow
this. (Exceptions are documented in `docs/codebase-layout.md`.)

## 5. Generated code is sacrosanct; change the recompiler, not the output

`src/engine/generated/` is regenerated from the disks by `tools/regen.sh`. Never
hand-edit it. To change emitted code (including the `#include`s it emits), edit
`tools/recomp/` (e.g. `recomp.py`) and regenerate.

## 6. Keep the docs honest and in lockstep

`docs/codebase-layout.md` is the canonical map. When you move/rename/add a module or a
key file, update it (and `CLAUDE.md`'s Key Files) in the SAME commit. A confidently-wrong
doc is worse than none — it sends the next cold-start agent down a dead end. If you act
on a note and find it false, fix the note.

## 7. Test via the harness/headless, not blind windowed runs

Use `benefactor-harness` (REPL: `fire`, `goto N`, `fbw`, `pcread`, …) or
`benefactor-pc --headless --level N` with `BENEFACTOR_PRESS`/`BENEFACTOR_DUMP_FRAME`.
Don't launch a windowed standalone with no input control as a "test". Trace via REPL
commands, not scattered `getenv()` debug logs.
