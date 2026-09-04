# Benefactor native/dynarec migration

This plan specializes `../../shared/jit-common/docs/migration.md` for Benefactor.
It replaces the offline 68000-to-C methodology; there is no static gameplay
alternative.

## Product and framework boundary

The shipped process combines Benefactor-native owners with `shared/amigaport`.
The framework dynamically decodes and translates all non-native 68000 code from
the live authenticated image. Benefactor supplies checked memory and device
callbacks, image identity, interrupts, native override policy, and bounded host
exits.

`amigaport` owns one complete 68000 context: PC; D0-D7; A0-A7; the full SR
including CCR, supervisor, trace, and interrupt-mask state; exception vectors
and frames; cycle accounting; decode/lowering; host emission; and translated
block lifetime. The reduced `M68KCtx` is migration input only, not the final
contract.

An interpreter may share that canonical state and memory interface only in a
separately built test target. Gameplay cannot link it, select it, call
interpreter-backed helpers, or fall back to it.

## Migration freeze

Until S023 passes, do not regenerate, build, launch, profile, or extend the
offline-generated product. Preserve it only long enough to migrate independently
verified addresses, ABI behavior, native subsystem contracts, and harness
scenarios. New evidence comes from PUAE/hardware, binary analysis, `amigaport`'s
shipping decoder/JIT, or the separately built test interpreter.

## Four runtime images

The production loaders already establish four distinct executable generations:

| Image generation | Existing loader fact | Representative entry/region |
| --- | --- | --- |
| main/intro | Disk.1 boot payload is loaded and ATN-decompressed | `$003000` main image |
| title/menu | loader block is restored, then title chunks load/decompress | `$003330` title image |
| gameplay | relocation table and code/data load at low and `$577000` regions | `$577000` gameplay entry |
| credits | Disk.3 payload loads/decompresses into low memory | `$003330` credits image |

Addresses overlap across generations, most visibly title and credits at
`$003330`. A raw PC is therefore never complete executable identity. The image
owner increments a generation token for every load/restore; block-cache and
override keys include that token. Publication records the source byte interval.
Image replacement invalidates all overlapping blocks before dispatch resumes.

## Runtime calls and overrides

Replace generated symbols with one executor API:

- call a guest address with explicit image identity, register/stack contract,
  and bounded return reason;
- consult the image-aware override table before ordinary translation;
- intercept OCS/CIA, disk/service, frame, interrupt, and host-exit boundaries
  through typed callbacks;
- implement scoped original call by suppressing only the current override key,
  entering the same address through the dynarec, and restoring selection on
  every bounded exit; and
- invalidate any direct link that captured a changed override decision.

Native code must not call `gfn_*`, `rt_call_generated`, or another emitted host
body after migration. Native ownership requires recovered address/image/ABI
evidence and cannot mask missing instruction semantics.

## First bounded discriminator

Build a diagnostic target that:

1. validates the three disks and loads main/intro through the production loader;
2. clones identical full CPU/memory/device state for JIT and test-oracle legs;
3. executes a bounded reached block starting at the authenticated main entry;
4. stops through a named executor exit rather than C-stack unwinding;
5. compares PC, D/A registers, full SR, exception/interrupt state, stack, cycles,
   service events, and every guest write;
6. reports translated blocks/instructions, cache hits/misses, invalidations, and
   oracle entries with denominators; and
7. mutates one expected field and proves first-divergence reporting rejects it.

This proves the real image and framework seam. It does not permit a mixed
static/JIT gameplay target.

## Expansion order

1. Route main/intro calls and host/device exits through the executor.
2. Load title/menu, prove image generation changes, and exercise a low address
   that differs from main through independent JIT/oracle instances.
3. Convert the native override registry to image-aware keys and prove disabled,
   enabled, and scoped-original sequences through the shipping dispatcher.
4. Route level selection into the relocated gameplay image and expand reached
   coverage through player, objects/enemies, collision, rendering, audio, and
   level transitions. Fix instruction semantics in `amigaport`, never at one
   Benefactor address.
5. Exercise reload/retry/title transitions and savestate restore as positive
   invalidation cases, plus controlled writes outside executable ranges as the
   negative case.
6. Load credits into `$003330` and prove it cannot reuse title translations or
   overrides despite the same address.
7. Compose gameplay only after link/selector inspection proves there is one
   native/dynarec product and no interpreter or generated body.

Renderer, UI, input, physics, and packaging owners remain intact. A visual
divergence is renderer-owned only after CPU, memory, and device events reaching
the renderer agree.

## Representative-gameplay retirement gate

One frozen semantic tree must satisfy all of the following.

### Provisioning and composition

- A fresh checkout validates and consumes Disk.1-Disk.3 without Ghidra,
  Kickstart, WHDLoad, an offline translator, generated source, or a pre-populated
  runtime cache.
- Gameplay symbol/link/selector inspection finds the dynarec and no interpreter,
  interpreter helper, generated guest function, static dispatcher, or engine
  selector.
- Zero-argument launch selects only this product and refuses unsupported host
  backends by name.

### Reached behavior

- The run crosses main/intro, title/menu, gameplay, a gameplay reload/transition,
  and credits generations with nonzero translated execution in each relevant
  image.
- A state-anchored route starts a representative cavern, moves and jumps the
  player, interacts with an object or merry man, observes an enemy/world update,
  completes a level transition, produces frames, SFX and music, services
  interrupts, and quits normally.
- Denominated telemetry reports JIT blocks, cache hits/misses, invalidations,
  native overrides, service exits, and at least one scoped original call; the
  product contains zero interpreter or generated-body capability by construction.
- Positive and controlled-negative cases prove title/credits address collision,
  gameplay reload, executable write, override change, and non-executable write
  behavior.

### Conformance and performance

- Deterministic checkpoints compare complete CPU/SR/exception state, relevant
  memory, ordered device/service events, interrupt/timing state, audio events,
  and frames against PUAE/hardware or the separately linked test oracle.
- Every compared class reports its denominator and every tolerance is bounded by
  field; residuals state a falsifier.
- Frame-time percentiles, sustained behavior, memory, loading, rendering, and
  audio meet declared budgets on each released host. Desktop evidence does not
  qualify Android.

Boot, menus, four historical matching frames, a trace, or a screenshot cannot
retire the old path.

## Removal milestone

After that gate passes, remove in the same milestone the offline translator,
generated directory/build rules, generated dispatch and symbols, generation
seeds/manifests, static-only translator tests, and remaining static methodology.
Move independently useful addresses, image facts, and ABI evidence to runtime
metadata or `docs/re-frontier.md` first. Do not keep a `legacy` directory,
compatibility mode, permanent generated oracle, or static launcher.
