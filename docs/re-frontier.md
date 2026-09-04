# RE frontier — Benefactor execution spine

This is the ordered ground-truth chain from disk image to representative
gameplay and credits. `docs/codemap.md` owns placement; this file records which
behavior is grounded in the disks/binary and which dependency remains.

Statuses: `re-verified`, `re-partial`, `in-progress`, `hack`, `authored`,
`todo`, and `skip-by-design`. A `hack` is debt and cannot be a resting state.

## image-loading

### IMG-MAIN — Main/intro disk image
- status: re-verified
- deps:
- evidence: production disk loader reads Disk.1 boot payload to `$003000` and runs the recovered ATN decompressor; the pre-migration product boots without Kickstart or Workbench
- where: src/engine/disk_boot.c, src/engine/overlay_load.c
- gap:
- notes: The loader behavior survives; generated C derived from the image does not.

### IMG-TITLE — Title/menu image activation
- status: re-verified
- deps: IMG-MAIN
- evidence: recovered `$6D714`/relocated `$150` loader restores the low loader block, loads title chunks, and transfers through the `$003330` image; the title/menu route and native menu owners are exercised
- where: src/engine/overlay_load.c, src/port/overrides/boot.c
- gap:
- notes: Runtime identity must distinguish this `$003330` image from credits.

### IMG-GAME — Gameplay image and relocation
- status: re-verified
- deps: IMG-TITLE
- evidence: the production loader reads the `$06E000`, `$003330`, and `$577000` payloads, applies both recovered relocation passes, and reaches the documented gameplay entry and all 60 levels
- where: src/engine/overlay_load.c, instructions/gameplay-engine-map.md
- gap:
- notes: Image bytes and relocation facts are reusable independent of static execution.

### IMG-CREDITS — Credits image activation
- status: re-verified
- deps: IMG-GAME
- evidence: recovered `$150 d0=3` path loads and ATN-decompresses Disk.3 payload to `$003330`; the documented W6L2 win route reaches credits and natural exit
- where: src/engine/overlay_load.c, src/port/overrides/boot.c, instructions/gameplay-engine-map.md
- gap:
- notes: Address reuse with IMG-TITLE is the mandatory cache/override discriminator.

## machine-services

### MACH-MEMORY — Guest RAM and device routing
- status: re-partial
- deps: IMG-MAIN
- evidence: existing accessors distinguish ordinary RAM from OCS/CIA regions and the PUAE harness observes chip-memory/device effects
- where: src/engine/rt.c, src/engine/hw.c, docs/hardware-layer.md
- gap: adapt the verified map to one complete amigaport CPU/memory/service contract and notify executable-image writes
- notes:

### MACH-INTERRUPTS — Level 3/6 interrupt and frame service
- status: re-partial
- deps: MACH-MEMORY
- evidence: recovered timer/audio notes and existing native delivery reach installed Level 3 and Level 6 handlers at their observed rates
- where: src/engine/hw.c, src/engine/hw_audio.c, instructions/audio-engine.md
- gap: reproduce full SR masking, supervisor state, exception frames/vectors, cycles, and bounded exits through amigaport
- notes:

### MACH-DYNAREC — Runtime 68000 execution
- status: todo
- deps: MACH-MEMORY
- evidence:
- where: shared/amigaport plus Benefactor executor adapter
- gap: implement complete PC/SR/exception/cycle state, runtime decoding/lowering, host emission, cache lifetime, and a separately built test interpreter
- notes: The reduced generated-function context is not sufficient.

### MACH-IMAGE-ID — Four-generation cache and override identity
- status: todo
- deps: IMG-CREDITS, MACH-DYNAREC
- evidence:
- where: shared/amigaport cache keys and Benefactor image/override owner
- gap: prove title and credits at `$003330` cannot share blocks or override decisions; cover reload, restore, and controlled-negative invalidation
- notes:

## native-ownership

### NATIVE-DISK — Disk loading, ATN decompression, and relocation
- status: re-verified
- deps: IMG-MAIN
- evidence: existing native loaders reproduce all four observed image paths and remove timed floppy I/O while preserving loaded memory effects
- where: src/engine/disk_boot.c, src/engine/overlay_load.c, src/port/overrides/boot.c
- gap:
- notes:

### NATIVE-HARDWARE — Blitter, render, and Paula audio services
- status: re-partial
- deps: MACH-MEMORY
- evidence: PUAE comparisons grounded word-aligned OCS blitter pointers, copper lists, framebuffer production, and four-channel Paula mixing; current host exposes faithful/native/Vulkan render paths
- where: src/engine/hw_blitter.c, src/engine/hw_audio.c, src/render/
- gap: reconform these service inputs and outputs through the dynarec product over representative gameplay
- notes:

### NATIVE-GAME — Title, gameplay, UI, and enhancement owners
- status: re-partial
- deps: IMG-GAME
- evidence: binary-derived addresses and host behavior are recorded in the gameplay map; native menu, level flow, input, camera, object capture, interaction, physics, and presentation routes exist
- where: src/port/overrides/, src/port/, instructions/gameplay-engine-map.md
- gap: move each generated-body call to image-aware runtime dispatch/scoped original calls and reconform it through the dynarec
- notes: Authored enhancements remain distinguished from faithful replacements.

### NATIVE-ORIGINAL — Scoped original-call behavior
- status: todo
- deps: MACH-DYNAREC, NATIVE-GAME
- evidence:
- where: Benefactor executor adapter and native override registry
- gap: prove disabled, enabled, and scoped-original sequences with complete image identity and no recursion
- notes:

## conformance

### CONF-HARNESS — Independent PUAE differential control
- status: re-partial
- deps: MACH-MEMORY
- evidence: the harness drives and inspects both legs, compares state/chip memory/frames, and has historical matched checkpoints plus known positive divergences
- where: src/harness/, vendor/libretro-uae/, instructions/harness.md
- gap: compare the shipping dynarec path with complete CPU/SR/exception/timing state and denominated translated execution/invalidation
- notes: Historical four-frame equality is not representative gameplay parity.

### CONF-GAMEPLAY — Representative native/dynarec gameplay
- status: todo
- deps: MACH-IMAGE-ID, MACH-INTERRUPTS, NATIVE-HARDWARE, NATIVE-ORIGINAL, CONF-HARNESS
- evidence:
- where: docs/migration.md
- gap: pass the bounded cavern, transition, audio, interrupt, four-image, conformance, and host-performance retirement gate
- notes: Only this step permits removal of the static pipeline.
