"""Загрузка страниц Авито.

Сначала обычный HTTP-запрос через curl_cffi с TLS-отпечатком настоящего Chrome —
это быстро и почти не нагружает сайт. Если в ответ прилетает защитная заглушка,
один раз повторяем через настоящий браузер (см. browser.py).

Между запросами выдерживается общая для всего приложения случайная пауза, чтобы
не устраивать Авито шквал обращений.
"""
from __future__ import annotations

import json
import logging
import random
import re
import threading
import time

from curl_cffi import requests as cffi

from . import browser
from .config import Settings
from .paths import cache_dir

log = logging.getLogger(__name__)

# Заголовки страниц-заглушек, которые Авито показывает вместо выдачи.
_BLOCK_TITLES = (
    "доступ ограничен",
    "проблема с ip",
    "проверка безопасности",
    "вы не робот",
    "подтвердите, что вы не робот",
    "access denied",
    "attention required",
)

# Так выглядит честный ответ «по запросу ничего нет» — это не блокировка.
_EMPTY_RESULT_MARKERS = ("ничего не найдено", "не найдено ни одного объявления")

_TITLE_RE = re.compile(r"<title[^>]*>(.*?)</title>", re.S | re.I)

_HEADERS = {
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,"
              "image/webp,*/*;q=0.8",
    "Accept-Language": "ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7",
    "Upgrade-Insecure-Requests": "1",
    "Sec-Fetch-Dest": "document",
    "Sec-Fetch-Mode": "navigate",
    "Sec-Fetch-Site": "same-origin",
    "Sec-Fetch-User": "?1",
    "Cache-Control": "max-age=0",
}


class BlockedError(RuntimeError):
    """Авито ответил защитной заглушкой и браузер не помог."""


class FetchError(RuntimeError):
    """Сетевая ошибка или неожиданный ответ."""


def page_title(html: str) -> str:
    match = _TITLE_RE.search(html)
    return match.group(1).strip().lower() if match else ""


def is_empty_result(html: str) -> bool:
    """Авито ответил, что по запросу ничего не нашлось."""
    lowered = html.lower()
    return any(marker in lowered for marker in _EMPTY_RESULT_MARKERS)


def looks_blocked(html: str, status: int, require: str = "") -> bool:
    """Отличает защитную заглушку от настоящей страницы.

    Судим по коду ответа и по заголовку страницы, а не по случайным словам
    в тексте: слова вроде «captcha» встречаются в обычных скриптах Авито и
    раньше давали ложные срабатывания. Дополнительно можно потребовать
    наличие куска разметки, без которого страница заведомо бесполезна.
    """
    if status in (403, 429, 503):
        return True
    title = page_title(html)
    if any(marker in title for marker in _BLOCK_TITLES):
        return True
    if is_empty_result(html):
        # Поиск честно отработал и ничего не нашёл — страница настоящая.
        return False
    if require and require not in html:
        return True
    return len(html) < 5000


class RateLimiter:
    """Общая на всё приложение пауза между обращениями к Авито."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._last = 0.0

    def wait(self, low: float, high: float, stop_event: threading.Event | None = None) -> None:
        with self._lock:
            gap = random.uniform(max(0.5, low), max(low + 0.1, high))
            sleep_for = self._last + gap - time.monotonic()
            if sleep_for > 0:
                if stop_event is not None:
                    stop_event.wait(sleep_for)
                else:
                    time.sleep(sleep_for)
            self._last = time.monotonic()


class Fetcher:
    """Держит HTTP-сессию с куками и умеет переключаться на браузер."""

    def __init__(self, settings: Settings, limiter: RateLimiter | None = None) -> None:
        self._settings = settings
        self._limiter = limiter or RateLimiter()
        self._lock = threading.Lock()
        self._session: cffi.Session | None = None
        self._cookie_file = cache_dir() / "cookies.json"
        self.last_transport = ""   # 'http' или 'browser' — для диагностики в UI

    def update_settings(self, settings: Settings) -> None:
        with self._lock:
            reset = (
                settings.impersonate != self._settings.impersonate
                or settings.proxy != self._settings.proxy
            )
            self._settings = settings
            if reset and self._session is not None:
                try:
                    self._session.close()
                except Exception:
                    pass
                self._session = None

    # --- сессия -----------------------------------------------------------
    def _ensure_session(self) -> cffi.Session:
        if self._session is not None:
            return self._session
        s = self._settings
        kwargs = {"impersonate": s.impersonate or "chrome", "timeout": s.request_timeout}
        if s.proxy:
            kwargs["proxies"] = {"http": s.proxy, "https": s.proxy}
        session = cffi.Session(**kwargs)
        session.headers.update(_HEADERS)
        self._load_cookies(session)
        self._session = session
        return session

    def _load_cookies(self, session: cffi.Session) -> None:
        try:
            data = json.loads(self._cookie_file.read_text("utf-8"))
        except Exception:
            return
        for name, value in data.items():
            try:
                session.cookies.set(name, value, domain=".avito.ru")
            except Exception:
                continue

    def _save_cookies(self) -> None:
        if self._session is None:
            return
        try:
            data = {c.name: c.value for c in self._session.cookies.jar if c.value}
            self._cookie_file.write_text(json.dumps(data), "utf-8")
        except Exception:
            pass

    def close(self) -> None:
        with self._lock:
            self._save_cookies()
            if self._session is not None:
                try:
                    self._session.close()
                except Exception:
                    pass
                self._session = None

    # --- загрузка ---------------------------------------------------------
    def get(
        self,
        url: str,
        referer: str = "https://www.avito.ru/",
        stop_event: threading.Event | None = None,
        require: str = "",
    ) -> str:
        s = self._settings
        self._limiter.wait(s.request_delay_min, s.request_delay_max, stop_event)
        if stop_event is not None and stop_event.is_set():
            raise FetchError("Проверка отменена")

        html, status, error = "", 0, ""
        try:
            with self._lock:
                session = self._ensure_session()
                response = session.get(url, headers={"Referer": referer})
                status = response.status_code
                html = response.text or ""
                self._save_cookies()
        except Exception as exc:
            error = str(exc)
            log.warning("HTTP-запрос не удался: %s", exc)

        if html and not looks_blocked(html, status, require):
            self.last_transport = "http"
            return html

        if not s.browser_fallback:
            if error:
                raise FetchError(f"Не удалось загрузить страницу: {error}")
            raise BlockedError(
                "Авито не отдал страницу (защита от ботов). "
                "Включите запасной браузер в настройках."
            )

        log.info("Переключаюсь на браузер для %s", url)
        try:
            html = browser.fetch(url, timeout=max(60, s.request_timeout * 2), proxy=s.proxy)
        except Exception as exc:
            if error:
                raise FetchError(f"HTTP: {error}; браузер: {exc}") from exc
            raise BlockedError(f"Авито блокирует запросы, браузер тоже не помог: {exc}") from exc

        if looks_blocked(html, 200, require):
            raise BlockedError(
                "Авито показывает проверку на робота. Сделайте паузу, "
                "увеличьте интервал проверки или укажите прокси в настройках."
            )
        self.last_transport = "browser"
        return html
