"""Фоновая загрузка картинок объявлений с кэшем на диске."""
from __future__ import annotations

import hashlib
import logging

import requests
from PySide6.QtCore import QObject, QRunnable, Qt, QThreadPool, Signal
from PySide6.QtGui import QPixmap

from ..config import store
from ..paths import cache_dir

log = logging.getLogger(__name__)

THUMB_W, THUMB_H = 128, 96


class _Signals(QObject):
    done = Signal(str, QPixmap)   # url, готовая миниатюра
    failed = Signal(str)


class _Job(QRunnable):
    def __init__(self, url: str, signals: _Signals) -> None:
        super().__init__()
        self._url = url
        self._signals = signals
        self.setAutoDelete(True)

    def run(self) -> None:
        try:
            path = cache_dir() / "img" / (
                hashlib.sha1(self._url.encode("utf-8")).hexdigest() + ".img"
            )
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.stat().st_size == 0:
                settings = store.current
                proxies = (
                    {"http": settings.proxy, "https": settings.proxy}
                    if settings.proxy else None
                )
                response = requests.get(self._url, timeout=20, proxies=proxies)
                response.raise_for_status()
                path.write_bytes(response.content)

            pixmap = QPixmap()
            if not pixmap.loadFromData(path.read_bytes()):
                self._signals.failed.emit(self._url)
                return
            thumb = pixmap.scaled(
                THUMB_W, THUMB_H,
                Qt.KeepAspectRatioByExpanding, Qt.SmoothTransformation,
            )
            self._signals.done.emit(self._url, thumb)
        except Exception as exc:
            log.debug("Картинка не загрузилась (%s): %s", self._url, exc)
            self._signals.failed.emit(self._url)


class ImageLoader(QObject):
    """Отдаёт миниатюру сразу из памяти или догружает её в фоне."""

    ready = Signal(str, QPixmap)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._memory: dict[str, QPixmap] = {}
        self._pending: set[str] = set()
        self._pool = QThreadPool(self)
        self._pool.setMaxThreadCount(4)
        self._signals = _Signals(self)
        self._signals.done.connect(self._on_done)
        self._signals.failed.connect(self._on_failed)

    def cached(self, url: str) -> QPixmap | None:
        return self._memory.get(url)

    def request(self, url: str) -> QPixmap | None:
        if not url:
            return None
        pixmap = self._memory.get(url)
        if pixmap is not None:
            return pixmap
        if url not in self._pending:
            self._pending.add(url)
            self._pool.start(_Job(url, self._signals))
        return None

    def _on_done(self, url: str, pixmap: QPixmap) -> None:
        self._pending.discard(url)
        if len(self._memory) > 600:
            self._memory.clear()
        self._memory[url] = pixmap
        self.ready.emit(url, pixmap)

    def _on_failed(self, url: str) -> None:
        self._pending.discard(url)

    def shutdown(self) -> None:
        self._pool.clear()
        self._pool.waitForDone(2000)
