from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AMIGAPORT = ROOT.parent / "shared" / "amigaport"
SCRATCH = ROOT / "scratch"
HARNESS_ACTIVITY = SCRATCH / "harness-puae"
DISK_NAMES = ("Disk.1", "Disk.2", "Disk.3")
