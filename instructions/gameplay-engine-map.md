# Gameplay engine map

The recompiled gameplay-engine bank (`$577000+`, file `src/engine/generated/game_gpl_*.c`,
1075 functions) is the largest piece of code we don't own. This document tracks what
each high-leverage routine does so we can replace them with native C one subsystem
at a time. **Updated by walking the recompiled output + `pc_freeze.bin` state.**

## Object-handler dispatch coverage (how the bank reaches 60/60 levels)

The engine dispatches per-object handlers via `jmp (a1,d0.w)` (`$57D3EC`, `$58C64E`,
…) where the handler address is computed from runtime object data — static descent
can't follow it, and `table_search` resolves a runtime jump by EXACT address, so
every handler must be registered at its true start. Coverage comes from three
AUTO-DERIVED static passes (no play-testing dependence), wired in `recomp.py` for
`bank=='gpl'`, plus a small structured seed tree for the irreducible remainder:

1. **Jump-table extraction** (`extract_jumptables.discover_jumptable_targets`): pc-rel
   `jmp $BASE(pc,Dn.w)` offset tables, installed handler-pointer immediates, and `dc.l`
   function-pointer tables (~207 targets).
2. **Prologue-pin** (`discover_object_handlers` in `scanner.py`): the canonical handler
   opening `movem (a0)/(a0)+/d(a0), <regs>` ($4C90/98/A8 .w, $4CD0/D8/E8 .l), pinned at
   its exact start (~276 targets; recovered ~19 levels, 35→54).
3. **Gap-fill** (phase 3 in `collect_functions`): every validated, terminated, emittable
   block not already covered. `_emittable` rejects misdecoded data (e.g. `ori.b #x,ccr`
   → `UNK_R_27`). ⚠️ Gap-fill ALONE regresses (54→35): it registers blocks at gap
   *starts*, so a dispatch target inside a fall-through chain gets absorbed — its exact
   address stops being a table entry → miss. Always pair it with the prologue-pin.

What the passes CAN'T reach is the **structured seed tree** `tools/recomp/seeds/`
(~430 entries, down from a flat 831-line file): the bulk are **mutually-terminating
dispatch-stub tables** (`$59DDxx-$59E0xx`, `$585xxx`, `$596xxx`) where each stub decodes
*through* its neighbours to a shared terminator — `_scan_func` only stops early when the
next stub is already a known entry, so they can only be segmented when the whole set is
seeded (they defeat static auto-segmentation). Plus a few non-`movem` / mis-aligned
stragglers (`09-dispatch-stragglers.txt`). A seed may carry a name
(`577000 game_overlay_entry`) → `gfn_gpl_577000_<name>` (with a bare-address alias in
the header) to document the engine map; add names as routines are understood.

Regen command:
```
python3 tools/recomp/recomp.py logs/gmem_after_load.bin --chip-dump \
  --base 3000 --code-size 5A0000 --areg 5=57EE12 --bank gpl \
  --out-dir src/engine/generated --seed-dir tools/recomp/seeds
```
After regen, `python3 tools/recomp/check_unregistered_targets.py` flags any literal
`rt_jump/rt_call` target that isn't a function (seed the clean-code ones; PC-rel-index /
skipdata results are data artifacts — don't seed). Verify with the level-entry sweep
(fire past the level card into `cop1lc=$003484` gameplay, run frames + joystick): all 60
levels must enter and survive with no `rt_call: NO FUNCTION ...`. Also
`grep UNK src/engine/generated/game_gpl_*.c`.

## `$57D3EC` object dispatcher — decoded (2026-05-31)

The per-frame object loop dispatches each object's handler through a list of
handler-struct pointers. Reverse-engineered from `$57D3A4`:

```
57D3B0  lea $10e6(a5), a2     ; a2 = $57FEF8 — handler-struct pointer list
57D3DA  movea.l (a2)+, a1     ; P = next struct pointer
57D3DC  move.w  (a1)+, d0     ; d0 = selector = *(P)  (a1 now = P+2)
57D3F0  jmp     (a1, d0.w)    ; handler = P + 2 + s16(selector)
```

So **dispatch target = `P + 2 + s16(*P)`**. The list is built at level setup
(`$57CC1A`) from the per-level object table at `$4d064` (disk-loaded), as
`list[i] = $58692A + offset_i`. There are three sub-lists for three dispatchers:
`$57FEF8` (=`$10E6(a5)`, `$57D3EC`), `$57FF74` (=`$1162(a5)`), `$5800A4`
(=`$1292(a5)`); terminator `$58692C` = separator, `0` = end.

**Completeness:** the list holds the level's FULL object set (not just on-screen
objects), so loading each of the 60 levels (`--level N`) and reading these lists
harvests every dispatch target deterministically — **240 distinct targets** across
all 60 levels. Of those, **227 are already auto-discovered** by
`discover_object_handlers` (they begin with the canonical `movem (a0)` prologue);
only ~13 are non-`movem` and need a seed (e.g. `$586B04` from struct `$586AF0`,
`$58865C`, `$586E40`). So object dispatch is essentially handled by the
prologue-scan + a few seeds; the bulk of the remaining hand-seeds are NON-dispatch
engine/player code (and some bogus historical odd-address seeds). A standalone
harvester proving this lives in the session notes; it isn't a build dependency.

## Level names: per-level name POINTER, not sequential (2026-06-02)

The 10-entry per-world name array (`"NAME"` strings, 44 bytes apart, first quote
at chunk offset `$60` → loads so slot 0's quote sits at **`$5786A8`**) is stored in
an arbitrary **slot order that is NOT play order**. The level→name mapping is an
explicit pointer:

- Each per-world chunk also embeds the engine's **`$32` level table**: one 12-byte
  entry per level `[data_off, name_off, 0]`. On disk `name_off = slot*44`; the
  relocation loop at `$57CC0E` adds base `$5786A8` to it (and `a0`-base to `data_off`).
- The dispatcher at `$5779C2` loads, for the selected level (`liw` from the contiguous
  `$57782E` table), `data_ptr → $10E0(a5)` and **`name_ptr → -$67CA(a5)`**. The card
  renders the name via that pointer.
- So play-order level `liw` → stored slot = `name_off/44`, a non-trivial permutation.
  World 0 is `[0,1,2,7,4,5,8,3,6]` (e.g. L4="SILENTS?" not "FOLLOW THE SIGNS",
  L7↔L9). Worlds 0,2,3,4,5 are permuted; worlds 1,6 are identity.

Reading the name array sequentially (`$5786AC + liw*44`) mislabels those levels —
this was the level-select bug. `pc_preload_all_level_names()` (pc.c) now locates the
table offline (its signature: N 12-byte entries whose 3rd long is 0 and whose
`name_off/44` values are a permutation of `0..N-1`) and applies the permutation.
**SSoT:** all world/level geometry + names go through `pc.h`
(`pc_levels_in_world` / `pc_world_first_level` / `pc_level_split` /
`pc_static_level_name`) — do not re-hardcode the `{9,9,10,10,10,10,2}` split or
re-extract names. See [[reference_compare_tool]] (`lnames`/`levelinfo` REPL).

## Cardinal facts

- **`a5 = $57EE12`** in the gameplay bank — set by `$5770B0: lea $57ee12(pc), a5`.
  Every `$X(a5)` reference in this doc has its absolute address shown as `(= $YYYY)`.
- **`a6 = $DFF000`** (custom-chip base) — set at `$57700A`.
- **`a7 = $5A1BE8`** initial gameplay SP — set at `$577004`.
- The whole engine assumes the gameplay overlay has been loaded into chip RAM and the
  loader-handoff registers (`d5=$1000, d6=$FFFF`) are valid (see [[direct-to-gameplay-entry]]).

## Entry sequence

| Addr | Role | Notes |
|------|------|-------|
| `$577000` | Engine prologue | SR=$2700; a7=$5A1BE8; a6=$DFF000; build copper list at $5773B8; install vectors $68/$6C/$78 ($578272); a5=$57EE12; init $1c/$2c/$1e/$46. **One-shot per gameplay session.** |
| `$5770D0` | Save-RNG branch | `tst.l $42.w` → if non-zero, persist via `jsr -$747C(a5)` |
| `$5770DC` | Save-RNG body | Pushes a5/a6, calls `jsr -$747C(a5)` ($57768E), then $42.w-driven branch |
| `$5770F8` | Per-level setup | `$1092(a5)=$20`, `$10D4(a5)=$28.w`, `$1c.w=$2c.w`, then **`jsr -$6B5E(a5) = $5782B4`** (level decompressor — the big one), then falls into `$577114` |
| `$577114` | **Per-frame main loop** | Calls 23 subsystems in order — see below |
| `$5771FE` | Level-complete | `$20.w++`, set $1c timer, fade, wait fire, branch back to `$5770D0` |

## Per-frame main loop — `$577114` callee list

Each call is `jsr -$X(a5)`. **Status = OWNED if there's a native override registered**;
otherwise we still run the recompiled function.

Order is significant — the engine relies on subsystem A having run before subsystem B
in the same frame. Don't reorder when native-porting.

| # | Call addr | Resolved | First instructions (RE hint) | Subsystem guess | Status |
|---|-----------|----------|-------------------------------|-----------------|--------|
| 1 | -$4008(a5) | `$57AE0A` | `lea $5A1BE8,a0; lea $43E9,a1; bclr.b d2,(a1,d0.w); adda.w #$50,a1` | Sprite-table bit clear (loops 16×, 80-byte stride) | not owned |
| 2 | -$26E8(a5) | `$57C72A` | `lea $692(a5),a0; lea $67A(a5),a1; move.l (a1)+,d0; move.l d0,(a1); move.l a3,-(a1)` | Double-buffer pointer swap | not owned |
| 3 | -$3FE2(a5) | `$57AE30` | `lea $5A231C,a0; lea $43E9,a1; lea $10A6(a5),a2` | Sprite mask/composite | not owned |
| 4 | -$3F42(a5) | `$57AED0` | Init d0/d1=0; `lea $1094(a5),a0; lea $49F4(a5),a1; lea $1098(a5),a2; lea $4A40(a5),a3` | Multi-channel state/scroll? | not owned |
| 5 | -$40FC(a5) | `$57AD16` | `lea $1028(a5),a0; lea $5AC4E6,a1; cmp.l (a1),d0` | Per-frame compare against constant table | not owned |
| 6 | -$278C(a5) | `$57C686` | `movea.l $10DC(a5),a0; move.l $10D8(a5),d1; add.w (a0)+,d1; swap d1; add.w $6(a6),d1` | Reads VPOSR `$6(a6)` — beam-position related, scrolling? | not owned |
| 7 | -$2738(a5) | `$57C6DA` | `lea $5A5272,a0; lea $5800F8,a1; lea $FB0(a5),a2; move.w $F82(a5),d0; cmpi.w #$8,d0` | Reads `$F82(a5)` ("game state index"), dispatch | not owned |
| 8 | -$5782(a5) | `$579690` | `move.l $F90(a5),$10C0(a5); move.w $FA4(a5),$FA6(a5); clr.w $FA4(a5); bsr $57DEAC` | Frame-state housekeeping | not owned |
| 9 | -$44DE(a5) | `$57A934` | `lea $10A6(a5),a2; move.w $F6A(a5),d7; cmp.w $2(a2),d7` | Player-vs-enemy compare? `$F6A` looks like player coord | not owned |
| 10 | -$1678(a5) | **`$57D79A`** | `move.l #$5A27DC,$78E0(a5); lea $16A6(a5),a0; lea $1162(a5),a2; lea $5A3F86,a6` | **Outer object-list walker** (the one whose loop body is `$57D7BC`) — animation cursor dispatcher | not owned |
| 11 | -$13D2(a5) | `$57DA40` | `move.w #$8400,$96(a6); btst #6,$2(a6) BBUSY wait; lea $5A3F86,a0; lea $50(a6),a2; lea $64(a6),a3` | Blitter pass over $5A3F86 list (animation cursor B?) | not owned |
| 12 | -$1B90(a5) | `$57D282` | `move.l $67E(a5),d6; move.l #$45864,d7; movea.l $682(a5),a0` | Buffer flip / page swap | not owned |
| 13 | -$4588(a5) | `$57A88A` | `move.w $F88(a5),d0; beq $57A932; movea.l $67E(a5),a1; lea $45864,a0` | Conditional buffer setup (gated by `$F88`) | not owned |
| 14 | -$12FC(a5) | `$57DB16` | `move.w #$8400,$96(a6); btst #6,$2(a6); lea $5A3B6C,a0` | Blitter pass over $5A3B6C list | not owned |
| 15 | -$3D96(a5) | `$57B07C` | `btst #6,$10AC(a5); beq $57B0AE; lea $5A452A,a0; moveq #$F,d0; and.w $FA2(a5),d0; lsl #6,d0` | `$10AC` flag-gated 64-byte table lookup | not owned |
| 16 | -$1A6E(a5) | `$57D3A4` | `subi.l #$20,$78E0(a5); lea $12E6(a5),a0; lea $10E6(a5),a2; movea.l $1036(a5),a3` | Counter dec + object update (mirror of `$57D79A`?) | not owned |
| 17a | -$47AC(a5) | `$57A666` | `lea $23E2(a5),a0; lea $253E(a5),a1; lea $F8A(a5),a2; lea $269A(a5),a3; lea $10A6(a5),a4` | Big multi-table routine (player input + position?) | not owned |
| 17b | -$18A6(a5) | `$57D56C` | `move.w #$8400,$96(a6); BBUSY wait; lea $5A371C,a0` | Blitter pass over $5A371C list | not owned |
| 17c | -$178A(a5) | `$57D688` | `move.w #$8400,$96(a6); BBUSY wait; movea.l $57D684(pc),a0` | Blitter pass via PC-relative pointer | not owned |
| | | | `*** Subsystems 17a/b/c are run in different orders depending on `btst #5,$10AD(a5)` ***` | | |
| 18 | -$1164(a5) | `$57DCAE` | `lea $2006(a5),a0; lea $5ABB60,a1; lea $1292(a5),a2; move.w #$FFFF,(a3); move.l (a2)+,d0` | Per-frame table build (some pipeline?) | not owned |

After the main loop, decrement timer `$108E(a5)`, then a state-machine branch:

| Test | If true → | Notes |
|------|----------|-------|
| `btst #3,$10AC(a5)` | jump `$579152` | a death/explosion path? |
| `btst #4,$1093(a5)` | jump `$578D0E` | another state path |
| `tst.w $10AC(a5); bmi` | jump `$5772FA` | "exit early" (a5-$61D4) |
| `cmpi.w #$1B, $F82(a5); beq` | sets `$10AC=$8243; $F70(a5)=$5799E6` | special state transition |
| `btst #5,$10AC(a5); bne` | jump **`$5771FE`** (level-complete) | the win path we saw |
| `btst #0,$1093(a5); bne` | jump `$5793DA` | another transition |
| else | loops back to `$577114` for next frame | normal continuation |

## Level decompressor — `$5782B4` (`-$6B5E(a5)`)

**Critical** — this is where per-level disk reads + per-level data table setup happen.
Not yet RE'd in detail. First instructions (from earlier reading):

```
$5782B4: btst.b #6, $2(a6)         ; BBUSY spin
$5782BA: bne   $5782B4
$5782BC: move.l #$1000000, $40(a6) ; BLTAFWM = all-bits
$5782C4: clr.w  $66(a6)            ; BLT?MOD
$5782C8: move.l #$2B3EC, $54(a6)   ; BLTBPT
$5782D0: move.w #$8032, $58(a6)    ; BLTCPT (or BLTSIZE? - $58 is BLTCPT, $58 of $DFF is BLTCPTH)
...
$5782F6: move.w #$258, $57FEF6.l   ; some state init
$578302: lea $2B3EC.l, a0
$578308: lea $5A4562.l, a1
$57830E: lea $45864.l, a2
$578314: lea $38628.l, a3
$57831C: jsr $59DC02.l              ; ← per-level data dispatcher (Disk.N reader chain)
$578322: move.l $57FEF2.l, d0
$578328: lea $4D064.l, a0
... another dispatcher call
```

That `jsr $59DC02` is the chain that runs the actual per-level `gp-disk-read` calls
(produces the `[gp-disk-read]` log lines). **The per-level data tables — including
the jump table at `$59AC6C` and the level-engine state byte at `$1890.w` — almost
certainly get populated here.** RE this function next when chasing any "per-level
state missing" symptom.

## Object / animation system

Reached from per-frame loop call #10 (`$57D79A`):

```
$57D79A: move.l #$5A27DC, $78E0(a5)
$57D7A2: lea $16A6(a5), a0       ; a0 = ANIMATION CURSOR base (= $5804B8)
$57D7A6: lea $1162(a5), a2       ; a2 = OBJECT POINTER LIST  (= $57FF74)
$57D7AA: lea $5A3B6C, a3         ; a3 = blitter list A
$57D7B0: lea $5A43A0, a4         ; a4 = blitter list B
$57D7B6: lea $5A3F86, a6         ; a6 = animation cursor mirror (NOTE: a6 overwritten!)

$57D7BC: tst.l (a2)              ; next object pointer
$57D7BE: beq.w $57DA28            ; end-of-list → cleanup (restores a6=$DFF000, RTS)
$57D7C2: movea.l (a2)+, a1        ; a1 = current object descriptor
$57D7C4: move.w (a1), d2          ; d2 = first word
$57D7C6: bmi.w $57D81C            ; high bit set → special-handler branch
$57D7CA: adda.w d2, a1            ; a1 += d2  (handler offset within object)
$57D7CC: move.l a0, -(a7)         ; save animation cursor
$57D7CE: tst.b -$1(a1)            ; flag byte before handler
$57D7D2: bpl.b $57D7DC            ; bit7 clear → normal path
$57D7D4: addi.l #$20, $78E0(a5)   ; advance global cursor
$57D7DC: tst.w (a0)+              ; read+advance animation script word
$57D7DE: bne.b $57D7F2             ; non-zero → in-script
$57D7E0: move.l #$ffffffff, (a4)+  ; zero → emit "no blit" sentinel into list B
$57D7E6: move.w #$3, (a4)+
$57D7EA: pop a0; lea $20(a0),a0
$57D7F0: bra.b $57D7BC             ; next object
$57D7F2: moveq #$3F, d2; and.w -$C(a1), d2
$57D7F8: bne.w $57D8B4             ; mask path
$57D816: movem.l a2-a4, -(a7); jmp (a1)   ; ← dispatch into the object's handler
```

The handler at `(a1)` is something like `$59AC38` for "ordinary" objects.

### `$57D8D0` clip + dirty-record persistence (RE'd 2026-06-10, definitive)

The per-object draw choke `$57D8D0` (entered by the handler's `jmp`) does its own
clip AND a per-object **dirty record** in the a4 queue — 6 bytes per object per
frame: a long *signature* (compared 32-bit against live `d5`, the anim gfx offset)
plus a word *counter*:

- `d3=(s16)(d0-cam)` (`cam=$57FDBA`): `<0` → `d6=w+(d3>>4)`; `>0` = **left-clip EMIT**
  (`$57D960`, writes the `$FFFFFFFF` sentinel then emits the clipped blit via a3),
  `<=0` = **CULL** (`$57D8F2`, sentinel only, no blit).
- else `span=((u16)(cam+$160)>>4)-(d0>>4)`: `<=0` CULL; `<w` **right-clip EMIT**
  (`$57D91C`); `>=w` **in-window dirty check** (`$57D9AE`): signature changed →
  store + counter=3 + emit; unchanged → emit (counter 2 then 1 = the two page
  buffers) and at counter==1 → `$57DA1A` writes 1 and emits **nothing** — the page
  persists the last-emitted pixels. This is why a transient garbage `d0` (the lever
  during a Marry-Man pull) never shows in vanilla.
- Stack at `$57D8D0` entry: dispatch `movem.l a2-a4,-(a7)` ⇒ a2@a7+0, a3@+4,
  **a4 (dirty slot)@+8**, and the per-object walker **node a0 @+12** (pushed at
  `$57D7CC`; 32-byte stride) — the only instance-unique key. The gfx descriptor A1
  is SHARED across instances of one object type — never key per-object state by it.

`native_objdraw_capture` (gameplay.c) replicates this decision pre-super-call and
keeps a node-keyed committed map so BenRen mirrors page persistence exactly
(EMIT=commit live, PERSIST=render committed, CULL=live for the widescreen margins).
The map is cleared on level load and `pc_loadstate`. The earlier c70636e attempt
(descriptor-keyed + a3/a6 stack-peek emit probe) was reverted: instance collisions
lost/duplicated objects and animated objects were misdetected as skipped.

`$59AC38` itself: object frame-advance handler. Reads 3 words from animation cursor
(`movem.w (a0), d0-d1/d5` = x, y, frame), advances frame by 2 (wraps at $9A),
writes frame back to `$4(a0)`, then a level-identity check (the `$1890.w` cmp) that
splits to either a jump-table dispatch at `$59AC64` or `adda.w #$645,a7; jmp (a0)`
at `$59AC5E` (the path whose status under real play is unknown — see
[[w6l2-crash-status-unknown]]).

### `$57D81C` multi-tile / animated PAGE PATCH path (RE'd 2026-06-10, owned)

The walker's `bmi` branch at `$57D7C4/C6` (object record's first word negative) goes to
`$57D81C` — a CPU-write path that draws a **16px-wide, 2-row, 5-plane patch** straight
into the page with ten `move.w (a2)+,(a3)` (NO blit, no descriptor): write order
p0r0,p0r1,p1r0,p1r1,… advancing a3 by `$2E` (next row) / `$29DE` (next plane −row).
This is the **animated water surface line** (and any other animated background strip).
Per record (cursor a0, stride `$20`): `+0` dest page offset, `+2` index into `$5A1D18`
(scroll-phase column offset, added via `adda.w (a4),a3`), `+4` a1-advance (per-record gfx
select), `+6` worldX (cull only: `worldX − $57FDBA > $150` → skip + reset counter 3).
Source: `a2 = *(a1+8).l`, `src = a2 + *((a1+16) + rec4 + (−$273e(a5) anim phase)).w`.
Dest base: normally the current page `$67e(a5)`; **every 4th tick per record** (counter
word in the a4 `$5A43A0` queue) it instead writes the clean background buffer `$45864`
so dirty-rect restores keep the patch. Page row = (rec0+tbl)/46 = playfield-relative
worldY (pf_top + row = view y, verified row 188 → y 201).
**Owned:** `native_anim_patch` (gameplay.c, override of `$57D81C`) captures every record
PRE-cull and delegates to the recomp body (vanilla byte-identical, verified);
`native_wswater_compose` (native_renderer.c) draws all patches as opaque 16×2 quads —
this fixed the water line missing in BenRen (in view AND wide margins). REPL `wswater`
dumps the capture; `WS_NOWATER=1` disables the compose.

## Sprite / object DRAW pipeline (RE'd 2026-06-05) — owned by the widescreen capture

The playfield sprites are drawn by THREE distinct producer→queue→executor systems, all
compositing into the double-buffer pages (`$02B3EC` / `$038628`, row stride `$2e`=46,
plane stride `$2A0C`). Knowing which system draws a sprite tells you where to capture it
for the native wide renderer (and avoids the per-level magic-src-range trap).

| system | producer (builder) | queue | executor | what | wide capture |
|--------|--------------------|-------|----------|------|--------------|
| **list-A objects** | object walker `$57D79A`→`$57D7BC`, choke `$57D8D0` | — | `$57DB34`/`$57DB5E` | platforms, pickups, ladders, box ($06xxxx) | `native_objdraw_capture` (`$57D8D0`, clean d0/d1) |
| **characters** | char loop `$57D3C2` (a2=`$10e6(a5)` list), per-type handler `jmp (a1,d0.w)` → builder `$57D3F4` | `$1032(a5)`(=`$5A371C`) + `$1036(a5)` | `$57D5AA` / `$57D6C4` (via `$57D56C`) | player-size walkers, enemies, FREED marry men ($05xxxx) | `native_char_capture` (`$57D3F4`, clean d0/d1/d5/a1) |
| **static-placement objects** | object compositor `$57B0B4` (a0=`$5A4562` records, per-record re-entry `$57B0EE`, common build `$57B19E`) | `$5A39EC` (+`$5A371C`) | `$57D6C4` (via `$57D56C`) | CAGED marry men (only) | `native_wsstatic_compose` (per record: in-view→queue `$5A39EC` exact gfx, paired by worldY; off-view→native `$4a72` resolver) |

- **The PLAYER** is its own path (`$57A666`, not in any list) — `native_player_capture`.
- **`$5A4562` placement record** = stride **64 bytes** (NOT 6 words — the compositor uses the
  later slots as per-object scratch). `+0 type` (0=end), `+2 worldX`, `+4 worldY`, `+$a LIVE
  anim cursor`, `+$c cached draw-handler ptr` (== `$57C13A` ⇒ a Marry Man). On L5 the list is
  records [0..2] = 3 marry men; [3..4] hold invalid handler ptrs (trailing data, not drawn).
  This list is Marry-Men-only; other gameplay sprites come through the other two systems.
- **`$57D6C4`/`$57D5AA` executor descriptor** (24 bytes, a6=`$dff000` so a5=BLTSIZE): `+0`
  data(B,5-plane), `+4` mask(A,1-plane, cookie-cut), `+8` con0/con1 (ASH=con0>>12), `+C`
  BLTAMOD/BLTDMOD (==BLTBMOD here), `+10` dst, `+14` BLTALWM.w, `+16` BLTSIZE.w (==0 ends).
  Displayed width = `rs/2` words (`rs=w*2+BMOD`; BLTALWM=0 kills the spillover); data plane
  stride = `h*rs` (B-channel auto-advance). Decode = cookie-cut: mask bit gates, 5 data
  planes → colour index, colour 0 transparent.
- **Why the wide renderer must capture all three:** `native_render_wide_bg` rebuilds the
  playfield from the tilemap and IGNORES the engine page, so anything only drawn into the
  page (i.e. every sprite) is lost unless separately captured. dst→worldX is ambiguous mod
  the 368px circular page — capture clean world coords at the BUILDER, never reverse-project
  the page (the deleted `native_wsmissedchar_compose`/`s_pg` did and produced wrap phantoms).
- **Double-buffer skew:** the queue is built into the BACK buffer, so a descriptor's dst is
  usually in the OTHER page than the displayed `bp0`. Project relative to the descriptor's
  own page base + the displayed coarse-scroll offset (see `native_wsstatic_compose`).
- **A 4th draw system — LINE-mode chains** (chandelier ropes): routine `$57DD42` draws a
  per-frame segment list at `$5ABB5E` (`{x0,y0,x1,y1}` world coords) via the OCS blitter LINE
  mode, into the page. Page-only ⇒ was invisible in the wide view; PORTED — ropes are
  captured pre-cull at the shared clip/emit entry `$57DCD4` (`native_wsrope_*`) and drawn
  by `native_wsrope_compose`.

### The static-object compositor `$57B0B4` — internals (RE'd 2026-06-05)

A per-frame routine that walks the `$5A4562` placement list and, for each object, runs its
per-type animation + a terrain-collision update, then resolves the blit descriptor and
appends it to the draw queues. This is the engine's own object renderer for the
static-placement class (currently just the caged Marry Men). Verified by disasm of
`logs/savestate.bin` + numeric replay against L1 record [0].

- **Entry `$57B07C`** (setup): `a0=$5A4562` (records), `a2=$5A39EC` (queue B out), `a4=$5A1D18`
  (row→page-offset table), `$1032(a5)=$5A371C` (queue A out, reset), `a6=$5A371C` initial,
  reads `$57FEB4`. Then per-record loop re-entry **`$57B0EE`** (`movem.w (a0)+,d0-d5` at
  `$57B0FC`): `d0=type, d1=worldX, d2=worldY, d3..d5=anim/aux`.
- **Per-type dispatch:** `d0&$f` + flags (`$1094/$1095(a5)`) select the draw handler, cached
  into the record at `+$c` (`move.l #$57c16e/$57c13a,(a0)`); `jmp (a1)` at `$57B19C` runs it.
  Handlers: `$57C13A` (Marry Man; anim table `$5d5a(a5)`), `$57C16E`/`$57C194`/`$57C1B8`
  (anim tables `$5e1c`/`$5a72`/`$5ae4(a5)`), `$57C364`, `$57C5CA`. Each: `frame =
  MR16(animtab(a5)+cursor)`, advance `cursor`(d5) by 2 (wrap to 0 at a <0 sentinel), then
  `jmp -$3c74(a5)` = `$57B19E` (common build).
- **Anim is DRAW-driven but camera-independent:** the loop processes EVERY record each frame
  and writes `d0-d5` (incl. the advanced cursor) back to the record at `$57B45C`
  (`movem.w d0-d5,(a0)`) — which is BEFORE the draw cull. So the anim cursor ticks even for
  off-screen objects (this is why reading it live gives correct off-screen animation).
- **Common build `$57B19E`→`$57B2B8`:** a dirty-rect / per-pixel terrain-collision pass
  (tables `$5a5d9c`/`$5a8c7e`/`$5a211c`, the `$57b330` loop) — NOT yet fully RE'd; it gates
  redraw + does object↔terrain collision. Then the descriptor build:
- **Cull `$57B4DC`:** `worldX ∈ [$fa8(a5), $fa8(a5)+$160]` (= `[cam, cam+352]`); outside → skip
  (`$57B5DC`). THIS is the engine's per-object draw cull (the wide renderer ignores it and
  resolves from records instead — the 368px page can't hold a wide view anyway).
- **Gfx resolution `$57B4F6`:** `entry = $4a72(a5) + (frame [+$55 if !d4.bit1]) * 8` → 4 words
  `{data_off, mask_off, yoff, BLTSIZE}`; `data = data_off+$EEFA`, `mask = mask_off+$12E7E`,
  `ASH=(worldX-8)&15`, BMOD=-2. (The `$4a72(a5)` table is non-uniform — always look up, never
  formula.) dst = `$5A1D18[clamp(worldY,$D7)+yoff] + (worldX-8)/8 + pageBase`.
- **Queue write `$57B546`:** 6 longs to `a2`(=$5A39EC): data, mask, con0/con1, `$FFFE002A`
  (mod), dst, `$0000`/BLTSIZE; plus `dst, $2A, BLTSIZE` to `a3`(=$1032(a5) region). Played by
  the executor `$57D56C` → `$57D5AA` (queue `$5A371C`) then `$57D6C4` (queue `$5A39EC`).
- **NATIVE OWNERSHIP:** `native_wsstatic_compose` (native_renderer.c) draws each Marry Man once
  — in view from the engine's exact `$5A39EC` descriptor (correct frame + variant), off view by
  natively replaying the RED `$57C13A` resolution (cursor→frame→`$4a72`→data/mask) across the
  full wide view (ignores the `$57B4DC` cull) → live anim + no margin cull, no duplication.
- **BLIND/gray variant — RE'd + VERIFIED 2026-06-09 (was "OPEN/per-level"; that was WRONG):**
  the blind sub-handler `$57BBF8` → build `$57B856` is structurally IDENTICAL to the red
  `$57B4F6` emit — same `$4a72(a5)` descriptor, same index `(frame [+$55 if !d4.bit1])*8`, same
  dst math, same `$FFFE002A` mod. The ONLY difference is the two absolute-offset adds: blind
  `data = data_off+$13B32`, `mask = mask_off+$17AB6` (vs red `+$EEFA` / `+$12E7E`). **Both deltas
  are EXACTLY `$4C38`** — verified: immediates at `game_gpl_0.c:53632/53638` (blind) vs
  `:27974/27980` (red); `$13B32−$EEFA == $17AB6−$12E7E == $4C38`. So **blind gfx = red gfx +
  $4C38, a HARDCODED CONSTANT — not a per-level table.** The earlier "+$4C38 (L9) / +$6AB0 (L11)"
  measurement was a DIFFERENT mechanism: `$57B856` special-cases resolved frame `$3a` (gated by
  `$10aa(a5)>=$2c` + a `$57FEB8`/`$10ad(a5)` parity) to a fixed gfx page `#$585138`
  (`game_gpl_0.c:52981`); that override produced the apparent variation. So off-view blind is
  legit-resolvable as **red-resolution + $4C38** (RE-grounded, not a learned-delta hack). Variant
  selection AT DRAW time = `tst.b d0; bmi $57b856` in the per-frame handler (d0<0 → blind),
  distinct from the record type bit7 (which only picks the initial handler pointer).
- **OFF-VIEW ANIMATION (the "idle-only correct" gap):** the off-view native resolver replays only
  `$57C13A`'s top-level cursor→frame table (`$5d5a(a5)`), but the engine routes the resolved frame
  `$a(a0)*4` through per-pose sub-handlers via `$526a(a5)`(d0≥0)/`$545a(a5)`(d0<0) → `$57C194`/
  `$57C1B8`/… each walking its OWN sub-sequence table (`$5a72`/`$5ae4(a5)`, advance d5 by 2, `<0`→
  reset). To get correct non-idle frames off-view, replay that sub-handler dispatch, not just the
  top table. FACING: blitter-frame facing = `d4` bit1 (clear → `+$55` frame block); resolve-tail
  facing = `d4` bit0.

### The character loop `$57D3C2` + queue executor `$57D56C` (RE'd 2026-06-05)

The CHARACTER system (walkers, enemies, freed Marry Men — gfx `$05xxxx`) is a sibling of the
static-object compositor; both feed the same two draw queues, played by one executor.

- **Char-draw entry `$57D3A4`:** `a0=$12e6(a5)` (anim aux), `a2=$10e6(a5)` (the char-pointer
  list), `a4=$5A1D18` (row→page table), `a3=$1036(a5)` & `a6=$1032(a5)` (the two queue write
  ptrs, shared with `$57B0B4`). Falls into the loop:
- **Loop `$57D3C2`:** `cmpi.l #$58692c,(a2)` → a SKIP sentinel (`$57D53E`: step past it,
  `a6+=$18`, continue); `tst.l (a2)` zero → end (`$57D54C`: store `a3→$1036(a5)`, rts);
  else `movea.l (a2)+,a1` (char struct), `move.w (a1)+,d0` (per-type handler offset),
  `jmp (a1,d0.w)` → the char's type handler, which computes draw values and tail-jumps to the
  builder `$57D3F4` (captured by `native_char_capture`, clean d0=worldX/d1=worldY/d5=anim/a1).
- **Builder `$57D3F4`:** clips to `[cam, cam+$180]`, builds a 6-long descriptor into `(a6)+`
  (queue A = `$5A371C`-based) and a short record into `(a3)+`; loops back to `$57D3C2`.
- **Two-queue executor `$57D56C`** (called once per frame from the main loop, NOT the char
  loop): plays queue A `$5A371C` via `$57D5AA`, then queue B `$5A39EC` via `$57D6C4` — both
  descriptor players that wait-blitter (`btst d4,(a6)`), stream {data,mask,con0,mod,dst} and
  trigger BLTSIZE per 5-plane sprite (24-byte descriptors, see above). So BOTH the char
  builder (`$57D3F4`) and the static-object compositor (`$57B0B4`) append to these queues each
  frame, and `$57D56C` blits them all into the page. (The native wide renderer bypasses the
  queues entirely — it captures/resolves each system at its builder and draws from world
  coords; the queues + executor are the engine's own page path.)

## Low chip-RAM state map (in-progress)

The engine reads low chip-RAM addresses (`$0..$2FFF`) heavily during boot + level
setup. These are mostly stack-pointer save slots, RNG seed, level number, IRQ
vectors, the disk-chunk pointer table, and the `$150..$2A57` block-copy region
(initialised by the `$6D714` block copy — see [[w6l2-crash-root-caused]]).

Observed reads during `--level 60` boot + cavern entry + 500 idle frames
(harvested via `pcread 0-2FFF`):

| Addr | Width | Read by (M68K PC) | Purpose (best read) |
|------|-------|-------------------|----------------------|
| `$00` | l | `$5782B4` | reset vector (read as a long during init?) |
| `$10` | w | `$57CC1A` | unknown system var |
| `$1C` | w | `$577000`, `$57AED0`, `$57CC1A` | engine timer or scroll counter (paired with `$2C`) |
| `$1E` | w | `$577000`, `$5782B4`, `$57CC1A`, `$57DEAC` | engine state (input-related?) |
| `$20` | w | `$577996`, `$5782B4` | **LEVEL NUMBER** (1..60) |
| `$28` | l | `$5770F8` | RNG / save-game seed snapshot |
| `$2C` | w | `$5770F8` | timer or copy of `$1C` |
| `$2E` | w | `$5782B4` | engine state |
| `$30` | w | `$57CC1A` | unknown |
| `$32` | l | `$577996` | unknown long |
| `$36` | w | `$577996` | unknown |
| `$3E` | l | `$577996`, `$578162` | **`$A68` sentinel** (display ptr — set by overlay loader) |
| `$42` | l | `$577000`, `$5770DC` | save-game flag (controls $5770D0 branch) |
| `$68` | l | `$577000` | IRQ vector ($68 = LVL2) |
| `$6C` | l | `$577000` | IRQ vector ($6C = LVL3 — vertical blank) |
| `$78` | l | `$577000`, `$59BF3E` | IRQ vector ($78 = LVL6 — CIA-B timer / music) |
| `$100` | l | `$57CC1A` | disk-chunk pointer (raw e1 dest = `$50000`) |
| `$104` | l | `$5782B4` | disk-chunk pointer (decrunched e2 dest = `$3330`) |
| `$108` | l | `$5782B4` | disk-chunk pointer (gameplay code dest = `$577000`) |
| `$10C` | l | `$5782B4` | disk-chunk pointer +4 (post-end?) |
| `$110` | l | `$57CC1A` | unknown chunk-table tail |
| `$114` | l | `$57CBBA` | unknown chunk-table tail |

Notable absences in this snapshot: **NO reads to `$150..$2A57`** during the
basic cavern-entry play. That region's reads only fire when specific
gameplay state engages (e.g. `$59AC4A: move.w $1890.w, d2` only runs when an
object of type `$59AC26` is being processed by the animation walker). Don't
assume the region is dormant — it just isn't exercised by idle cavern frames.

Method: this map was built by `pcread 0-2FFF` (REPL command) + grepping the
log. Re-running with different gameplay activities will surface more reads
and progressively flesh out which low-RAM word holds what state.

## A5-relative state map (in-progress)

Each `$X(a5)` is an absolute address `$57EE12+X` (or `−X` for negative
displacements). Tracking only the ones we've seen referenced so far.

| Offset | Abs addr | Type | Use |
|--------|----------|------|-----|
| `+$0248` | `$57F05A` | ptr | extra-life count area? (`lea $248(a5),a4` in `$5772DE`) |
| `+$067A` | `$57F48C` | longs | double-buffer ptr A |
| `+$067E` | `$57F490` | longs | double-buffer ptr B |
| `+$0682` | `$57F494` | long | active drawing target |
| `+$0F70` | `$57FD82` | long ptr | current top-level handler (the `$5799E6` default) |
| `+$0F6A` | `$57FD7C` | word | player something (`cmp.w $f6a, $2(a2)` in `$57A934`) |
| `+$0F82` | `$57FD94` | word | "game state index" (cmpi #$8 / #$1B branches) |
| `+$0F88` | `$57FD9A` | word | gated subsystem flag (`$57A88A`) |
| `+$0F90` | `$57FDA2` | long | per-frame backup target |
| `+$0F98` | `$57FDAA` | word | clear-on-frame counter |
| `+$0FA2` | `$57FDB4` | word | `and.w $FA2,d0` — selector |
| `+$0FA4` | `$57FDB6` | word | copied to `$FA6` then cleared (input edge?) |
| `+$0FA6` | `$57FDB8` | word | (paired with `$FA4`) |
| `+$0FB0` | `$57FDC2` | ? | `lea $FB0(a5),a2` in `$57C6DA` |
| `+$1028` | `$57FE3A` | long | compared against `$5AC4E6` |
| `+$10A6` | `$57FEB8` | array | player position+state block (`movem.w (a4),d1-d4` reads 4 words) |
| `+$10AC` | `$57FEBE` | bits | **MAIN STATE-FLAG WORD** (bits 1/3/4/5/6/7/15 all tested) |
| `+$10AD` | `$57FEBF` | bits | low byte of above; bit5 swaps subsystem 17 order |
| `+$0FA8` | `$57FDBA` | word | **CAMERA X (screen-left world coordinate)** — confirmed via differential memory scan (`scratch/camhunt.py`): tracks player world X (`$10A6`) 1:1 in mid-level but offset by the player's screen X (held constant ≈175 while scrolling). This is the engine's scroll position, already clamped to the level's real edges (a normal camera "stops" here). For native widescreen: read this as the camera, clamp the wide view to its min/max (the level edges). camera_tile = `$0FA8`>>4, fine = `$0FA8`&15. |
| `+$10C0` | `$57FED2` | long | backup of `$F90` |
| `+$10D4` | `$57FEE6` | long | snapshot of `$28.w` at level start |
| `+$10D8` | `$57FEEA` | long | (`$57C686`) scroll/position acc |
| `+$10DC` | `$57FEEE` | long | (`$57C686`) ptr fed to acc |
| `+$1092` | `$57FEA4` | word | set to `$20` at `$5770F8` |
| `+$1093` | `$57FEA5` | bits | bit7 = "no-input-fire" flag, bit0/bit5 = branches |
| `+$1162` | `$57FF74` | array | **OBJECT POINTER LIST** (null-terminated longs) |
| `+$16A6` | `$5804B8` | array | **ANIMATION CURSOR base** |
| `+$78E0` | `$5866F2` | long | global animation cursor counter |

`$5A3F86`, `$5A3B6C`, `$5A43A0`, `$5A1D18`, `$5A27DC` are gameplay-bank tables
(not a5-relative); accessed as absolute longs.

`$1890.w` — low chip RAM, read by `$59AC38`, **never written by any literal-abs
instruction in the binary** (verified via static scan). Writer must be
register-indirect; identifying it is currently an open question.

## Overlay-loader (`$6D714` / `$150`) — chunk descriptor table

The loader body relocated to `$150..$2A57` (via `$6D714`'s block copy) contains a
chunk descriptor table at relocated `$8D6` (source `$6DEBA`). `D0` selects which
group to load. Decoded from `gmem_after_load.bin`:

| `D0` | Caller | Loads | Then |
|------|--------|-------|------|
| 0 | title-fire `$003B08` | Disk.1: reloc-table `$0548A0→$6E000`, gameplay `$0565BA→$3330`, level engine `$0689BE→$577000` | `jmp $108` → `$577000` (gameplay entry). Implemented natively. |
| 1 | boot `$003144 → $6D714` | Disk.1 raw `$F3780→$50000` + ATN `$026270→$3330` | `jmp $104` → `$3330`. Boot-phase loader. Implemented natively (`native_overlay_load`). |
| 2 | unknown | Disk.1 raw `$F3780→$70000` + ATN `$0DE080→$3330` | `jmp $104` → `$3330`. Path not yet exercised. |
| **3** | **`$5773A2` (win-tail)** | **Disk.3 `$0C7100→$3330` (ATN, len `$1888C`)** | **`jmp $100` → `$3330`. Loads end-game/credits engine. Implemented natively in `native_overlay_loader_reloc`.** |

Loader-internal entry format (per entry, 16 bytes / 4 longs):
- `+0`: header marker (non-null = real entry; null = group terminator)
- `+4`: `(disk_off<<8) | (disk_idx_zero_based)` source
- `+8`: chunk length
- `+12`: destination (high-bit-set forms add `mem[$10.w]` as base)

After a group's null-terminator long, the next long is a CALLBACK POINTER. The
loader does `movea.l next, a0; movea.l (a0), a0; jmp (a0)` — i.e. it dereferences
the pointer (e.g. `$100`, the dest-pointer stack) and jumps to whatever address
got pushed there.

## Credits / end-game engine (`$3330` post-d0=3) — owned (basic)

**Recompiled as a third bank.** `tools/recomp/recomp.py --bank credits --base 3330
--code-size 1888C --areg 5=355C --seed "$(cat tools/recomp/credits_seeds.txt)"`
produces `game_credits_*.c` + `game_credits.h` + `game_credits_table.c` with
14 functions (entry `$3330`, LVL3 ISR `$350A`, LVL6 ISR `$351C`, plus 11 reached
by static descent). Wired into the build via `GP_GAME_SRCS` glob.

Bank selection: `g_credits_active` (in `g_state`) is flipped on by
`native_overlay_loader_reloc d0=3` together with clearing `g_gameplay_active`
and `g_overlay_active`. `rt.c`'s `dispatch_lookup` prefers
`g_fn_table_credits` when this flag is set; the IRQ-delivery in `pc.c`
(`coro_deliver_timer_irq` + `pc_music_tick`) treats `g_credits_active` like
the gameplay/overlay path so the credits-engine's own LVL3/LVL6 vectors fire
each frame.

Seeds live in `tools/recomp/credits_seeds.txt` — extend if rt-misses surface
during the credits run.

Status: with the user's "marry-man-with-gear" savestate, the credits engine
runs clean to natural exit (0 rt-misses). Exits by calling `$150 d0=2` which
is still unhandled (would load the return-to-title overlay).

## Credits / end-game engine (`$3330` post-d0=3) — original notes (kept for reference)

After `$150 d0=3` runs, the bytes at `$3330` are a **separate ~98KB credits/
cutscene engine**, not the gameplay engine. First instructions:

```
$003330  lea $355c(pc), a5       ; credits-engine base register
$003334  lea $DFF002.l, a6
$00333A  lea $80000.l, a7
$003340  btst #6, (a6) ; bne     ; blitter wait
$003346  btst #0, $3(a6) ; ...   ; vblank parity wait
$003356  move.l #$7FFF7FFF, d0    ; clear INTREQ + INTENA + DMACON
$00336E  move.l #$350a, $6c.w     ; install own LVL3 IRQ
$003376  move.l #$351c, $78.w     ; install own LVL6 IRQ
$00338C  jsr (a5)                  ; main entry into credits engine
```

It's a complete mini-game (own IRQ vectors, own state base, own copper). The
recompiler hasn't seen these bytes — they only exist in chip RAM after the d0=3
load. Reaching the credits / "you beat the game" cutscene **requires** one of:

1. **Separate recompiler bank**: capture chip dump after d0=3 load (`PC_DUMP_CREDITS=1` style hook), run recompiler with `--bank credits --base 3330 --code-size 1888C`, integrate the resulting `game_credits_*.c`, route rt dispatch to that bank when `g_overlay_active` is the credits flavour. Same shape as the existing gpl bank.
2. **Native port** of the credits sequence (text scroll + animation playback).
3. **Stub-out**: replace `$5773A2`'s `jmp $150` with a native "show end card / return to title" override. Loses original credits visuals.

This is the gating issue for the W6L2 win-sequence crash the user observed:
`$5773A2` is now correctly handled (via `native_overlay_loader_reloc d0=3` path
that loads the chunk + jumps `$3330`), but `$3330` post-load has no recompiled
function so dispatch rt-misses.

## Subsystems we DO own (overrides)

These hook the gameplay bank via `pc_register_overrides()`:

| Addr | Native fn | Notes |
|------|-----------|-------|
| `$577B8C` | `native_gp_disk_read` | The engine's "stream-from-disk" routine (raw MFM on hardware, file-read on PC). Critical — every per-level data load goes through this. |
| `$003160` / `$0055A0` | timer ISR | LVL6 music tick (not gameplay-bank but interacts) |
| `$0058C2` | audio-shadow copy | LVL6 second leg |

That's it. Everything else in `$577000+` is recompiled.

## Highest-leverage next ownership targets

1. **`$5782B4` level decompressor** — owns per-level data loading; once native, we control
   every byte of per-level state and can directly fill `$1890.w`, the `$59AC6C` jump
   table, etc. instead of relying on opaque decompressed disk data.
2. **`$57D79A` outer object-list walker** — small loop, well-bounded. Once native, every
   object-handler dispatch goes through C we wrote, so weird `jmp(a0)` cases like
   `$59AC5E` become explicit C code we can reason about.
3. **`$577000` engine prologue** — one-shot, foundational, easy to verify (compare chip RAM
   + register state after). Replaces the implicit "engine boots itself" magic with code we
   wrote.

(1) gives the biggest ownership win because it eliminates the "what does the engine think
the level is?" black box. (3) is the safest first step because it's bounded and one-shot.

## Player movement & JUMP state machine (RE'd 2026-06-02)

a5 (gameplay) = **`$57EE12`** (`$5770B0: lea $57ee12(pc),a5`). All offsets below are
a5-relative; absolute = `$57EE12 + off`.

### Input sampling — `$57DEAC`
Reads JOY1DAT (`$c(a6)`) + fire (`$bfe001`), decodes to a bitmask, **doubles it**, and
stores: `$f7e(a5)` = previous frame's input, `$f80(a5)` = current. Bit values (post-double):
`$02`=down, `$04`=right, `$08`=up, `$10`=left, `$20`=fire (so long-jump = fire+dir =
`$24` right / `$30` left; hop = up = `$08`). Gated by `$1093(a5)` bit0 (input-disable).
When `$1e.w==8` it instead plays/records a DEMO stream (RLE at `$22.w`/count `$26.w`).
(`$57DEAC` is invoked from the once-per-frame `$57DEB4`-owning path; the `$578E62`
joystick-decode in `$578C3E`/`$578D0E` is a *different*, cutscene "press fire to skip".)

### Player object state (a5-relative)
- `$f70(a5)` (a5+3952) = **current action-handler pointer** (the state-machine state).
- `$f82(a5)` = a sub-state/anim index (compared to `$14`, `$8` …).
- `$fa2(a5)` = free-running frame counter (`& $f` → 16-phase animation).
- `$10ac(a5)` = player flags word; `d4` mirror, bits: 0, 1, 6, `$e` (tested in `$579D84`).
- Position lives in a per-object struct (X increases moving right). `$57A666` (map row
  17a) reads an object struct at `$10a6(a5)` and computes screen pos `$f94/$f96(a5)`
  (adds scroll `$f98(a5)`); but `$10a6(a5)` is NOT uniquely the player (it's the
  generic draw slot) — do not treat it as the player coord.

### State machine (values written to `$f70(a5)`)
Per frame the engine calls the handler at `$f70(a5)`. Known states:
`$579D84` hop/jump-arc · `$579DDC` (sibling arc, likely long-jump) · `$579F0E` ·
`$579F3A` fall/land · `$579F86` · `$57A018` · `$57A2A2` · `$57A2D6` · `$57A30C` ·
`$57E43C`/`$57E4E6`/`$57E4EE` grounded (input dispatch).

### Hop arc — `$579D84`
Plays a precomputed jump trajectory from tables indexed by phase counter `d5`
(`d5+=2`/frame): vertical deltas `$309c(a5)`, horizontal deltas `$2ce6(a5)`, plus
`$5d02(a5)` (gated by `d4` bit `$e`). At `d5==$18` (24): `jsr $775c(a5)` (=`$58656E`,
landing/snap helper), `bset #6,d4`, `$f6e(a5)=$e`. When the vertical delta hits 0 it
sets `(a0)=$579f3a` (transition to fall/land).

### Jump TRIGGER — `$57E43C` (writes `$f70(a5)=$579D84` at `$57E526`)
Dispatched when up/jump input selects this grounded handler. It does a TILE/terrain
check (indexes tile tables `$46cc(a5)`, tests tile flag bit4, and `$f82(a5)!=$14`)
before committing the hop — i.e. "can I jump here?". The three writers of `$579D84`
into `$f70(a5)` are `$57E43C`(`$57E526`), `$57E4E6`, `$57E4EE`.

### Jump SFX (grunt) — STILL TO PIN
The grunt is attached to the jump (per user: up=hop, fire+dir=long jump, both grunt).
Audio output is the combined player via CIA-B ISR `$59BF3E` (see current-state.md); the
trigger writes a voice + ready-flag upstream. NOT yet located in the jump path; pin by
pcwatching the audio voice/ready-flags (`$59cf1c/1d`, mask `$59d240`) on a single hop
frame and noting the non-ISR writer fn.

### How this was found (repro for next session)
Harness REPL: `goto 1` → dismiss card (fire cycles) → `pc 300` (physics live) →
differential dumps (`dumpall`) of "no input" vs "hold up" / "hold right", diff the
`$57E000..$580800` window. Up-press sets `$f70(a5)=$579D84`. `pcwatch <abs>` gives the
writer fn (g_rt_last_call) + M68K pc per write.

## SFX engine (RE'd 2026-06-02) — isolated from music!

Gameplay SFX has its own state, SEPARATE from the music decode (resolves the
wrong-SFX "can't separate from music" dead end in current-state.md):
- `$57fe4e.b` = SFX pending flag (`$FF` = a sound is active, `$00` = idle).
- `$57fe50` = active SFX **descriptor** (~20 bytes copied from the request): +0 sample
  ptr, +4 period/word, +len/priority fields. `$57fe5c.w` = remaining segment count.
- **`$58656E`** (`$775c(a5)`) = **SFX TRIGGER / request**. Caller sets `a3` = pointer to
  a sound descriptor; it priority-arbitrates against the active sound (`$57fe50`, via
  fields `$6`/`$10`), and if it wins copies the descriptor, sets `$57fe4e=$FF`, writes
  DMACON `$dff096`. 67 call sites game-wide (mostly `lea <desc>,a3; jsr $775c(a5)`).
- **`$586612`** (`$7800(a5)`) = per-frame SFX **streamer**: advances the active sample
  ptr through its segments (`$57fe50 += $6C`/frame) decrementing `$57fe5c` 5→0, then
  clears `$57fe4e`. (A multi-chunk sample played as a stream.)
- **`$586790`** = per-frame SFX housekeeping (clears `$57fe4f`).

JUMP grunt: captured sample base `$5B0BE4` (descriptor period `$0280`, len `$0032`).
The jump landing in `$579D84` (`jsr $775c(a5)` at `d5==$18`, `a3` from `-$273c(a5)`)
is one SFX call; the takeoff grunt fires from the grounded/long-jump path similarly.

**This enables clean PC-vs-PUAE SFX comparison** with NO music confound: diff `$57fe50`
(active descriptor) on the SAME jump on both cores. Dynamic capture: `pcwatch
57FE4E-57FE63` then inject a jump; the writer fn = `$58656E` (trigger) / `$586612`
(streamer). The sample ptr in `$57fe50` is the exact grunt being played.

## Object PICKUP mechanic — RE'd + native-widened (2026-06-03)

Each collectible's per-frame handler collects on: `$f80(a5)==$20` (FIRE alone) AND
player pos within a tiny ONE-SIDED window of the item. **AXES (corrected 2026-06-10,
verified live on the hanging-under-a-gear savestate): `$f94(a5)` = player Y,
`$f96(a5)` = player X, `(a0)+0` = obj X, `(a0)+2` = obj Y** — the earlier note here
(and the first pickup.c) had both pairs swapped, which made the "horizontal"
interact_extend actually extend VERTICALLY (grabbing a gear up through a platform
while hanging under it). Window: `0 <= playerY-objY <= rangeY`, `0 <= playerX-objX
<= rangeX` (unsigned). rangeX≈$6–$f → "must be on top of it." On win: busy flag
($1098/$1094/$108e), `clr -$2(a0)`/`clr (a0)`, `$6c24(a5)` pickup sound via `$775c(a5)`,
jmp tail `-$1a1e(a5)`. 19 handlers (lea $6c24(a5)): 586B1C 586B2A 586C10 586C1E 586D14
586E6C 586ECC 586F9C 587006 5870CE 5871F4 587272 58733A 58743C 5874FE 587554 587616
58766E 5876C4. **$586C10 = universal item** (every level; `PICKUP_SCAN=1` to map per level).

NATIVE port (`src/port/overrides/pickup.c`, reworked 2026-06-10): each handler gates X
on the identical pattern `move.w $f96(a5),d4; sub.w d0,d4; cmpi.w #range,d4; bhi fail`
with d0 = objX+bias → pass iff playerX ∈ [objX+bias, objX+bias+range]. Per-handler
(bias, range) constants are RE-extracted into the `s_xwin` table in pickup.c (objX is
`(a0)` for all but $595FF4 = caller's d0 and $59B0B0 = `$84(a0)`). Biases must be
PATH-verified, not linear-summed (addq's from mutually exclusive paths double-count);
$58BCF2 adds a dynamic `(a3)` to d0 so it has no table entry (never nudged). The lever
family's DISPATCH ENTRY is **$595FE4** ($595FF4 is its interior label — wrapping only
that never fired). ~130 more handlers share this gate pattern but most are
hazards/enemies (no $f80 fire gate) — do NOT blanket-wrap them. `interact_extend`
semantics (we OWN the zone; the handler window is just the delivery mechanism): the
player interacts when within `extend` px of the object's 16px TILE [objX, objX+16];
in zone, $f96 is presented clamped into the handler's own window so its check passes.
(Earlier designs failed: nudging to objX overshoots windows with bias>0 — zone
SHIFTED right by extend; nudging to the window edge made reach uneven because vanilla
windows are arbitrary offset slices of the tile, e.g. the lever's [objX+4,objX+12].)
Y ($f94) is never touched (horizontal-only by design). Logging: REPL `pklog`. Revert:
`BENEFACTOR_RECOMP_PICKUP=1`.

### Held-item USE / THROW / DROP system (for the X+Down drop port — task #9)

Carrying state: `$1094(a5)` = carried item id (0 = empty), `$109c(a5)` = carried item
ptr, `$f84(a5)` = held item TYPE (index into the action table). When you pick up a
carryable item the item object is deactivated (`clr.w (a0)`) and these are set.

The held item's behaviour comes from the **action table at `$5834de`** (chip RAM,
indexed by item type → a per-item action descriptor; `$4(descr)` is an allowed-input
mask). `$579A00` is the held-item action DISPATCHER: reads `$f84`, indexes `$5834de`,
`d0 = 3 & d4 & $4(a2)` (direction bits ∧ item mask) → if non-zero switches the player
action to the throw/place state (`$579eb0`/`$57c522`…). Direction selects the action:
**DOWN = place-at-feet (drop); LEFT/RIGHT = throw.** `d4` = decoded input flags (built
in `$57DF78`/`$57DEAC`); the **whole held-item-use flow is entered by FIRE** (the
player fire-action), exactly like the old fire-pickup — which is the conflict with the
long-jump.

DROP execution = `$57EB20` ("place carried item at tile"): no input check of its own;
it's *called only when fire+down+carrying* selected it (confirmed at runtime via the
`BENEFACTOR_DBG_DROP` probe — `native_place_probe`, registered on `$57EB20`: fires with
`$f80=$0022` (fire+down), `d4` bit1=down, `$1094!=0`; then clears `$1094`/`$109c` at
`$57EBA2`). It's reached by `rt_jump` (so `rt_get_last_insn`=0), i.e. via the action
state, NOT a literal `$f70` write or a direct `rt_call`.

**Port plan (task #9):** move the held-item-use ENTRY from FIRE to the interact key
(same decoupling already done for pickup/levers), so interact+down drops / interact+
dir throws and FIRE stays purely jump. That means re-gating the fire-entry into the
throw/place flow on `hw_get_interact()` — NOT post-processing `$f80` globally. The
`$57EB20` probe override is the confirmed hook/anchor for this work.

#### Verified input behaviors (runtime, level 3) + why the input-level re-gate failed

Measured via the HTTP harness (`/input`, `/mem`, `BENEFACTOR_DBG_DROP` probe):
- **Jump = UP** alone (`$f70` → `$579D84`). Fire is NOT used for the normal jump.
- **Long-jump = Fire + Left/Right**, and it still works *while carrying* (player X moves,
  carry kept) — i.e. carrying does NOT turn Fire+dir into a throw. So **throw is unused**;
  the only carried-item action in normal play is the **drop = Fire + Down**.
- DROP fires while carrying (`$1094!=0`) with `$f80=$0022` (Fire `$20` + Down `$2`).

**Dead end (do not repeat):** re-gating drop onto Interact+Down by patching the input
around `$57DEAC` does NOT work. The drop *selection* reads the engine's **decoded input
word `$10ac(a5)` (loaded into d4 at `$5796B0`)**, not `$f80` directly, so: stripping
`$f80` fire after `$57DEAC` is too late, and presenting fire via `$bfe001` before
`$57DEAC` didn't reach the decoded path either. Results were also unreliable because the
test FAKED the carry state by poking `$1094` — the engine manages it, so it isn't stable.
**Next attempt must:** (a) handle the `$10ac`/d4 decoded-input path (or find the actual
fire-entry into the held-item flow), and (b) verify with a REAL carried item (pick one up
in-game), not a poked `$1094`.
