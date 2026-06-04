# Skill: Create Override

Add a native C override for a Benefactor M68K recompiled function. Use when: a recompiled function has wrong behavior, uses hardware waits, or needs to rebuild copper list / hardware state that the recompiler misses.

> **Self-evolution:** If you extend or build a tool that changes this skill's procedure, update this file in the same step.

## Boundary: override vs recompiler (read first)
The recompiler only does FAITHFUL translation of the binary. Touch it ONLY for a
recompiler **bug** (mistranslated instruction) or a **missed dispatch target**.
**Every change to game BEHAVIOR is an override** — including changing a constant that
lives in generated code (widen a window, alter logic for a PC-native feature). Emitting a
different value than the ROM holds is mistranslation and breaks the oracle diff. Override
instead (super-call + adjust, or re-implement the loop natively). Full rule:
`instructions/create-override.md`.

## When to Use
- A recompiled function produces wrong output (wrong D0, wrong addresses).
- A function runs an infinite hardware-wait loop (VHPOSR, BLTSIZE, etc.).
- A function needs to rebuild copper list entries the recompiler misses.
- The recompiler generates bad C (e.g., wrong An-dest flag handling).
- You need different *behavior* for a PC-native feature (widescreen window widening, etc.).

## File: `src/pc.c`

### Step 1: Write the native function

```c
static void native_XXXXXX(M68KCtx *ctx)
{
    /* Read inputs from ctx->D[], ctx->A[], g_mem[], r16()/r32() */
    uint32_t a5 = ctx->A[5];   /* always $00531C */
    /* ... compute ... */
    /* Write outputs back to ctx->D[], ctx->A[], g_mem[], w16()/w32() */
    /* Write hardware registers via hw_write16(0xDFFxxx, value) */
}
```

**Macros available in pc.c:**
| Macro | Meaning |
|-------|---------|
| `r8(addr)` | Read byte from chip RAM |
| `r16(addr)` | Read 16-bit big-endian from chip RAM |
| `r32(addr)` | Read 32-bit big-endian from chip RAM |
| `w8(addr,v)` | Write byte to chip RAM |
| `w16(addr,v)` | Write 16-bit big-endian to chip RAM |
| `w32(addr,v)` | Write 32-bit big-endian to chip RAM |
| `hw_write16(reg,v)` | Write to hardware register ($DFFxxx) |
| `hw_write32(reg,v)` | Write 32-bit to hardware register |
| `call_fn(ctx, addr)` | Call a recompiled function (saves/restores all regs) |
| `A5` | `0x00531Cu` — base pointer |
| `A6` | `0x00DFF002u` — hardware base |

### Step 2: Register the override in `pc_run()`

```c
int pc_run(void)
{
    rt_register_override(0x0030C2u, native_0030C2);
    rt_register_override(0x003818u, native_003818);
    rt_register_override(0x0074AAu, native_0074AA);
    rt_register_override(0xXXXXXXu, native_XXXXXX);  /* ADD HERE */
    ...
}
```

### Step 3: Verify
```bash
cd <repo>/build
cmake --build . --target benefactor-harness -j$(nproc) 2>&1 | tail -3
cd <repo>
bash run_harness.sh --frames 3 --boot-frames 600 2>&1 | grep -E "DIFF|ok$|MATCH"
```

## Common Override Patterns

### Copper list static entry rebuilder
```c
static void native_rebuild_copper_static(M68KCtx *ctx)
{
    (void)ctx;
    /* Rebuild BPLCON0/1/2 entries at $8720–$8728 that blitter clobbered */
    w16(0x8720u, 0x0100u); w16(0x8722u, 0x0200u);  /* BPLCON0=$0200 */
    w16(0x8724u, 0x0102u); w16(0x8726u, 0x0000u);  /* BPLCON1=$0000 */
    w16(0x8728u, 0x0104u); w16(0x872Au, 0x0040u);  /* BPLCON2=$0040 */
}
```

### Hardware-wait eliminator
```c
static void native_wait_for_blitter(M68KCtx *ctx)
{
    (void)ctx;
    /* hw_do_blit() already ran synchronously — nothing to wait for */
}
```

### Partial recompile with fixed computation
```c
static void native_XXXXXX(M68KCtx *ctx)
{
    /* Run the original recompiled code but fix a specific sub-result */
    gfn_XXXXXX(ctx);           /* call original */
    ctx->D[0] = fixed_value;   /* patch the broken output */
}
```

## Existing Overrides (Do Not Duplicate)
| Address | Function | Reason |
|---------|----------|--------|
| `$0030C2` | `native_0030C2` | No-op (hw wait, not needed) |
| `$003818` | `native_003818` | Recompiled falls through to garbage |
| `$0074AA` | `native_0074AA` | Boot animation table iterator |
| `$00405C` | `native_00405C` | Engine hook for text/copper pointer writes |
| `$0041A4` | `native_0041A4` | Sprite blitter setup + static copper rebuild |
| `$0055A0` | `native_0055A0` | Timer interrupt override hook |
| `$003488` | `native_003488` | Game-frame hook + static copper rebuild |
| `helper` | `native_rebuild_copper_static` | Rewrites copper static BPL pointer value words with `w16` |
