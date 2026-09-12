# Issue 0013: Level Card Transition, Stale Interrupts, and Card Dismissal Input

## Status
Resolved

## Symptom
During startup or upon selecting "PLAY GAME" on the main menu, the game hung on the level card screen (`COP1LC = $003914`, "UNDERWORLD / AFRAID OF FUNGIES?"):
- The screen froze and became unresponsive.
- In some scenarios, a main-thread spin inside audio tick delivery resulted in hundreds of thousands of refused yields (`yield.refused` climbing into millions) or a 2-second watchdog timeout at `$00577130`.
- In windowed GUI sessions, pressing primary confirm buttons (Pad A or Space on modern control mappings) or mouse clicks did not dismiss the card to start gameplay.

## Root Cause
Three distinct root causes combined to create the hang and input unresponsiveness on the level card:
1. **Interrupt Vector Bleed Across Overlays**:
   When the overlay loader transitioned from the title overlay to the gameplay overlay, the Level 6 timer interrupt vector (`$78`) and Level 3 VBLANK vector (`$6C`) in low memory still pointed to title routines (such as `$005694`, which executes a 3-link self-rewriting interrupt chain). Before the newly spawned gameplay thread at `$577000` could initialize its own ATN music parser and install its vectors (`$59B80A`), `pc_step` on the main thread delivered audio ticks (`pc_music_tick`) into the title's stale handlers while the game thread was parked or initializing.
2. **Erroneous Subroutine Override on Music Interrupt Branch (`$59C5B0`)**:
   `$59C5B0` had been registered with `native_gameover_menu` using `rt_call(ctx, ctx->image, 0x0059C5B0u)`. In the retail game binary, `$59C5B0` is reached via branch (`bra.w $59c5b0`) inside the Level 6 music interrupt handler `$59BA7A`, not via a `jsr` subroutine call. Calling it via `rt_call` caused reentrant guest execution that corrupted the supervisor stack frame on the main thread during audio ticks, causing `rt_call_interrupt` to loop and fail to return to the host.
3. **Control Mapping Gating on Level Card**:
   The Amiga title card loop (`$578498..$5784BA`) polls `tst.b $BFE001; bmi.b $578498`, requiring an active-low Fire button press (bit 7 = 0) to fall through to `$5784BC` and begin playfield setup. In `src/engine/hw.c`, `apply_bound_input()` mapped `s_hop` (Pad A, Space under modern controls) to `fire` only when `!g_gameplay_active`. Because `g_gameplay_active` was set to 1 when entering gameplay, pressing Pad A or Space was treated strictly as `s_hop` (jump) and did not set `s_fire_pressed`, preventing the player from dismissing the level card using the same button they used to start the game on the menu. Additionally, `s_mouse_lmb` was unconditionally overwritten by `fire`, discarding mouse left-clicks.

## Remediation
1. **Vector & Interrupt Isolation in Gameplay Handoff**:
   In `src/engine/gameplay_handoff.c:gameplay_handoff_prepare_low_memory()`, explicitly cleared vectors `$6C` and `$78` and disabled interrupts via `hw_write16(0xDFF09Au, 0x7FFFu)` so stale handlers from the preceding image cannot execute before the new overlay installs its own routines.
2. **Removed Non-Subroutine Override `$59C5B0`**:
   Disabled the override registration for `$0059C5B0u` in `src/port/overrides/register.c`. The branch inside `$59BA7A` now executes natively through the 68000 interpreter without supervisor stack corruption.
3. **Unified Confirm Button on Level Card & Mouse Click Retention**:
   In `src/engine/hw.c`, updated the confirm mapping so `s_hop` drives `fire` outside gameplay OR while on the level card (`hw_get_cop1lc() == 0x003914u`). Retained raw mouse clicks via `s_mouse_lmb_raw` so mouse clicks also clear CIA-A bit 7 and dismiss the card.
4. **End-to-End Verification**:
   Verified full boot -> poster -> menu -> level card -> fire dismissal -> active playfield (`COP1LC = $003484`) -> player movement right -> level progression. Captured framebuffer and confirmed pristine visuals and sprite placement.

## Verification
- `CC=clang uv run --frozen python -m tools.verify` passed all 58 checks.
- End-to-end headless flow verified via Python HTTP script: clean transition from level card to gameplay playfield with zero refused yields (`yield.refused == 0`).
