#!/usr/bin/env python3
"""Loader for the structured gpl seed tree (tools/recomp/seeds/).

Replaces the old flat gpl_seeds.txt. Seeds are split into per-subsystem files
so the entry list is navigable and the RE map lives next to the addresses.

File format (one entry per token-run on a line; '#' starts a comment):

    577000  game_overlay_entry   # SR=$2700; build copper; install vectors
    586E40                       # bare address, no name yet
    596212 596224 596236         # several bare addresses on one line

A token of 4-6 hex digits is an ADDRESS; any other token is the NAME of the
most recently seen address (so each address may carry one optional symbol name).
Names flow into the generated symbol (gfn_gpl_<addr>_<name>) to help map the
engine — see recomp.py. Bare addresses keep the plain gfn_gpl_<addr> name.

load_seed_tree(root) -> (sorted_addrs, {addr: name}).
"""
import os, re, glob

_HEX = re.compile(r'^[0-9A-Fa-f]{4,6}$')
_IDENT = re.compile(r'[^0-9A-Za-z_]')


def _sanitize(name):
    """Make a name safe as a C identifier fragment."""
    n = _IDENT.sub('_', name.strip()).strip('_')
    return n.lower() or None


def load_seed_tree(root):
    """Read every *.txt under `root`. Returns (sorted address list, name map)."""
    addrs = set()
    names = {}
    for path in sorted(glob.glob(os.path.join(root, '**', '*.txt'), recursive=True)):
        with open(path) as f:
            for line in f:
                line = line.split('#', 1)[0].strip()
                if not line:
                    continue
                last = None
                for tok in line.split():
                    if _HEX.match(tok):
                        last = int(tok, 16)
                        addrs.add(last)
                    elif last is not None:
                        nm = _sanitize(tok)
                        if nm and last not in names:
                            names[last] = nm
                        last = None   # one name per address
    return sorted(addrs), names


if __name__ == '__main__':
    import sys
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'seeds')
    a, n = load_seed_tree(root)
    print(f'{len(a)} addresses, {len(n)} named, from {root}')
    for addr in a:
        if addr in n:
            print(f'  {addr:06X} {n[addr]}')
