# Hardware Layer (`hw.c` / `hw.h`)

## OCS Custom Chip Register Map (handled)

All addresses relative to `$DFF000`.

| Offset | Name | Direction | Notes |
|--------|------|-----------|-------|
| `$002` | DMACONR | R | Returns `s_dmacon \| 0x4000` |
| `$004` | VPOSR | R | Synthetic beam: bit 0 = scanline > 255 |
| `$006` | VHPOSR | R | Synthetic beam: `(scanline & 0xFF) << 8` |
| `$00A` | JOY0DAT | R | Joystick via `hw_joystick()` |
| `$080` | COP1LCH | W | Copper list pointer high word (shadowed) |
| `$082` | COP1LCL | W | Copper list pointer low word (shadowed) |
| `$088` | COPJMP1 | W | Restart copper (shadowed) |
| `$08E` | DIWSTRT | W | Display window start — stored in `s_diwstrt` |
| `$090` | DIWSTOP | W | Display window stop |
| `$096` | DMACON | W | Set/clear DMA enable bits |
| `$09A` | INTENA | W | Set/clear interrupt enable |
| `$09C` | INTREQ | W | Set/clear interrupt request |
| `$0E0–$0F6` | BPL1PTH–BPL6PTL | W | Bitplane pointers → `s_bplptr[0–5]` |
| `$100` | BPLCON0 | W | nplanes = bits 14–12; EHB = bit 7 |
| `$108` | BPL1MOD | W | Odd bitplane modulo |
| `$10A` | BPL2MOD | W | Even bitplane modulo |
| `$180–$1BE` | COLOR00–COLOR31 | W | Palette → `s_palette[]` via `amiga_to_argb()` |

## CIA Register Map (handled)

### CIA-B (`$BFD000–$BFDxFF`)

| Reg index | Name | Notes |
|-----------|------|-------|
| 0 (PRA) | Port A | Returns `0xFF` (disk ready) |
| 4 (TALO) | Timer A low | `s_ciab_ta_cnt & 0xFF` |
| 5 (TAHI) | Timer A high | `s_ciab_ta_cnt >> 8` |
| 13 (ICR) | Interrupt control | Read clears; bit 7 set if any enabled |
| 14 (CRA) | Control A | Bit 0 = timer running; bit 4 = LOAD strobe |

### CIA-A (`$BFE000–$BFExFF`)

| Reg index | Name | Notes |
|-----------|------|-------|
| 0 (PRA) | Port A | Bit 0 = fire button (active low) |

## Framebuffer Rendering

`hw_render_frame()` decodes raw bitplane data from `g_mem`:

```
for each pixel (x, y):
    for each bitplane p in [0..nplanes):
        byte = g_mem[s_bplptr[p] + x/8]
        bit  = (byte >> (7 - x%8)) & 1
        cidx |= bit << p
    pixel = s_palette[cidx]
```

- Supports 1–6 bitplanes
- EHB (Extra Half-Brite): 32 extra colours = `s_palette[0..31] >> 1`
- BPL1MOD / BPL2MOD applied each scanline (odd/even bitplane groups)

## Synthetic Beam Counter

The game polls `VPOSR` to detect vertical blank.  
`hw_advance_scanline()` increments `s_scanline` (0–311). Call this from the game's vblank loop or tick it from `hw_present_frame()`.  
`hw_present_frame()` is the hard vsync point — it calls `SDL_RenderPresent` which blocks on vsync.

## Disk Loading

```c
int hw_load_disk(int disk, uint32_t offset, uint32_t len, uint32_t dst_amiga);
```

- `disk`: 1-based index into `s_disk_paths[]`
- `offset`: byte offset within the disk image file
- `len`: number of bytes to read
- `dst_amiga`: Amiga address in `g_mem` to write into

Called from the ILLEGAL instruction handler in `rt.c` when the game triggers a WHDLoad disk Load call.

## Colour Conversion

```c
static uint32_t amiga_to_argb(uint16_t c) {
    r = ((c >> 8) & 0xF) * 0x11;
    g = ((c >> 4) & 0xF) * 0x11;
    b = ((c     ) & 0xF) * 0x11;
    return 0xFF000000 | (r<<16) | (g<<8) | b;
}
```

Amiga colour registers are 12-bit RGB4 (`0xRGB`). Each nibble × 0x11 = 8-bit channel.
