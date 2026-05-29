#!/usr/bin/env python3
"""
Test harness: runs the game for N frames, optionally sends input,
dumps memory regions for comparison.

Usage:
  python3 test_input.py [--press] [--frames N] [--dump ADDR:SIZE ...]

Examples:
  # Run 10 frames, dump $30C8-$30D6 (dispatch table) and $41A2 (toggle flag)
  python3 test_input.py --frames 10 --dump 30C8:14 41A2:2

  # Press fire after 5 frames, run 10 more, dump dispatch table
  python3 test_input.py --press 5 --frames 15 --dump 30C8:14

Requires: the benefactor-pc binary built with hw_set_fire/hw_set_mouse_lmb
"""

import subprocess
import sys
import os
import struct
import tempfile

BINARY = os.path.join(os.path.dirname(__file__), '..', 'build', 'benefactor-pc')
CHIP_DUMP = os.path.join(os.path.dirname(__file__), '..', '..', 'chip_ram_dump.bin')
DISKS = [
    os.path.join(os.path.dirname(__file__), '..', 'whdload', 'Benefactor', 'Disk.1'),
    os.path.join(os.path.dirname(__file__), '..', 'whdload', 'Benefactor', 'Disk.2'),
    os.path.join(os.path.dirname(__file__), '..', 'whdload', 'Benefactor', 'Disk.3'),
]

def dump_state(frames, press_after=-1, dump_addrs=None):
    """Run game for `frames` frames, optionally press fire after `press_after` frames,
    dump memory at `dump_addrs` (list of (addr, size) tuples) before and after."""
    
    env = os.environ.copy()
    env['BENEFACTOR_TEST'] = str(frames)

    # Use SDL dummy driver to avoid needing a display
    env['SDL_VIDEODRIVER'] = 'dummy'
    
    if press_after >= 0:
        env['BENEFACTOR_PRESS'] = str(press_after)

    result = subprocess.run(
        [BINARY, CHIP_DUMP] + DISKS,
        capture_output=True, text=True, timeout=30, env=env
    )
    return result.stdout + result.stderr

def main():
    frames = 10
    press_after = -1
    dump_addrs = []
    
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == '--frames':
            i += 1
            frames = int(args[i])
        elif args[i] == '--press':
            i += 1
            press_after = int(args[i])
        elif args[i] == '--dump':
            i += 1
            parts = args[i].split(':')
            addr = int(parts[0], 16)
            size = int(parts[1]) if len(parts) > 1 else 16
            dump_addrs.append((addr, size))
        else:
            print(f"Unknown arg: {args[i]}")
            sys.exit(1)
        i += 1
    
    print(f"Running {frames} frames, press after={press_after}, dumps: {[hex(a) for a,_ in dump_addrs]}")
    print("=" * 60)
    output = dump_state(frames, press_after, dump_addrs)
    
    # Extract dump lines from output (we'd need the game to print them)
    # For now, just show game output
    for line in output.split('\n'):
        if 'frame' in line or 'BLIT' in line or 'PRA' in line or 'BPLCON0' in line or 'BPL' in line:
            print(line)

if __name__ == '__main__':
    main()
