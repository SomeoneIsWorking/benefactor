#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from tools.disk_identity import validate_disk_set


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate the exact Benefactor disk set")
    parser.add_argument("disks", nargs=3, type=Path, metavar="DISK")
    args = parser.parse_args()
    try:
        validate_disk_set((args.disks[0], args.disks[1], args.disks[2]))
    except ValueError as error:
        parser.error(str(error))
    print("Benefactor disk identity verified: 3/3 exact images")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
