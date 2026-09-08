from __future__ import annotations

from pathlib import Path

from tools.disk_identity import EXPECTED_DISKS


def browse_for_disks(initial_dir: Path) -> tuple[Path, Path, Path]:
    try:
        import tkinter as tk
        from tkinter import filedialog
    except ImportError as error:
        raise RuntimeError("the desktop disk browser requires Python Tk support") from error

    root = tk.Tk()
    root.withdraw()
    try:
        selected = filedialog.askopenfilenames(
            title="Select Benefactor Disk.1, Disk.2, and Disk.3",
            initialdir=str(initial_dir),
            filetypes=(("Amiga disk files", "*"), ("All files", "*.*")),
        )
    finally:
        root.destroy()

    by_name = {Path(path).name: Path(path).resolve() for path in selected}
    expected_names = tuple(disk.name for disk in EXPECTED_DISKS)
    if set(by_name) != set(expected_names):
        raise ValueError("select exactly one Disk.1, Disk.2, and Disk.3 file")
    return tuple(by_name[name] for name in expected_names)  # type: ignore[return-value]
