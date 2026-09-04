# Benefactor binary analysis

Analyze the original 68000 bytes, never a host-language translation of them.
The authoritative inputs are the user's validated disks and exact memory/image
captures made by the separately built PUAE oracle.

## Address model

- The preserved main-memory capture is `logs/harness_puae_chipram.bin` when an
  operator has produced it locally. It is untracked user evidence.
- File offset and chip-RAM address are equal in that 512 KiB capture.
- Treat main/title/gameplay/credits as different executable-image identities.
  An address is not sufficient without the active image and generation.
- Use `tools/disasm2.py <image> <file-offset> <guest-address> <length>` for a
  focused decode. Ghidra analysis may be used for call graphs and wider xrefs;
  it is a maintainer tool, not a player prerequisite.

## Find a writer

1. Select one exact image identity and address range.
2. Add a diagnostic watchpoint at the canonical amigaport memory boundary, or
   at the PUAE oracle boundary for an oracle-only run.
3. Prove the diagnostic sees both a known write and a known non-write case.
4. Capture the guest PC and image identity at the first matching write.
5. Disassemble the original bytes around that PC and follow its callers.
6. Record the recovered address/ABI/behavior fact in the nearest living note.

Decode guest instructions from the authenticated retail image bytes. Do not add
watchpoints through gameplay logging or environment-variable gates.

## Find a copper-list owner

1. Watch the exact copper address (for example, BPLCON2 at `$8728`).
2. If no CPU write occurs, instrument the title-neutral blitter boundary and
   identify the blit whose destination overlaps the address.
3. Capture the subsequent guest writes that reconstruct the list.
4. Decode the original routine and compare the resulting memory against PUAE.

Copper instructions are pairs of 16-bit words: register then value. Display
register words use the `$01xx` range.

## Preserved findings

- The observed blitter fill uses `BLTCON0=$19F0`, destination `$8720`, and
  writes `$FFFF` through `$872C`, overlapping BPLCON0/1/2 entries.
- Guest routine `$00405C` rebuilds BPL1PT through BPL4PT at
  `$8788-$87AE`; it does not rebuild the BPLCON entries.
- Guest routine `$0041A4` triggers that blitter fill; it does not rebuild the
  BPLCON copper entries.
- PUAE evidence has BPLCON2 value `$0040` at `$872A` after level setup.

See `instructions/harness.md` for the corresponding oracle observations.
