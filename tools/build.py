"""Сборка dist\\AvitoWatcher.exe одной командой."""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(args: list[str]) -> None:
    print(">", " ".join(args))
    result = subprocess.run(args, cwd=ROOT)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main() -> int:
    for folder in ("build", "dist"):
        shutil.rmtree(ROOT / folder, ignore_errors=True)

    run([sys.executable, str(ROOT / "tools" / "make_icon.py")])
    run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean",
         str(ROOT / "AvitoWatcher.spec")])

    exe = ROOT / "dist" / "AvitoWatcher.exe"
    if not exe.exists():
        print("Сборка не дала exe-файла")
        return 1
    print(f"\nГотово: {exe} ({exe.stat().st_size / 1024 / 1024:.1f} МБ)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
