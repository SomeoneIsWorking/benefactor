# Benefactor port — working agreement

Benefactor is one native/interpreter hybrid product. Hand-written native owners provide
disk loading, Amiga services, rendering, audio, input, UI, and deliberately
replaced game behavior. Every other 68000 instruction executes directly from
the player's authenticated disks through `shared/amigaport`'s maintained 68000
execution owner.

The offline 68000-to-C translator, generated corpus, static dispatcher, and
static launch/build paths have been deleted. Never recreate them as a bridge,
comparison arm, cache, fallback, or convenience.

## Read before non-trivial work

| Authority | Answers |
| --- | --- |
| `docs/project-goals.md` | durable product outcomes and success conditions |
| `docs/project-state.md` | verified, partial, blocked, and missing capabilities |
| `docs/migration.md` | runtime integration order and conformance gate |
| `docs/codemap.md` | subsystem ownership and placement |
| `docs/re-frontier.md` | ordered binary-grounded facts and gaps |
| `docs/issues/` | atomic work, findings, blockers, and dead ends |

Start with `../shared/re-harness/tools/info.py brief <terms>` and consult the
issues before re-deriving an address, ABI, state transition, or failed approach.

## Execution contract

- Gameplay is native code plus `amigaport`'s maintained 68000 interpreter. It
  consumes the player's disk images directly and never emits guest C/C++ or
  host objects. Benefactor does not embed its own CPU core or full emulator.
- The interpreter is a permitted shipping CPU for this Amiga-class title.
  Representative interactive gameplay—not boot, menus, video, or idle
  presentation—must prove correctness and sustained performance on x86-64,
  Apple Silicon macOS, and Android arm64-v8a.
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
  New evidence comes from independent PUAE/hardware comparisons, binary
  analysis, or the shipping `amigaport` interpreter path.

## Preserved native seams

- `src/engine/disk_boot.*` and `src/engine/overlay_load.*` retain disk, ATN,
  relocation, and four-image knowledge. Move them to explicit adapter memory;
  do not regenerate source from their bytes.
- `src/engine/hw*`, `src/render/`, and `src/port/` retain native device,
  presentation, input, UI, and enhancement behavior.
- `src/port/overrides/` calls guest addresses through the image-qualified seam
  in `src/runtime/guest_runtime.h`. Finish that seam through `amigaport`; do not
  reintroduce generated-body symbols.
- `src/harness/`, `vendor/libretro-uae/`, and `instructions/harness.md` preserve
  oracle scenarios. Recompose them only as a separate diagnostic product.

## Structure and quality

- The repository is self-contained; no external project is a structure guide.
  In C, stateful owners use opaque contexts and injected cohesive interfaces.
  In C++, use focused RAII classes and composition. Entry points only wire owners.
- One configurable logger in `src/common/log.*` owns process output through a
  narrow sink that can be composed with Lucent. Product code does not write
  directly to stderr. `log_level` controls its global filter and category
  filters belong to the same owner. One configuration owner reads environment, JSON, and
  test-session overrides through typed accessors; other modules never call
  `getenv`.
- `tools/source_policy.py` scans every retained first-party product and test
  source, not merely CMake-selected files. It prevents retired static paths,
  interfaces, and vocabulary; direct product coupling to the diagnostic PUAE
  harness; process-stream
  output outside `src/common/log.c`; environment reads outside
  `src/port/config.c`; non-launcher shell tooling; new source above 1,200 lines;
  and growth of the explicitly frozen oversized files.
- Project automation is modular Python. `run.sh` is the only shell exception and
  remains a slim `uv run --frozen` launcher. Builds live in `build/`; disposable
  diagnostics use bounded stable paths under `scratch/`.
- Use Clang for agent C/C++ verification without rejecting other supported user
  compilers. Format and lint first-party code, keep files cohesive, and do not
  grow a monolith.
- Never use raw `rm`, `pkill`, or commit/package copyrighted game inputs.

The product is intentionally unavailable until `shared/amigaport` and the
Benefactor runtime adapter exist. CMake and `./run.sh` must refuse with that one
named boundary; they must never launch PUAE or the deleted implementation.
