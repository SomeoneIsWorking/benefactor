"""Recursive-descent function scanner for the Benefactor M68K binary."""
from capstone import *
from capstone.m68k import *

try:
    from .helpers import mem_base, mem_index
except ImportError:
    from helpers import mem_base, mem_index  # type: ignore

# ---------------------------------------------------------------------------
# Data-region detection  (Benefactor-specific heuristics)
# ---------------------------------------------------------------------------

def is_data_region(data, base, addr, md):
    """Return True if addr starts a data region (not code)."""
    off = addr - base
    if off < 0 or off + 4 > len(data):
        return True
    try:
        decoded = list(md.disasm(data[off:off + 16], addr))
    except Exception:
        return True
    if not decoded or decoded[0].address != addr:
        return True
    insn = decoded[0]
    m = insn.mnemonic.lower().split('.')[0]

    # Runs of 3+ ORI/ANDI are data in this binary
    if m in ('ori', 'andi'):
        a = addr + insn.size
        for _ in range(3):
            if a - base + 16 > len(data):
                return True
            try:
                dd = list(md.disasm(data[a - base:a - base + 16], a))
                if not dd:
                    return True
                if dd[0].mnemonic.lower().split('.')[0] != m:
                    return False
                a += dd[0].size
            except Exception:
                return True
        return True

    # BTST with non-hardware-register memory operands → data
    if m == 'btst':
        ops = insn.operands
        if len(ops) >= 2 and ops[1].type == M68K_OP_MEM:
            b = mem_base(ops[1])
            if b is None or b not in (M68K_REG_A6,):
                return True

    return False


# ---------------------------------------------------------------------------
# Low-level disassembly helpers
# ---------------------------------------------------------------------------

def _disasm_one(data, base, addr, md):
    """Disassemble one instruction.  Returns (insn, next_addr) or (None, addr)."""
    off = addr - base
    if off < 0 or off + 4 > len(data):
        return None, addr
    try:
        decoded = list(md.disasm(data[off:off + 16], addr))
    except Exception:
        return None, addr
    if not decoded or decoded[0].address != addr:
        return None, addr
    return decoded[0], addr + decoded[0].size


def _scan_func(data, base, start, md, known_entries=None):
    """Scan one function from start until rts/rte/jmp/illegal/SKIPDATA.

    Handles functions with internal branches past rts (e.g. switch-case tails).
    Returns (list_of_insns, end_addr).

    Only *conditional* branch targets (Bcc) are added to branch_targets to
    determine whether the scan should continue past a terminal instruction.
    Unconditional calls/jumps (bsr, bra to known entries) are excluded because
    they call *other* functions and must not keep this function's scan alive
    past its own rts/rte, which would incorrectly include dead code.
    """
    if known_entries is None:
        known_entries = set()
    addr = start
    insns = []
    branch_targets = set()

    while True:
        insn, next_addr = _disasm_one(data, base, addr, md)
        if insn is None:
            break
        insns.append(insn)
        addr = next_addr

        m = insn.mnemonic.lower().split('.')[0]
        try:
            ops = insn.operands
        except Exception:
            break  # SKIPDATA

        for op in ops:
            if op.type == M68K_OP_BR_DISP:
                t = insn.address + 2 + op.br_disp.disp
                if t >= start:
                    # bsr is a call to another function — never intra-function.
                    # bra to a known entry point is a tail-call — same rule.
                    if m == 'bsr':
                        continue
                    if m == 'bra' and t in known_entries:
                        continue
                    branch_targets.add(t)

        # Forward unconditional bra over a small inline data table (≤64 bytes)
        # — the skipped bytes are data, not code. If nothing else branches into
        # the gap, resume scanning AT the target so we don't decode the data as
        # bogus instructions (which would misalign and hide the real landing
        # from `cur_insn_addrs`, causing the emitter to fall back to rt_jump
        # for an intra-function goto).
        if m == 'bra' and not any(o.type == M68K_OP_REG for o in ops):
            t = insn.address + 2 + ops[0].br_disp.disp
            gap = t - addr
            if 4 <= gap <= 64 and not any(addr <= bt < t for bt in branch_targets):
                addr = t
                continue

        can_stop = False
        if m in ('rts', 'rte', 'illegal'):
            can_stop = True
        if m == 'jmp' and not any(o.type == M68K_OP_REG for o in ops):
            can_stop = True
        if m == 'bra' and not any(o.type == M68K_OP_REG for o in ops):
            can_stop = True
        if insn.size < 2:
            can_stop = True

        if can_stop and not any(t >= addr for t in branch_targets):
            break

    return insns, addr


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def collect_functions(data, base, entries, md, areg_bases=None, bank=None):
    """Collect all reachable functions starting from the given entry points.

    `areg_bases` optionally maps a capstone address-register id (e.g.
    M68K_REG_A5 == 14) to a known constant base value.  The gameplay overlay
    calls functions as `jsr $d(a5)` with a5 a fixed code-base pointer; resolving
    those against the known base lets the scanner discover the whole call graph.

    Returns a dict mapping start_addr → list[insn].
    """
    areg_bases = areg_bases or {}
    funcs = {}
    user_entries = set(entries)
    queue = sorted(set(entries))
    visited = set()
    ranges = []  # list of (start, end)

    def in_any_range(addr):
        return any(s <= addr < e for s, e in ranges)

    # Phase 1: collect functions reachable from entry points
    while queue:
        entry = queue.pop(0)
        if entry in visited:
            continue
        if in_any_range(entry) and entry not in user_entries:
            continue
        visited.add(entry)

        insns, end_addr = _scan_func(data, base, entry, md, known_entries=user_entries)
        ranges.append((entry, end_addr))
        funcs[entry] = insns

        for insn in insns:
            m = insn.mnemonic.lower().split('.')[0]
            try:
                ops = insn.operands
            except Exception:
                continue
            # In bank mode (areg_bases set) the targets we queue come from
            # explicit jsr/jmp/bsr/a5-relative call instructions, which prove
            # the target is code — so we trust them rather than re-deriving
            # code-vs-data with the intro-tuned `is_data_region` heuristics
            # (which over-reject legitimate gameplay code such as btst spins).
            trust = bool(areg_bases)
            def _queue(t):
                if base <= t <= base + len(data) - 4 and t not in visited:
                    if trust or not is_data_region(data, base, t, md):
                        queue.append(t)
            for op in ops:
                if op.type == M68K_OP_BR_DISP:
                    _queue(insn.address + 2 + op.br_disp.disp)
                elif op.type == M68K_OP_IMM and m in ('jsr', 'jmp'):
                    _queue(op.value.imm)
                elif (op.type == M68K_OP_MEM and m in ('jsr', 'jmp')
                      and areg_bases and op.mem.index_reg == 0
                      and op.mem.base_reg in areg_bases):
                    # `jsr/jmp $d(a5)` with a5 a known constant code-base: the
                    # control-flow target is base+disp (an address, not a deref).
                    _queue(areg_bases[op.mem.base_reg] + op.mem.disp)

    # Phase 2 (ITERATIVE): a bsr/jsr/jmp can target an address the descent
    # absorbed into another function's body (so it was never emitted as its own
    # callable entry) — calling it at runtime then misses. Repeatedly scan ALL
    # discovered functions (including ones split out in earlier iterations) for
    # constant call targets and split/add any that aren't yet a function, until
    # no new ones appear. This makes the call graph transitively complete in one
    # pass instead of via runtime whack-a-mole re-seeding.
    def _call_targets(insns):
        out = set()
        for insn in insns:
            m = insn.mnemonic.lower().split('.')[0]
            try:
                ops = insn.operands
            except Exception:
                continue
            for op in ops:
                t = None
                if op.type == M68K_OP_BR_DISP and m == 'bsr':
                    t = insn.address + 2 + op.br_disp.disp
                # gpl-bank-specific: FORWARD `bra` targets in the
                # $577xxx/$58xxxx gameplay code are runtime-callable entries
                # — the dispatcher jumps over inline jump-offset data tables
                # to alternate landings. Both long form (`bra.w X` 4 bytes,
                # e.g. $589792 -> $5897D0 over the offset table at $589796)
                # and short form (`bra.b X` 2 bytes, e.g. $57DE02 -> $57DE0C
                # over a small data table) appear. Phase 2's overlap check
                # still rejects promotions that would corrupt other
                # functions' ranges. Backward bra is left alone here — it's
                # typically intra-function loop control; backward
                # cross-function tail-jumps (e.g. dispatcher loop tops like
                # $57DD3E -> $57DCC4) are caught by Pattern E in
                # discover_benefactor_targets.py instead.
                elif (op.type == M68K_OP_BR_DISP and m == 'bra'
                      and bank == 'gpl' and insn.size >= 4
                      and op.br_disp.disp > 0):
                    t = insn.address + 2 + op.br_disp.disp
                elif op.type == M68K_OP_IMM and m in ('jsr', 'jmp'):
                    t = op.value.imm
                elif (op.type == M68K_OP_MEM and m in ('jsr', 'jmp')
                      and areg_bases and op.mem.index_reg == 0
                      and op.mem.base_reg in areg_bases):
                    t = areg_bases[op.mem.base_reg] + op.mem.disp
                if t is not None and base <= t <= base + len(data) - 4:
                    out.add(t)
        return out

    changed = True
    while changed:
        changed = False
        targets = set()
        for _entry, insns in funcs.items():
            targets |= _call_targets(insns)
        for t in sorted(targets):
            if t in funcs:
                continue
            sub_insns, sub_end = _scan_func(data, base, t, md, known_entries=user_entries)
            if not sub_insns:
                continue
            # Don't create a function that overlaps a DIFFERENT existing range
            # (other than the one it splits out of); that would double-emit code.
            containing = next(((s, e) for s, e in ranges if s <= t < e), None)
            overlap = any(
                (not containing or s != containing[0]) and not (sub_end <= s or t >= e)
                for s, e in ranges
            )
            if overlap:
                continue
            ranges.append((t, sub_end))
            funcs[t] = sub_insns
            changed = True

    return funcs
