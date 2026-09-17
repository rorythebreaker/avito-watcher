"""Отправка уведомлений по электронной почте через SMTP."""
from __future__ import annotations

import html
import smtplib
import ssl
from email.message import EmailMessage
from email.utils import formataddr

from ..config import Settings
from ..models import Listing

# Подсказки для популярных почтовых служб — используются в окне настроек.
PRESETS = {
    "gmail.com": ("smtp.gmail.com", 465, True),
    "yandex.ru": ("smtp.yandex.ru", 465, True),
    "ya.ru": ("smtp.yandex.ru", 465, True),
    "mail.ru": ("smtp.mail.ru", 465, True),
    "bk.ru": ("smtp.mail.ru", 465, True),
    "inbox.ru": ("smtp.mail.ru", 465, True),
    "list.ru": ("smtp.mail.ru", 465, True),
    "outlook.com": ("smtp-mail.outlook.com", 587, False),
    "hotmail.com": ("smtp-mail.outlook.com", 587, False),
    "rambler.ru": ("smtp.rambler.ru", 465, True),
}


def preset_for(address: str) -> tuple[str, int, bool] | None:
    domain = address.strip().rsplit("@", 1)[-1].lower()
    return PRESETS.get(domain)


def _recipients(settings: Settings) -> list[str]:
    raw = settings.email_to.replace(";", ",")
    return [a.strip() for a in raw.split(",") if a.strip()]


def _card_html(listing: Listing, task_name: str) -> str:
    image = ""
    if listing.image_url:
        image = (
            f'<td style="padding-right:14px;vertical-align:top;">'
            f'<img src="{html.escape(listing.image_url)}" width="120" '
            f'style="border-radius:8px;display:block;" alt=""></td>'
        )
    meta = " · ".join(
        html.escape(x) for x in (listing.location, listing.date_text, listing.seller) if x
    )
    score = f'<div style="color:#7a7a7a;">Похожесть: {listing.score}%</div>' if listing.score < 100 else ""
    return f"""
    <table style="width:100%;border-collapse:collapse;margin:0 0 18px;
                  border-bottom:1px solid #e8e8e8;padding-bottom:18px;">
      <tr>{image}
        <td style="vertical-align:top;font-family:Arial,sans-serif;font-size:14px;color:#1a1a1a;">
          <div style="font-size:16px;font-weight:bold;margin-bottom:4px;">
            <a href="{html.escape(listing.url)}" style="color:#1a1a1a;text-decoration:none;">
              {html.escape(listing.title or 'Без названия')}</a>
          </div>
          <div style="font-size:18px;font-weight:bold;color:#0a7d33;margin-bottom:4px;">
            {html.escape(listing.price_label())}</div>
          <div style="color:#7a7a7a;">{meta}</div>
          {score}
          <div style="color:#7a7a7a;margin-top:4px;">Задача: {html.escape(task_name)}</div>
          <div style="margin-top:8px;">
            <a href="{html.escape(listing.url)}"
               style="background:#0af;color:#fff;padding:8px 16px;border-radius:6px;
                      text-decoration:none;display:inline-block;">Открыть на Авито</a>
          </div>
        </td>
      </tr>
    </table>"""


def _build_message(settings: Settings, listings: list[Listing], task_name: str) -> EmailMessage:
    message = EmailMessage()
    count = len(listings)
    message["Subject"] = f"Avito Watcher: {count} новых по задаче «{task_name}»"
    message["From"] = formataddr(("Avito Watcher", settings.smtp_user))
    message["To"] = ", ".join(_recipients(settings))

    plain = [f"Новых объявлений: {count} (задача «{task_name}»)", ""]
    for listing in listings:
        plain.append(f"{listing.title} — {listing.price_label()}")
        if listing.location or listing.date_text:
            plain.append(f"  {listing.location} {listing.date_text}".rstrip())
        plain.append(f"  {listing.url}")
        plain.append("")
    message.set_content("\n".join(plain))

    cards = "".join(_card_html(l, task_name) for l in listings)
    message.add_alternative(f"""<html><body style="background:#f5f5f5;margin:0;padding:24px;">
      <div style="max-width:640px;margin:0 auto;background:#fff;border-radius:12px;padding:24px;">
        <div style="font-family:Arial,sans-serif;font-size:20px;font-weight:bold;
                    margin-bottom:20px;color:#1a1a1a;">
          Новых объявлений: {count}</div>
        {cards}
        <div style="font-family:Arial,sans-serif;font-size:12px;color:#9a9a9a;">
          Письмо отправлено приложением Avito Watcher.</div>
      </div></body></html>""", subtype="html")
    return message


def _send(settings: Settings, message: EmailMessage) -> None:
    host = settings.smtp_host.strip()
    if not host:
        raise RuntimeError("Не указан SMTP-сервер")
    if not _recipients(settings):
        raise RuntimeError("Не указан адрес получателя")

    context = ssl.create_default_context()
    if settings.smtp_ssl:
        with smtplib.SMTP_SSL(host, settings.smtp_port, context=context, timeout=30) as smtp:
            if settings.smtp_user:
                smtp.login(settings.smtp_user, settings.smtp_password)
            smtp.send_message(message)
    else:
        with smtplib.SMTP(host, settings.smtp_port, timeout=30) as smtp:
            smtp.ehlo()
            smtp.starttls(context=context)
            smtp.ehlo()
            if settings.smtp_user:
                smtp.login(settings.smtp_user, settings.smtp_password)
            smtp.send_message(message)


def send_batch(settings: Settings, listings: list[Listing], task_name: str) -> None:
    """Одно письмо на всю пачку новых объявлений."""
    if not listings:
        return
    _send(settings, _build_message(settings, listings, task_name))


def check(settings: Settings) -> str:
    """Отправляет пробное письмо. Бросает исключение при неудаче."""
    message = EmailMessage()
    message["Subject"] = "Avito Watcher: проверка почты"
    message["From"] = formataddr(("Avito Watcher", settings.smtp_user))
    message["To"] = ", ".join(_recipients(settings))
    message.set_content(
        "Проверка связи. Если вы читаете это письмо, уведомления о новых "
        "объявлениях будут приходить сюда."
    )
    _send(settings, message)
    return ", ".join(_recipients(settings))
