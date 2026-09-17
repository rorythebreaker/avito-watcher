"""Пути приложения. Данные лежат в %APPDATA%/AvitoWatcher."""
from __future__ import annotations

import os
import sys
from pathlib import Path

from . import APP_NAME


def data_dir() -> Path:
    base = os.environ.get("APPDATA") or os.path.expanduser("~")
    path = Path(base) / APP_NAME
    path.mkdir(parents=True, exist_ok=True)
    return path


def cache_dir() -> Path:
    path = data_dir() / "cache"
    path.mkdir(parents=True, exist_ok=True)
    return path


def db_path() -> Path:
    return data_dir() / "watcher.db"


def settings_path() -> Path:
    return data_dir() / "settings.json"


def log_path() -> Path:
    return data_dir() / "watcher.log"


def resource_dir() -> Path:
    """Каталог с ресурсами: рядом с исходниками или внутри распакованного exe."""
    bundled = getattr(sys, "_MEIPASS", None)
    if bundled:
        return Path(bundled) / "assets"
    return Path(__file__).resolve().parent.parent / "assets"


def executable_path() -> Path:
    """Путь, который надо прописывать в автозапуск."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable)
    return Path(sys.executable)
