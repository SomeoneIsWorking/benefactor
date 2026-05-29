#!/usr/bin/env python3
"""Post-process generated game.c:
Replace VPOSR poll loops with hw_vsync(), BZERO waits with hw_blitter_sync()."""
import re, sys

def patch_file(path):
    with open(path) as f:
        text = f.read()

    changes = 0

    # Pattern: VPOSR bit 0 wait (btst.b #$0, $3(a6))
    # Matches the generated C pattern for:
    #   L_XXXX: btst.b #0, $3(a6) / beq L_XXXX
    #   L_XXXX: btst.b #0, $3(a6) / bne L_XXXX
    def replace_vposr(m):
        nonlocal changes
        addr = m.group(1)
        inst = m.group(2)
        addr2 = m.group(3)
        inst2 = m.group(4)
        # Verify it's a VPOSR wait pattern
        if 'ctx->A[6] + 0x3u' in inst and 'ctx->A[6] + 0x3u' in inst2:
            changes += 1
            return f'  /* VPOSR wait replaced */\n  hw_vsync(ctx);\n'
        return m.group(0)

    # btst.b #$0, $3(a6) + beq -> VPOSR sync (wait for bit 0=1)
    text = re.sub(
        r'L_([0-9A-Fa-f]{6}):;\n'
        r'  (/\\* [0-9A-Fa-f]{6}: btst\.b #\$.+ \\*/)\n'
        r'  (ctx->Z = !\(\(MR8.+ctx->A\[6\].+\)\).+\);\n)'
        r'  (/\\* [0-9A-Fa-f]{6}: beq.b.*\\*/)\n'
        r'  if \(RT_CC_EQ\) goto (L_[0-9A-Fa-f]{6});',
        replace_vposr, text
    )

    # btst.b #$0, $3(a6) + bne -> VPOSR sync (wait for bit 0=0)
    text = re.sub(
        r'((?:L_[0-9A-Fa-f]{6}):;\n)'
        r'  (/\\* [0-9A-Fa-f]{6}: btst\.b #\$.+ \\*/)\n'
        r'  (ctx->Z = !\(\(MR8.+ctx->A\[6\].+\)\).+\);\n)'
        r'  (/\\* [0-9A-Fa-f]{6}: bne\.b \$\w+ \\*/)\n'
        r'  if \(RT_CC_NE\) goto (\1\2);',
        replace_vposr, text
    )

    # btst.b #$6, (a6) + bne -> BZERO wait (blitter done)
    text = re.sub(
        r'L_([0-9A-Fa-f]{6}):;\n'
        r'  (/\\* [0-9A-Fa-f]{6}: btst\.b #\$6, \(a6\) \\*/)\n'
        r'  (ctx->Z = !\(\(MR8\(\(uint32_t\)\(ctx->A\[6\]\)\)\)[^;]+;)\n'
        r'  (/\\* [0-9A-Fa-f]{6}: bne\.b \$\w+ \\*/)\n'
        r'  if \(RT_CC_NE\) goto L_[0-9A-Fa-f]{6};',
        lambda m: (_setattr(sys.modules[__name__],'changes',changes+1) or '') + '  hw_blitter_wait(ctx);\n',
        text
    )

    with open(path, 'w') as f:
        f.write(text)

    print(f"[patch] {changes} loops replaced in {path}" if changes else f"[patch] no loops found")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        # Default: patch generated game.c
        import os
        p = os.path.join(os.path.dirname(__file__), '..', 'src', 'generated', 'game.c')
        if os.path.exists(p):
            patch_file(p)
    else:
        patch_file(sys.argv[1])
