"""Recursive-descent function scanner for the Benefactor M68K binary."""
from capstone import *
from capstone.m68k import *

try:
    from .helpers import mem_base, mem_index
    from .extract_jumptables import discover_jumptable_targets
except ImportError:
    from helpers import mem_base, mem_index  # type: ignore
    from extract_jumptables import discover_jumptable_targets  # type: ignore

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

        # A control-flow terminator was reached (rts / bra / tail-call jmp —
        # incl. this engine's `jmp d(a5)` dispatcher returns, which are MEM, not
        # REG, operands). If a forward label still pends, the bytes between here
        # and the NEAREST one are unreachable: Benefactor's object handlers embed
        # a pc-relative jump table right after their tail-call, with execution
        # resuming at the next case label that branched OVER the data (e.g.
        # $599BB2: data $599C0C..$599CBE, code at $599CBE). Skip the inline data
        # to that label and resume — unless something branches INTO the gap, in
        # which case it's real code and we keep decoding as before. A label
        # exactly AT addr means code continues here, so don't stop.
        if can_stop and not any(t == addr for t in branch_targets):
            fwd = [t for t in branch_targets if t > addr]
            if not fwd:
                break
            nxt = min(fwd)
            if not any(addr < bt < nxt for bt in branch_targets):
                addr = nxt
                continue
            # gap contains a branch target -> keep scanning (fall through)

    return insns, addr


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def _edge_targets(insn, source_addrs, lo, hi, areg_bases, bank):
    """Yield each control-flow target of `insn` that ESCAPES the source
    function's own instruction set (`source_addrs`) — i.e. EXACTLY the targets
    the emitter renders as a cross-function `rt_jump`/`rt_call`. (The emitter
    emits a local `goto` only when the target is one of the function's own
    instruction addresses; otherwise `rt_jump(target)`.) Every escaping target
    must therefore be a registered function at its EXACT start, or the runtime
    aborts "NO FUNCTION at $X". One rule covers Bcc, bra, bsr and jmp/jsr (imm /
    abs-long `$X.l` / a5-relative) uniformly — the split-by-mnemonic logic that
    preceded this absorbed Bcc and cross-function `bra` targets ($577384) and
    never followed abs-long `jmp $X.l` (capstone `M68K_OP_MEM`, addr_mode 16/17,
    target in `op.imm`), which the emitter DID follow. Scanner and emitter now
    derive targets from the same place, so they cannot disagree."""
    m = insn.mnemonic.lower().split('.')[0]
    try:
        ops = insn.operands
    except Exception:
        return
    for op in ops:
        t = None
        # `always` = the emitter renders this as rt_call/rt_jump unconditionally,
        # so the target must register even if it falls inside the source
        # function. `bra`/`Bcc` instead become a LOCAL goto when the target is
        # one of this function's own instructions (emit_branch) — only the
        # escaping ones need registering. (Getting this wrong both ways bit us:
        # a `bsr` target that happened to land inside the source range — $57C79E
        # — was wrongly filtered out and left dangling.)
        always = False
        if op.type == M68K_OP_BR_DISP:            # Bcc / bra / bsr
            t = insn.address + 2 + op.br_disp.disp
            always = (m == 'bsr')                 # bsr is a real call (rt_call)
        elif m in ('jsr', 'jmp'):                 # jsr->rt_call, jmp->rt_jump
            always = True                         # never a local goto
            if op.type == M68K_OP_IMM:
                t = op.imm
            elif op.type == M68K_OP_MEM:
                amode = getattr(op, 'address_mode', 0)
                if amode in (16, 17):             # abs.short / abs.long $X.l
                    t = op.imm
                elif op.mem.index_reg == 0 and op.mem.base_reg in areg_bases:
                    # `jsr/jmp $d(a5)` with a5 a known constant code base.
                    t = areg_bases[op.mem.base_reg] + op.mem.disp
                else:
                    t = None                      # computed jmp (a1,d0) — runtime
        if t is not None and lo <= t <= hi and (always or t not in source_addrs):
            yield t


def scan_program(data, base, md, entries, areg_bases=None, bank=None,
                 region=None, raw=None):
    """THE scan: one recursive worklist that discovers every function in the
    bank. Replaces the old multi-pass pipeline (separate jump-table + object-
    handler sweeps, then collect's Phase 1 descent + Phase 2 iterative split +
    a blunt gap-fill) — every seam between those passes leaked a target. There
    is ONE block scanner (`_scan_func`), ONE edge rule (`_edge_targets`, which
    mirrors the emitter exactly), and ONE worklist drained to a fixpoint. The
    game-specific entry rules feed that same worklist instead of running their
    own scans:

      * object-handler prologues       -> discover_object_handlers(region)
      * function-pointer / jump tables -> discover_jumptable_targets(raw)
      * region superset                -> every validated code block in `region`
        not reached above. The engine reaches per-object handlers through
        runtime-built pointer chains ($57D3EC) that NO static edge follows, and
        which handlers a level uses is per-level disk data — so this superset is
        the only way to be complete without play-testing. It is registered AFTER
        the exact entries, so a swept block can never absorb a real dispatch
        target (those are already their own functions); that pairing is what
        makes it safe — the blunt sweep ALONE mis-segments (the documented
        54->35 level regression).

    `entries` are the seeds. Returns {start_addr: list[insn]}.
    """
    from collections import deque
    areg_bases = areg_bases or {}
    lo_a, hi_a = base, base + len(data) - 4
    trust = bool(areg_bases)

    seed_entries = set(entries)
    if region is not None:
        seed_entries |= discover_object_handlers(data, base, md,
                                                 region[0], region[1])
    if raw is not None:
        seed_entries |= discover_jumptable_targets(raw, md)

    funcs = {}
    ranges = []                       # (start, end) of every emitted function
    seen = set()                      # addrs already attempted
    work = deque(sorted(seed_entries))

    def add_func(addr, insns, end):
        ranges.append((addr, end))
        funcs[addr] = insns
        src = {i.address for i in insns}
        for insn in insns:
            for t in _edge_targets(insn, src, lo_a, hi_a, areg_bases, bank):
                if t not in funcs:
                    work.append(t)

    def drain():
        while work:
            addr = work.popleft()
            if addr in funcs or addr in seen:
                continue
            seen.add(addr)
            # Intro bank (no trusted a-reg base): re-derive code-vs-data so a
            # branch into a data table isn't decoded as a bogus function. The
            # gpl bank trusts its edges (every one is a real branch/call/jump).
            if not trust and is_data_region(data, base, addr, md):
                continue
            insns, end = _scan_func(data, base, addr, md,
                                    known_entries=seed_entries)
            if not insns:
                continue
            # Register at the EXACT start, even if it lands interior to an
            # already-emitted function (overlap = double-emit, which is just
            # bloat, NOT incorrectness: table_search dispatches to this entry;
            # falling through from the parent runs the same code in-line). This
            # is mandatory for correctness — every escaping target the emitter
            # renders as `rt_jump`/`rt_call` MUST resolve to a function at its
            # exact address. The old overlap guard suppressed these (e.g. the
            # mutually-fall-through dispatch stubs $59DDxx-$59E0xx), which then
            # had to be hand-seeded; registering them closes that by construction.
            add_func(addr, insns, end)

    # 1) Exact entries (seeds + prologue handlers + jump tables) and everything
    #    transitively reachable from them by escaping edges.
    drain()

    # 2) Region superset: computed-dispatch-only engine code. Walk the region;
    #    register each validated, terminated, emittable block no exact function
    #    already covers, then drain its edges through the SAME worklist.
    if region is not None:
        lo, hi = region

        def next_start_after(a):
            best = hi
            for s, _e in ranges:
                if a < s < best:
                    best = s
            return best

        a = lo
        while a < hi:
            cov = [e for s, e in ranges if s <= a < e]
            if cov:
                a = max(cov)
                continue
            insns, end = _scan_func(data, base, a, md, known_entries=seed_entries)
            gap_end = next_start_after(a)
            if (insns and end <= gap_end and end <= hi
                    and _terminates(insns) and _emittable(insns)):
                add_func(a, insns, end)
                drain()                       # follow the block's own edges
                a = end
            else:
                a += 2

    return funcs


def collect_functions(data, base, entries, md, areg_bases=None, bank=None,
                      gapfill=None):
    """Backward-compatible shim around `scan_program`. The blunt `gapfill`
    ("emit every validated block in [lo,hi)") sweep is retired — it absorbed
    exact dispatch targets into neighbouring blocks (the documented 54->35
    regression). Its real yield, object handlers, now comes from the prologue
    rule feeding the one scan via `region`."""
    return scan_program(data, base, md, entries, areg_bases=areg_bases,
                        bank=bank, region=(gapfill or None))


def _terminates(insns):
    """True if the block ends in a real control-flow terminator (rts/rte/rtr or
    an unconditional non-register jmp/bra). Guards gap-fill against accepting a
    run of data bytes that merely happened to disassemble without faulting."""
    if not insns:
        return False
    last = insns[-1]
    m = last.mnemonic.lower().split('.')[0]
    if m in ('rts', 'rte', 'rtr'):
        return True
    if m in ('jmp', 'bra'):
        try:
            return not any(o.type == M68K_OP_REG for o in last.operands)
        except Exception:
            return False
    return False


# Registers the emitter can name (see helpers.reg_name): the integer file plus
# PC/SR. Anything else — CCR, FP0-7, USP, SFC/DFC and the other control
# registers — only appears when gap-fill misdecodes a data table (e.g. the
# $003C `ori.b #x,ccr` alignment/padding word) as code, and would emit an
# undefined `ctx->UNK_R_*` field that won't compile. Real integer handler code
# in this bank never references them, so such a block is data, not a handler.
_EMITTABLE_REGS = frozenset(
    [M68K_REG_PC, M68K_REG_SR]
    + [globals()[f'M68K_REG_D{i}'] for i in range(8)]
    + [globals()[f'M68K_REG_A{i}'] for i in range(8)]
)


def _emittable(insns):
    """True if every register operand in the block maps to a real emitter
    field. Guards gap-fill against accepting misdecoded data that happens to
    terminate cleanly but references a register the translator can't name."""
    for insn in insns:
        try:
            ops = insn.operands
        except Exception:
            continue
        for op in ops:
            if op.type == M68K_OP_REG:
                if op.reg not in _EMITTABLE_REGS:
                    return False
            elif op.type == M68K_OP_MEM:
                for r in (mem_base(op), mem_index(op)):
                    if r not in (None, M68K_REG_INVALID) and r not in _EMITTABLE_REGS:
                        return False
    return True


# Object-handler prologue opcodes: mem-to-reg `movem.w/.l (a0)|(a0)+|d(a0),
# <regs>`. A0 is the per-object state pointer, so this is the canonical first
# instruction of a $57D3EC dispatch handler.
_HANDLER_PROLOGUES = frozenset((0x4C90, 0x4C98, 0x4CA8,    # .w (a0)/(a0)+/d(a0)
                                0x4CD0, 0x4CD8, 0x4CE8))   # .l (a0)/(a0)+/d(a0)


def discover_object_handlers(data, base, md, scan_lo, scan_hi):
    """Return absolute addresses in [scan_lo, scan_hi) that begin a per-object
    handler. The $57D3EC dispatcher computes handler addresses from runtime
    object data (`jmp (a1,d0.w)`), so static descent never reaches them — and
    `table_search` resolves a runtime jump by EXACT address, so each handler
    must be registered at its true start. Gap-fill alone can merge a handler
    into a preceding fall-through block (then its exact address isn't a table
    entry and the dispatch misses); pinning the prologue addresses as their own
    entries guarantees the boundary. Almost every handler opens with
    `movem (a0)..., <regs>`; keep candidates that disassemble to a clean,
    rts-terminated, emittable block."""
    found = set()
    a = scan_lo
    while a < scan_hi:
        off = a - base
        if off < 0 or off + 2 > len(data):
            break
        word = (data[off] << 8) | data[off + 1]
        if word in _HANDLER_PROLOGUES:
            insns, _end = _scan_func(data, base, a, md, known_entries=set())
            if (insns and insns[0].mnemonic.lower().startswith('movem')
                    and _terminates(insns) and _emittable(insns)):
                found.add(a)
        a += 2
    return found
