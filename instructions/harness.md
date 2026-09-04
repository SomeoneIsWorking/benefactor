# Benefactor harness: established evidence

This file records durable PUAE comparison evidence. The harness is test-only;
the shipped game uses native Benefactor owners plus the image-qualified
`shared/amigaport` interpreter. Harness code and PUAE are never linked into the
gameplay product.

## PUAE determinism

A live PUAE boot is not deterministic: disk-load completion varies by roughly
one emulated frame with host I/O. Runs were identical through Kickstart frame
23, then differed by one frame at the game-load boundary (`cop1lc=$001000`,
observed at frame 171 versus 172). Chip RAM at synchronization was identical,
but CPU, CIA, and beam state retained the phase difference.

The reliable oracle procedure freezes a complete PUAE machine state at a named
synchronization point and restores that state for each comparison. A deliberate
refreeze is required after disk or core changes. The discriminator must prove a
known-equal run and a controlled known-different run before its output is
trusted. Compare image identity, CPU registers, SR, exceptions, cycles, memory
writes, device events, and frames; a framebuffer-only comparison is not CPU
conformance evidence.

All recurring oracle artifacts use the one stable project activity directory
`scratch/harness-puae/`, resolved before PUAE starts by the typed project-path
owner. A frozen machine state is eligible only when its adjacent
`puae_sync.contract` contains the current scratch contract marker and the exact
resolved `WHDLoad` mount path. Moving the checkout invalidates the state rather
than restoring against an embedded stale path. Older states that embed a
transient mount are rejected; the harness does not recreate a compatibility
mount. A refreeze overwrites `puae_sync.state` and its contract in that same
activity directory.

An established deterministic comparison reported perfect displayed-region
agreement through more than 41 frames. Its stable frame-35 residual was 21
off-screen bytes in `$069FB-$06AB5`, `$0718BA`, `$078DC2/$078DEA`, and
`$07FF9D+`. These numbers are historical evidence, not a current product gate.

## Memory and image identity

- Main-image chip RAM begins at guest address zero; the observed chip-RAM range
  is `$00000-$7FFFF` (512 KiB).
- The historical main-image context used `A5=$00531C`, `A6=$00DFF002`, and stack
  top `$080000`.
- Main/title, gameplay, and credits addresses are meaningful only with their
  executable-image identity and generation. A restored or newly loaded image
  creates a new generation before execution resumes.
- The shipping interpreter and native override registry use
  `(image kind, generation, guest address)`. Never dispatch by an address alone.

## Copper-list facts

- `$7BC8` is the zero-plane/loading-screen copper list when the flag at
  `A5-$117A` is zero.
- `$86CC` is the three-plane/gameplay copper list when that flag is nonzero.
- Guest routine `$0041A4` toggles the flag with `not.w -$117A(a5)`.
- Copper instructions are 16-bit register/value pairs. For list base `$86CC`,
  instruction `N` occupies `$86CC + N*4` and its value occupies the next word.

| Instruction | Address | Register | PUAE value | Meaning |
| --- | --- | --- | --- | --- |
| 21 | `$8720` | `$0100` | `$0200` | `BPLCON0` |
| 22 | `$8724` | `$0102` | `$0000` | `BPLCON1` |
| 23 | `$8728` | `$0104` | `$0040` | `BPLCON2` |
| 47 | `$8788` | `$01E0` | `$0002` | `BPL1PTH` |
| 47 value | `$878A` | — | `$5334` | `BPL1PTL` value |

The blitter fill launched by `$0041A4` uses `BLTCON0=$19F0`, source `$8748`,
and destination `$8720`. Its minterm produces `$FFFF`, clobbering the copper
entries through `$872C`. Guest routine `$00405C` rebuilds bitplane pointers at
`$8788-$87AE` but does not restore the `BPLCON` entries. The native owner must
therefore restore the proven list words after the blit. PUAE showed
`BPLCON2=$0040` at `$872A` after level setup.

## Confirmed native root causes

### Copper value writes

An old native path used a 32-bit write at copper value-word addresses such as
`$7C86`, `$7CDE`, `$878A`, and `$87E2`, overwriting the adjacent register word.
The correct operation is two explicitly addressed 16-bit writes. After this
fix, a three-frame comparison reported matching copper-list words.

### Static copper words and `$7BC8` pointers

The `$0041A4` fill clobbers `BPLCON0/1/2` and `BPL1/2MOD` at `$8720-$8732`.
Routine `$00405C` also writes screen-derived bitplane pointers into both lists,
which gives `$7BC8` the wrong screen-number values. The native wrappers restore
the static words and rewrite `$7BC8` with screen-zero bitplane pointers while
leaving the valid `$86CC` pointers unchanged.

### Blitter channel-pointer alignment

OCS blitter DMA is word-addressed and ignores bit zero of channel pointers. The
native blitter once honored byte-odd pointers, shifting the title car by eight
pixels on alternating frames. Masking bit zero from all four channel pointers
made the car buffer `$73680-$77000` bit-exact against PUAE for the measured
frames.

### Ascending B-channel shift

The ascending native blitter shifted channel A but not channel B. The title-car
cookie-cut uses both mask and data with the same sub-word shift; leaving B
unshifted produced frame-varying corruption. Shifting B from its previous and
current source words in the same way as A matched PUAE in the measured frames.

### Required 68000 semantics

The retail title relies on all of the following semantics. They are regression
requirements for the maintained interpreter, not title-address exceptions:

- `ror`/`rol` with 8-, 16-, and 32-bit destinations and correct C/N/Z results;
- `Scc`, `st`, and `sf` byte writes, including data-register low-byte behavior;
- byte/word add and subtract flags after width truncation;
- predecrement address-register updates before memory access; and
- PC-relative indexed `movem` using the extension-word-relative architectural
  base. The level-22 object case at `$5988AE` must select offset `200`, not `64`.

The title-car routine at `$003488` uses `ror.w #4,d2` to place the bob shift in
`BLTCON1` bits 12-15. Its exit flags are written by `st.b` at `$35DA` and
`$35E6`; without those writes, the `$3652` exit check loops forever. These
addresses are compact positive test cases for the interpreter.

## Frame-hook interpretation

The historical frame hook was reached from the presentation boundary. A
three-frame title run observed snapshots in this order: gameplay list `$86CC`,
title list `$7BC8`, gameplay list `$86CC`. Comparison intentionally excluded
register shadows whose sampling phase differs from PUAE, including bitplane
pointers, palette shadows, and Paula volume state. Diagnostic output that labels
those values as different must not silently promote them into gate failures.

## Native address owners retained by the port

| Image address | Native owner | Responsibility |
| --- | --- | --- |
| `$0030C2` | `native_hw_wait` | host hardware-wait boundary |
| `$0031A0` | `native_blitter_wait_clear` | clear/wait behavior |
| `$003818` | `native_sprite_table_init` | sprite-table initialization boundary |
| `$0074AA` | `native_boot_anim_iterator` | guarded boot palette animation |
| `$00405C` | `native_text_sprite_render` | render wrapper and `$7BC8` pointer repair |
| `$0040B6-$004236` | render wrappers | image-address render dispatch boundaries |
| `$0041A4` | `native_sprite_blitter_setup` | blit plus copper-word restoration |
| `$0052A4` | `native_post_blit_handler` | post-blit wrapper |
| `$0055A0` | `native_timer_interrupt` | timer interrupt boundary |
| `$003488` | `native_game_frame` | game frame plus copper-word restoration |

Calling an original routine from an override must use the scoped
image-qualified interpreter call, preserving the 68000 ABI and stack exactly.
Native wrappers are not permission to emulate an instruction incorrectly or to
special-case one guest PC.

## Harness boundary

The oracle may embed a locally built PUAE/libretro core for test-only tracing,
watchpoints, and snapshots. Diagnostics must route through the harness logging
owner. Artifact creation must route through `src/harness/artifacts.c` and
`src/port/project_paths.c`; call sites do not invent output roots. The harness
must remain independently buildable and must not supply CPU execution, disk
loading, or device behavior to the shipped product.
