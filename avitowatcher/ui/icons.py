"""Иконки приложения. Рисуются кодом, чтобы в exe не тащить бинарные файлы."""
from __future__ import annotations

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import (
    QBrush,
    QColor,
    QFont,
    QIcon,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
)

ACCENT = QColor("#00aaff")
ACCENT_DARK = QColor("#0077cc")
ALERT = QColor("#ff5a3c")

_cache: dict[tuple[int, bool], QIcon] = {}


def _draw_logo(size: int, alert: bool) -> QPixmap:
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.Antialiasing)

    # Скруглённый квадрат с градиентом.
    gradient = QLinearGradient(0, 0, size, size)
    gradient.setColorAt(0.0, ACCENT)
    gradient.setColorAt(1.0, ACCENT_DARK)
    body = QRectF(size * 0.04, size * 0.04, size * 0.92, size * 0.92)
    path = QPainterPath()
    path.addRoundedRect(body, size * 0.24, size * 0.24)
    painter.fillPath(path, QBrush(gradient))

    # Лупа — приложение «высматривает» объявления.
    pen = QPen(QColor("#ffffff"), max(2.0, size * 0.085), Qt.SolidLine, Qt.RoundCap)
    painter.setPen(pen)
    painter.setBrush(Qt.NoBrush)
    lens = QRectF(size * 0.24, size * 0.22, size * 0.42, size * 0.42)
    painter.drawEllipse(lens)
    painter.drawLine(
        int(lens.center().x() + lens.width() * 0.38),
        int(lens.center().y() + lens.height() * 0.38),
        int(size * 0.76),
        int(size * 0.76),
    )

    if alert:
        radius = size * 0.17
        painter.setPen(Qt.NoPen)
        painter.setBrush(ALERT)
        painter.drawEllipse(QRectF(size - radius * 2.1, 0, radius * 2, radius * 2))

    painter.end()
    return pixmap


def app_icon(alert: bool = False) -> QIcon:
    key = (0, alert)
    if key in _cache:
        return _cache[key]
    icon = QIcon()
    for size in (16, 24, 32, 48, 64, 128, 256):
        icon.addPixmap(_draw_logo(size, alert))
    _cache[key] = icon
    return icon


def placeholder_pixmap(width: int, height: int) -> QPixmap:
    """Заглушка на месте картинки объявления, пока она грузится."""
    pixmap = QPixmap(width, height)
    pixmap.fill(QColor("#2a2f36"))
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.Antialiasing)
    painter.setPen(QPen(QColor("#4a525c")))
    font = QFont()
    font.setPointSize(max(7, height // 8))
    painter.setFont(font)
    painter.drawText(pixmap.rect(), Qt.AlignCenter, "нет фото")
    painter.end()
    return pixmap
