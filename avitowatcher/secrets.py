"""Шифрование секретов (токен бота, пароль почты) через Windows DPAPI.

Ключ привязан к учётной записи Windows: другой пользователь или другая машина
расшифровать файл настроек не сможет. Если DPAPI недоступен (не Windows,
ошибка вызова) — значение хранится как есть, приложение продолжает работать.
"""
from __future__ import annotations

import base64
import ctypes
import sys
from ctypes import wintypes

_PREFIX = "dpapi:"


class _Blob(ctypes.Structure):
    _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]


def _blob(data: bytes) -> _Blob:
    buf = ctypes.create_string_buffer(data, len(data))
    return _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))


def _blob_bytes(blob: _Blob) -> bytes:
    return ctypes.string_at(blob.pbData, blob.cbData)


def _available() -> bool:
    return sys.platform == "win32"


def encrypt(value: str) -> str:
    """Возвращает строку вида 'dpapi:<base64>' либо исходное значение."""
    if not value or value.startswith(_PREFIX) or not _available():
        return value
    try:
        crypt32 = ctypes.windll.crypt32
        out = _Blob()
        src = _blob(value.encode("utf-8"))
        ok = crypt32.CryptProtectData(
            ctypes.byref(src), None, None, None, None, 0, ctypes.byref(out)
        )
        if not ok:
            return value
        try:
            return _PREFIX + base64.b64encode(_blob_bytes(out)).decode("ascii")
        finally:
            ctypes.windll.kernel32.LocalFree(out.pbData)
    except Exception:
        return value


def decrypt(value: str) -> str:
    if not value or not value.startswith(_PREFIX):
        return value
    if not _available():
        return ""
    try:
        crypt32 = ctypes.windll.crypt32
        out = _Blob()
        src = _blob(base64.b64decode(value[len(_PREFIX):]))
        ok = crypt32.CryptUnprotectData(
            ctypes.byref(src), None, None, None, None, 0, ctypes.byref(out)
        )
        if not ok:
            return ""
        try:
            return _blob_bytes(out).decode("utf-8")
        finally:
            ctypes.windll.kernel32.LocalFree(out.pbData)
    except Exception:
        return ""
