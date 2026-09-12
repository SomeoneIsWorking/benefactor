import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AMIGAPORT = ROOT.parent / "shared" / "amigaport"


def amigaport_dir() -> Path:
    configured = os.environ.get("BENEFACTOR_AMIGAPORT_DIR")
    return Path(configured).expanduser().resolve() if configured else AMIGAPORT


SCRATCH = ROOT / "scratch"
HARNESS_ACTIVITY = SCRATCH / "harness-puae"
DISK_NAMES = ("Disk.1", "Disk.2", "Disk.3")
