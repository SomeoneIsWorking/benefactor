# 0010 — Subroutine Original Calls and Gameplay Physics

## Why

In the native/interpreter hybrid architecture, certain 68000 routines are wrapped with native C overrides (for example, enhanced platformer physics, extended object pickup reach, modern controller input remapping, and sprite blitter coordination). When modern features or special handling are inactive, these overrides fall back to the original guest implementation.

During live gameplay comparison against the reference oracle via HTTP control endpoints (`BENEFACTOR_HTTP`), candidate player jump physics exhibited severe corruption: pressing or holding UP moved the player upward by only 1 pixel instead of initiating the full jump trajectory (8+ pixels on initial launch, parabolic apex, and clean landing).

Investigation revealed that native override functions in `platformer.c`, `gameplay.c`, `pickup.c`, and `copper.c` were invoking `rt_call_original()` instead of `rt_call_original_subroutine()`.

In `amigaport::Executor`, `rt_call_original()` merely suppresses the override at the target address and enters the execution loop without setting a stop condition. When the guest subroutine reaches its `rts` instruction, the interpreter does not halt; it continues executing subsequent memory instructions linearly. As a result:
1. The return address remains unpopped or the stack frame is corrupted.
2. Control never properly returns to the native wrapper at the end of the subroutine.
3. Subroutine callers experience register and stack corruption, aborting jump and animation physics.

In contrast, `rt_call_original_subroutine()` inspects the return address on the guest stack (`MR32(ctx->A[7])`), configures `stop_at_pc` with that return address, and executes the guest body until the `rts` instruction transfers control back to the caller.

Additionally, two peripheral workflow issues were observed:
- `./run.sh` rejected user arguments (such as `--level 1` and `--headless`) because `tools/config.py` used `argparse.parse_args()` instead of forwarding extra arguments to the binary.
- In-game savestates (`/save` and `/load`) failed: the savestate verification rejected the main gameplay loop parking address (`$577130` scanline wait vs `$577114`), and `pc_savestate()` failed to create parent directories when saving to `logs/savestate.bin`.

## What changed

### 1. Subroutine Original Calls
Converted all subroutine fallback calls from `rt_call_original` to `rt_call_original_subroutine`:
- `src/port/overrides/platformer.c`: `native_pf_hop`, `native_pf_lj`, `native_pf_diag`, `native_pf_fall`, `air_track`, `native_pf_landing_impact`, `native_pf_collision`.
- `src/port/overrides/gameplay.c`: `native_gameplay_input`, `native_place_probe`.
- `src/port/overrides/pickup.c`: `interact_wide`, `native_mm_pickup_gate`, `PKSCAN` macro.
- `src/port/overrides/copper.c`: `native_sprite_blitter_setup`.

### 2. Savestate Resume Point & Directory Creation
- In `src/port/savestate.c`, updated `pc_savestate_allowed()` and `pc_loadstate()` to accept `$577130` (the scanline wait in the level 1 main loop) in addition to `$577114`.
- In `src/port/game_loop.c`, updated `pc_resume_gameplay_thread()` to resume from `rt_get_pc()` if set.
- In `src/port/savestate.c`, added parent directory creation (`mkdir`) so paths like `logs/savestate.bin` succeed without pre-existing directories.

### 3. Launcher Argument Passthrough
- In `tools/config.py`, updated `parse_launch_config()` to use `parse_known_args()` and store `extra_args` in `LaunchConfig`.
- In `tools/launcher.py`, passed `config.extra_args` to `benefactor-pc` so flags like `./run.sh --level 1 --headless` are forwarded directly to the game binary.

### 4. Oracle Diff Harness Tooling
- In `tools/oracle_diff.py`, ensured disk images (`Disk.1`, `Disk.2`, `Disk.3`) are symlinked into the reference worktree, and the active Python virtualenv is included in `PATH` so `ninja` and build tools find `python3`.
- Added `capstone` dependency to `pyproject.toml` and updated `uv.lock`.

## Verification

- `CC=clang uv run --frozen python -m tools.verify` passes all 58 repository checks.
- Driving Candidate and Oracle simultaneously via HTTP control (`/hold?r=1`, `/hold?u=1`, `/hold?l=1`):
  - Initial ground position: `(0x0020, 0x00A6)`.
  - Walk right: smoothly advances X position from `0x0020` to `0x00A2` with correct right-facing animation (`0x0002`).
  - Jump UP: accelerates vertically from `0x00A6` (166) to `0x00A6 - dy` trajectory and lands back on ground at `0x00A6`.
  - Walk left: turns around (`0x0001` facing), walks left from `0x00A2` to `0x0087`.
  - Long jump (Fire + Right): executes rolling forward leap.
- Live savestate verification:
  - Saved state at `(0x0020, 0x00A6)` via `/save` (writing 8.1 MB snapshot).
  - Walked right to `0x005D`.
  - Restored state via `/load`, immediately returning player position to `0x0020, 0x00A6` and resuming gameplay cleanly.
