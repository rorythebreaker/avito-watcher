"""Каналы уведомлений.

Уведомление внутри приложения (лента + всплывашка Windows) рисует UI, поэтому
здесь только внешние каналы: Telegram и почта. Ошибка одного канала не мешает
остальным — приложение не должно падать из-за недоступного SMTP.
"""
from __future__ import annotations

import logging

from ..config import Settings
from ..models import Listing
from . import mail, telegram

log = logging.getLogger(__name__)


def dispatch(settings: Settings, listings: list[Listing], task_name: str) -> list[str]:
    """Рассылает пачку объявлений по включённым каналам.

    Возвращает список описаний ошибок — их показывает UI, не прерывая слежение.
    """
    errors: list[str] = []
    if not listings:
        return errors

    if settings.telegram_enabled and settings.telegram_token and settings.telegram_chat_id:
        try:
            telegram.send_batch(settings, listings, task_name)
        except Exception as exc:
            log.warning("Telegram: %s", exc)
            errors.append(f"Telegram: {exc}")

    if settings.email_enabled and settings.smtp_host and settings.email_to:
        try:
            mail.send_batch(settings, listings, task_name)
        except Exception as exc:
            log.warning("Почта: %s", exc)
            errors.append(f"Почта: {exc}")

    return errors


__all__ = ["dispatch", "mail", "telegram"]
