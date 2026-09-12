# Issue 0014: Title-Owned Interpreter Call Policy

## Status

Resolved

## Symptom

The generic `rt_call` migration made dispatch uniform, but the shared
interpreter also had to guess whether a native callback's nested call was a
guest subroutine or a flat branch trampoline. Incorrect boundary ownership
caused guest execution to cross an RTS into unrelated code, producing missing
gameplay graphics and unstable native/guest returns.

## Cause

`amigaport::Executor` was inspecting its retired-instruction history to infer a
Benefactor-specific control-flow convention. That coupled a title policy to a
shared CPU owner and left the return-to-main-loop hand-off implicit.

## Resolution

- `shared/amigaport` exposes an explicit `CallBoundary` contract. A
  `GuestSubroutine` call stops at the return address already owned by A7; a
  `TailTransfer` call continues without consuming a guest return.
- `src/runtime/guest_call_policy.*` records the opcode that entered each native
  body and selects the boundary for a nested `rt_call`. It is the Benefactor
  owner of the title's JSR/BSR versus JMP/BRA rule.
- `rt_call` remains the only native-to-guest dispatch door: an active override
  is called when present, otherwise the guest body runs. `rt_jump` is used for
  explicit flat trampoline continuation and `rt_exit_to_host` for deliberate
  game-thread hand-off.
- The shared executor no longer reads title execution history to make this
  decision, and the adapter's obsolete `rt_call_original*` wrappers are gone.

## Verification

- `shared/amigaport/build/verify/amigaport_tests` passes the nested guest
  subroutine and explicit tail-transfer scenarios.
- `build/run/benefactor-pc` builds with the policy owner and reaches level 1
  gameplay from authenticated Disk.1-Disk.3 input without an execution fault.
- The live generic renderer now captures the player and major cavern objects;
  the capture was compared visually with the static-recomp oracle.
