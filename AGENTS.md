# Benefactor port — working agreement

Benefactor is becoming one native/dynarec product. Hand-written native owners
provide the host shell, disk loading, Amiga hardware services, rendering,
audio, input, UI, and deliberately replaced game behavior. Every other 68000
instruction must execute on demand from the player's authenticated disk images
through `shared/amigaport`.

The checked-out tree still contains an offline 68000-to-C pipeline and its
generated-call runtime. They are migration input, not a supported product or
oracle. Do not regenerate, build, launch, extend, or use that path for new
evidence. It is removed only after the representative-gameplay gate in
`docs/migration.md` passes.

## Read before non-trivial work

| Authority | Answers |
| --- | --- |
| `docs/project-goals.md` | durable product outcomes and success conditions |
| `docs/project-state.md` | verified, partial, blocked, and missing capabilities; current focus |
| `docs/migration.md` | the Benefactor-specific native/dynarec migration and retirement gate |
| `docs/codemap.md` | subsystem ownership and where new work belongs |
| `docs/re-frontier.md` | ordered binary-grounded execution and native-replacement facts |
| `docs/issues/` | atomic work, findings, blockers, and dead ends |
| `instructions/gameplay-engine-map.md` | recovered gameplay addresses and behavior |
| `instructions/audio-engine.md` | recovered replayer, SFX, and interrupt behavior |
| `instructions/harness.md` | durable PUAE comparison facts |

Start with `../shared/re-harness/tools/info.py brief <terms>` and search
`docs/issues/` before re-deriving an address, ABI, state transition, or failed
approach.

## Non-negotiable execution contract

- Gameplay is native code plus `amigaport`'s on-demand 68000 dynarec. No build,
  install, launcher, or package step emits guest C/C++, objects, or a
  precompiled title substrate.
- An interpreter may exist only in a separately built test/diagnostic target.
  Gameplay must not link it, select it, or fall back to it. Build, link, and
  selector checks enforce absence.
- `amigaport` owns the complete 68000 architectural state: D0-D7, A0-A7, PC,
  complete SR/CCR and supervisor/interrupt state, exception frames and vectors,
  instruction semantics, translated-block lifetime, and bounded executor exits.
  Benefactor must not keep a reduced parallel CPU model.
- Benefactor owns disk identity and loading, the live memory/image map, OCS/CIA
  services, title policy, native override registration, and host subsystems.
- The four runtime images—main/intro, title/menu, gameplay, and credits—reuse
  guest addresses. Cache and override identity therefore includes image
  generation plus address. Loading or restoring an image invalidates every
  affected translation before execution resumes.
- A normal guest call observes overrides. A native override's scoped original
  call suppresses only that override for one call and executes the original
  guest body through the dynarec without recursion. It never names a generated
  host function.
- Unsupported guest behavior fails with the guest PC, image generation, and
  decoded bytes. It never becomes a no-op, interpreter step, or address-specific
  guessed translation.
- New comparison evidence comes from PUAE/hardware, direct binary analysis, or
  the separately linked test interpreter—not the retired generated-C path.

## Preserve the working seams

The CPU migration adapts the existing port instead of rewriting it.

- `src/engine/disk_boot.*` and `src/engine/overlay_load.*` retain the verified
  disk, ATN decompression, relocation, and four-image activation behavior.
- `src/engine/hw*` retains chip RAM, OCS/CIA, blitter, audio, and frame-service
  behavior behind a narrow `amigaport` memory/service interface.
- `src/port/overrides/` retains grounded native behavior. Generated-symbol calls
  become address-and-image runtime calls and scoped original calls.
- `src/render/`, input, UI, touch, configuration, packaging, and platform code
  remain peer host owners. Renderer redesign is not a prerequisite for CPU
  execution.
- `src/harness/` and PUAE remain the independent differential oracle, adapted
  to compare the shipping dispatcher, decoder, translated blocks, cache, and
  invalidation rather than generated functions.
- Existing captures and traces establish historical facts only. Current product
  claims require the native/dynarec path and denominated reachability.

## First dynarec discriminator

Before gameplay composition, a separate diagnostic target must authenticate the
player's disks, load the main image through the production loader, execute a
bounded real 68000 block through `amigaport`, and compare it with the separately
linked test interpreter or PUAE. Compare PC, all data/address registers, full SR,
supervisor and interrupt state, exception/return state, stack, cycles, and every
guest write. Require nonzero translated blocks and a controlled negative that
the comparison rejects. This checkpoint does not authorize a mixed static/JIT
gameplay executable.

## Representative-gameplay retirement gate

The offline translator, generated corpus, static dispatcher, seeds, and
static-only tests are removed together only after one frozen native/dynarec tree:

- provisions from the three authenticated disks without offline translation;
- proves gameplay links the dynarec and no interpreter or generated guest body;
- traverses main, title, gameplay, and credits image generations and exercises
  an address reused by different images with positive and controlled-negative
  cache/override identity checks;
- executes native overrides and one scoped original call through the shipping
  dispatcher without recursion;
- reaches representative interactive cavern gameplay with movement,
  interaction, enemies, level/world updates, rendering, SFX, music, interrupts,
  a transition that reloads an image, and normal quit;
- compares complete CPU, memory, exception/interrupt, timing, device-event,
  audio, and frame checkpoints against PUAE or hardware; and
- meets a declared frame-time/correctness budget on every released host.

Boot, a logo, title/menu, a clean trace, four matching frames, or a single level
entry is a checkpoint, not this gate.

## Ownership and quality guardrails

- Follow the self-contained structure in `docs/codemap.md`. In C, stateful
  owners use opaque contexts and cohesive module APIs; in C++, use focused RAII
  classes with constructor-injected dependencies and composition. Entry points
  only compose owners.
- New source files are capped at 1,200 lines. Existing oversized files may not
  grow, and 2,000+ lines require extraction before extension. The normal
  verifier must reject forbidden layer edges, direct stderr/debug output outside
  the logging owner, and process-environment reads outside configuration.
- Lucent is the one configurable process logger. Product modules emit one record
  per call site through the logging boundary; no direct `printf`, `fprintf`,
  `write(2)`, or debug-flag-wrapped logging.
- One configuration owner parses CLI, environment/`.env`, files, defaults, and
  precedence into immutable typed configuration. Other modules receive only the
  fields they use and never call `getenv`.
- Tests and diagnostics exercise production seams, report denominators, and
  prove that they can show the opposite answer.
- Build products live in `build/`; disposable diagnostics live in stable
  `scratch/<activity>/` paths. Project automation is Python; `run.sh` is only
  the thin locked-environment launcher. Never issue raw `rm` or `pkill`.

## Knowledge and landing

Update one nearest living authority when a fact changes. Project state owns
capability coverage, the codemap owns placement, the frontier owns ordered RE
grounding, and issues own atomic work. A verified milestone must leave no
unexplained worktree changes and is committed and pushed on `main` by the
operator after combined validation. Never commit or package original disks,
Kickstart, WHDLoad files, derived guest code, or reconstructable game data.
