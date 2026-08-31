# Project state

## Current focus

S005 is the current focus.

## Capability inventory

| ID | Capability or outcome | State | Factual dependency | Goals |
| --- | --- | --- | --- | --- |
| S001 | The native desktop product boots and plays the retail game from player-supplied disks | verified | — | G001 |
| S002 | Widescreen, speed, camera, difficulty, level selection, and persistent options work in the shipping game | verified | S001 | G001 |
| S003 | Keyboard and hot-pluggable controller input support authentic, modern, and rebindable mappings | verified | S001 | G001 |
| S004 | AppImage and Android releases provide no-terminal disk setup without packaging game assets | verified | S001 | G001 |
| S005 | Recompiled M68K gameplay behavior is replaced by readable native engine subsystems | partial | S001 | G001 |
| S006 | A differential PUAE harness compares the port with the original execution | verified | S001 | G001 |

## Capability details

### S001 — Playable native desktop product

Evidence: the documented `benefactor-pc` product boots directly from the three validated disk images,
reaches all 60 levels, and runs without an Amiga emulator in the shipping process.

### S002 — Modern presentation and options

Evidence: the shipping options and pause surfaces expose 4:3, 16:9, ultrawide, speed, camera,
difficulty, level selection, fullscreen, and persistent configuration paths.

### S003 — Native input

Evidence: keyboard and SDL controller paths support hot-plug, per-device authentic or modern control
policy, dedicated actions, and persistent chorded rebinding.

### S004 — Packaged setup

Evidence: the AppImage selects a disk folder through a graphical first-run path and Android imports
the validated three-disk set into private storage; neither package contains the disks.

### S005 — Native engine ownership

The renderer, blitter, audio, disk loading, decompression, menus, and selected game flows have native
owners while the gameplay bank still contains mechanically translated M68K functions.

Gap: replace the remaining recompiled gameplay behavior with readable, verified native subsystems.

### S006 — Reference comparison

Evidence: the repository includes the side-by-side PUAE harness, frame/state comparison, and the
documented analysis route used while replacing translated functions.
