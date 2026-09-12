# Issue 0011: Scanline-Wait Presentation and Unified Call Dispatch

## Status
Resolved

## Symptom
1. Horizontal glitching and tearing occurred during interactive gameplay frames.
2. In-level jump (Fire + direction) appeared truncated prematurely in early testing, transitioning early into an abort arc.
3. Multiple divergent call primitives (`rt_call`, `rt_call_original`, `rt_call_original_subroutine`) existed in the codebase with divergent boundary contracts across host and interpreted dispatch.

## Root Cause
1. **Scanline Wait Mid-Frame Presentation**: In `src/port/overrides/wait_idioms.c`, `PC_WAIT_SCANLINE` (such as scanline `$3B` = 59 at `$577130` in the main gameplay loop) was mapped directly to `hw_vblank_wait()`. In the gameplay loop, `$577114` waits for VBLANK (`PC_WAIT_FRAME`), yields to the main thread, presents the completed frame, and delivers vertical-blank interrupts. Reaching scanline 59 shortly thereafter and calling `hw_vblank_wait()` yielded to the main thread a second time per frame, presenting a half-drawn/partially updated playfield mid-frame and injecting tearing artifacts.
2. **Unified Call Architecture**: Having distinct `rt_call_original` and `rt_call_original_subroutine` primitives created confusion between host dispatch and interpreted dispatch. The architecture requires a single unified `rt_call` where both the interpreter and host dispatch share identical "if overridden and not suppressed, call override; otherwise call guest" logic. The title adapter must still state the guest boundary: `amigaport::Executor::call` accepts either a guest-subroutine return boundary or an explicit tail-transfer boundary, while Benefactor's `GuestCallPolicy` records whether a native body was entered by `JSR`/`BSR` or by a `JMP`/`BRA` trampoline. Shared execution does not infer title control flow from its own trace.

## Remediation
1. Added `hw_beam_wait_scanline(uint8_t line)` in `src/engine/hw_beam.c` and declared it in `src/engine/hw.h`. When the guest polls for a specific scanline, the beam cycle clock advances to that scanline within the current frame without yielding or presenting a frame. Only if the beam has already wrapped past the target does it wrap and yield.
2. Updated `src/port/overrides/wait_idioms.c` so `PC_WAIT_SCANLINE` dispatches to `hw_beam_wait_scanline(found.scanline)` with the recognized scanline target stored in `PcWaitIdiom`.
3. Refactored `amigaport::Executor::call` to handle unified override dispatch, scoped self-suppression, and an explicit caller-selected boundary (`stop_at_pc` is only installed for a guest subroutine call).
4. Replaced all occurrences of `rt_call_original` and `rt_call_original_subroutine` across all native overrides (`boot.c`, `copper.c`, `gameplay.c`, `level_load.c`, `pickup.c`, `platformer.c`, `render.c`) with `rt_call(ctx, ctx->image, address)`.
5. Removed `rt_call_original` and `rt_call_original_subroutine` from `src/runtime/guest_runtime.h` and `src/runtime/guest_runtime.cpp`.
6. Verified long jump (Fire + Left/Right) maintains state register `$f80` (`0x0024`) across frames and executes the full rise, apex, fall, and landing impact arcs.
7. Verified captured gameplay frame buffer via HTTP `/fb.ppm` confirms zero horizontal tearing or line artifacts.

## Verification
- `CC=clang uv run --frozen python -m tools.verify` passed all 58 unit tests, formatting, clang-tidy, and source policy audits.
- Headless test driving level 1 via HTTP confirmed held fire and direction sustains `$f80 = 0x0024` and state handler `0x00579A62` across 9+ consecutive frames of rising trajectory.
- Frame capture verified pixel-clean rendering of terrain, player sprite, and HUD.

## Follow-up: title-owned interpreter call policy

The first implementation put the JSR-versus-tail inference inside the shared
executor by inspecting its retired-opcode ring. That was the wrong ownership
boundary: the branch conventions belong to Benefactor's native adapter, and a
shared runtime must not infer a consuming title's control-flow contract. The
adapter now records the transfer opcode when each native body is entered and
passes `GuestSubroutine` or `TailTransfer` explicitly for nested `rt_call`.
Nested calls to another override return to the native caller; nested calls to
guest code stop at the caller's existing A7 return address. `rt_exit_to_host`
remains the explicit hand-off for transitions that leave the guest run and
return to the host game loop.
