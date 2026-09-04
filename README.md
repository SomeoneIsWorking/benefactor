# Benefactor — native PC and Android port

This repository is an in-progress native/interpreter port of *Benefactor* (1994,
Digital Illusions/Psygnosis). The intended product keeps its established native
disk, hardware, renderer, audio, input, UI, and enhancement owners while
executing every remaining 68000 instruction on demand through
`shared/amigaport`.

The retired offline 68000-to-C product and its generated corpus have been
deleted. Runtime interpreter integration and representative-gameplay conformance
are still missing; see `docs/project-state.md` and `docs/migration.md`.

No original game disk, Kickstart ROM, WHDLoad file, generated guest code, or
reconstructable game asset belongs in the repository or release packages.

<p align="center">
  <img src="screenshots/gameplay-egypt.png" alt="Tombs of Egypt gameplay" width="320" />
  <img src="screenshots/gameplay-treetop.png" alt="Treetop Rescue gameplay" width="320" />
  <img src="screenshots/gameplay-stones-and-bones.png" alt="Stones and Bones gameplay" width="320" />
</p>

<p align="center">
  <img src="screenshots/widescreen-16x9.png" alt="16:9 widescreen gameplay" width="500" />
  <img src="screenshots/widescreen-ultrawide.png" alt="21:9 ultrawide gameplay" width="658" />
</p>

These captures document the pre-migration native host and its user-visible
features. They are not evidence that the interpreter product is complete.

## Current feature state

The canonical inventory is `docs/project-state.md`. In summary:

| Capability | State |
| --- | --- |
| Native pause/options and persistent settings | partial |
| Keyboard, controllers, touch, hot-plug, and rebinding | partial |
| 60-level selector, progress, locks, and restored difficulty | partial |
| Classic/optional platformer physics and interaction reach | partial |
| Turbo/Hyper/5x fast-forward with normal-speed audio | partial |
| Faithful/native software/Vulkan renderers and effects | partial |
| True 16:9, 21:9, and live-window widescreen | partial |
| Native disk loading, instant boot, and no floppy waits | partial |
| Free camera, cheats, accessibility, savestate diagnostics | partial |
| Player save-slot UI and rewind | missing |
| `amigaport` maintained 68000 interpreter integration | missing |
| Static translator, generated corpus, and dispatcher removed | verified |
| Single-owner native/interpreter gameplay composition | missing |
| Representative cross-host gameplay conformance and performance | missing |

## Player-supplied files

The intended product consumes the original three disk images directly. The
known SHA-256 identities are:

```text
Disk.1  25416a6e390cbe94e4b2375c9513a2adf3411072fc5b6069ea34a0f3ff697916  1003520 bytes
Disk.2  f3649c8db4adfce3c7da5e21cb018be098404771eceeec44741c2528e9071b73  1003520 bytes
Disk.3  8dd262d02174a6706d5214b25f7bd9fc4bffe94761e16c209b880bc1dd8e7a42  1003520 bytes
```

PUAE development comparisons additionally use player-supplied files under
`harness/`:

```text
Benefactor.slave       7ee0edba0e0f3eb8da38fb3aaccead4324e7aa12a6d99ad81a9c15ecf33d4670  1084 bytes
kick40068.A1200        6d43840d4099a74170ea0f0425b6257c3891ebcaa39c4d1840075a9ab22b5707  524288 bytes
```

The intended standalone product does not require Kickstart, WHDLoad, PUAE, or
Ghidra.

## Build and run status

`./run.sh` is the fresh-clone interface and delegates to a locked Python
initializer. It currently refuses with the exact missing `shared/amigaport` /
Benefactor adapter boundary; it does not launch an emulator or the deleted
product. CMake exposes the same deliberate refusal as `benefactor_product`.
Tests and future oracle runs remain separate commands.

## Intended runtime

Benefactor loads four executable image generations that reuse guest addresses:
main/intro, title/menu, gameplay, and credits. The product therefore keys active
execution and native overrides by image generation plus address. A
native override can make a scoped original call through the interpreter owner.
For this Amiga-class title, that interpreter may be the default shipping CPU.
Only representative interactive gameplay can prove it: x86-64 desktop, Apple
Silicon macOS, and Android arm64-v8a each need correctness and sustained
performance evidence; boot, menus, and FMV-like output are insufficient.

The existing native owners remain valuable:

- disk reads, ATN decompression, overlay relocation, and instant loading;
- chip RAM, OCS/CIA service boundaries, blitter, and Paula audio;
- faithful/native/Vulkan rendering and deterministic widescreen;
- logical input, controller/touch policies, pause/options, and level selection;
- native game-flow, interaction, physics, camera, effects, cheats, and
  accessibility behavior; and
- PUAE differential driving, state/frame inspection, and runtime probes.

Architecture and responsibility placement are in `docs/codemap.md`. Recovered
gameplay and audio facts remain in `instructions/gameplay-engine-map.md` and
`instructions/audio-engine.md`.

## Credits

*Benefactor* was developed by Digital Illusions and published by Psygnosis.
This unofficial project distributes only original port/runtime code and
documentation. SDL, Lucent, and libretro-uae/PUAE retain their respective
licenses; PUAE is a development oracle, not part of the gameplay product.
