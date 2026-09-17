"""Окно настроек: расписание, уведомления, сеть."""
from __future__ import annotations

from dataclasses import replace

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .. import browser
from ..config import Settings
from ..notify import mail, telegram

IMPERSONATE_CHOICES = [
    ("Chrome (последний)", "chrome"),
    ("Chrome 146", "chrome146"),
    ("Chrome 142", "chrome142"),
    ("Chrome 136", "chrome136"),
    ("Chrome 131", "chrome131"),
    ("Chrome 124", "chrome124"),
    ("Edge", "edge"),
    ("Safari", "safari"),
]


class _TestSignals(QObject):
    done = Signal(str)
    failed = Signal(str)


class _TestJob(QRunnable):
    """Проверка канала уведомлений — сеть, поэтому в отдельном потоке."""

    def __init__(self, func, settings: Settings, signals: _TestSignals) -> None:
        super().__init__()
        self._func = func
        self._settings = settings
        self._signals = signals

    def run(self) -> None:
        try:
            self._signals.done.emit(str(self._func(self._settings)))
        except Exception as exc:
            self._signals.failed.emit(str(exc))


class SettingsDialog(QDialog):
    def __init__(self, parent, settings: Settings) -> None:
        super().__init__(parent)
        self._settings = settings
        self._pool = QThreadPool(self)
        self._pool.setMaxThreadCount(2)

        self.setWindowTitle("Настройки")
        self.setMinimumWidth(600)

        layout = QVBoxLayout(self)
        tabs = QTabWidget()
        tabs.addTab(self._tab_general(), "Общее")
        tabs.addTab(self._tab_telegram(), "Telegram")
        tabs.addTab(self._tab_email(), "Почта")
        tabs.addTab(self._tab_network(), "Сеть")
        layout.addWidget(tabs)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.button(QDialogButtonBox.Ok).setText("Сохранить")
        buttons.button(QDialogButtonBox.Cancel).setText("Отмена")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        self._load()

    # --- вкладки ----------------------------------------------------------
    def _tab_general(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)

        behaviour = QGroupBox("Поведение окна")
        form = QFormLayout(behaviour)
        self.chk_tray = QCheckBox("Сворачивать в область уведомлений вместо закрытия")
        self.chk_start_min = QCheckBox("Запускаться свёрнутым в трей")
        self.chk_autostart = QCheckBox("Запускать вместе с Windows")
        self.chk_autowatch = QCheckBox("Начинать слежение сразу при запуске")
        for widget in (self.chk_tray, self.chk_start_min, self.chk_autostart,
                       self.chk_autowatch):
            form.addRow("", widget)
        layout.addWidget(behaviour)

        notifications = QGroupBox("Уведомления в приложении")
        nform = QFormLayout(notifications)
        self.chk_desktop = QCheckBox("Показывать всплывающее уведомление Windows")
        self.chk_sound = QCheckBox("Звуковой сигнал при новом объявлении")
        nform.addRow("", self.chk_desktop)
        nform.addRow("", self.chk_sound)
        layout.addWidget(notifications)

        schedule = QGroupBox("Расписание и история")
        sform = QFormLayout(schedule)
        self.spin_interval = QSpinBox()
        self.spin_interval.setRange(60, 86400)
        self.spin_interval.setSingleStep(60)
        self.spin_interval.setSuffix(" сек")
        sform.addRow("Интервал по умолчанию:", self.spin_interval)

        self.spin_feed = QSpinBox()
        self.spin_feed.setRange(50, 5000)
        self.spin_feed.setSingleStep(50)
        sform.addRow("Объявлений в ленте:", self.spin_feed)

        self.spin_history = QSpinBox()
        self.spin_history.setRange(1, 365)
        self.spin_history.setSuffix(" дн.")
        sform.addRow("Помнить просмотренные:", self.spin_history)
        layout.addWidget(schedule)
        layout.addStretch(1)
        return page

    def _tab_telegram(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)

        hint = QLabel(
            "Как подключить: в Telegram напишите боту <b>@BotFather</b>, команда "
            "<b>/newbot</b> — он выдаст токен. Затем откройте своего нового бота, "
            "нажмите «Старт» и нажмите здесь «Определить chat_id»."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#8a93a0;")
        layout.addWidget(hint)

        form = QFormLayout()
        self.chk_tg = QCheckBox("Присылать уведомления в Telegram")
        form.addRow("", self.chk_tg)

        self.edit_tg_token = QLineEdit()
        self.edit_tg_token.setPlaceholderText("1234567890:AA...")
        self.edit_tg_token.setEchoMode(QLineEdit.PasswordEchoOnEdit)
        form.addRow("Токен бота:", self.edit_tg_token)

        chat_row = QHBoxLayout()
        self.edit_tg_chat = QLineEdit()
        self.edit_tg_chat.setPlaceholderText("123456789")
        self.btn_tg_chat = QPushButton("Определить chat_id")
        self.btn_tg_chat.clicked.connect(self._resolve_chat_id)
        chat_row.addWidget(self.edit_tg_chat, 1)
        chat_row.addWidget(self.btn_tg_chat)
        form.addRow("Chat ID:", chat_row)

        self.chk_tg_photo = QCheckBox("Прикладывать фотографию объявления")
        form.addRow("", self.chk_tg_photo)
        layout.addLayout(form)

        self.btn_tg_test = QPushButton("Отправить пробное сообщение")
        self.btn_tg_test.clicked.connect(self._test_telegram)
        layout.addWidget(self.btn_tg_test)
        layout.addStretch(1)
        return page

    def _tab_email(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)

        hint = QLabel(
            "Для Gmail, Яндекса и Mail.ru обычный пароль не подойдёт — "
            "создайте в настройках почты <b>пароль приложения</b> и вставьте его сюда. "
            "Сервер и порт подставятся сами, как только вы введёте свой адрес."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#8a93a0;")
        layout.addWidget(hint)

        form = QFormLayout()
        self.chk_mail = QCheckBox("Присылать уведомления на почту")
        form.addRow("", self.chk_mail)

        self.edit_smtp_user = QLineEdit()
        self.edit_smtp_user.setPlaceholderText("you@gmail.com")
        self.edit_smtp_user.editingFinished.connect(self._apply_mail_preset)
        form.addRow("Ваш адрес (логин):", self.edit_smtp_user)

        self.edit_smtp_pass = QLineEdit()
        self.edit_smtp_pass.setEchoMode(QLineEdit.Password)
        form.addRow("Пароль приложения:", self.edit_smtp_pass)

        self.edit_mail_to = QLineEdit()
        self.edit_mail_to.setPlaceholderText("куда слать, можно несколько через запятую")
        form.addRow("Получатель:", self.edit_mail_to)

        server_row = QHBoxLayout()
        self.edit_smtp_host = QLineEdit()
        self.edit_smtp_host.setPlaceholderText("smtp.gmail.com")
        self.spin_smtp_port = QSpinBox()
        self.spin_smtp_port.setRange(1, 65535)
        server_row.addWidget(self.edit_smtp_host, 1)
        server_row.addWidget(QLabel("порт"))
        server_row.addWidget(self.spin_smtp_port)
        form.addRow("SMTP-сервер:", server_row)

        self.combo_smtp_security = QComboBox()
        self.combo_smtp_security.addItem("SSL (обычно порт 465)", True)
        self.combo_smtp_security.addItem("STARTTLS (обычно порт 587)", False)
        form.addRow("Шифрование:", self.combo_smtp_security)
        layout.addLayout(form)

        self.btn_mail_test = QPushButton("Отправить пробное письмо")
        self.btn_mail_test.clicked.connect(self._test_mail)
        layout.addWidget(self.btn_mail_test)
        layout.addStretch(1)
        return page

    def _tab_network(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)

        hint = QLabel(
            "Авито ограничивает частые обращения. Не ставьте паузы меньше "
            "нескольких секунд и интервал проверки меньше минуты — иначе сайт "
            "начнёт отдавать проверку на робота."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#8a93a0;")
        layout.addWidget(hint)

        form = QFormLayout()
        delay_row = QHBoxLayout()
        self.spin_delay_min = QDoubleSpinBox()
        self.spin_delay_min.setRange(0.5, 120.0)
        self.spin_delay_min.setSingleStep(0.5)
        self.spin_delay_max = QDoubleSpinBox()
        self.spin_delay_max.setRange(0.5, 300.0)
        self.spin_delay_max.setSingleStep(0.5)
        delay_row.addWidget(QLabel("от"))
        delay_row.addWidget(self.spin_delay_min, 1)
        delay_row.addWidget(QLabel("до"))
        delay_row.addWidget(self.spin_delay_max, 1)
        delay_row.addWidget(QLabel("сек"))
        form.addRow("Пауза между запросами:", delay_row)

        self.spin_timeout = QSpinBox()
        self.spin_timeout.setRange(5, 300)
        self.spin_timeout.setSuffix(" сек")
        form.addRow("Таймаут запроса:", self.spin_timeout)

        self.combo_impersonate = QComboBox()
        for title, value in IMPERSONATE_CHOICES:
            self.combo_impersonate.addItem(title, value)
        form.addRow("Представляться как:", self.combo_impersonate)

        self.edit_proxy = QLineEdit()
        self.edit_proxy.setPlaceholderText("http://user:pass@host:port — необязательно")
        form.addRow("Прокси:", self.edit_proxy)

        self.chk_browser = QCheckBox(
            "При блокировке повторять запрос через установленный браузер"
        )
        form.addRow("", self.chk_browser)
        layout.addLayout(form)

        found = browser.browser_name()
        status = QLabel(
            f"Найден браузер для запасного режима: <b>{found}</b>" if found
            else "<span style='color:#ffb545;'>Chromium-браузер не найден. "
                 "Установите Microsoft Edge или Google Chrome, иначе запасной "
                 "режим работать не будет.</span>"
        )
        status.setWordWrap(True)
        layout.addWidget(status)
        layout.addStretch(1)
        return page

    # --- загрузка и сохранение -------------------------------------------
    def _load(self) -> None:
        s = self._settings
        self.chk_tray.setChecked(s.minimize_to_tray)
        self.chk_start_min.setChecked(s.start_minimized)
        self.chk_autostart.setChecked(s.autostart)
        self.chk_autowatch.setChecked(s.autostart_watching)
        self.chk_desktop.setChecked(s.notify_desktop)
        self.chk_sound.setChecked(s.notify_sound)
        self.spin_interval.setValue(s.default_interval)
        self.spin_feed.setValue(s.max_feed_items)
        self.spin_history.setValue(s.keep_history_days)

        self.chk_tg.setChecked(s.telegram_enabled)
        self.edit_tg_token.setText(s.telegram_token)
        self.edit_tg_chat.setText(s.telegram_chat_id)
        self.chk_tg_photo.setChecked(s.telegram_with_photo)

        self.chk_mail.setChecked(s.email_enabled)
        self.edit_smtp_host.setText(s.smtp_host)
        self.spin_smtp_port.setValue(s.smtp_port)
        self.combo_smtp_security.setCurrentIndex(0 if s.smtp_ssl else 1)
        self.edit_smtp_user.setText(s.smtp_user)
        self.edit_smtp_pass.setText(s.smtp_password)
        self.edit_mail_to.setText(s.email_to)

        self.spin_delay_min.setValue(s.request_delay_min)
        self.spin_delay_max.setValue(s.request_delay_max)
        self.spin_timeout.setValue(s.request_timeout)
        index = self.combo_impersonate.findData(s.impersonate)
        self.combo_impersonate.setCurrentIndex(index if index >= 0 else 0)
        self.edit_proxy.setText(s.proxy)
        self.chk_browser.setChecked(s.browser_fallback)

    def _apply_mail_preset(self) -> None:
        """Подставляет сервер и порт по домену адреса."""
        if self.edit_smtp_host.text().strip():
            return
        preset = mail.preset_for(self.edit_smtp_user.text())
        if preset is None:
            return
        host, port, use_ssl = preset
        self.edit_smtp_host.setText(host)
        self.spin_smtp_port.setValue(port)
        self.combo_smtp_security.setCurrentIndex(0 if use_ssl else 1)
        if not self.edit_mail_to.text().strip():
            self.edit_mail_to.setText(self.edit_smtp_user.text().strip())

    def collect(self) -> Settings:
        use_ssl = bool(self.combo_smtp_security.currentData())
        low = self.spin_delay_min.value()
        high = max(low, self.spin_delay_max.value())
        return replace(
            self._settings,
            minimize_to_tray=self.chk_tray.isChecked(),
            start_minimized=self.chk_start_min.isChecked(),
            autostart=self.chk_autostart.isChecked(),
            autostart_watching=self.chk_autowatch.isChecked(),
            notify_desktop=self.chk_desktop.isChecked(),
            notify_sound=self.chk_sound.isChecked(),
            default_interval=self.spin_interval.value(),
            max_feed_items=self.spin_feed.value(),
            keep_history_days=self.spin_history.value(),
            telegram_enabled=self.chk_tg.isChecked(),
            telegram_token=self.edit_tg_token.text().strip(),
            telegram_chat_id=self.edit_tg_chat.text().strip(),
            telegram_with_photo=self.chk_tg_photo.isChecked(),
            email_enabled=self.chk_mail.isChecked(),
            smtp_host=self.edit_smtp_host.text().strip(),
            smtp_port=self.spin_smtp_port.value(),
            smtp_ssl=use_ssl,
            smtp_user=self.edit_smtp_user.text().strip(),
            smtp_password=self.edit_smtp_pass.text(),
            email_to=self.edit_mail_to.text().strip(),
            request_delay_min=low,
            request_delay_max=high,
            request_timeout=self.spin_timeout.value(),
            impersonate=str(self.combo_impersonate.currentData()),
            proxy=self.edit_proxy.text().strip(),
            browser_fallback=self.chk_browser.isChecked(),
        )

    # --- проверки ---------------------------------------------------------
    def _run_test(self, button: QPushButton, label: str, func) -> None:
        button.setEnabled(False)
        original = button.text()
        button.setText("Отправляю…")

        signals = _TestSignals(self)

        def finish() -> None:
            button.setEnabled(True)
            button.setText(original)

        def on_done(result: str) -> None:
            finish()
            QMessageBox.information(self, "Готово", f"{label}: {result}")

        def on_failed(error: str) -> None:
            finish()
            QMessageBox.warning(self, "Не получилось", error)

        signals.done.connect(on_done)
        signals.failed.connect(on_failed)
        self._pool.start(_TestJob(func, self.collect(), signals))

    def _test_telegram(self) -> None:
        self._run_test(self.btn_tg_test, "Сообщение отправлено ботом", telegram.check)

    def _test_mail(self) -> None:
        self._run_test(self.btn_mail_test, "Письмо отправлено на", mail.check)

    def _resolve_chat_id(self) -> None:
        def worker(settings: Settings) -> str:
            return telegram.resolve_chat_id(settings)

        button = self.btn_tg_chat
        button.setEnabled(False)
        original = button.text()
        button.setText("Ищу…")

        signals = _TestSignals(self)

        def on_done(chat_id: str) -> None:
            button.setEnabled(True)
            button.setText(original)
            self.edit_tg_chat.setText(chat_id)
            self.chk_tg.setChecked(True)

        def on_failed(error: str) -> None:
            button.setEnabled(True)
            button.setText(original)
            QMessageBox.warning(self, "Не получилось", error)

        signals.done.connect(on_done)
        signals.failed.connect(on_failed)
        self._pool.start(_TestJob(worker, self.collect(), signals))
