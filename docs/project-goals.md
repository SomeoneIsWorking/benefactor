# Project goals

This document owns durable Benefactor product intent. Factual coverage is in
`docs/project-state.md`, execution order and retirement gates in
`docs/migration.md`, and atomic work in `docs/issues/`.

## G001 — Ship one faithful native/interpreter Benefactor product

The complete 1994 game runs from the player's authenticated disks with selected
behavior owned natively and every remaining 68000 instruction executed by
`shared/amigaport`'s maintained interpreter owner.

Why it matters: the port must preserve the complete game without distributing
copyrighted assets, generating a title-sized C corpus, or maintaining multiple
gameplay engines.

Success conditions:

- A fresh checkout consumes the exact three player-supplied disks directly;
  build, setup, and release paths emit no guest source, object corpus, or
  precompiled title substrate.
- Gameplay combines native owners with one maintained 68000 interpreter behind
  `amigaport`; it does not embed a second CPU core or a full emulator.
- `amigaport` models complete PC, SR/CCR, supervisor/interrupt, exception-frame,
  and cycle state rather than the reduced state used by generated functions.
- Runtime execution and override lookup distinguish main, title, gameplay, and
  credits image generations even when they reuse a guest address.
- Native overrides and scoped original calls execute through one interpreter
  boundary without regeneration or recursion.
- The offline translator, generated corpus, static dispatcher, generation-only
  seeds, and static-only tests remain absent throughout runtime implementation.

Constraints and non-goals:

- Do not rewrite remaining portable game behavior merely to avoid integrating
  complete 68000 execution.
- Keep the working generated-C product only as an independent, local behavioral
  oracle. Never make it a compatibility mode, fallback, build input, or
  alternate release of the native/interpreter product.
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
  conformance through the native/interpreter product.
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
  native/interpreter product and performs no tests.
- First-run desktop and Android setup validates the complete three-disk set,
  preserves a prior valid selection on failure, and persists only approved paths
  or app-private copies.
- Packages contain no disks, Kickstart/WHDLoad inputs, generated guest source,
  precompiled guest code, full emulator frontend, or diagnostic PUAE harness.
- x86-64 desktop, Apple Silicon macOS, and Android arm64-v8a each pass the same
  representative interactive correctness, frame-time, sustained-performance,
  audio, and lifecycle gates; boot or FMV evidence is insufficient.

Contributing state items: S004, S021-S024.

## G004 — Publish a complete asset-free release matrix

Every claimed host has a reproducible CI artifact, and browser delivery is
published through GitHub Pages without distributing the player's disks.

Why it matters: the port is only usable when a player can obtain a package for
their host and supply the original files through an explicit setup path.

Success conditions:

- Hosted CI builds a Windows package, an Android arm64-v8a APK, a macOS `.app`,
  a Linux AppImage, and a WASM package from the canonical locked build owners.
- A qualified version tag publishes the four native packages together as a
  GitHub Release, retaining the published Android signing identity.
- The WASM package is deployed to GitHub Pages from the same shipping runtime
  boundary as desktop; no placeholder or green no-op job represents a missing
  backend.
- Desktop, Android, and browser setup provide a Browse/file-picker path,
  validate exactly Disk.1, Disk.2, and Disk.3, preserve the prior valid set on
  failure, and never package the files.
- Release staging rejects original disks, Kickstart files, and derived guest
  executable content before an artifact is uploaded.

Constraints and non-goals:

- A package job must fail at the named runtime boundary while the interpreter
  adapter is absent; it must not publish a shell, emulator, or placeholder.
- GitHub Pages hosts the browser package only; it is not a disk-file host.

Contributing state items: S026-S032.
