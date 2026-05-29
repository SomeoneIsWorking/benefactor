# Harness Refactoring Roadmap

## Current State
- `harness_main.c`: 1716 lines (monolithic)
- Sections: helpers, PUAE phase, PC phase, comparison, reporting, main orchestration

## Proposed Modularization (Priority Order)

### Phase 1: Extract Compare Module (380 lines → new file)
**File:** `harness_compare.c`
**Extract from harness_main.c:**
- Lines 112-162: `reg_name()` helper
- Lines 167-203: `copper_disasm()`
- Lines 209-451: `print_divider()`, `print_cause()`, `report_divergence()`
- Lines 1085-1137: `frames_differ()`
- Lines 1139-1276: `compare_framebuffers()`
- Lines 1278-1313: `report_chipram_diff()`
- Lines 1315-1527: `compare_phases()` (main reporter)

**Benefit:** Isolates all reporting/diagnostic logic from harness orchestration

### Phase 2: Extract PUAE Module (220 lines → new file)
**File:** `harness_puae.c`
**Extract from harness_main.c:**
- Lines 453-459: `is_game_frame()`
- Lines 465-683: `run_puae_phase()`

**Benefit:** Clean separation of PUAE-specific boot/capture logic

### Phase 3: Extract PC Module (100 lines → new file)
**File:** `harness_pc.c`
**Create new functions:**
- `run_pc_boot_phase()` — replaces inline PC boot in main()
- Helper for PC frame loop

**Benefit:** Symmetric structure to PUAE module

### Phase 4: Refactor Main (200 lines)
**Simplify harness_main.c to:**
```c
int main(int argc, char **argv) {
    // Parse args
    // Phase 1: run_puae_phase()
    // Phase 2: run_pc_boot_phase()
    // Phase 3: boot comparison
    // Phase 4: interleaved frame loop + per-frame comparison
    // Phase 5: compare_phases() final report
}
```

**Result:** Clear orchestration, no phase logic in main()

### Phase 5: Create Shared Header
**File:** `harness_internal.h`
- Forward declarations
- Extern state arrays (s_puae_log, s_pc_log, etc.)
- Function signatures for all modules
- Shared constants (MAX_FRAMES, FB_W, FB_H, etc.)

## Size Reduction Target
- Before: 1716 lines in harness_main.c
- After refactoring:
  - `harness_main.c`: ~250 lines (orchestration only)
  - `harness_puae.c`: ~220 lines
  - `harness_pc.c`: ~100 lines
  - `harness_compare.c`: ~380 lines
  - `harness_internal.h`: ~80 lines

## Implementation Notes
- Each module compiles independently (check include dependencies)
- CMakeLists.txt must list all harness/*.c files
- No circular dependencies (state defined once in harness_internal.h)
- All state arrays remain static in main harness (not per-module)

## Quick Win (if full refactor is deferred)
Add section markers to harness_main.c:
```
/* ════════════════════════════════════════════════
   SECTION: Copper Disassembler (Lines 112-203)
   ════════════════════════════════════════════════ */
   
/* ════════════════════════════════════════════════
   SECTION: Divergence Reporting (Lines 205-451)
   ════════════════════════════════════════════════ */
```
This makes the structure visible without moving code.
