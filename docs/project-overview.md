# Project Overview

## What This Is

PC port of **Benefactor** (1994, Psygnosis / Digital Illusions CE).  
Approach: **N64Recomp-style static recompilation** — the Amiga 68k binary is translated once, offline, to native C functions. At runtime there is no 68k interpreter; the game runs natively. Only hardware I/O (custom chip / CIA register reads/writes) is intercepted and routed to SDL2.

## Why Not Emulation

Two prior sessions built a full Musashi + OCS/CIA emulator. It was abandoned because:
- Copper list corruption: game rebuilds the copper list each frame at variable addresses; the chip_ram_dump was taken mid-execution so the emulator's copper parser ran off into random memory.
- Vblank sync loop took 400+ real-time frames to unblock.
- Debugging Amiga hardware timing has unbounded scope.

## Inputs

| File | Description |
|------|-------------|
| `chip_ram_dump.bin` | 512 KB PUAE chip RAM snapshot used as runtime and recompilation input |
| `Disk.1–3` | WHDLoad pre-processed disk images (user-supplied) |

## Architecture

```
chip_ram_dump.bin
      │
      ▼
tools/recomp/recomp.py   (capstone 5.0 Python, offline)
      │
      ▼
src/engine/generated/game.c   – native C function per 68k subroutine
src/engine/generated/game.h   – forward decls + dispatch table
      │
      ├── src/engine/rt.h / rt.c    – memory routing, dispatch
      └── src/engine/hw.h / hw.c    – SDL2 hardware layer
```

## Register Mapping

| 68k | C |
|-----|---|
| D0–D7 | `ctx->D[0]`–`ctx->D[7]` |
| A0–A6 | `ctx->A[0]`–`ctx->A[6]` |
| A7 / SP | `ctx->A[7]` |
| CCR | `ctx->N`, `ctx->Z`, `ctx->V`, `ctx->C`, `ctx->X` |

## Initial CPU State (from WHDLoad install file)

```c
ctx.A[7] = 0x07FFF0;   // Stack pointer
ctx.A[5] = 0x076000;   // WHDLoad resident base (game uses offsets from here)
```

## Key Addresses

| Address | Meaning |
|---------|---------|
| `$003000` | Start of game binary in chip RAM |
| `$003740` | Game main loop entry |
| `$00531C(PC)` | WHDLoad resident data structure base (A5 target) |
| `$DFF000` | OCS custom chip base |
| `$BFE001` | CIA-A base |
| `$BFD000` | CIA-B base |
| `$007BC8` | Copper list (normal) |
| `$0086C8` | Copper list (CD32 / alternate) |
