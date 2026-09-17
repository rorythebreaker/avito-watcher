"""Главное окно: список задач, лента находок, журнал и значок в трее."""
from __future__ import annotations

import time

from PySide6.QtCore import QByteArray, Qt, QTimer, QUrl
from PySide6.QtGui import QAction, QBrush, QColor, QDesktopServices, QKeySequence
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSizePolicy,
    QSplitter,
    QSystemTrayIcon,
    QToolBar,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .. import APP_TITLE, autostart
from ..config import store
from ..db import Database
from ..engine import WatcherEngine
from ..models import (
    KIND_SIMILAR,
    STATUS_BLOCKED,
    STATUS_ERROR,
    STATUS_OK,
    STATUS_RUNNING,
    Listing,
    Task,
)
from .feed import FeedView
from .icons import app_icon
from .images import ImageLoader
from .settings_dialog import SettingsDialog
from .task_dialog import TaskDialog
from .theme import DARK_QSS

STATUS_COLORS = {
    STATUS_OK: "#4ade80",
    STATUS_RUNNING: "#00aaff",
    STATUS_BLOCKED: "#ffb545",
    STATUS_ERROR: "#ff6b5c",
}


def _interval_label(seconds: int) -> str:
    if seconds < 3600:
        return f"{seconds // 60} мин"
    hours = seconds / 3600
    return f"{hours:.0f} ч" if hours.is_integer() else f"{hours:.1f} ч"


class MainWindow(QMainWindow):
    def __init__(self, db: Database, engine: WatcherEngine) -> None:
        super().__init__()
        self._db = db
        self._engine = engine
        self._loader = ImageLoader(self)
        self._quitting = False
        self._last_listing: Listing | None = None
        self._unseen = 0

        self.setWindowTitle(APP_TITLE)
        self.setWindowIcon(app_icon())
        self.setStyleSheet(DARK_QSS)
        self.resize(1180, 760)

        self._build_toolbar()
        self._build_central()
        self._build_tray()
        self._connect_engine()

        self._reload_tasks()
        self._reload_feed()
        self._restore_geometry()

        # Раз в минуту освежаем колонку «последняя проверка» и подписи «N мин назад».
        self._ticker = QTimer(self)
        self._ticker.timeout.connect(self._refresh_times)
        self._ticker.start(60_000)

    # --- построение интерфейса -------------------------------------------
    def _build_toolbar(self) -> None:
        bar = QToolBar("Основная панель")
        bar.setMovable(False)
        bar.setIconSize(bar.iconSize() * 0.9)
        self.addToolBar(bar)

        self.action_watch = QAction("▶  Начать слежение", self)
        self.action_watch.setShortcut(QKeySequence("F5"))
        self.action_watch.triggered.connect(self._toggle_watching)
        bar.addAction(self.action_watch)
        bar.addSeparator()

        self.action_add = QAction("Новая задача", self)
        self.action_add.setShortcut(QKeySequence.New)
        self.action_add.triggered.connect(self._add_task)
        bar.addAction(self.action_add)

        self.action_edit = QAction("Изменить", self)
        self.action_edit.triggered.connect(self._edit_task)
        bar.addAction(self.action_edit)

        self.action_check = QAction("Проверить сейчас", self)
        self.action_check.triggered.connect(self._check_now)
        bar.addAction(self.action_check)

        self.action_delete = QAction("Удалить", self)
        self.action_delete.triggered.connect(self._delete_task)
        bar.addAction(self.action_delete)

        bar.addSeparator()
        self.action_open_search = QAction("Открыть на Авито", self)
        self.action_open_search.triggered.connect(self._open_task_url)
        bar.addAction(self.action_open_search)

        spacer = QWidget()
        spacer.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        bar.addWidget(spacer)

        self.action_settings = QAction("Настройки", self)
        self.action_settings.triggered.connect(self._open_settings)
        bar.addAction(self.action_settings)

    def _build_central(self) -> None:
        splitter = QSplitter(Qt.Horizontal)

        # --- левая колонка: задачи ---
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(8, 8, 4, 8)
        left_layout.addWidget(QLabel("Задачи"))

        self.task_tree = QTreeWidget()
        self.task_tree.setColumnCount(5)
        self.task_tree.setHeaderLabels(
            ["Задача", "Тип", "Интервал", "Статус", "Найдено"]
        )
        self.task_tree.setRootIsDecorated(False)
        self.task_tree.setAlternatingRowColors(True)
        self.task_tree.itemChanged.connect(self._on_task_item_changed)
        self.task_tree.itemSelectionChanged.connect(self._on_task_selected)
        self.task_tree.itemDoubleClicked.connect(lambda *_: self._edit_task())
        header = self.task_tree.header()
        header.setSectionResizeMode(0, QHeaderView.Stretch)
        for column in range(1, 5):
            header.setSectionResizeMode(column, QHeaderView.ResizeToContents)
        left_layout.addWidget(self.task_tree, 1)

        self.task_hint = QLabel()
        self.task_hint.setWordWrap(True)
        self.task_hint.setObjectName("hint")
        left_layout.addWidget(self.task_hint)
        splitter.addWidget(left)

        # --- правая колонка: лента и журнал ---
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(4, 8, 8, 8)

        head = QHBoxLayout()
        head.addWidget(QLabel("Найденные объявления"))
        head.addStretch(1)
        self.filter_combo = QComboBox()
        self.filter_combo.setMinimumWidth(200)
        self.filter_combo.currentIndexChanged.connect(lambda *_: self._reload_feed())
        head.addWidget(QLabel("Показывать:"))
        head.addWidget(self.filter_combo)
        clear_button = QPushButton("Очистить ленту")
        clear_button.clicked.connect(self._clear_feed)
        head.addWidget(clear_button)
        right_layout.addLayout(head)

        vertical = QSplitter(Qt.Vertical)
        self.feed = FeedView(self._loader)
        vertical.addWidget(self.feed)

        log_box = QWidget()
        log_layout = QVBoxLayout(log_box)
        log_layout.setContentsMargins(0, 6, 0, 0)
        log_layout.addWidget(QLabel("Журнал"))
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(500)
        log_layout.addWidget(self.log_view)
        vertical.addWidget(log_box)
        vertical.setSizes([560, 160])
        right_layout.addWidget(vertical, 1)
        splitter.addWidget(right)

        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([430, 750])
        self.setCentralWidget(splitter)

        self.status_label = QLabel("Слежение остановлено")
        self.statusBar().addWidget(self.status_label)
        self.counter_label = QLabel("")
        self.statusBar().addPermanentWidget(self.counter_label)

    def _build_tray(self) -> None:
        self.tray = QSystemTrayIcon(app_icon(), self)
        self.tray.setToolTip(APP_TITLE)

        menu = QMenu()
        show_action = QAction("Показать окно", menu)
        show_action.triggered.connect(self.restore_window)
        menu.addAction(show_action)

        self.tray_watch_action = QAction("Начать слежение", menu)
        self.tray_watch_action.triggered.connect(self._toggle_watching)
        menu.addAction(self.tray_watch_action)
        menu.addSeparator()

        quit_action = QAction("Выход", menu)
        quit_action.triggered.connect(self._quit)
        menu.addAction(quit_action)

        self.tray.setContextMenu(menu)
        self.tray.activated.connect(self._on_tray_activated)
        self.tray.messageClicked.connect(self._on_notification_clicked)
        self.tray.show()

    def _connect_engine(self) -> None:
        self._engine.task_started.connect(self._on_task_started)
        self._engine.task_finished.connect(self._on_task_finished)
        self._engine.new_listings.connect(self._on_new_listings)
        self._engine.notify_errors.connect(self._on_notify_errors)
        self._engine.message.connect(self.log)
        self._engine.running_changed.connect(self._on_running_changed)

    # --- задачи -----------------------------------------------------------
    def _reload_tasks(self) -> None:
        selected = self._selected_task_id()
        self.task_tree.blockSignals(True)
        self.task_tree.clear()
        tasks = self._db.list_tasks()
        for task in tasks:
            item = QTreeWidgetItem([
                task.name,
                task.kind_title(),
                _interval_label(task.interval),
                task.status_title(),
                str(task.found_total),
            ])
            item.setData(0, Qt.UserRole, task.id)
            item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
            item.setCheckState(0, Qt.Checked if task.enabled else Qt.Unchecked)
            colour = STATUS_COLORS.get(task.status)
            if colour:
                item.setForeground(3, QBrush(QColor(colour)))
            if task.last_error:
                item.setToolTip(3, task.last_error)
            self.task_tree.addTopLevelItem(item)
        self.task_tree.blockSignals(False)

        if selected is not None:
            self._select_task(selected)
        elif tasks:
            self.task_tree.setCurrentItem(self.task_tree.topLevelItem(0))

        self._rebuild_filter(tasks)
        self._update_counters(tasks)
        self._on_task_selected()

    def _rebuild_filter(self, tasks: list[Task]) -> None:
        current = self.filter_combo.currentData()
        self.filter_combo.blockSignals(True)
        self.filter_combo.clear()
        self.filter_combo.addItem("Все задачи", None)
        for task in tasks:
            self.filter_combo.addItem(task.name, task.id)
        index = self.filter_combo.findData(current)
        self.filter_combo.setCurrentIndex(index if index >= 0 else 0)
        self.filter_combo.blockSignals(False)

    def _update_counters(self, tasks: list[Task] | None = None) -> None:
        tasks = tasks if tasks is not None else self._db.list_tasks()
        active = sum(1 for t in tasks if t.enabled)
        total_found = sum(t.found_total for t in tasks)
        self.counter_label.setText(
            f"Задач: {len(tasks)} (активных {active}) · найдено объявлений: {total_found}"
        )

    def _selected_task_id(self) -> int | None:
        item = self.task_tree.currentItem()
        return None if item is None else int(item.data(0, Qt.UserRole))

    def _selected_task(self) -> Task | None:
        task_id = self._selected_task_id()
        return None if task_id is None else self._db.get_task(task_id)

    def _select_task(self, task_id: int) -> None:
        for index in range(self.task_tree.topLevelItemCount()):
            item = self.task_tree.topLevelItem(index)
            if int(item.data(0, Qt.UserRole)) == task_id:
                self.task_tree.setCurrentItem(item)
                return

    def _on_task_selected(self) -> None:
        task = self._selected_task()
        has_task = task is not None
        for action in (self.action_edit, self.action_delete, self.action_check,
                       self.action_open_search):
            action.setEnabled(has_task)
        if task is None:
            self.task_hint.setText(
                "Задач пока нет. Нажмите «Новая задача»: можно следить за "
                "поисковым запросом или за объявлениями, похожими на выбранное."
            )
            return

        parts = []
        if task.last_check:
            parts.append(
                "Последняя проверка: "
                + time.strftime("%H:%M:%S", time.localtime(task.last_check))
            )
        else:
            parts.append("Ещё ни разу не проверялась")
        if task.kind == KIND_SIMILAR and task.source_title:
            parts.append(f"Образец: {task.source_title}")
        if task.last_error:
            parts.append(f"⚠ {task.last_error}")
        self.task_hint.setText("\n".join(parts))

    def _on_task_item_changed(self, item: QTreeWidgetItem, column: int) -> None:
        if column != 0:
            return
        task = self._db.get_task(int(item.data(0, Qt.UserRole)))
        if task is None:
            return
        enabled = item.checkState(0) == Qt.Checked
        if enabled == task.enabled:
            return
        task.enabled = enabled
        self._db.update_task(task)
        self.log(f"«{task.name}»: {'включена' if enabled else 'выключена'}")
        if enabled:
            self._engine.schedule_soon(task.id)
        self._update_counters()

    def _add_task(self) -> None:
        dialog = TaskDialog(
            self, self._engine.fetcher,
            default_interval=store.current.default_interval,
        )
        if dialog.exec() != TaskDialog.Accepted:
            return
        task = self._db.add_task(dialog.result_task())
        self.log(f"Добавлена задача «{task.name}»")
        self._reload_tasks()
        self._select_task(task.id)
        if task.enabled:
            self._engine.check_now(task.id)
            if not self._engine.is_running:
                self._engine.start()

    def _edit_task(self) -> None:
        task = self._selected_task()
        if task is None:
            return
        dialog = TaskDialog(self, self._engine.fetcher, task=task)
        if dialog.exec() != TaskDialog.Accepted:
            return
        updated = dialog.result_task()
        self._db.update_task(updated)
        self._engine.schedule_soon(updated.id)
        self.log(f"Задача «{updated.name}» изменена")
        self._reload_tasks()

    def _delete_task(self) -> None:
        task = self._selected_task()
        if task is None:
            return
        answer = QMessageBox.question(
            self, "Удалить задачу",
            f"Удалить задачу «{task.name}»?\n\n"
            "Её объявления пропадут из ленты, а история просмотренных "
            "объявлений будет забыта.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No,
        )
        if answer != QMessageBox.Yes:
            return
        self._db.delete_task(task.id)
        self._engine.forget_task(task.id)
        self.log(f"Задача «{task.name}» удалена")
        self._reload_tasks()
        self._reload_feed()

    def _check_now(self) -> None:
        task = self._selected_task()
        if task is None:
            return
        if not self._engine.is_running:
            self._engine.start()
        self._engine.check_now(task.id)
        self.log(f"«{task.name}»: проверяю…")

    def _open_task_url(self) -> None:
        task = self._selected_task()
        if task is None:
            return
        url = task.search_url or task.url
        if url:
            QDesktopServices.openUrl(QUrl(url))
        else:
            QMessageBox.information(
                self, "Ссылки пока нет",
                "Ссылка на поиск появится после первой проверки задачи.",
            )

    # --- лента ------------------------------------------------------------
    def _reload_feed(self) -> None:
        settings = store.current
        self.feed.feed_model.set_limit(settings.max_feed_items)
        task_id = self.filter_combo.currentData()
        listings = self._db.recent_listings(settings.max_feed_items, task_id)
        self.feed.feed_model.set_items(listings)

    def _clear_feed(self) -> None:
        task_id = self.filter_combo.currentData()
        scope = "по выбранной задаче" if task_id else "полностью"
        answer = QMessageBox.question(
            self, "Очистить ленту",
            f"Убрать объявления из ленты {scope}?\n\n"
            "Список просмотренных объявлений сохранится, повторных "
            "уведомлений об этих объявлениях не будет.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No,
        )
        if answer != QMessageBox.Yes:
            return
        self._db.clear_listings(task_id)
        self._reload_feed()

    def _refresh_times(self) -> None:
        self.feed.viewport().update()
        self._on_task_selected()

    # --- реакция на движок ------------------------------------------------
    def _on_task_started(self, task_id: int) -> None:
        self._set_task_status_cell(task_id, STATUS_RUNNING, "")

    def _on_task_finished(self, task_id: int, status: str, error: str) -> None:
        self._set_task_status_cell(task_id, status, error)
        self._on_task_selected()

    def _set_task_status_cell(self, task_id: int, status: str, error: str) -> None:
        from ..models import STATUS_TITLES
        for index in range(self.task_tree.topLevelItemCount()):
            item = self.task_tree.topLevelItem(index)
            if int(item.data(0, Qt.UserRole)) != task_id:
                continue
            item.setText(3, STATUS_TITLES.get(status, status))
            colour = STATUS_COLORS.get(status)
            if colour:
                item.setForeground(3, QBrush(QColor(colour)))
            item.setToolTip(3, error)
            task = self._db.get_task(task_id)
            if task is not None:
                item.setText(4, str(task.found_total))
            return

    def _on_new_listings(self, task_id: int, listings: list) -> None:
        current_filter = self.filter_combo.currentData()
        if current_filter in (None, task_id):
            self.feed.feed_model.prepend(list(listings))
        self._update_counters()

        settings = store.current
        self._last_listing = listings[0] if listings else None
        if settings.notify_sound:
            QApplication.beep()
        if settings.notify_desktop and listings:
            self._show_tray_message(listings)
        if not self.isActiveWindow():
            self._unseen += len(listings)
            self.tray.setIcon(app_icon(alert=True))
            self.tray.setToolTip(f"{APP_TITLE} — новых объявлений: {self._unseen}")

    def _show_tray_message(self, listings: list) -> None:
        first: Listing = listings[0]
        if len(listings) == 1:
            title = "Новое объявление"
            body = f"{first.title}\n{first.price_label()}"
        else:
            title = f"Новых объявлений: {len(listings)}"
            body = f"{first.title}\n{first.price_label()} и ещё {len(listings) - 1}"
        self.tray.showMessage(title, body, app_icon(), 10_000)

    def _on_notification_clicked(self) -> None:
        if self._last_listing is not None and self._last_listing.url:
            QDesktopServices.openUrl(QUrl(self._last_listing.url))
        else:
            self.restore_window()

    def _on_notify_errors(self, errors: list) -> None:
        for error in errors:
            self.log(f"⚠ Уведомление не отправлено — {error}")

    def _on_running_changed(self, running: bool) -> None:
        self.action_watch.setText("■  Остановить слежение" if running else "▶  Начать слежение")
        self.tray_watch_action.setText("Остановить слежение" if running else "Начать слежение")
        self.status_label.setText("Слежение работает" if running else "Слежение остановлено")

    def _toggle_watching(self) -> None:
        if self._engine.is_running:
            self._engine.stop()
        else:
            if not self._db.list_tasks():
                QMessageBox.information(
                    self, "Нет задач",
                    "Сначала создайте хотя бы одну задачу слежения.",
                )
                return
            self._engine.start()

    def log(self, text: str) -> None:
        self.log_view.appendPlainText(f"{time.strftime('%H:%M:%S')}  {text}")

    # --- настройки --------------------------------------------------------
    def _open_settings(self) -> None:
        dialog = SettingsDialog(self, store.current)
        if dialog.exec() != SettingsDialog.Accepted:
            return
        settings = dialog.collect()
        store.replace(settings)
        self._engine.settings_changed()
        try:
            if autostart.is_enabled() != settings.autostart:
                autostart.set_enabled(settings.autostart)
        except OSError as exc:
            self.log(f"⚠ Не удалось изменить автозапуск: {exc}")
        self._reload_feed()
        self.log("Настройки сохранены")

    # --- окно и трей ------------------------------------------------------
    def _on_tray_activated(self, reason) -> None:
        if reason in (QSystemTrayIcon.Trigger, QSystemTrayIcon.DoubleClick):
            if self.isVisible() and not self.isMinimized():
                self.hide()
            else:
                self.restore_window()

    def restore_window(self) -> None:
        self.showNormal()
        self.raise_()
        self.activateWindow()
        self._unseen = 0
        self.tray.setIcon(app_icon())
        self.tray.setToolTip(APP_TITLE)

    def event(self, event):
        if event.type() == event.Type.WindowActivate:
            self._unseen = 0
            self.tray.setIcon(app_icon())
            self.tray.setToolTip(APP_TITLE)
        return super().event(event)

    def _restore_geometry(self) -> None:
        saved = store.current.window_geometry
        if saved:
            try:
                self.restoreGeometry(QByteArray.fromBase64(saved.encode("ascii")))
            except Exception:
                pass

    def _save_geometry(self) -> None:
        try:
            data = bytes(self.saveGeometry().toBase64()).decode("ascii")
            store.update(window_geometry=data)
        except Exception:
            pass

    def closeEvent(self, event) -> None:
        if self._quitting or not store.current.minimize_to_tray:
            self._shutdown()
            event.accept()
            return
        event.ignore()
        self.hide()
        self.tray.showMessage(
            APP_TITLE,
            "Приложение продолжает следить за объявлениями. "
            "Чтобы выйти — правый щелчок по значку в трее.",
            app_icon(), 5000,
        )

    def _quit(self) -> None:
        self._quitting = True
        self._shutdown()
        QApplication.quit()

    def _shutdown(self) -> None:
        self._save_geometry()
        self._engine.shutdown()
        self._loader.shutdown()
        self.tray.hide()
