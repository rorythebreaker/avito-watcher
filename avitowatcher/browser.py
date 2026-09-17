"""Запасной способ получить страницу — через настоящий браузер.

Авито иногда отдаёт защитную заглушку на обычные HTTP-запросы. Тогда страницу
забираем уже установленным в системе Chromium-браузером (Edge есть в Windows 11
из коробки): запускаем его в фоновом режиме с включённым отладочным портом и
управляем по протоколу Chrome DevTools. Так exe остаётся лёгким — Chromium
внутрь не кладём и лишних библиотек не тянем.

Ключ --dump-dom здесь не годится: msedge.exe собран как оконное приложение и
в канал вывода ничего не пишет, поэтому DOM забираем через отладочный порт.
"""
from __future__ import annotations

import base64
import json
import logging
import os
import secrets as _secrets
import socket
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from .paths import cache_dir

log = logging.getLogger(__name__)

# Порядок важен: сначала Edge (есть всегда), потом Chrome, потом Brave.
_CANDIDATES = [
    r"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe",
    r"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe",
    r"%ProgramFiles%\Google\Chrome\Application\chrome.exe",
    r"%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe",
    r"%LocalAppData%\Google\Chrome\Application\chrome.exe",
    r"%ProgramFiles%\BraveSoftware\Brave-Browser\Application\brave.exe",
]

_CREATE_NO_WINDOW = 0x08000000  # чтобы не мигало консольное окно

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36"
)


def find_browser() -> Path | None:
    """Путь к первому найденному браузеру или None."""
    for pattern in _CANDIDATES:
        path = Path(os.path.expandvars(pattern))
        if "%" not in str(path) and path.exists():
            return path
    return None


def browser_name() -> str:
    path = find_browser()
    return path.name if path else ""


def is_available() -> bool:
    return find_browser() is not None


# --- минимальный клиент WebSocket ----------------------------------------
class _WebSocket:
    """Ровно столько протокола RFC 6455, сколько нужно для DevTools."""

    def __init__(self, url: str, timeout: float = 30.0) -> None:
        rest = url.split("://", 1)[1]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self._sock = socket.create_connection((host, int(port or 80)), timeout=timeout)
        self._sock.settimeout(timeout)
        self._buffer = b""

        key = base64.b64encode(_secrets.token_bytes(16)).decode("ascii")
        handshake = (
            f"GET /{path} HTTP/1.1\r\n"
            f"Host: {hostport}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self._sock.sendall(handshake.encode("ascii"))

        header = b""
        while b"\r\n\r\n" not in header:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise RuntimeError("Браузер закрыл отладочное соединение")
            header += chunk
        head, _, tail = header.partition(b"\r\n\r\n")
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError("Браузер не принял отладочное соединение")
        self._buffer = tail

    def _read(self, count: int) -> bytes:
        while len(self._buffer) < count:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise RuntimeError("Браузер закрыл отладочное соединение")
            self._buffer += chunk
        data, self._buffer = self._buffer[:count], self._buffer[count:]
        return data

    def send(self, text: str) -> None:
        payload = text.encode("utf-8")
        header = bytearray([0x81])  # FIN + текстовый кадр
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        mask = _secrets.token_bytes(4)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self._sock.sendall(bytes(header) + masked)

    def recv(self) -> str:
        """Возвращает следующее текстовое сообщение, склеивая части кадра."""
        message = b""
        while True:
            first, second = self._read(2)
            fin = bool(first & 0x80)
            opcode = first & 0x0F
            length = second & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            if second & 0x80:                      # сервер маскировать не должен
                mask = self._read(4)
                raw = self._read(length)
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(raw))
            else:
                payload = self._read(length)

            if opcode == 0x8:
                raise RuntimeError("Браузер разорвал отладочное соединение")
            if opcode == 0x9:                      # ping — отвечаем pong
                self._sock.sendall(bytes([0x8A, 0x80]) + _secrets.token_bytes(4))
                continue
            if opcode == 0xA:
                continue
            message += payload
            if fin:
                return message.decode("utf-8", errors="replace")

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass


# --- управление браузером -------------------------------------------------
class _Session:
    """Запущенный браузер с открытым отладочным портом."""

    def __init__(self, exe: Path, profile: Path, proxy: str, timeout: float) -> None:
        self._deadline = time.monotonic() + timeout
        args = [
            str(exe),
            "--headless=new",
            "--disable-gpu",
            "--disable-extensions",
            "--disable-background-networking",
            "--disable-sync",
            "--disable-blink-features=AutomationControlled",
            "--no-first-run",
            "--no-default-browser-check",
            "--mute-audio",
            "--window-size=1440,900",
            "--lang=ru-RU",
            f"--user-agent={USER_AGENT}",
            f"--user-data-dir={profile}",
            "--remote-debugging-port=0",
        ]
        if proxy:
            args.append(f"--proxy-server={proxy}")
        args.append("about:blank")

        self._process = subprocess.Popen(
            args,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=_CREATE_NO_WINDOW if sys.platform == "win32" else 0,
        )
        self._port = self._wait_for_port(profile)

    def _wait_for_port(self, profile: Path) -> int:
        """Браузер записывает выбранный порт в файл DevToolsActivePort.

        Завершение запущенного процесса тут ни о чём не говорит: msedge.exe
        перезапускает сам себя и первый процесс сразу выходит с кодом 0,
        пока настоящий браузер продолжает подниматься. Поэтому ждём именно
        файл с портом, а не жизнь процесса.
        """
        marker = profile / "DevToolsActivePort"
        while time.monotonic() < self._deadline:
            try:
                first_line = marker.read_text("utf-8").splitlines()[0]
                return int(first_line)
            except (OSError, IndexError, ValueError):
                time.sleep(0.2)
        raise RuntimeError("Браузер не открыл отладочный порт за отведённое время")

    def _http(self, path: str) -> object:
        url = f"http://127.0.0.1:{self._port}{path}"
        with urllib.request.urlopen(url, timeout=15) as response:
            return json.loads(response.read().decode("utf-8"))

    def page_socket_url(self) -> str:
        while time.monotonic() < self._deadline:
            try:
                targets = self._http("/json/list")
            except Exception:
                time.sleep(0.2)
                continue
            for target in targets:
                if target.get("type") == "page" and target.get("webSocketDebuggerUrl"):
                    return target["webSocketDebuggerUrl"]
            try:
                created = self._http("/json/new?about:blank")
                if created.get("webSocketDebuggerUrl"):
                    return created["webSocketDebuggerUrl"]
            except Exception:
                pass
            time.sleep(0.3)
        raise RuntimeError("Не удалось открыть вкладку в браузере")

    @property
    def deadline(self) -> float:
        return self._deadline

    def close(self) -> None:
        """Закрывает браузер.

        Убить запущенный процесс мало — он давно вышел, а работает его
        потомок. Поэтому просим сам браузер закрыться по отладочному
        протоколу; нашей копии это касается, чужие окна не трогаем.
        """
        try:
            version = self._http("/json/version")
            socket_url = version.get("webSocketDebuggerUrl")
            if socket_url:
                websocket = _WebSocket(socket_url, timeout=10)
                try:
                    websocket.send(json.dumps({"id": 1, "method": "Browser.close"}))
                    time.sleep(0.3)
                finally:
                    websocket.close()
        except Exception as exc:
            log.debug("Не удалось закрыть браузер штатно: %s", exc)
        if self._process.poll() is None:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()


def fetch(url: str, timeout: int = 60, proxy: str = "") -> str:
    """Загружает страницу браузером и возвращает HTML после исполнения скриптов."""
    exe = find_browser()
    if exe is None:
        raise RuntimeError(
            "Не найден Chromium-браузер (Edge/Chrome). "
            "Установите Microsoft Edge или Google Chrome."
        )

    profile = cache_dir() / "browser-profile"
    profile.mkdir(parents=True, exist_ok=True)
    # Файл с портом остаётся от прошлого запуска — иначе подключимся к старому.
    (profile / "DevToolsActivePort").unlink(missing_ok=True)

    session = _Session(exe, profile, proxy, float(timeout))
    websocket: _WebSocket | None = None
    try:
        websocket = _WebSocket(session.page_socket_url(), timeout=float(timeout))
        message_id = 0

        def call(method: str, params: dict | None = None) -> dict:
            nonlocal message_id
            message_id += 1
            current = message_id
            websocket.send(json.dumps(
                {"id": current, "method": method, "params": params or {}}
            ))
            while True:
                data = json.loads(websocket.recv())
                if data.get("id") == current:
                    if "error" in data:
                        raise RuntimeError(data["error"].get("message", method))
                    return data.get("result", {})

        def wait_for(event: str, until: float) -> bool:
            while time.monotonic() < until:
                try:
                    data = json.loads(websocket.recv())
                except socket.timeout:
                    return False
                if data.get("method") == event:
                    return True
            return False

        call("Page.enable")
        call("Page.navigate", {"url": url})
        wait_for("Page.loadEventFired", min(session.deadline, time.monotonic() + timeout))
        time.sleep(1.5)  # даём дорисоваться тому, что подгружается скриптами

        # Фотографии нижних карточек грузятся только когда до них долистают,
        # поэтому прокручиваем страницу до конца и возвращаемся наверх.
        try:
            for step in range(1, 5):
                call("Runtime.evaluate", {
                    "expression": f"window.scrollTo(0, document.body.scrollHeight*{step}/4)",
                })
                time.sleep(0.6)
            call("Runtime.evaluate", {"expression": "window.scrollTo(0, 0)"})
            time.sleep(0.4)
        except RuntimeError:
            pass

        result = call("Runtime.evaluate", {
            "expression": "document.documentElement.outerHTML",
            "returnByValue": True,
        })
        html = result.get("result", {}).get("value") or ""
    finally:
        if websocket is not None:
            websocket.close()
        session.close()

    if len(html) < 500:
        raise RuntimeError("Браузер вернул пустую страницу")
    return html


def self_test() -> str:
    """Проверка запасного режима — используется кнопкой в настройках."""
    html = fetch("https://www.avito.ru/", timeout=90)
    return f"{browser_name()}: получено {len(html)} символов"
