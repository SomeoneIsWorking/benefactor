# Benefactor native/interpreter migration

The old offline 68000-to-C implementation has been removed. There is no static
gameplay bridge or compatibility target. Work resumes at the missing
`shared/amigaport` runtime and Benefactor adapter boundary.

## Product boundary

The shipped process combines Benefactor-native owners with `shared/amigaport`.
The framework executes every non-native 68000 path from the player's
authenticated disks through one maintained interpreter owner. Benefactor owns
disk identity, the four runtime image generations, OCS/CIA services, host
presentation, and title override policy.

The interpreter may be the default shipping CPU because Benefactor is an
Amiga-class title. `amigaport` should reuse a maintained 68000/PUAE execution
owner behind its typed CPU, memory, and service API rather than copy a core into
this repository. The separate PUAE harness remains a diagnostic oracle. Boot,
menus, FMV-like presentation, and idle frames are not correctness or
performance proof.

## Current break-first boundary

Removed now:

- `tools/recomp/`, its seed trees, discovery tools, emitter, and translator tests;
- `src/engine/generated/` and the configure-time regeneration route;
- the static dispatcher/runtime implementation in `src/engine/rt.c`;
- the generated-function headers, direct native calls to `gfn_*`, and
  `rt_call_generated` interface;
- the bank dumper and static-only build/run/harness shell workflows; and
- the former rule that delayed deletion until representative gameplay.

Retained native owners call an image-qualified runtime/original-call seam in
`src/runtime/guest_runtime.h`. It is declarations-only until `amigaport` and the
title adapter exist; CMake and `./run.sh` therefore refuse at that exact boundary.

## Four runtime images

| Image | Existing loader evidence | Representative entry |
| --- | --- | --- |
| main/intro | Disk.1 boot payload loads and ATN-decompresses | `$003000` |
| title/menu | restored loader block loads title chunks | `$003330` |
| gameplay | low-memory and `$577000` regions are relocated | `$577000` |
| credits | Disk.3 payload replaces low memory | `$003330` |

The adapter assigns a new generation to every load or restore. Active execution
and override identity is `(image kind, generation, guest address)`. A load or
restore replaces that identity before execution resumes.

## Implementation order

1. Create `shared/amigaport` with one complete 68000 state, a maintained
   interpreter execution owner, typed memory/service callbacks, executable-image
   ownership, and supported host backends.
2. Refactor the native disk/ATN/relocation owners to accept the adapter's memory
   mapping explicitly, then execute a bounded authenticated main-image slice.
3. Add an independent shipping-interpreter-versus-oracle discriminator covering all
   registers, SR, exceptions, cycles, writes, and a controlled negative.
4. Convert the native override registry to an injected image-qualified owner and
   prove ordinary, disabled, and scoped-original calls through the interpreter.
5. Expand reached execution through title, gameplay, reload, and credits. Fix
   CPU semantics in `amigaport`, never at one Benefactor address.
6. Recompose the native host only after build/link inspection proves that no
   generated body, offline translator, direct diagnostic-harness dependency, or
   second CPU owner can enter the gameplay product.

## Conformance gate

A representative run crosses all four image generations, begins a cavern,
moves and jumps, interacts with an object, observes enemy/world updates,
produces video/SFX/music and interrupts, reloads an image, reaches credits, and
quits normally. Deterministic checkpoints compare CPU/SR/exception state,
memory, service events, timing, audio, and frames against PUAE, hardware, or the
separate test oracle. Every compared class reports denominators and controlled
negative evidence. The x86-64 desktop, Apple Silicon macOS, and Android
arm64-v8a releases each need frame-time percentiles, memory, loading, audio, and
sustained-performance evidence from that representative interactive route.

Boot, menus, screenshots, FMV-like presentation, or a few matching frames do
not establish gameplay conformance.
