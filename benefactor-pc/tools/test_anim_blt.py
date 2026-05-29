#!/usr/bin/env python3
"""test_anim_blt.py — Narrow the root cause of the $7BC8 clobber.

Tests:
  T1: No ANIM_BLT write lands in $7BC8..$7BD6 (the copper list header).
      FAILS if the $0BFA blit clobbers the copper list.  Prints first offender.

  T2: Reconstruct actual row_start for y=39 from (da, x). Back-calc dpt_initial.

  T3: Compute what initial dpt PUAE must have used for y=39 to miss $7BC8.
      Prints the required minimum dpt_initial.

  T4: Verify apt=$0006A8 source content from the chip RAM dump.

  T5: Scan PUAE chip RAM at $7B80..$7BE0 to find where PUAE's y=39 row landed.
      If PUAE wrote $CCCC-masked values there, show the range.
      If PUAE skipped that region, the blit had fewer rows.

  T6: Compute PUAE dpt_initial from T5 evidence and compare to PC's dpt_initial.

Run AFTER `bash run_harness_headless.sh 2>&1 | tee logs/harness_run.txt`.
"""

import re, sys, struct

HARNESS_LOG  = "logs/harness_run.txt"
CHIPRAM_DUMP = "logs/harness_puae_chipram.bin"
COPPER_START = 0x7BC8
COPPER_END   = 0x7BD7   # inclusive last byte of 10-word sprite pointer section
APT_ADDR     = 0x0006A8
APT_WORDS    = 20
ROW_WIDTH    = 20       # words per blit row
DMOD         = -12544   # bltdmod for the $0BFA animation blit
ROW_STRIDE   = ROW_WIDTH * 2 + DMOD   # -12504 bytes between rows

# ── Parse ANIM_BLT lines ─────────────────────────────────────────────────────
# Format: [ANIM_BLT] y=%d x=%d write $%06X = $%04X  bltsize=$%04X bltcon0=$%04X
#         bltcon1=$%04X dmod=%d apt=$%06X cpt=$%06X dpt0=$%06X
ANIM_RE = re.compile(
    r"\[ANIM_BLT\] y=(\d+) x=(\d+) write \$([\dA-Fa-f]+) = \$([\dA-Fa-f]+)"
    r".*?bltsize=\$([\dA-Fa-f]+).*?bltcon0=\$([\dA-Fa-f]+)"
    r".*?dmod=(-?\d+).*?apt=\$([\dA-Fa-f]+)"
    r".*?dpt0=\$([\dA-Fa-f]+)"
)

def load_anim_blts(path):
    rows = []
    try:
        with open(path) as f:
            for line in f:
                m = ANIM_RE.search(line)
                if m:
                    rows.append({
                        "y":       int(m.group(1)),
                        "x":       int(m.group(2)),
                        "addr":    int(m.group(3), 16),
                        "val":     int(m.group(4), 16),
                        "bltsize": int(m.group(5), 16),
                        "bltcon0": int(m.group(6), 16),
                        "dmod":    int(m.group(7)),
                        "apt":     int(m.group(8), 16),
                        "dpt0_raw":int(m.group(9), 16),  # = dpt - x*2 (logged wrong name)
                    })
    except FileNotFoundError:
        print(f"ERROR: {path} not found. Run the harness first.")
        sys.exit(1)
    return rows

def run_tests():
    rows = load_anim_blts(HARNESS_LOG)
    # Focus on the $0BFA blit (apt=$0006A8, dmod=-12544)
    blt_rows = [r for r in rows if r["bltcon0"] == 0x0BFA and r["apt"] == APT_ADDR]

    print(f"=== ANIM_BLT $0BFA (apt=${APT_ADDR:06X}) entries: {len(blt_rows)} ===")
    if blt_rows:
        bs = blt_rows[0]["bltsize"]
        h = (bs >> 6) if (bs >> 6) != 0 else 1024
        w = bs & 0x3F
        print(f"    BLTSIZE=${bs:04X}: height={h}, width={w}, stride={ROW_STRIDE}")
    print()

    # ── T1: no write to $7BC8..$7BD7 ────────────────────────────────────────
    clobbers = [r for r in blt_rows
                if COPPER_START <= r["addr"] <= COPPER_END + 1]
    if clobbers:
        print(f"T1 FAIL: {len(clobbers)} write(s) land in $7BC8..$7BD7 (copper header)")
        for r in clobbers[:5]:
            row_start = r["addr"] - r["x"] * 2   # actual row start = da - x*2
            print(f"  y={r['y']} x={r['x']} da=${r['addr']:06X} val=${r['val']:04X} "
                  f"row_start=${row_start:06X}")
    else:
        print("T1 PASS: No $0BFA blit write reaches $7BC8..$7BD7")
    print()

    # ── T2: back-calculate dpt_initial from y=39 actual row_start ───────────
    # dpt0_raw = dpt - x*2 where dpt = row_start (the row-start pointer in hw_do_blit)
    # => row_start = da - x*2    (da = row_start + x*2)
    # => dpt_initial = row_start_y39 - 39 * ROW_STRIDE
    y39_x_min = min((r for r in blt_rows if r["y"] == 39), key=lambda r: r["x"], default=None)
    if y39_x_min:
        row39_start = y39_x_min["addr"] - y39_x_min["x"] * 2
        dpt_initial = (row39_start - 39 * ROW_STRIDE) & 0xFFFFFF
        print(f"T2: y=39 row_start = ${row39_start:06X}  (from da=${y39_x_min['addr']:06X}, x={y39_x_min['x']})")
        print(f"    PC dpt_initial  = ${dpt_initial:06X}  (back-calculated, 24-bit)")
        print(f"    y=39 covers ${row39_start:06X}..${row39_start + ROW_WIDTH*2 - 1:06X}")
        overlap_start = max(row39_start, COPPER_START)
        overlap_end   = min(row39_start + ROW_WIDTH*2 - 1, COPPER_END)
        if overlap_start <= overlap_end:
            print(f"    OVERLAPS copper header at ${overlap_start:06X}..${overlap_end:06X}")
        # Also show where y=1 lands (next closest to copper area)
        row1_start = (dpt_initial + ROW_STRIDE) & 0xFFFFFF
        print(f"    y=1  row_start  = ${row1_start:06X}  (covers ..${row1_start + ROW_WIDTH*2 - 1:06X})")
    else:
        dpt_initial = None
        print("T2 SKIP: no y=39 entries in ANIM_BLT")
    print()

    # ── T3: what dpt_initial would make y=39 SAFE? ──────────────────────────
    # Row y=39 safe if: row_start + 39 < COPPER_START  (row ends before $7BC8)
    #   => row_start < $7BA8   => dpt_initial < $7BA8 - 39*ROW_STRIDE  (24-bit)
    # OR: row_start > COPPER_END   (row starts after $7BD7)
    #   => row_start > $7BD7   => dpt_initial > $7BD7 - 39*ROW_STRIDE
    safe_below_threshold = (COPPER_START - ROW_WIDTH*2 - 39 * ROW_STRIDE) & 0xFFFFFF
    safe_above_threshold = (COPPER_END + 1           - 39 * ROW_STRIDE) & 0xFFFFFF
    print(f"T3: For y=39 row to miss copper header ($7BC8..$7BD7):")
    print(f"    row_start must be < ${COPPER_START - ROW_WIDTH*2:06X}  (row ends before $7BC8)")
    print(f"    => PC dpt_initial must be < ${safe_below_threshold:06X}")
    print(f"    OR row_start must be > ${COPPER_END:06X}           (row starts after $7BD7)")
    print(f"    => PC dpt_initial must be > ${safe_above_threshold:06X}")
    if dpt_initial is not None:
        diff_above = (safe_above_threshold - dpt_initial) & 0xFFFFFF
        diff_below = (dpt_initial - safe_below_threshold) & 0xFFFFFF
        if diff_above < 0x80000:
            print(f"    PC actual ${dpt_initial:06X} needs +${diff_above:04X} ({diff_above} bytes) to be safe above")
        if diff_below < 0x80000:
            print(f"    PC actual ${dpt_initial:06X} needs -${diff_below:04X} ({diff_below} bytes) to be safe below")
    print()

    # ── T4: verify apt source content from PUAE chip RAM dump ───────────────
    try:
        chip = open(CHIPRAM_DUMP, "rb").read()
        src_words = [struct.unpack_from(">H", chip, APT_ADDR + i*2)[0]
                     for i in range(APT_WORDS)]
        unique = set(src_words)
        print(f"T4: PUAE chip RAM at ${APT_ADDR:06X} (apt source, {APT_WORDS} words):")
        print(f"    Values: {', '.join(f'${v:04X}' for v in src_words[:8])} ...")
        if unique == {0xCCCC}:
            print("    T4 PASS: All words = $CCCC")
        else:
            print(f"    T4 NOTE: Mixed values: {sorted(unique)}")
    except FileNotFoundError:
        print(f"T4 SKIP: {CHIPRAM_DUMP} not found")
    print()

    # ── T5: scan PUAE chip RAM at y=39 row range to find where PUAE's blit wrote
    # The $0BFA blt produces d = A | C. A = $CCCC mask means every output has
    # bits 15,14,11,10,7,6,3,2 set (the $CCCC bits). If PUAE's y=39 row landed
    # at a different start address, those CCCC-masked words would be there.
    try:
        chip = open(CHIPRAM_DUMP, "rb").read()
        print(f"T5: PUAE chip RAM at $7B80..$7BE0 (around y=39 row)")
        print(f"    addr   word   has_cccc_bits?")
        for addr in range(0x7B80, 0x7BE0, 2):
            w = struct.unpack_from(">H", chip, addr)[0]
            # $CCCC = 1100_1100_1100_1100 — check if all CCCC bits are set
            cccc_bits = w & 0xCCCC
            marker = " <-- CCCC bits set" if cccc_bits == 0xCCCC else ""
            if addr == COPPER_START:
                marker += "  ← copper header start"
            print(f"    ${addr:06X}  ${w:04X}{marker}")
    except FileNotFoundError:
        print(f"T5 SKIP: {CHIPRAM_DUMP} not found")
    print()

    # ── T6: find the last address < $7BC8 in PUAE chip RAM with all CCCC bits set
    # This tells us where PUAE's y=39 row actually ended
    try:
        chip = open(CHIPRAM_DUMP, "rb").read()
        # Scan $7B00..$7BD7 for words with all CCCC bits, contiguous range
        cccc_words = [(addr, struct.unpack_from(">H", chip, addr)[0])
                      for addr in range(0x7B00, COPPER_END+2, 2)
                      if (struct.unpack_from(">H", chip, addr)[0] & 0xCCCC) == 0xCCCC]
        if cccc_words:
            last_cccc = cccc_words[-1][0]
            print(f"T6: Last word with all CCCC bits in $7B00..$7BD8 range:")
            print(f"    ${last_cccc:06X} = ${cccc_words[-1][1]:04X}")
            # Expected PUAE row_39_end = just before COPPER_START
            if last_cccc < COPPER_START:
                print(f"    => PUAE's y=39 row ends at ${last_cccc:06X} (safe, below $7BC8)")
                # Estimate PUAE row_39_start
                row_end_est = last_cccc
                # row covers [row_start .. row_start + ROW_WIDTH*2 - 1]
                # row_start = row_end_est - ROW_WIDTH*2 + 2 (if last_cccc is last word)
                row39_start_puae = row_end_est - (ROW_WIDTH - 1) * 2
                dpt_initial_puae = (row39_start_puae - 39 * ROW_STRIDE) & 0xFFFFFF
                print(f"    Estimated PUAE row_39_start = ${row39_start_puae:06X}")
                print(f"    Estimated PUAE dpt_initial  = ${dpt_initial_puae:06X}")
                if dpt_initial is not None:
                    delta = (dpt_initial_puae - dpt_initial) & 0xFFFFFF
                    if delta >= 0x80000:
                        delta -= 0x1000000
                    print(f"    Delta PUAE - PC = {delta} bytes (${delta & 0xFFFFFF:04X})")
            else:
                print(f"    => PUAE's CCCC words reach $7BC8 or beyond — unexpected!")
        else:
            print(f"T6: No words with all CCCC bits in $7B00..$7BD8 (PUAE never wrote there?)")
    except FileNotFoundError:
        print(f"T6 SKIP: {CHIPRAM_DUMP} not found")
    print()

if __name__ == "__main__":
    run_tests()
