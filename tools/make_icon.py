"""Готовит assets/icon.ico из той же картинки, что рисует приложение.

Запускается при сборке exe. Файл .ico собираем вручную из PNG-кадров: так
результат не зависит от того, есть ли в сборке Qt-плагин записи ICO.
"""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QBuffer, QByteArray  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from avitowatcher.ui.icons import _draw_logo  # noqa: E402

SIZES = (16, 24, 32, 48, 64, 128, 256)


def png_bytes(size: int) -> bytes:
    # QBuffer не владеет массивом: временный QByteArray успел бы исчезнуть
    # до записи, поэтому держим его в переменной, пока идёт сохранение.
    storage = QByteArray()
    buffer = QBuffer(storage)
    buffer.open(QBuffer.WriteOnly)
    _draw_logo(size, alert=False).save(buffer, "PNG")
    buffer.close()
    return bytes(storage)


def build_ico(path: Path) -> None:
    frames = [(size, png_bytes(size)) for size in SIZES]
    header = struct.pack("<HHH", 0, 1, len(frames))
    directory = b""
    offset = len(header) + 16 * len(frames)
    payload = b""
    for size, data in frames:
        # В каталоге ICO размер 256 записывается нулём.
        directory += struct.pack(
            "<BBBBHHII",
            size if size < 256 else 0,
            size if size < 256 else 0,
            0, 0, 1, 32, len(data), offset,
        )
        payload += data
        offset += len(data)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + directory + payload)


def main() -> int:
    app = QApplication(sys.argv)  # noqa: F841 — нужен для отрисовки QPixmap
    target = ROOT / "assets" / "icon.ico"
    build_ico(target)
    print(f"Иконка записана: {target} ({target.stat().st_size} байт)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
