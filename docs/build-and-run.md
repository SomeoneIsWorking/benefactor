# Build & Run

## Prerequisites

- A C compiler + CMake 3.20+
- SDL2 system library (`libsdl2-dev`)
- Python 3 + capstone: `pip install capstone`
- Your own **Disk.1 / Disk.2 / Disk.3** Benefactor images in the repo root.

## The recompiled code is generated from your disks (not shipped)

`src/engine/generated/` is a recompilation of the copyrighted game and is **not in the
repo** (gitignored). CMake regenerates it automatically from your `Disk.*` images
at configure time via `tools/regen.sh` — which builds a small bootstrap dumper,
extracts the four bank images from the disks (pure disk read + ATN! decrunch, no
PUAE/Kickstart/WHDLoad/slave), and runs the recompiler. It is hash-gated, so it
only re-runs when the recompiler/loaders/seeds change or the code is missing.

So the whole build is just:

```bash
# Disk.1 Disk.2 Disk.3 in the repo root
cmake -S . -B build              # auto-regenerates src/engine/generated/ from the disks
cmake --build build --target benefactor-pc -j"$(nproc)"
./build/benefactor-pc Disk.1 Disk.2 Disk.3      # window: arrows=move, Z/Ctrl/Space=fire
#   --level N   start directly at level 1..60 (skips intro/title/menu)
```

Native disk boot needs only the `Disk.*` images — no kickstart ROM. To force a
regen, delete `src/engine/generated/.regen-hash` (or the whole dir) and reconfigure.

## Render backend (SDL / Vulkan)

Present is delegated to a render backend (`src/render/present_*.c`), selected by
`BENEFACTOR_RENDER=sdl|vulkan` (default **sdl** — the SDL_Renderer path).

- **Vulkan is optional.** CMake `find_package(Vulkan)`: if the Vulkan SDK (headers +
  `glslc`) is present, the Vulkan backend is compiled in; otherwise the build is
  SDL-only and `BENEFACTOR_RENDER=vulkan` falls back to SDL with a log line. On
  Linux install the LunarG SDK / distro `vulkan` packages + `glslc` (shaderc).
  On **macOS** the Vulkan path runs through **MoltenVK** (`brew install molten-vk
  vulkan-headers shaderc`, or the LunarG macOS SDK).
- **Verify the Vulkan pipeline without a display:** `./build/benefactor-pc
  --vk-selftest` renders a gradient through the offscreen Vulkan pipeline and
  compares the readback to the input (prints the max channel diff; 0 = exact).
- The windowed Vulkan swapchain present is still being brought up; until then the
  default windowed path is SDL. The software renderer is always the fallback.

## macOS (Apple Silicon / Intel)

The game is portable: SDL2 + pthread, game on a worker thread, all SDL on the
main thread (Cocoa's requirement). `brew install sdl2 cmake` — CMake auto-adds
`brew --prefix` to the search path. Then the same two commands as above. The
**harness** (`benefactor-harness`) compiles the whole PUAE/libretro-uae tree
and is a dev-only comparison tool (not needed to play; may need macOS porting) —
build just the `benefactor-pc` target.

## Legacy: PUAE chip dump (superseded by tools/regen.sh)

The sections below describe the old PUAE-based input extraction. The disk-based
`tools/regen.sh` flow above supersedes it; these are kept for reference only.

## Step 1 — Extract PUAE chip RAM dump from ROMs/disks

```bash
cd <repo>
python3 tools/extract_chipram_from_roms.py --build --output chip_ram_dump.bin
# Produces: chip_ram_dump.bin
```

## Step 2 — Run the recompiler from chip dump

```bash
python3 tools/recomp/recomp.py \
    chip_ram_dump.bin --chip-dump \
    --out-c src/engine/generated/game.c \
    --out-h src/engine/generated/game.h
```

## Step 3 — CMake build

```bash
cd benefactor-pc
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --parallel 4
```

The CMakeLists.txt **no longer uses Musashi**. It runs the recompiler against `chip_ram_dump.bin` as a build step.

To force a clean recompile:
```bash
rm src/engine/generated/game.c src/engine/generated/game.h
cmake --build . --parallel 4
```

## Step 4 — Run

```bash
cd <repo>
./build/benefactor-pc \
    chip_ram_dump.bin \
    Disk.1 \
    Disk.2 \
    Disk.3
```

**Controls:**
| Key | Action |
|-----|--------|
| Arrow keys | Move / joystick direction |
| Z or Left Ctrl | Fire button |
| F11 | Toggle fullscreen |
| Escape | Quit |

## Debugging Tips

- Build with `-DCMAKE_BUILD_TYPE=Debug` — adds `-g`, no `-O2`
- `fprintf(stderr, ...)` goes to terminal; no log files needed
- `rt.c` `is_hw()` gate: addresses `$BFD000`, `$BFE000`, `$DFF000` are hardware — everything else is plain RAM
- To trace hardware register writes, add prints to `hw_write16()` in `hw.c`
- `hw_present_frame()` is the vsync point — the game's vblank poll loop terminates here
