# Issue 0012: Nested Override Host Exit Propagation

## Status
Resolved

## Symptom
Pressing Fire on the main menu to begin gameplay (selecting "CONTINUE" / "PLAY GAME") caused a watchdog crash:
```
error: signal: PC
error: signal: frame never finished: pc / call / last hw read / cop1lc $00000622 $00003700 $00DFF006 $0000097E
error: signal: recent guest calls $00003330 $00003872 $00003700
```
Instead of transitioning to the level card and active gameplay, the screen hung showing the Amiga floppy disk prompt copper list (`COP1LC = $0000097E`) while spinning indefinitely at scanline wait address `$00000622`.

## Root Cause
1. The title menu loop at `$003872` is overridden by `native_menu_setup`, which sets up menu labels and calls `rt_call(ctx, ctx->image, 0x00003872u)`.
2. When the user selects "CONTINUE" or "PLAY GAME", `native_main_menu_fire_dispatch` at `$0039D0` executes `rt_jump(ctx, ctx->image, 0x150u)`, jumping to the relocated disk overlay loader at `$150`.
3. At `$150`, `native_overlay_loader_reloc` loads the gameplay overlay into chip RAM, sets `g_enter_gameplay = 1`, and invokes `rt_exit_to_host(ctx)`.
4. `rt_exit_to_host()` flagged `hand_off_to_host = true` only on the innermost override scope (`$150`). When the inner `rt_call(0x3872)` returned `ExitReason::ReturnToHost` with `hand_off_to_host = true`, `rt_call` ignored the return value and did not propagate the host hand-off.
5. Consequently, when `native_menu_setup` completed, its outer override wrapper saw `hand_off_to_host = false` and returned `ExitReason::NativeOverride` to the outer run (`$3330` poster flow).
6. In `Runtime::execute(0x3330)`, the while loop on `result.reason == ExitReason::NativeOverride` resumed guest execution via `executor.execute()` instead of returning to host.
7. Because the overlay loader had already switched the active image to `BENEFACTOR_IMAGE_GAMEPLAY`, address `$150` was no longer overridden (it was registered only for Main, Title, and Credits masks).
8. The CPU resumed executing the raw guest bytes of the retail Amiga floppy disk loader relocated at `$150`. The loader failed to communicate with physical floppy drive hardware, displayed the "PLEASE INSERT DISK" screen (`COP1LC = $0000097E`), and spun on VHPOSR at `$00000622` until the 2-second watchdog alarm fired.

## Remediation
1. In `src/runtime/guest_runtime.cpp`: updated `rt_call` to inspect the `ExecutionExit` returned by `runtime().execute(address)`. If `exit.hand_off_to_host` is true and the caller is inside an override (`!runtime().native_host_exits.empty()`), it calls `runtime().exit_to_host()`, propagating the host unwind across all nested override call frames up to `game_thread_main`.
2. Verified that `game_thread_main` completes its execution cleanly, allowing `pc_step_threaded` on the main thread to observe `g_enter_gameplay == 1` and dispatch `pc_cps_start_at(g_gameplay_entry, 0x0057EE12u, 1, 0x1000u, 0xFFFFu)` into active gameplay at `$577000`.
3. Verified headless full flow: poster (`$3330`) -> fire -> main menu (`$3872`) -> cursor navigation -> fire -> level card (`COP1LC = $003914`) -> fire -> playfield (`COP1LC = $003484`) -> player movement right -> active frame progression.

## Verification
- `CC=clang uv run --frozen python -m tools.verify` passed all 58 checks (unit tests, formatting, clang-tidy, source policy).
- Verified full interactive startup via HTTP control server without timeouts, crashes, or watchdog signals.
