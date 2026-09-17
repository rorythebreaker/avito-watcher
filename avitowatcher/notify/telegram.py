"""Отправка уведомлений в Telegram-бота."""
from __future__ import annotations

import html
import logging

import requests

from ..config import Settings
from ..models import Listing

log = logging.getLogger(__name__)

API = "https://api.telegram.org/bot{token}/{method}"
CAPTION_LIMIT = 1024
MESSAGE_LIMIT = 4096
MAX_CARDS = 10  # больше не шлём, чтобы не устроить бомбардировку


def _proxies(settings: Settings) -> dict | None:
    if not settings.proxy:
        return None
    return {"http": settings.proxy, "https": settings.proxy}


def _call(settings: Settings, method: str, payload: dict) -> dict:
    url = API.format(token=settings.telegram_token, method=method)
    response = requests.post(
        url, json=payload, timeout=20, proxies=_proxies(settings)
    )
    try:
        data = response.json()
    except ValueError:
        raise RuntimeError(f"Telegram ответил не по протоколу (код {response.status_code})")
    if not data.get("ok"):
        raise RuntimeError(data.get("description") or f"Ошибка Telegram ({response.status_code})")
    return data


def format_listing(listing: Listing, task_name: str) -> str:
    lines = [f"<b>{html.escape(listing.title or 'Без названия')}</b>"]
    lines.append(f"💰 {html.escape(listing.price_label())}")
    where_when = " · ".join(x for x in (listing.location, listing.date_text) if x)
    if where_when:
        lines.append(f"📍 {html.escape(where_when)}")
    if listing.seller:
        lines.append(f"👤 {html.escape(listing.seller)}")
    if listing.score < 100:
        lines.append(f"🎯 Похожесть: {listing.score}%")
    lines.append(f"🔎 {html.escape(task_name)}")
    lines.append(f'\n<a href="{html.escape(listing.url)}">Открыть на Авито</a>')
    return "\n".join(lines)


def send_listing(settings: Settings, listing: Listing, task_name: str) -> None:
    text = format_listing(listing, task_name)
    chat_id = settings.telegram_chat_id.strip()

    if settings.telegram_with_photo and listing.image_url:
        try:
            _call(settings, "sendPhoto", {
                "chat_id": chat_id,
                "photo": listing.image_url,
                "caption": text[:CAPTION_LIMIT],
                "parse_mode": "HTML",
            })
            return
        except RuntimeError as exc:
            # Картинка может не открыться со стороны Telegram — не повод терять
            # уведомление, отправляем обычным сообщением.
            log.info("sendPhoto не прошёл (%s), шлю текстом", exc)

    _call(settings, "sendMessage", {
        "chat_id": chat_id,
        "text": text[:MESSAGE_LIMIT],
        "parse_mode": "HTML",
        "disable_web_page_preview": False,
    })


def send_batch(settings: Settings, listings: list[Listing], task_name: str) -> None:
    for listing in listings[:MAX_CARDS]:
        send_listing(settings, listing, task_name)
    extra = len(listings) - MAX_CARDS
    if extra > 0:
        _call(settings, "sendMessage", {
            "chat_id": settings.telegram_chat_id.strip(),
            "text": f"…и ещё {extra} объявлений по задаче «{html.escape(task_name)}». "
                    f"Смотрите в приложении.",
            "parse_mode": "HTML",
        })


def check(settings: Settings) -> str:
    """Проверка настроек. Возвращает имя бота или бросает исключение."""
    if not settings.telegram_token.strip():
        raise RuntimeError("Не заполнен токен бота")
    if not settings.telegram_chat_id.strip():
        raise RuntimeError("Не заполнен chat_id")
    me = _call(settings, "getMe", {})
    _call(settings, "sendMessage", {
        "chat_id": settings.telegram_chat_id.strip(),
        "text": "✅ Avito Watcher подключён. Сюда будут приходить новые объявления.",
    })
    return me.get("result", {}).get("username", "бот")


def resolve_chat_id(settings: Settings) -> str:
    """Пытается определить chat_id из последних сообщений боту."""
    data = _call(settings, "getUpdates", {"limit": 10})
    for update in reversed(data.get("result", [])):
        message = update.get("message") or update.get("channel_post") or {}
        chat = message.get("chat") or {}
        if chat.get("id") is not None:
            return str(chat["id"])
    raise RuntimeError(
        "Не вижу сообщений. Откройте своего бота в Telegram, нажмите «Старт» "
        "и попробуйте снова."
    )
