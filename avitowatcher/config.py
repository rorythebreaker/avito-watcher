"""Настройки приложения: чтение/запись settings.json."""
from __future__ import annotations

import json
import threading
from dataclasses import asdict, dataclass, fields
from typing import Any

from . import secrets
from .paths import settings_path

# Поля, которые кладём в файл в зашифрованном виде.
SECRET_FIELDS = {"telegram_token", "smtp_password"}


@dataclass
class Settings:
    # Общее
    default_interval: int = 300          # секунды между проверками задачи
    request_delay_min: float = 3.0       # пауза между HTTP-запросами
    request_delay_max: float = 8.0
    minimize_to_tray: bool = True
    start_minimized: bool = False
    autostart: bool = False
    autostart_watching: bool = True      # сразу начинать слежение при запуске
    max_feed_items: int = 500            # сколько объявлений держать в ленте
    keep_history_days: int = 30

    # Сеть
    proxy: str = ""                      # http://user:pass@host:port
    impersonate: str = "chrome"       # профиль TLS-отпечатка curl_cffi
    browser_fallback: bool = True        # при блокировке пробовать Playwright
    request_timeout: int = 30

    # Уведомления в приложении
    notify_desktop: bool = True
    notify_sound: bool = True

    # Telegram
    telegram_enabled: bool = False
    telegram_token: str = ""
    telegram_chat_id: str = ""
    telegram_with_photo: bool = True

    # Email
    email_enabled: bool = False
    smtp_host: str = ""
    smtp_port: int = 465
    smtp_ssl: bool = True                # True = SSL(465), False = STARTTLS(587)
    smtp_user: str = ""
    smtp_password: str = ""
    email_to: str = ""

    # Служебное
    window_geometry: str = ""

    @classmethod
    def load(cls) -> "Settings":
        path = settings_path()
        if not path.exists():
            return cls()
        try:
            raw: dict[str, Any] = json.loads(path.read_text("utf-8"))
        except Exception:
            return cls()
        known = {f.name: f for f in fields(cls)}
        data: dict[str, Any] = {}
        for key, value in raw.items():
            if key not in known:
                continue
            if key in SECRET_FIELDS and isinstance(value, str):
                value = secrets.decrypt(value)
            data[key] = value
        try:
            return cls(**data)
        except TypeError:
            return cls()

    def save(self) -> None:
        data = asdict(self)
        for key in SECRET_FIELDS:
            if data.get(key):
                data[key] = secrets.encrypt(data[key])
        tmp = settings_path().with_suffix(".json.tmp")
        tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), "utf-8")
        tmp.replace(settings_path())


class SettingsStore:
    """Единственный экземпляр настроек, безопасный для чтения из потоков."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._settings = Settings.load()

    @property
    def current(self) -> Settings:
        with self._lock:
            return self._settings

    def replace(self, settings: Settings) -> None:
        with self._lock:
            self._settings = settings
            settings.save()

    def update(self, **kwargs: Any) -> None:
        with self._lock:
            for key, value in kwargs.items():
                setattr(self._settings, key, value)
            self._settings.save()


store = SettingsStore()
