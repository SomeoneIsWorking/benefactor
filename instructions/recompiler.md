# Recompiler Use and Improvement

The recompiler (`recomp.py`) translates the M68K binary to C once. Better recompiler output means fewer native overrides needed. Improving it is higher-leverage than writing overrides.

## Output quality goal

The primary recompiler improvement target is **readable, macro-based C**. Example of what must not be emitted:

```c
/* BAD — double MR16 read, no named locals, unreadable */
{ uint64_t _r = (uint64_t)(uint16_t)ctx->D[0] - (uint64_t)(uint16_t)MR16((uint32_t)(ctx->A[1] + 6u)); RT_SUB_FLAGS_16(ctx->D[0],MR16((uint32_t)(ctx->A[1] + 6u)),_r); }
```

Target style — capture reads into locals, one step per line:
```c
{ uint16_t _s = MR16(ctx->A[1] + 6u); uint16_t _d = (uint16_t)ctx->D[0];
  RT_SUB_FLAGS_16(_d, _s, (uint64_t)_d - _s); }
```

Fix the emitter in `recomp.py`, never edit `generated/game.c`.

## When to improve the recompiler vs write an override

| Improve recompiler | Write native override |
|--------------------|-----------------------|
| A class of M68K instructions is mis-translated | One specific function has unique wrong behavior |
| Fix would apply to many `gfn_` functions | Function needs hardware-wait elimination |
| The emitted C is provably wrong M68K semantics | Function needs per-frame side effects PC-side |

## Workflow

```bash
# 1. Run tests first — establish baseline
python3 tools/test_recomp.py

# 2. After fixing recomp.py — tests must still pass
python3 tools/test_recomp.py

# 3. Regenerate game.c
python3 tools/recomp/recomp.py chip_ram_dump.bin --chip-dump \
    --out-c src/engine/generated/game.c \
    --out-h src/engine/generated/game.h

# 4. Rebuild + verify harness
cd build && cmake --build . --target benefactor-harness -j$(nproc)
cd ../.. && bash run_harness_headless.sh 2>&1 | grep -E "DIFF|ok$|MATCH"
```

## Finding what game.c emits for a function

```bash
# Find the C translation of a specific function
grep -A 50 "^void gfn_00XXXX" src/engine/generated/game.c | head -60

# Find all uses of a specific M68K register write pattern
grep -n "MW16\|MW32" src/engine/generated/game.c | grep "0x01" | head -20
```

## Key recompiler structure

- `Translator.translate(insn)` in `recomp.py` — one method handles all instruction emission
- `entries.py` — add new entry points here; controls which functions are emitted
- Tests in `test_recomp.py` encode raw M68K bytes and assert exact C output strings

## Known fixed bugs (DO NOT RE-FIX)
- `addq/subq` to An: no CCR update. Fixed. 17/17 tests pass.
