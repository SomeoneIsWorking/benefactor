"""Instruction translator (Translator) and function emitter for the Benefactor recompiler."""
import os
import re
from capstone.m68k import *

try:
    from .helpers import reg_name, mem_base, mem_index, CC_MAP
    from .entries  import get_fn_name
except ImportError:
    from helpers import reg_name, mem_base, mem_index, CC_MAP  # type: ignore
    from entries  import get_fn_name                           # type: ignore

# ---------------------------------------------------------------------------
# Instruction translator
# ---------------------------------------------------------------------------

class Translator:
    def __init__(self, func_addrs):
        self.func_addrs = set(func_addrs)
        self.current_pc = None
        self.cur_insn_addrs = set()   # instruction addrs of the function being emitted

    # ── Effective-address computation ────────────────────────────────────

    def ea(self, op, sz=4, pc=None):
        """Compute effective address as a C expression (no memory read)."""
        if pc is None:
            pc = self.current_pc
        if op.type == M68K_OP_REG:
            return f'ctx->{reg_name(op.reg)}'
        if op.type == M68K_OP_IMM:
            return hex(op.imm)
        if op.type == M68K_OP_MEM:
            mode = getattr(op, 'address_mode', 0)
            b    = mem_base(op)
            disp = op.mem.disp
            idx  = ''
            mi   = mem_index(op)
            if mi is not None:
                idx = f' + RT_SX16(ctx->{reg_name(mi)})'

            # Capstone M68K address_mode values:
            #  3 = (An)        4 = (An)+      5 = -(An)
            #  6 = d16(An) / d8(An,Xn)
            # 11 = d16(PC)    12 = d8(PC,Xn)
            # 16 = abs.short / #imm    17 = abs.long
            if mode == 3:
                return f'ctx->A[{op.imm - 9}]'
            if mode == 4:
                return f'ctx->A[{op.imm - 9}]'
            if mode == 5:
                return f'(ctx->A[{op.imm - 9}] - {sz})'
            if mode in (11, 12):
                base = hex(pc) if pc else '0'
                dabs = abs(disp)
                if disp < 0:
                    return f'(uint32_t)({base} - {dabs}u{idx})'
                return f'(uint32_t)({base} + {dabs}u{idx})'
            if mode in (16, 17):
                return hex(op.imm)
            # d16(An) / d8(An,Xn) — An is uint32_t, no cast needed
            if b is not None and b != M68K_REG_INVALID:
                base = f'ctx->{reg_name(b)}'
                dabs = abs(disp)
                if disp < 0:
                    return f'{base} - {dabs}u{idx}'
                return f'{base} + {dabs}u{idx}'
            if b == M68K_REG_INVALID:
                if disp != 0:
                    return f'(uint32_t)({hex(pc)} + {disp}u)' if pc else hex(disp)
                return hex(op.imm)
            return '/* ??ea */'
        return '/* ??ea */'

    # ── Operand readers / writers ────────────────────────────────────────

    def _pre_rd(self, op, sz=4, pc=None):
        """Return (preamble_stmt, value_expr) for an operand.

        For auto-increment/pre-decrement operands this captures the value in a
        C local so the expression can be used multiple times without repeating
        the side-effect.  All other operands return an empty preamble.
        """
        if pc is None:
            pc = self.current_pc
        if op.type == M68K_OP_MEM:
            mode = getattr(op, 'address_mode', 0)
            if mode == 4:  # (An)+
                reg = op.imm - 9
                t   = f'_pi{reg}_{self.current_pc:x}'
                pre = (f'uint{sz*8}_t {t} = MR{sz*8}(ctx->A[{reg}]);\n'
                       f'ctx->A[{reg}] += {sz};')
                return (pre, t)
            if mode == 5:  # -(An)
                reg = op.imm - 9
                t   = f'_pd{reg}_{self.current_pc:x}'
                pre = (f'ctx->A[{reg}] -= {sz};\n'
                       f'uint{sz*8}_t {t} = MR{sz*8}(ctx->A[{reg}]);')
                return (pre, t)
        return ('', self.rd(op, sz, pc))

    def rd(self, op, sz=4, pc=None):
        """Read operand as a C expression (single-use only for auto-inc/dec)."""
        if pc is None:
            pc = self.current_pc
        if op.type == M68K_OP_REG:
            return f'ctx->{reg_name(op.reg)}'
        if op.type == M68K_OP_IMM:
            return hex(op.imm)
        if op.type == M68K_OP_MEM:
            mode = getattr(op, 'address_mode', 0)
            if mode == 4:  # (An)+ via GCC stmt-expr
                reg = op.imm - 9
                t   = f'_pi{reg}s'
                return (f'({{ uint{sz*8}_t {t} = MR{sz*8}(ctx->A[{reg}]); '
                        f'ctx->A[{reg}] += {sz}; {t}; }})')
            addr = self.ea(op, sz, pc)
            if sz == 1: return f'MR8({addr})'
            if sz == 2: return f'MR16({addr})'
            return f'MR32({addr})'
        return '/* ??rd */'

    def wr(self, op, val, sz=4, pc=None):
        """Write to an operand; returns a C statement (or multi-line string)."""
        if pc is None:
            pc = self.current_pc
        if op.type == M68K_OP_REG:
            r = reg_name(op.reg)
            if r == 'SR':
                return (f'ctx->S = (({val}) >> 13) & 1;\n'
                        f'ctx->I = (({val}) >> 8) & 7;\n'
                        f'RT_SET_NZ_16({val});\n'
                        f'ctx->V = ctx->C = 0;')
            if sz == 1:
                return f'ctx->{r} = (ctx->{r} & 0xFFFFFF00u) | (uint8_t){val};'
            if sz == 2:
                return f'ctx->{r} = (ctx->{r} & 0xFFFF0000u) | (uint16_t){val};'
            # sz == 4: plain numeric literals get a 'u' suffix instead of a cast;
            # expressions already typed as (uint32_t) need no extra cast either.
            if re.fullmatch(r'0x[0-9a-fA-F]+|[0-9]+', val):
                return f'ctx->{r} = {val}u;'
            cast = '' if val.startswith('(uint32_t)') else '(uint32_t)'
            return f'ctx->{r} = {cast}{val};'
        if op.type == M68K_OP_MEM:
            mode = getattr(op, 'address_mode', 0)
            if mode == 4:  # (An)+ destination
                reg  = op.imm - 9
                t_wa = f'_wa_{self.current_pc:x}'
                return (f'uint32_t {t_wa} = ctx->A[{reg}];\n'
                        f'ctx->A[{reg}] += {sz};\n'
                        f'MW{sz*8}({t_wa}, {val});')
            if mode == 5:  # -(An) destination: predecrement An, THEN write.
                # ea() returns the bare expression (A[reg]-sz) with no side
                # effect, so without this branch the move wrote to the right
                # address but never advanced An — e.g. the gp ATN! decruncher's
                # `move.b -(a3),-(a4)` copy loop ($373C) left a4 frozen and spun
                # forever. Capture the value first (M68K evaluates the source
                # before the predecrement).
                reg = op.imm - 9
                t_wd = f'_wd_{self.current_pc:x}'
                return (f'uint{sz*8}_t {t_wd} = (uint{sz*8}_t)({val});\n'
                        f'ctx->A[{reg}] -= {sz};\n'
                        f'MW{sz*8}(ctx->A[{reg}], {t_wd});')
            addr = self.ea(op, sz, pc)
            if sz == 1: return f'MW8({addr}, {val});'
            if sz == 2: return f'MW16({addr}, {val});'
            return f'MW32({addr}, {val});'
        return '/* ??wr */'

    # ── Read-modify-write (shared by add/sub/and/or/…) ──────────────────

    def _emit_rmw(self, d, sz, op, s, t_old, t_res, flags_stmt):
        """Emit a read-modify-write sequence for destination operand `d`.

        Captures the address once for auto-inc/dec modes to prevent
        double-advancing the address register.
        """
        fszname = 32 if sz == 4 else (16 if sz == 2 else 8)
        if d.type == M68K_OP_MEM:
            mode = getattr(d, 'address_mode', 0)
            if mode == 4:  # (An)+
                reg    = d.imm - 9
                t_addr = f'_rmw_addr_{self.current_pc:x}'
                return (f'uint32_t {t_addr} = ctx->A[{reg}];\n'
                        f'ctx->A[{reg}] += {sz};\n'
                        f'uint{fszname}_t {t_old} = (uint{fszname}_t)MR{sz*8}({t_addr});\n'
                        f'uint64_t {t_res} = (uint64_t){t_old} {op} (uint64_t)(uint{fszname}_t)({s});\n'
                        f'MW{sz*8}({t_addr}, {t_res});\n'
                        f'{flags_stmt}')
            if mode == 5:  # -(An)
                reg    = d.imm - 9
                t_addr = f'_rmw_addr_{self.current_pc:x}'
                return (f'ctx->A[{reg}] -= {sz};\n'
                        f'uint32_t {t_addr} = ctx->A[{reg}];\n'
                        f'uint{fszname}_t {t_old} = (uint{fszname}_t)MR{sz*8}({t_addr});\n'
                        f'uint64_t {t_res} = (uint64_t){t_old} {op} (uint64_t)(uint{fszname}_t)({s});\n'
                        f'MW{sz*8}({t_addr}, {t_res});\n'
                        f'{flags_stmt}')
        old_expr = self.rd(d, sz)
        # t_res keeps the FULL (untruncated) result so the flag macro derives
        # carry/borrow from the bit above the operand width (res >> width);
        # wr()/MW* truncate to `sz` on store. Previously t_res was truncated to
        # the operand width, forcing carry to 0 — which broke add.b/sub.b carry
        # (e.g. the menu ATN! decruncher bit-reader at $3700 spun forever).
        capture  = (f'uint{fszname}_t {t_old} = (uint{fszname}_t){old_expr};\n'
                    f'uint64_t {t_res} = (uint64_t){t_old} {op} (uint64_t)(uint{fszname}_t)({s});')
        return capture + '\n' + self.wr(d, t_res, sz) + '\n' + flags_stmt

    # ── Branch helpers ───────────────────────────────────────────────────

    def br_target(self, insn, add=0):
        """Compute branch target address."""
        for op in insn.operands:
            if op.type == M68K_OP_BR_DISP:
                return insn.address + add + op.br_disp.disp
        return None

    def emit_branch(self, insn, cond=None):
        """Emit an intra- or cross-function branch."""
        t = self.br_target(insn, 2)
        if t is None:
            return '/* bad branch */'
        if t in self.func_addrs:
            # A branch (bra/bcc) to another function is a TAIL TRANSFER: real M68K
            # pushes NO return address. Emit rt_jump (is_call=0), NOT rt_call —
            # rt_call reserves a return-address slot in the gameplay bank, so every
            # branch-to-function would leak 4 bytes of stack (a7 drifts low),
            # corrupting later $X(a7) argument reads and dropping objects. (bsr/jsr
            # are the real calls and still use rt_call, in translate().)
            call = f'rt_jump(ctx, 0x{t:06X}u);'
            if cond:
                return (f'if (RT_CC_{cond}) {{\n'
                        f'  {call}\n'
                        f'  return;\n'
                        f'}}')
            return f'{call}\nreturn;'
        # When cur_insn_addrs is unpopulated (unit tests call translate()
        # directly) assume an intra-function branch and emit a goto, preserving
        # the historical contract.  During real emission emit_func populates it.
        if (not self.cur_insn_addrs) or (t in self.cur_insn_addrs):
            if cond:
                return f'if (RT_CC_{cond}) goto L_{t:06X};'
            return f'goto L_{t:06X};'
        # Target is neither a known function entry nor a label inside this
        # function (e.g. a tail branch past a terminal jmp into code the scan
        # didn't fold in).  Dispatch by address so it compiles and resolves at
        # runtime (and is logged for discovery if no handler exists yet).
        if cond:
            return f'if (RT_CC_{cond}) {{ rt_jump(ctx, 0x{t:06X}u); return; }}'
        return f'rt_jump(ctx, 0x{t:06X}u);\nreturn;'

    # ── Main dispatch ────────────────────────────────────────────────────

    def translate(self, insn):
        self.current_pc = insn.address + 2
        m       = insn.mnemonic.lower().split('.')[0]
        ops     = insn.operands
        sz_attr = getattr(insn, 'op_size', None)
        sz      = sz_attr.size if sz_attr else 4

        # ── VPOSR / BZERO wait-loop detection ───────────────────────────
        if m == 'btst' and len(ops) >= 2:
            bit_val = ops[0].imm if ops[0].type == M68K_OP_IMM else None
            dst     = ops[1]
            if bit_val is not None and dst.type == M68K_OP_MEM:
                mode_d = getattr(dst, 'address_mode', 0)
                b      = mem_base(dst)
                disp   = dst.mem.disp
                # Mode 3 = (An) direct: register is in op.imm, disp is always 0
                if mode_d == 3:
                    b, disp = dst.imm, 0
                if bit_val == 0 and b == M68K_REG_A6 and disp == 3:
                    return 'ctx->Z = !(hw_vposr_read() & 1);'
                if bit_val == 6 and b == M68K_REG_A6 and disp == 0:
                    return 'ctx->Z = !hw_blitter_busy();'

        # ── Instruction dispatch ─────────────────────────────────────────
        if m == 'nop':
            return ''

        if m == 'moveq':
            v = ops[0].imm & 0xFF
            if v >= 0x80: v -= 0x100
            return (self.wr(ops[1], f'(int32_t){v}', 4) + '\n'
                    + f'ctx->N = ({v} < 0);\nctx->Z = ({v} == 0);\nctx->V = ctx->C = 0;')

        if m in ('move', 'movea'):
            pre, s = self._pre_rd(ops[0], sz)
            # If src is a plain memory read (not already captured in a local),
            # capture it so we don't call MR*() twice (once for wr, once for SET_NZ).
            if (m == 'move' and sz in (1, 2, 4)
                    and not pre and ops[0].type == M68K_OP_MEM
                    and ops[1].type == M68K_OP_REG):
                bits = sz * 8
                t    = f'_mv_{self.current_pc:x}'
                pre  = f'uint{bits}_t {t} = {s};'
                s    = t
            flags  = ''
            if m == 'move' and sz in (1, 2, 4):
                szname = '8' if sz == 1 else ('16' if sz == 2 else '32')
                flags  = f'\nRT_SET_NZ_{szname}({s});\nctx->V = ctx->C = 0;'
            # SR writes emit their own flag updates via wr()
            if (m == 'move' and ops[1].type == M68K_OP_REG
                    and reg_name(ops[1].reg) == 'SR'):
                flags = ''
            if m == 'movea':
                An   = reg_name(ops[1].reg)
                body = (f'ctx->{An} = RT_SX16({s});'
                        if sz == 2 else f'ctx->{An} = (uint32_t){s};')
            else:
                body = self.wr(ops[1], s, sz) + flags
            return (pre + '\n' + body) if pre else body

        if m == 'lea':
            aop = ops[0]
            if aop.type == M68K_OP_MEM:
                return self.wr(ops[1], self.ea(aop), 4)
            return '/* lea ??? */'

        if m in ('add', 'adda', 'addq', 'addi'):
            pre, s   = self._pre_rd(ops[0], sz)
            d        = ops[1]
            no_flags = (m == 'adda') or (
                d.type == M68K_OP_REG and M68K_REG_A0 <= d.reg <= M68K_REG_A7)
            if not no_flags:
                fszname = 32 if sz == 4 else (16 if sz == 2 else 8)
                t_old   = f'_old_{self.current_pc:x}'
                t_res   = f'_res_{self.current_pc:x}'
                body    = self._emit_rmw(d, sz, '+', s, t_old, t_res,
                                         f'RT_ADD_FLAGS_{fszname}({t_old}, {s}, {t_res});')
            elif m == 'adda':
                An    = reg_name(d.reg)
                s_ext = f'RT_SX16({s})' if sz == 2 else f'(uint32_t){s}'
                body  = f'ctx->{An} += {s_ext};'
            else:  # addq/add to An: no flags, simple increment
                An   = reg_name(d.reg)
                body = f'ctx->{An} += {s};'
            return (pre + '\n' + body) if pre else body

        if m in ('sub', 'suba', 'subq', 'subi'):
            pre, s   = self._pre_rd(ops[0], sz)
            d        = ops[1]
            no_flags = (m == 'suba') or (
                d.type == M68K_OP_REG and M68K_REG_A0 <= d.reg <= M68K_REG_A7)
            if not no_flags:
                fszname = 32 if sz == 4 else (16 if sz == 2 else 8)
                t_old   = f'_old_{self.current_pc:x}'
                t_res   = f'_res_{self.current_pc:x}'
                body    = self._emit_rmw(d, sz, '-', s, t_old, t_res,
                                         f'RT_SUB_FLAGS_{fszname}({t_old}, {s}, {t_res});')
            elif m == 'suba':
                An    = reg_name(d.reg)
                s_ext = f'RT_SX16({s})' if sz == 2 else f'(uint32_t){s}'
                body  = f'ctx->{An} -= {s_ext};'
            else:  # subq/sub to An: no flags, simple decrement
                An   = reg_name(d.reg)
                body = f'ctx->{An} -= {s};'
            return (pre + '\n' + body) if pre else body

        if m == 'clr':
            return self.wr(ops[0], '0', sz) + '\nctx->N = ctx->V = ctx->C = 0;\nctx->Z = 1;'

        if m == 'not':
            pre, v = self._pre_rd(ops[0], sz)
            bits   = sz * 8
            t      = f'_res_{self.current_pc:x}'
            body   = (f'uint{bits}_t {t} = (uint{bits}_t)(~({v}));\n'
                      f'{self.wr(ops[0], t, sz)}\n'
                      f'RT_SET_NZ_{bits}({t});\n'
                      f'ctx->V = ctx->C = 0;')
            return (pre + '\n' + body) if pre else body

        if m == 'tst':
            s     = self.rd(ops[0], sz)
            szstr = '8' if sz == 1 else ('16' if sz == 2 else '32')
            return f'RT_SET_NZ_{szstr}({s});\nctx->V = ctx->C = 0;'

        if m in ('cmp', 'cmpi', 'cmpa'):
            if m == 'cmpa':
                pre_s, s = self._pre_rd(ops[0], sz)
                An       = reg_name(ops[1].reg)
                lines    = []
                if pre_s: lines.append(pre_s)
                if sz == 2:
                    lines.append(f'{{ uint32_t _s = RT_SX16({s}); uint32_t _d = ctx->{An};')
                else:
                    lines.append(f'{{ uint32_t _s = (uint32_t)({s}); uint32_t _d = ctx->{An};')
                lines.append(f'  RT_SUB_FLAGS_32(_d, _s, (uint64_t)_d - _s); }}')
                return '\n'.join(lines)
            pre_s, s = self._pre_rd(ops[0], sz)
            pre_d, d = self._pre_rd(ops[1], sz)
            bits     = sz * 8
            lines    = []
            if pre_s: lines.append(pre_s)
            if pre_d: lines.append(pre_d)
            lines.append(f'{{ uint{bits}_t _s = (uint{bits}_t)({s}); '
                         f'uint{bits}_t _d = (uint{bits}_t)({d});')
            lines.append(f'  RT_SUB_FLAGS_{bits}(_d, _s, (uint64_t)_d - _s); }}')
            return '\n'.join(lines)

        if m == 'bra':
            return self.emit_branch(insn)

        if m in CC_MAP:
            return self.emit_branch(insn, CC_MAP[m])

        if m == 'bsr':
            t = self.br_target(insn, 2)
            return f'rt_call(ctx, 0x{t:06X}u);'

        if m == 'jsr':
            if ops[0].type == M68K_OP_IMM:
                return f'rt_call(ctx, 0x{ops[0].imm:06X}u);'
            return f'rt_call(ctx, {self.ea(ops[0])});'

        if m == 'jmp':
            return f'rt_jump(ctx, {self.ea(ops[0])});\nreturn;'

        if m == 'rts':
            return 'return;'

        if m == 'rte':
            return 'return; /* RTE */'

        # DBcc Dn,label: if (cc TRUE) fall through; else { Dn.w--; if Dn!=-1 branch }.
        # dbra/dbf = condition always false (always decrement); dbt = always true
        # (never decrement); db<cc> decrements when <cc> is FALSE.
        if m.startswith('db') and (m in ('dbra', 'dbf', 'dbt') or m[2:] in CC_MAP):
            r     = reg_name(ops[0].reg)
            t     = self.br_target(insn, 2)
            c_var = f'_c_{self.current_pc:x}'
            # Loop-back target: a label inside this function -> goto. Otherwise
            # the target lives in another translation unit (a different gfn_
            # function), so a `goto L_X` here would reference a label that
            # isn't defined in the current C function — tail rt_jump instead,
            # same fallback emit_branch uses. The runtime will either resolve
            # it via the dispatch table or rt-miss cleanly.
            if (not self.cur_insn_addrs) or (t in self.cur_insn_addrs):
                back = f'if ({c_var} != -1) goto L_{t:06X};'
            else:
                back = f'if ({c_var} != -1) {{ rt_jump(ctx, 0x{t:06X}u); return; }}'
            dec   = (f'int16_t {c_var} = (int16_t)(uint16_t)ctx->{r} - 1;\n'
                     f'ctx->{r} = (ctx->{r} & 0xFFFF0000u) | (uint16_t){c_var};\n'
                     f'{back}')
            if m in ('dbra', 'dbf'):
                return dec
            if m == 'dbt':
                return '/* dbt: cc always true, no decrement */'
            cond = CC_MAP[m[2:]]
            body = '  ' + dec.replace('\n', '\n  ')
            return f'if (!RT_CC_{cond}) {{\n{body}\n}}'

        if m == 'swap':
            r = reg_name(ops[0].reg)
            return (f'ctx->{r} = (ctx->{r} >> 16) | (ctx->{r} << 16);\n'
                    f'RT_SET_NZ_32(ctx->{r});\n'
                    f'ctx->V = ctx->C = 0;')

        if m in ('and', 'or', 'eor', 'andi', 'ori', 'eori'):
            s      = self.rd(ops[0], sz)
            d      = self.rd(ops[1], sz)
            op_sym = {'and':'&','or':'|','eor':'^','andi':'&','ori':'|','eori':'^'}[m]
            bits   = sz * 8
            t      = f'_res_{self.current_pc:x}'
            return (f'uint{bits}_t {t} = (uint{bits}_t)(({d}) {op_sym} ({s}));\n'
                    f'{self.wr(ops[1], t, sz)}\n'
                    f'RT_SET_NZ_{bits}({t});\n'
                    f'ctx->V = ctx->C = 0;')

        if m in ('bclr', 'bset', 'bchg'):
            # M68K: bit ops on a data register are LONG (bit number mod 32);
            # on memory they are BYTE (bit number mod 8).
            is_reg = (ops[1].type == M68K_OP_REG)
            szb    = 4 if is_reg else 1
            bits   = szb * 8
            modm   = 31 if is_reg else 7
            bit    = self.rd(ops[0], 1)
            dst    = self.rd(ops[1], szb)
            op_sym = {'bclr':'& ~', 'bset':'|', 'bchg':'^'}[m]
            b_var  = f'_b_{self.current_pc:x}'
            return (f'uint{bits}_t {b_var} = (uint{bits}_t)1u << (({bit}) & {modm});\n'
                    f'ctx->Z = !(({dst}) & {b_var});\n'
                    f'{self.wr(ops[1], f"(({dst}) {op_sym} {b_var})", szb)}')

        if m == 'btst':
            is_reg = (ops[1].type == M68K_OP_REG)
            szb    = 4 if is_reg else 1
            bits   = szb * 8
            modm   = 31 if is_reg else 7
            bit = self.rd(ops[0], 1)
            dst = self.rd(ops[1], szb)
            return f'ctx->Z = !(({dst}) & ((uint{bits}_t)1u << (({bit}) & {modm})));'

        if m == 'neg':
            d = self.rd(ops[0], sz)
            return self.wr(ops[0], f'(-({d}))', sz)

        if m == 'negx':
            d = self.rd(ops[0], sz)
            return self.wr(ops[0], f'(~({d}) - ctx->X)', sz)

        if m == 'addx':
            bits = sz * 8
            s = self.rd(ops[0], sz); d = self.rd(ops[1], sz)
            p = self.current_pc
            # X-extend add; Z is STICKY (cleared only if result nonzero) for
            # multi-precision chains.
            return (f'{{ uint{bits}_t _axs{p:x}=(uint{bits}_t)({s}); '
                    f'uint{bits}_t _axd{p:x}=(uint{bits}_t)({d}); '
                    f'uint64_t _axr{p:x}=(uint64_t)_axs{p:x} + _axd{p:x} + ctx->X; '
                    f'uint{bits}_t _axv{p:x}=(uint{bits}_t)_axr{p:x}; '
                    f'ctx->C = ctx->X = (_axr{p:x} >> {bits}) & 1u; '
                    f'ctx->V = ((~(_axs{p:x} ^ _axd{p:x}) & (_axd{p:x} ^ _axv{p:x})) >> {bits-1}) & 1u; '
                    f'ctx->N = (_axv{p:x} >> {bits-1}) & 1u; if (_axv{p:x}) ctx->Z = 0; '
                    f'{self.wr(ops[1], f"_axv{p:x}", sz)} }}')

        if m == 'movep':
            # Transfer alternating bytes between Dn and memory (8-bit peripheral
            # access).  sz: 2=.w (2 bytes), 4=.l (4 bytes).
            p = self.current_pc
            nbytes = sz
            if ops[0].type == M68K_OP_REG:          # Dn -> memory
                dn   = self.rd(ops[0], 4)
                addr = self.ea(ops[1])
                parts = [f'{{ uint32_t _mpa{p:x} = ({addr});']
                for i in range(nbytes):
                    shift = (nbytes - 1 - i) * 8
                    parts.append(f'MW8(_mpa{p:x} + {i*2}, (uint8_t)(({dn}) >> {shift}));')
                parts.append('}')
                return ' '.join(parts)
            else:                                   # memory -> Dn
                addr = self.ea(ops[0])
                r = reg_name(ops[1].reg)
                if nbytes == 2:
                    return (f'{{ uint32_t _mpa{p:x} = ({addr}); '
                            f'ctx->{r} = (ctx->{r} & 0xFFFF0000u) | '
                            f'((uint32_t)MR8(_mpa{p:x}) << 8) | MR8(_mpa{p:x} + 2); }}')
                return (f'{{ uint32_t _mpa{p:x} = ({addr}); '
                        f'ctx->{r} = ((uint32_t)MR8(_mpa{p:x}) << 24) | '
                        f'((uint32_t)MR8(_mpa{p:x} + 2) << 16) | '
                        f'((uint32_t)MR8(_mpa{p:x} + 4) << 8) | MR8(_mpa{p:x} + 6); }}')

        if m == 'chk':
            # Bounds-check trap: the game uses it for index validation and
            # never trips it in normal play; omit the trap.
            return '/* chk (bounds trap omitted) */'

        if m == 'mulu':
            s = self.rd(ops[0], 2)
            d = self.rd(ops[1], 4)
            t = f'_res_{self.current_pc:x}'
            return (f'uint32_t {t} = (uint32_t)((uint16_t)({s}) * (uint16_t)({d}));\n'
                    f'{self.wr(ops[1], t, 4)}\n'
                    f'RT_SET_NZ_32({t});\n'
                    f'ctx->V = ctx->C = 0;')

        if m == 'muls':
            s = self.rd(ops[0], 2)
            d = self.rd(ops[1], 4)
            t = f'_res_{self.current_pc:x}'
            return (f'uint32_t {t} = (uint32_t)((int16_t)({s}) * (int16_t)({d}));\n'
                    f'{self.wr(ops[1], t, 4)}\n'
                    f'RT_SET_NZ_32({t});\n'
                    f'ctx->V = ctx->C = 0;')

        if m in ('divs', 'divu'):
            signed = (m == 'divs')
            s  = self.rd(ops[0], 2)
            d  = self.rd(ops[1], 4)
            dn = reg_name(ops[1].reg)
            if signed:
                q_type   = 'int32_t'
                s_cast   = f'(int32_t)RT_SX16({s})'
                d_cast   = f'(int32_t)({d})'
                overflow = '_quot < -32768 || _quot > 32767'
                nz_cast  = 'int16_t'
            else:
                q_type   = 'uint32_t'
                s_cast   = f'(uint32_t)(uint16_t)({s})'
                d_cast   = f'(uint32_t)({d})'
                overflow = '_quot > 65535u'
                nz_cast  = 'uint16_t'
            return (
                f'{{ {q_type} _dvsr = {s_cast};\n'
                f'  {q_type} _dvd = {d_cast};\n'
                f'  if (_dvsr != 0) {{\n'
                f'    {q_type} _quot = _dvd / _dvsr;\n'
                f'    {q_type} _rem = _dvd % _dvsr;\n'
                f'    if ({overflow}) {{\n'
                f'      ctx->V = 1;\n'
                f'      ctx->C = 0;\n'
                f'    }} else {{\n'
                f'      ctx->{dn} = ((uint32_t)(uint16_t)_rem << 16) | (uint16_t)_quot;\n'
                f'      ctx->V = ctx->C = 0;\n'
                f'      RT_SET_NZ_16(({nz_cast})_quot);\n'
                f'    }}\n'
                f'  }}\n'
                f'}}'
            )

        if m == 'ext':
            r = reg_name(ops[0].reg)
            if sz == 4:  # ext.l: word → long
                return (f'ctx->{r} = RT_SX16(ctx->{r});\n'
                        f'RT_SET_NZ_32(ctx->{r});\n'
                        f'ctx->V = ctx->C = 0;')
            else:         # ext.w: byte → word (preserve D[31:16])
                return (f'ctx->{r} = (ctx->{r} & 0xFFFF0000u) | (uint16_t)RT_SX8W(ctx->{r});\n'
                        f'RT_SET_NZ_16(RT_SX8W(ctx->{r}));\n'
                        f'ctx->V = ctx->C = 0;')

        if m == 'extb':
            r = reg_name(ops[0].reg)
            return (f'ctx->{r} = RT_SX8(ctx->{r});\n'
                    f'RT_SET_NZ_32(ctx->{r});\n'
                    f'ctx->V = ctx->C = 0;')

        if m == 'rol' or m == 'ror':
            pre_d, d = self._pre_rd(ops[1], sz)
            s        = self.rd(ops[0], sz)
            bits     = sz * 8
            t        = f'_rot_{self.current_pc:x}'
            sh       = f'_sh_{self.current_pc:x}'
            if m == 'rol':
                rot = f'{t} = (uint{bits}_t)(({t} << {sh}) | ({t} >> ({bits} - {sh}))); ctx->C = {t} & 1u;'
            else:  # ror
                rot = f'{t} = (uint{bits}_t)(({t} >> {sh}) | ({t} << ({bits} - {sh}))); ctx->C = ({t} >> {bits-1}) & 1u;'
            body = (f'{{ uint{bits}_t {t} = (uint{bits}_t)({d}); '
                    f'unsigned {sh} = (unsigned)({s}) & {bits - 1}u;\n'
                    f'  if ({sh}) {{ {rot} }}\n'
                    f'  {self.wr(ops[1], t, sz)}\n'
                    f'  RT_SET_NZ_{bits}({t}); ctx->V = 0; }}')
            return (pre_d + '\n' + body) if pre_d else body

        if m == 'lsl':
            pre_d, d = self._pre_rd(ops[1], sz)
            s        = self.rd(ops[0], sz)
            bits     = sz * 8
            t        = f'_res_{self.current_pc:x}'
            body     = (f'uint{bits}_t {t} = (uint{bits}_t)(({d}) << ({s}));\n'
                        f'{self.wr(ops[1], t, sz)}\n'
                        f'RT_SET_NZ_{bits}({t});\n'
                        f'ctx->V = 0;')
            return (pre_d + '\n' + body) if pre_d else body

        if m == 'lsr':
            pre_d, d = self._pre_rd(ops[1], sz)
            s        = self.rd(ops[0], sz)
            bits     = sz * 8
            t        = f'_res_{self.current_pc:x}'
            # Mask destination to operation size BEFORE shifting: lsr.b/.w must
            # only shift the low byte/word, otherwise upper register bits leak
            # down into the result.
            body     = (f'uint{bits}_t {t} = (uint{bits}_t)((uint{bits}_t)({d}) >> ({s}));\n'
                        f'{self.wr(ops[1], t, sz)}\n'
                        f'RT_SET_NZ_{bits}({t});\n'
                        f'ctx->V = 0;')
            return (pre_d + '\n' + body) if pre_d else body

        if m == 'asl':
            d = self.rd(ops[1], sz); s = self.rd(ops[0], sz)
            return self.wr(ops[1], f'(({d}) << ({s}))', sz)

        if m == 'asr':
            d = self.rd(ops[1], sz); s = self.rd(ops[0], sz)
            t = f'(int{sz*8}_t)({d}) >> ({s})'
            return self.wr(ops[1], f'(uint{sz*8}_t)({t})', sz)

        if m == 'movem':
            M68K_REG_BITS_TYPE = 6
            if ops[0].type == M68K_REG_BITS_TYPE:
                reg_bits = ops[0].register_bits
                ea_op    = ops[1]
                is_load  = False
            else:
                ea_op    = ops[0]
                reg_bits = ops[1].register_bits
                is_load  = True

            regs = []
            for bit in range(16):
                if reg_bits & (1 << bit):
                    regs.append(('A' if bit >= 8 else 'D',
                                 bit - 8 if bit >= 8 else bit))

            if not regs:
                return '/* movem: empty reglist */'

            mode  = getattr(ea_op, 'address_mode', 0)
            lines = []

            if is_load:
                if mode == 4:  # (An)+
                    an = ea_op.imm - 9
                    for r, idx in regs:
                        if sz == 4:
                            lines.append(f'ctx->{r}[{idx}] = MR32(ctx->A[{an}]);\n'
                                         f'ctx->A[{an}] += 4u;')
                        else:
                            lines.append(f'ctx->{r}[{idx}] = RT_SX16(MR16(ctx->A[{an}]));\n'
                                         f'ctx->A[{an}] += 2u;')
                else:
                    base = self.ea(ea_op, sz)
                    vn   = f'_movem_{self.current_pc:x}'
                    lines.append(f'{{ uint32_t {vn} = {base};')
                    for r, idx in regs:
                        if sz == 4:
                            lines.append(f'  ctx->{r}[{idx}] = MR32({vn});\n  {vn} += 4u;')
                        else:
                            lines.append(f'  ctx->{r}[{idx}] = RT_SX16(MR16({vn}));\n  {vn} += 2u;')
                    lines.append('}')
            else:  # store
                if mode == 5:  # -(An)
                    an = ea_op.imm - 9
                    for r, idx in reversed(regs):
                        lines.append(f'ctx->A[{an}] -= {sz}u;\n'
                                     f'MW{sz*8}(ctx->A[{an}], ctx->{r}[{idx}]);')
                else:
                    base = self.ea(ea_op, sz)
                    vn   = f'_movem_{self.current_pc:x}'
                    lines.append(f'{{ uint32_t {vn} = {base};')
                    for r, idx in regs:
                        if sz == 4:
                            lines.append(f'  MW32({vn}, ctx->{r}[{idx}]);\n  {vn} += 4u;')
                        else:
                            lines.append(f'  MW16({vn}, (uint16_t)ctx->{r}[{idx}]);\n  {vn} += 2u;')
                    lines.append('}')

            return '\n'.join(lines)

        if m == 'link':
            r    = reg_name(ops[0].reg)
            disp = ops[1].imm if ops[1].type == M68K_OP_IMM else 0
            return (f'rt_push32(ctx, ctx->{r});\n'
                    f'ctx->{r} = ctx->A[7];\n'
                    f'ctx->A[7] -= {abs(disp)}u;')

        if m == 'unlk':
            r = reg_name(ops[0].reg)
            return f'ctx->A[7] = ctx->{r};\nctx->{r} = rt_pop32(ctx);'

        if m == 'pea':
            aop = ops[0]
            if aop.type == M68K_OP_MEM:
                return f'rt_push32(ctx, {self.ea(aop)});'
            return '/* pea ??? */'

        if m == 'exg':
            r1    = reg_name(ops[0].reg)
            r2    = reg_name(ops[1].reg)
            t_var = f'_exg_{self.current_pc:x}'
            return (f'uint32_t {t_var} = ctx->{r1};\n'
                    f'ctx->{r1} = ctx->{r2};\n'
                    f'ctx->{r2} = {t_var};')

        if m == 'trap':
            v = ops[0].imm if ops[0].type == M68K_OP_IMM else 0
            return f'rt_trap(ctx, {v});'

        if m == 'stop':
            return 'hw_running = 0; /* STOP */'

        # ── Scc / ST / SF — set byte EA to $FF if condition true, else $00 ──
        # (Scc is always byte-sized; targets a data register or memory.)
        SCC = {'st':None, 'sf':False, 'shi':'HI', 'sls':'LS', 'scc':'CC',
               'shs':'CC', 'scs':'CS', 'slo':'CS', 'sne':'NE', 'seq':'EQ',
               'svc':'VC', 'svs':'VS', 'spl':'PL', 'smi':'MI', 'sge':'GE',
               'slt':'LT', 'sgt':'GT', 'sle':'LE'}
        if m in SCC:
            cc = SCC[m]
            if m == 'st':      val = '0xFFu'
            elif m == 'sf':    val = '0x00u'
            else:              val = f'(RT_CC_{cc} ? 0xFFu : 0x00u)'
            dst = ops[0]
            if dst.type == M68K_OP_REG:
                r = reg_name(dst.reg)
                return f'ctx->{r} = (ctx->{r} & 0xFFFFFF00u) | {val};'
            return f'MW8({self.ea(dst, 1)}, (uint8_t)({val}));'

        return f'/* UNK: {insn.mnemonic} {insn.op_str} */'


# ---------------------------------------------------------------------------
# Pattern detection (multi-instruction analysis)
# ---------------------------------------------------------------------------

def _find_patterns(insns):
    """Pre-scan instruction list to detect high-level patterns.

    Returns three dicts/sets:
      skip_insns : set[addr]        — addresses to not emit at all
      pre_label  : dict[addr->str]  — text to emit before the label at addr
      override   : dict[addr->str]  — emit this string instead of translation
                                      (None means emit nothing)
    """
    skip_insns = set()
    pre_label  = {}
    override   = {}

    n = len(insns)
    addr_to_idx = {ins.address: i for i, ins in enumerate(insns)}

    def get_target(insn):
        try:
            for op in insn.operands:
                if op.type == M68K_OP_BR_DISP:
                    return insn.address + 2 + op.br_disp.disp
        except Exception:
            pass
        return None

    def mnem0(insn):
        return insn.mnemonic.lower().split('.')[0]

    def is_bltbusy_btst(insn):
        try:
            if mnem0(insn) != 'btst':
                return False
            ops = insn.operands
            if len(ops) < 2 or ops[0].type != M68K_OP_IMM or ops[0].imm != 6:
                return False
            if ops[1].type != M68K_OP_MEM:
                return False
            mode = getattr(ops[1], 'address_mode', 0)
            # Mode 3 = (An) direct: Capstone stores register in op.imm, not mem.base_reg
            if mode == 3:
                return ops[1].imm == M68K_REG_A6
            return mem_base(ops[1]) == M68K_REG_A6 and ops[1].mem.disp == 0
        except Exception:
            return False

    def is_fire_wait_tst(insn):
        # The post-level-complete "press fire to continue" idiom is
        #   tst.b $bfe001.l   ; CIA-A PRA byte; bit7 = /FIR1 (active-low)
        #   bmi/bpl  self     ; loop while fire NOT pressed / IS pressed
        # On real OCS the busy-loop is fine; in our coroutine model it never
        # yields so the input layer can't update fire state and the watchdog
        # fires. Fold the self-loop into hw_wait_fire(...) which yields a frame
        # per check (same approach as hw_vblank_wait for the VPOSR pairs).
        try:
            if mnem0(insn) != 'tst':
                return False
            ops = insn.operands
            if len(ops) < 1 or ops[0].type != M68K_OP_MEM:
                return False
            mode = getattr(ops[0], 'address_mode', 0)
            return mode in (16, 17) and ops[0].imm == 0xBFE001
        except Exception:
            return False

    def is_vposr_btst(insn):
        # The vblank-sync idiom is `btst #0, d(a6)` testing the VPOSR frame-parity
        # byte, used in tight (wait-set / wait-clear) pairs == "wait one frame".
        # The intro/title code reads it at $3(a6); the gameplay-engine bank reads
        # the same parity at $5(a6). Both are the frame-parity byte of VPOSR
        # ($DFF004) as the hw layer models it — accept either so BOTH banks get
        # the spin folded into a clean hw_vblank_wait() (a coroutine frame-yield)
        # instead of a busy-loop the hw layer has to time-guess.
        try:
            if mnem0(insn) != 'btst':
                return False
            ops = insn.operands
            if len(ops) < 2 or ops[0].type != M68K_OP_IMM or ops[0].imm != 0:
                return False
            if ops[1].type != M68K_OP_MEM:
                return False
            return mem_base(ops[1]) == M68K_REG_A6 and ops[1].mem.disp in (3, 5)
        except Exception:
            return False

    # Build set of all branch targets so we can detect labels with multiple refs
    all_targets_by_source = {}  # target_addr -> list of source insn indices
    for idx, insn in enumerate(insns):
        t = get_target(insn)
        if t is not None:
            all_targets_by_source.setdefault(t, []).append(idx)

    i = 0
    while i < n:
        insn = insns[i]
        m    = mnem0(insn)

        # ── Blitter busy wait: btst #6,(a6) ; bne L_self ─────────────────
        if is_bltbusy_btst(insn) and i + 1 < n:
            nxt = insns[i + 1]
            if mnem0(nxt) == 'bne' and get_target(nxt) == insn.address:
                override[insn.address] = 'hw_blitter_sync();'
                skip_insns.add(nxt.address)
                i += 2
                continue

        # ── Fire-wait self-loop: tst.b $bfe001; b(mi|pl) self ────────────
        # bmi self  = loop while bit7=1 = fire NOT pressed → wait FOR press
        # bpl self  = loop while bit7=0 = fire IS pressed  → wait FOR release
        if is_fire_wait_tst(insn) and i + 1 < n:
            nxt = insns[i + 1]
            nm  = mnem0(nxt)
            if nm in ('bmi', 'bpl') and get_target(nxt) == insn.address:
                want = '1' if nm == 'bmi' else '0'
                override[insn.address] = f'hw_wait_fire({want});'
                skip_insns.add(nxt.address)
                i += 2
                continue

        # ── VPOSR vblank wait ─────────────────────────────────────────────
        # Full pair: (btst vposr; beq self) + (btst vposr; bne self) → hw_vblank_wait()
        # Single:    (btst vposr; beq/bne self)                       → suppress
        if is_vposr_btst(insn) and i + 1 < n:
            nxt  = insns[i + 1]
            nm   = mnem0(nxt)
            tgt  = get_target(nxt)
            if tgt == insn.address and nm in ('beq', 'bne'):
                if nm == 'beq' and i + 3 < n:
                    n2, n3 = insns[i + 2], insns[i + 3]
                    if (is_vposr_btst(n2) and mnem0(n3) == 'bne'
                            and get_target(n3) == n2.address):
                        # Full vblank wait pair
                        override[insn.address] = 'hw_vblank_wait();'
                        skip_insns.update([nxt.address, n2.address, n3.address])
                        i += 4
                        continue
                # Single self-loop
                override[insn.address] = None   # suppress both insns
                skip_insns.add(nxt.address)
                i += 2
                continue

        # ── DBRA / DBF loop → C for loop ─────────────────────────────────
        # COSMETIC ONLY: folds a simple dbra countdown into a C for-loop for
        # readability. The default DBcc emitter (translate(), goto-based) is
        # always correct, so folding is OPT-IN via RECOMP_FOLD_LOOPS=1. It is
        # off by default because a folded C for-loop can only be entered at its
        # head — when loops partially overlap or a forward branch lands in a
        # folded body, the braces desync and the output won't compile (seen in
        # the gameplay engine's $59Dxxx/$59Exxx functions).
        # Conditions for conversion:
        #   • target label is inside this function
        #   • loop body has no branch instructions (simple linear body)
        #   • target label is only referenced by this single DBRA (safe goto→for)
        if os.environ.get('RECOMP_FOLD_LOOPS') and m in ('dbra', 'dbf'):
            target = get_target(insn)
            if target and target in addr_to_idx:
                start_idx = addr_to_idx[target]
                body      = insns[start_idx:i]
                BRANCH_MNEMS = frozenset({
                    'bra','beq','bne','blt','bgt','ble','bge','bcc','bcs',
                    'bhi','bls','bvc','bvs','bmi','bpl','bsr','jsr','jmp',
                    'rts','rte','dbra','dbf','dbcc',
                })
                body_clean = not any(mnem0(b) in BRANCH_MNEMS or mnem0(b).startswith('db')
                                     for b in body)
                # Ensure only this DBRA targets the label
                only_ref = (all_targets_by_source.get(target, []) == [i])
                # The loop body must have NO other branch targets — otherwise a
                # goto/branch from outside would jump into the C for-loop scope,
                # producing mismatched braces (a folded C for-loop can only be
                # entered at its top). Check every body insn after the head.
                no_inner_target = not any(b.address in all_targets_by_source
                                          for b in body[1:])
                # Also: no branch target inside the function may lie strictly
                # between the loop head and the dbra from OUTSIDE this range, and
                # no source outside the loop range may branch into the body. A
                # folded C for-loop can only be safely entered at its head; any
                # other entry (or a fold that partially overlaps another fold)
                # corrupts brace nesting. Require the loop body to be a maximal
                # straight-line region whose only referenced label is the head.
                lo_addr, hi_addr = target, insn.address
                safe_range = True
                for tgt_addr, srcs in all_targets_by_source.items():
                    if lo_addr < tgt_addr <= hi_addr:
                        safe_range = False; break   # label inside body (besides head)
                    if tgt_addr == lo_addr and srcs != [i]:
                        safe_range = False; break   # head entered from elsewhere too
                if body_clean and only_ref and no_inner_target and safe_range:
                    dn_reg = reg_name(insn.operands[0].reg)
                    count  = None
                    if start_idx > 0:
                        pi = insns[start_idx - 1]
                        if (mnem0(pi) == 'moveq'
                                and len(pi.operands) >= 2
                                and reg_name(pi.operands[1].reg) == dn_reg
                                and pi.address not in skip_insns
                                and pi.address not in override):
                            v = pi.operands[0].imm & 0xFF
                            if v >= 0x80:
                                v -= 0x100
                            count = v
                            skip_insns.add(pi.address)
                    lv = f'_l{target:x}'
                    init = str(count) if count is not None else f'(int16_t)(uint16_t)ctx->{dn_reg}'
                    pre_label[target] = f'for (int {lv} = {init}; {lv} >= 0; {lv}--) {{'
                    override[insn.address] = '}'

        i += 1

    return skip_insns, pre_label, override


# ---------------------------------------------------------------------------
# Function emitter
# ---------------------------------------------------------------------------

def emit_func(c, addr, insns, translator, cname=None):
    """Write one translated C function to the file object `c`.

    `cname(addr) -> str` resolves the C symbol suffix (after 'gfn_').  Defaults
    to the curated/hex name from entries.py.  A separate bank (e.g. the gameplay
    overlay) passes a prefixing resolver so its symbols don't collide.
    """
    name = cname(addr) if cname else get_fn_name(addr)

    # Collect branch targets within this function.
    branch_targets = set()
    for insn in insns:
        try:
            for op in insn.operands:
                if op.type == M68K_OP_BR_DISP:
                    branch_targets.add(insn.address + 2 + op.br_disp.disp)
        except Exception:
            pass
    insn_addrs      = {insn.address for insn in insns}
    branch_targets &= insn_addrs
    translator.cur_insn_addrs = insn_addrs

    skip_insns, pre_label, override = _find_patterns(insns)

    c.write(f'/* ${addr:06X} */\n')
    c.write(f'void gfn_{name}(M68KCtx *ctx) {{\n')

    for insn in insns:
        a = insn.address

        if a in skip_insns:
            # A pattern-folded instruction can still be a branch/dbra target
            # (e.g. the head of an inner copy loop re-entered by an outer dbra).
            # Emit its label before skipping so the goto resolves; control then
            # falls into the folded construct, which re-runs the inner loop.
            if a in branch_targets:
                c.write(f'\nL_{a:06X}: ;\n')
            continue

        # Emit for-loop opener / other pre-label text before the label line.
        if a in pre_label:
            c.write(f'  {pre_label[a]}\n')

        if a in branch_targets:
            c.write(f'\nL_{a:06X}:\n')

        # Override: emit replacement body (or nothing if None) and move on.
        if a in override:
            body = override[a]
            if body is not None:
                c.write(f'  {body}\n')
            continue

        try:
            ops = insn.operands  # noqa: F841
        except Exception:
            c.write(f'  /* {a:06X}: data */\n')
            continue

        mnem = insn.mnemonic.replace('\\"', '"').replace('\\n', ' ')
        c.write(f'  /* {a:06X}: {mnem} {insn.op_str} */\n')
        c.write(f'  RT_TRACE_INSN(0x{a:06X}u, "{mnem}");\n')

        text = translator.translate(insn)
        if text:
            for line in text.split('\n'):
                if line.strip():
                    c.write(f'  {line}\n')

    c.write('}\n\n')
