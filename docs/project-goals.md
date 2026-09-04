# Project goals

This document owns durable Benefactor product intent. Factual coverage is in
`docs/project-state.md`, execution order and retirement gates in
`docs/migration.md`, and atomic work in `docs/issues/`.

## G001 — Ship one faithful native/dynarec Benefactor product

The complete 1994 game runs from the player's authenticated disks with selected
behavior owned natively and every remaining 68000 instruction dynamically
translated by `shared/amigaport`.

Why it matters: the port must preserve the complete game without distributing
copyrighted assets, generating a title-sized C corpus, or maintaining multiple
gameplay engines.

Success conditions:

- A fresh checkout consumes the exact three player-supplied disks directly;
  build, setup, and release paths emit no guest source, object corpus, or
  precompiled title substrate.
- Gameplay links native owners and the `amigaport` dynarec only. Any interpreter
  is separately built for tests and absent from product link, selector, and
  fallback surfaces.
- `amigaport` models complete PC, SR/CCR, supervisor/interrupt, exception-frame,
  and cycle state rather than the reduced state used by generated functions.
- Runtime cache and override lookup distinguish main, title, gameplay, and
  credits image generations even when they reuse a guest address.
- Native overrides and scoped original calls execute through one runtime
  dispatcher without regeneration or recursion.
- The representative-gameplay gate in `docs/migration.md` passes before the
  offline translator, generated corpus, static dispatcher, generation-only
  seeds, and static-only tests are deleted together.

Constraints and non-goals:

- Do not rewrite remaining portable game behavior merely to avoid implementing
  68000 dynarec coverage.
- Do not use the old generated-C product as a permanent oracle, compatibility
  mode, fallback, or alternate release.
- Do not make a rendering redesign or one-address special case a CPU-correctness
  prerequisite.

Contributing state items: S001, S005, S020-S024.

## G002 — Preserve the complete game and its native enhancements

The port retains the original 60-level experience while keeping its modern
presentation, controls, menus, pacing, accessibility, and developer-facing
runtime tools discoverable and independently verifiable.

Why it matters: changing the CPU execution owner is useful only if the game and
the port's established user-visible improvements survive intact.

Success conditions:

- Boot/title, difficulty selection, all 60 levels, level transitions, game over,
  ending/credits, rendering, audio, input, timing, and saves pass representative
  conformance through the native/dynarec product.
- True widescreen continues to reveal additional simulated world through
  deterministic world/view/culling boundaries, never by stretching or sampling
  adjacent frames.
- Native pause/options, level selection, remapping, controller/touch support,
  alternate physics, speed controls, free camera, render effects, cheats, and
  accessibility controls remain independently configurable.
- Player-facing save slots and rewind are implemented without weakening exact
  image/runtime-state identity.

Contributing state items: S001-S003, S007-S019, S023.

## G003 — Deliver lawful, portable desktop and Android releases

Linux AppImage and Android packages launch without a terminal, guide players to
their own disks, persist data in OS user storage, and contain no original or
derived game content.

Why it matters: a source-only runtime is not a usable port, and a distributable
package cannot contain the game.

Success conditions:

- Zero-argument `./run.sh` is a slim locked-environment launcher for the live
  native/dynarec product and performs no tests.
- First-run desktop and Android setup validates the complete three-disk set,
  preserves a prior valid selection on failure, and persists only approved paths
  or app-private copies.
- Packages contain no disks, Kickstart/WHDLoad inputs, generated guest source,
  precompiled guest code, or interpreter.
- Every released x86-64 or ARM64 host has a qualified dynarec backend and passes
  correctness, frame-time, sustained-performance, audio, and lifecycle gates.

Contributing state items: S004, S021-S024.
