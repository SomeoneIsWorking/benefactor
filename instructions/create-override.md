# Creating Native Overrides

## Recompiler vs override — the boundary (read first)

The recompiler does ONE thing: **faithful translation of the original binary**. Touch it
ONLY for:
1. **a recompiler bug** — an instruction translated wrong (mistranslation, bad flag/operand
   handling, an unimplemented opcode), or
2. **a missed dispatch target** — a jump/call target the scanner didn't register.

**Everything else is an OVERRIDE.** Any change to *game behavior* — widening a window,
changing a constant, altering logic for a PC-native feature (widescreen), skipping a wait,
fixing a wrong runtime result — is an override, never a recompiler edit.

A constant living in generated code does **NOT** make it a recompiler change. Deliberately
emitting a *different* value than the ROM holds is mistranslation: it breaks the
"generated code is sacrosanct / diffable against the oracle" property and makes every
divergence comparison meaningless. To change such a constant for a feature, **override the
function** — super-call the recompiled body and adjust around it, or re-implement the loop
natively (still dispatching the recompiled per-object handlers). The recompiled body stays
byte-faithful and A/B-toggleable.

## Native Override Pattern

Use this to replace a recompiled function with hand-written C:

```c
// In pc_overrides.c:
static void native_XXXXXX(M68KCtx *ctx) { /* C implementation */ }

// Register in pc_register_overrides():
rt_register_override(0xXXXXXXu, native_XXXXXX);
```

Macros in `pc_internal.h`: `r8/r16/r32(addr)`, `w8/w16/w32(addr,v)`, `hw_write16(reg,v)`, `call_fn(ctx, addr)`, `A5`, `A6`.

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
