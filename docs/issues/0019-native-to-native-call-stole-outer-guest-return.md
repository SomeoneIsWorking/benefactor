---
id: 19
title: Native-to-native rt_call stole the outer guest return
status: investigating
symptom: A nested rt_call into an override had no callee return frame; accepted SFX could consume the outer return, while a rejected SFX could leave PC on the callee entry.
state_items: S005,S021
tags: interpreter,overrides,audio,call-boundary
created: 2026-09-12
updated: 2026-09-12
---

`takeoff_grunt` calls `$58656E` through `rt_call`; that address is also a native
replacement. The previous nested-call path entered it without pushing a
callee return. On success, `native_sfx_trigger` explicitly popped A7 and thus
consumed the outer override's guest return. On rejection, it left PC unchanged;
the adapter deliberately skipped automatic RTS for nested replacements, and
resuming could re-enter the same callee or cross the outer boundary. The shared
executor also discarded its stop-at-return rule after a bounded override or
instruction-budget exit when the title resumed with plain `execute()`.

The shared executor now supports a host-owned subroutine frame and an explicit
continuation token. Each slice stays bounded, while the return PC and current
override policy survive intermediate exits. Benefactor uses this boundary for
native calls to other addresses; self-calls still use the existing guest
return or title-classified tail transfer. Replacements complete one RTS on both
accepted and rejected paths. Synthetic tests execute both native and guest
callees across override and budget exits and fail closed on an unterminated
nested override. The Clang Benefactor product compiles with the new boundary.

Gap: exercise the changed call paths in authenticated gameplay and compare
first-divergence video/audio against the pinned static recomp. The measured
frame-phase difference in issue 0016 is independent until that comparison
shows otherwise. No PUAE oracle is used.
