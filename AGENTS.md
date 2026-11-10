# Benefactor

Benefactor is one native/interpreter hybrid product. Hand-written native owners provide
disk loading, Amiga services, rendering, audio, input, UI, and deliberately
replaced game behavior. Every other 68000 instruction executes directly from
the player's authenticated disks through `shared/amigaport`'s maintained 68000
execution owner.

The offline 68000-to-C translator, generated corpus, static dispatcher, and
static launch/build paths are deleted from the product tree. Never recreate
them here as a bridge, cache, fallback, or convenience; the `origin/oracle`
branch is a separate behavioral reference only (`docs/oracle.md`).

## Repo docs

Beyond the standard docs: `docs/migration.md` (runtime integration order and
conformance gate), `docs/oracle.md` (the retired static product used as a
behavioral reference), `docs/hardware-layer.md`.
`tools/harness_tools.py` resolves the re-harness checkout (`../re-harness` by
default, or `RE_HARNESS_DIR`); `uv run --frozen python -m tools.harness_tools`
prints where it found it.

## Execution contract

- Gameplay is native code plus `amigaport`'s maintained 68000 interpreter. It
  consumes the player's disk images directly and never emits guest C/C++ or
  host objects. Benefactor does not embed its own CPU core or full emulator.
- The interpreter is a permitted shipping CPU for this title. Representative
  interactive gameplay (not boot, menus, video, or idle presentation) must
  prove correctness and sustained performance on x86-64, Apple Silicon macOS,
  and Android arm64-v8a.
- `amigaport` owns the complete architectural CPU state, instruction semantics,
  execution lifetime, exceptions, interrupts, and bounded exits.
  Benefactor must not maintain a reduced parallel CPU model.
- Benefactor owns exact disk identity, live image mapping, OCS/CIA services,
  title policy, native override registration, and host subsystems.
- Main, title, gameplay, and credits reuse guest addresses. Override and active
  execution identity includes image kind, generation, and address. Loading or
  restoring an image replaces that executable-image identity before execution resumes.
- A scoped original call suppresses only its current override key and executes
  the original guest body through the execution owner. It never names a host-generated
  function.
- Unsupported behavior fails with guest PC, image generation, and decoded bytes.
  The static recomp on `origin/oracle` is the behavioral oracle for interpreter
  differences; binary analysis and the shipping interpreter path explain the
  cause. The static product stays outside this tree's build and release.

## Preserved native seams

- `src/engine/disk_boot.*` and `src/engine/overlay_load.*` retain disk, ATN,
  relocation, and four-image knowledge. Move them to explicit adapter memory;
  do not regenerate source from their bytes.
- `src/engine/hw*`, `src/render/`, and `src/port/` retain native device,
  presentation, input, UI, and enhancement behavior.
- `src/port/overrides/` calls guest addresses through the image-qualified seam
  in `src/runtime/guest_runtime.h`. Keep that seam backed by `amigaport`; do not
  reintroduce generated-body symbols.
- `tools/oracle_diff.py` and the `origin/oracle` branch preserve the static
  reference for local differential runs. They never enter the product.

## Native override boundary

Native overrides enter in the middle of interpreted 68000 execution. Every
body explicitly completes its guest boundary:

| Native body | Register with | Completion |
| --- | --- | --- |
| Wraps a guest routine | `rt_register_override` / `_gp` | `rt_call(ctx, ctx->image, its address)` suppresses only the active override |
| Replaces an RTS-terminated subroutine | `rt_register_replacement` / `_gp` | Adapter completes the RTS |
| Tail-jumps, including into a loaded image | Either | `rt_jump` |
| Unwinds to host screen ownership | Either | `rt_exit_to_host` |

The executor fails closed if an outer native override returns without
completing that boundary. `GuestCallPolicy` distinguishes `JSR`/`BSR` calls
from `JMP`/`BRA` tail transfers for nested `rt_call`; it does not treat `BSR`
as an ordinary branch. Calls and continuations carry the active image kind and
generation; the adapter rejects stale image tokens, and the host rebinds its
borrowed context before interrupt delivery because a parked game flow may have
loaded a new overlay. API: `src/runtime/guest_runtime.h`.

Native replacements own proven guest wait idioms, blitter/beam polls, and
host-service boundaries.

## Guest time and presentation

- `rt_get_guest_cycles()` is the clock. A PAL beam line is 454 cycles; a frame
  is 312 lines or 141,648 cycles. The beam is sampled on every custom-chip
  access, including blitter polls, not only position-register reads.
- Guest time never rolls back. If an interrupt does not reach RTE and restores
  CPU state, `elapsed_cycles` carries across the restore.
- Crossing a beam-frame boundary raises a pending boundary in
  `src/engine/hw_beam.c`. The game's own wait loops, recognized from the
  player's image by `src/port/wait_idiom.h`, land it; the hold is capped at one
  frame. A cycle-budget cutoff may park the guest midway through a draw or blit.
- Present only at a beam read and at most once per beam frame. The game flow
  can park for host presentation; an interrupt on the host thread presents
  and paces in place. `hw_present_frame` declines a duplicate request.
- Never park between the first `BLTxxx` write and `BLTSIZE`: interrupt code
  could overwrite the shared blitter registers. The boundary stays pending.
- Blits are charged to guest time with `rt_add_guest_cycles`: one bus cycle per
  enabled DMA channel per word, two 68000 cycles per bus cycle, or two per
  pixel in line mode. `WaitBlit` loops have no other clock.

Derivation: `docs/issues/0007-interpreter-boundaries-and-beam-time.md` and
`docs/issues/0008-reference-product-differential.md`.

## Owners and source policy

- Logging: `src/common/log.*`, a narrow sink composable with Lucent; `log_level`
  sets its global filter and category filters live in the same owner.
  Configuration: `src/port/config.c` reads environment, JSON, and test-session
  overrides through typed accessors.
- `version.txt` is the single version. CMake, the Android package, and
  `tools/publish_release.py` all read it.
- Release check: `src/port/update_check.*` owns what an answer means (a check
  that could not run is never "up to date"); each host fetches through
  `platform_update_check_begin` with its own client: libcurl, WinHTTP, the
  Android activity, or the page's `fetch`. Windows uses WinHTTP so its package
  stays one executable. Nothing else performs network I/O.
- `tools/source_policy.py` scans every retained first-party product and test
  source, not only CMake-selected files. It rejects retired static paths,
  interfaces, and vocabulary; direct product coupling to the diagnostic PUAE
  harness; process-stream output outside `src/common/log.c`; environment reads
  outside `src/port/config.c`; non-launcher shell tooling; new source above
  1,200 lines; and growth of the frozen oversized files.
- Emscripten stringifies JavaScript in an `EM_ASM` block token by token, so
  `!==` reaches the page as `!= =` and fails the link. Use `!=` and `==` there.
- `tools/build_wasm.py` copies the static page files (`index.html`,
  `disk_setup.js`, `isolation.mjs`, `service-worker.js`) with their source
  mtimes while `benefactor.js` and `benefactor.wasm` get fresh ones; differing
  timestamps in `build/release/web` are expected. Compare hashes against the CI
  artifact.
- A `std::filesystem::path` never converts to `std::string` on Windows, where its
  `value_type` is `wchar_t`; the Linux build cannot see the difference. Call
  `.string()`, and syntax-check Windows-relevant sources with a Windows
  cross-compiler when a Windows job reports such a conversion error.

## Build and test

```sh
BENEFACTOR_AMIGAPORT_DIR=../shared/amigaport cmake -S . -B build/run -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/run --target benefactor_product --parallel
uv run --frozen python -m unittest tests.test_disk_identity   # one focused Python test module
```

Full verifier: `uv run --frozen python -m tools.verify` (also CTest
`benefactor_source_policy`). It runs the unit tests and re-harness's two C++
gates. The configuration audit always runs; the ownership scan reads the real
compile commands and says so rather than passing when there is no configured
build. Sites the C boundary still holds are listed with reasons in
`tools/cpp_ownership_accepted.txt`; an unlisted site fails, and a listed site
that stops occurring fails too. The scan skips every path where
`tools/shared_checkouts.py` may put a shared checkout (`shared/`,
`dependencies/`).

Shared trees: `tools/shared_checkouts.py` reads the revisions
`.github/workflows/release.yml` pins and gives CMake paths for amigaport,
lucent, setup-ui and RmlUi (and Freetype when the host has no font engine). An
existing checkout is used; a clean one behind the pin is fast-forwarded; one
carrying its own work is used untouched and reported; a missing one is checked
out at the pin under `shared/` or `dependencies/`. `BENEFACTOR_*_DIR` and
`SETUP_UI_*_DIR` override any of them. CMake refuses a missing amigaport by
name. The cmake invocation above builds against whatever the adjacent
checkouts hold; `uv run --frozen python -m tools.shared_checkouts` prints the
resolved paths and aligns them to the pins.

## Debugging and comparison

- Do not rebuild an executable while `tools/oracle_diff.py` measures it. Its
  frame signature (`src/port/frame_signature.h`) checks palette and per-channel
  audio sample, period, and volume alongside screen/frame counts; matching
  counts alone do not show that fades and music advanced.
- `BENEFACTOR_LOG_LEVEL=debug` logs named guest-call exits: reason, PC,
  instruction count, image, and entry (`execute` or `interrupt`).
- `pc_trap_vector_execution` traps wild jumps at `$000000` before executing
  vector-table zeroes and records the guest stack and retired-instruction ring.
- `rt_insn_ring_snapshot` / `rt_insn_ring_entries` hold the last 256 guest PCs
  and opcodes; `/trace` or `pc_log_retired_instructions` dumps them.
- A fatal signal writes a report (`src/port/crash_report.c`): signal, faulting
  address, guest PC, active call, last hardware read, `cop1lc`, the retired
  ring, and the host backtrace. It goes to stderr and to `scratch/crash.log`,
  or wherever `crash_log` / `BENEFACTOR_CRASH_LOG` names. A fault in a native
  override shows `$000150` as guest PC and the dying C frame on the host side.
- `uv run --frozen python -m tools.menu_soak` loops level select, play, EXIT TO
  MAIN MENU and fails if the game dies. It needs a window (the headless path
  does not reproduce) and runs it hidden with SDL's dummy audio; `--show`
  shows it. The fault is probabilistic, so the default is forty rounds. To
  prove the soak still catches a fault, build the fix-reverted binary outside
  the tree `./run.sh` launches and pass it with `--exe`.
- The watchdog reports guest PC, active call, last hardware-register read,
  `cop1lc`, and retired tail. `src/port/guest_profile.h` samples hot PCs by
  owner at every custom-chip access and reports them at screen end.
- `src/port/frame_accounting.h` and `/state` split guest cycles by game flow,
  level-3, and level-6 interrupt, with per-owner peaks and beam/wait/present
  counts. An owner far above 141,648 cycles is a runaway frame. Add fields
  through `benefactor::diag::FrameAccounting`, accounted by named owner.
- `BENEFACTOR_PRESSES=7300:8,7420:8,7560:8` supplies frame-indexed fire input
  for reproducible comparison with `oracle_diff --play`; it is indexed by
  `frames.game`.

## Control channel

- `./run.sh` opens it on 8613 or the next free port and records it in
  `build/control-port`; `uv run --frozen python -m tools.control_port` prints
  the running game's URL. `BENEFACTOR_HTTP=<port>` or `--http N` names the port.
  A direct launch of the binary needs `BENEFACTOR_HTTP` or `--http`; for
  headless interaction run `Benefactor --headless --disk Disk.1 Disk.2 Disk.3`
  with one of them. Server: `src/port/control/` (Lucent).
- Endpoints: `/` (interactive page), `/state`, `/cpu`, `/mem`, `/poke`, `/hold`,
  `/press?fire=1&frames=4`, `/key?name=escape`, `/pause`, `/resume`,
  `/step?frames=N`, `/fb.ppm`, `/trace`, `/recent`, `/save`, `/load`, `/menu`,
  `/pacing`, `/break?at=3732`, `/breaks`, `/update?tag=…|error=…`.
- `/press` and `/hold` move the emulated joystick (what the engine reads);
  `/key` delivers a key as the window would, the only way to reach ESC on the
  level picker and the save/load keys. Neither moves the title menu's option
  cursor, which the engine reads through the joystick's quadrature encoding; a
  driver pokes it (see `tools/menu_soak.py`).
- `/state`: `frame` is frames shown; `frames` splits `presented`, `game`
  (produced by the guest) and `beam` (guest beam-frame boundaries).
  Fast-forward samples the display at PAL, so `game` runs ahead of
  `presented`. `level` is `$20.w`, the requested level; the loaded level is
  `$2E.w`, visible as `cop1lc` changing, one card later. `ui` names the host
  overlay that owns the screen (main menu, level picker, toast, pause menu, and
  the picker's level), since `level` stays 0 across the title flow.
- `/menu` toggles the pause menu and returns page, cursor and every row's label
  and value; `/menu?show=1` reads without toggling;
  `/menu?nav=up|down|left|right|select|back` walks it. `/fb.ppm` is the
  composed output with host overlays, not the guest's 4:3 render.
- `/break?at=3732` stops before that guest instruction; the hold stays in the
  game thread. `/state` reports `script_paused:1` and a frozen `frame`, and the
  log says `breakpoint $XXXXXX reached ... holding`. Clearing breakpoints does
  not release the hold; `/resume` does, and the server accepts it while held.
  A breakpoint inside a wait idiom (`$577130` and siblings in
  `src/port/wait_idiom.h`) re-hits on every `/step`; set it on the instruction
  after the loop and use `/resume`.
- `/pacing` reports the frame period, frames on target, and
  `refreshes_per_frame`; a non-integer value is judder no pacer can remove.
  `pace_to_display` (`MATCH DISPLAY` in OPTIONS: `off` / `free`, the default /
  `always`) rounds the period to whole refreshes; `free` does so only when the
  speed barely changes, so on 60 and 120 Hz panels it needs `always`.
  `/pacing?reset=1` reports and then clears.
- `/update?tag=…|error=…` injects an update-check result to drive its panel.
