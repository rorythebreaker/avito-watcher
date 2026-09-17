"""Точка входа: запуск приложения."""
from __future__ import annotations

import logging
import logging.handlers
import sys

from PySide6.QtCore import QTimer
from PySide6.QtNetwork import QLocalServer, QLocalSocket
from PySide6.QtWidgets import QApplication, QMessageBox, QSystemTrayIcon

from . import APP_NAME, APP_TITLE, VERSION, autostart
from .config import store
from .db import Database
from .engine import WatcherEngine
from .paths import log_path
from .ui.icons import app_icon
from .ui.main_window import MainWindow

SINGLE_INSTANCE_KEY = f"{APP_NAME}-single-instance"


def setup_logging() -> None:
    handler = logging.handlers.RotatingFileHandler(
        log_path(), maxBytes=1_000_000, backupCount=2, encoding="utf-8"
    )
    handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    )
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    root.addHandler(handler)


def _already_running() -> bool:
    """Второй запуск не нужен — просто показываем уже открытое окно."""
    socket = QLocalSocket()
    socket.connectToServer(SINGLE_INSTANCE_KEY)
    if socket.waitForConnected(300):
        socket.write(b"show")
        socket.waitForBytesWritten(300)
        socket.disconnectFromServer()
        return True
    return False


def _start_instance_server(window: MainWindow) -> QLocalServer:
    QLocalServer.removeServer(SINGLE_INSTANCE_KEY)
    server = QLocalServer()
    server.listen(SINGLE_INSTANCE_KEY)

    def on_connection() -> None:
        connection = server.nextPendingConnection()
        if connection is not None:
            connection.disconnected.connect(connection.deleteLater)
        window.restore_window()

    server.newConnection.connect(on_connection)
    return server


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv if argv is None else argv
    start_in_tray = "--tray" in argv

    setup_logging()
    logging.getLogger(__name__).info("Запуск %s %s", APP_TITLE, VERSION)

    app = QApplication(argv)
    app.setApplicationName(APP_NAME)
    app.setApplicationDisplayName(APP_TITLE)
    app.setApplicationVersion(VERSION)
    app.setWindowIcon(app_icon())
    app.setQuitOnLastWindowClosed(False)

    if _already_running():
        return 0

    if not QSystemTrayIcon.isSystemTrayAvailable():
        logging.getLogger(__name__).warning("Область уведомлений недоступна")

    settings = store.current
    db = Database()
    engine = WatcherEngine(db, store)
    window = MainWindow(db, engine)
    server = _start_instance_server(window)
    app.aboutToQuit.connect(server.close)

    # Настройка автозапуска могла разойтись с реестром (папку перенесли,
    # пользователь убрал запись вручную) — приводим к тому, что в настройках.
    try:
        if autostart.is_enabled() != settings.autostart:
            autostart.set_enabled(settings.autostart)
    except OSError:
        logging.getLogger(__name__).warning("Не удалось синхронизировать автозапуск")

    if start_in_tray or settings.start_minimized:
        window.hide()
    else:
        window.show()

    window.log(f"{APP_TITLE} {VERSION} готов к работе")
    if not db.list_tasks():
        window.log("Создайте задачу кнопкой «Новая задача» на панели сверху")
    elif settings.autostart_watching:
        QTimer.singleShot(800, engine.start)

    exit_code = app.exec()
    engine.shutdown()
    db.close()
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
