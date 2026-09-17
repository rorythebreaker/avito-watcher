"""Окно создания и правки задачи слежения."""
from __future__ import annotations

import urllib.parse

from PySide6.QtCore import QObject, QRunnable, Qt, QThreadPool, Signal
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QRadioButton,
    QSlider,
    QSpinBox,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from .. import similarity
from ..engine import fetch_item_info, looks_like_item_url, normalize_search_url
from ..models import KIND_SEARCH, KIND_SIMILAR, Task
from ..parser import BASE, ItemInfo

# Города, которые чаще всего нужны. Значение — кусок адреса на Авито.
REGIONS = [
    ("Вся Россия", "rossiya"),
    ("Москва", "moskva"),
    ("Санкт-Петербург", "sankt-peterburg"),
    ("Новосибирск", "novosibirsk"),
    ("Екатеринбург", "ekaterinburg"),
    ("Казань", "kazan"),
    ("Нижний Новгород", "nizhniy_novgorod"),
    ("Челябинск", "chelyabinsk"),
    ("Самара", "samara"),
    ("Омск", "omsk"),
    ("Ростов-на-Дону", "rostov-na-donu"),
    ("Уфа", "ufa"),
    ("Красноярск", "krasnoyarsk"),
    ("Воронеж", "voronezh"),
    ("Пермь", "perm"),
    ("Волгоград", "volgograd"),
    ("Краснодар", "krasnodar"),
    ("Саратов", "saratov"),
    ("Тюмень", "tyumen"),
    ("Сочи", "sochi"),
    ("Владивосток", "vladivostok"),
    ("Калининград", "kaliningrad"),
]

INTERVALS = [
    ("1 минута", 60),
    ("2 минуты", 120),
    ("5 минут", 300),
    ("10 минут", 600),
    ("15 минут", 900),
    ("30 минут", 1800),
    ("1 час", 3600),
    ("3 часа", 10800),
    ("6 часов", 21600),
]


class _ProbeSignals(QObject):
    done = Signal(object)
    failed = Signal(str)


class _ProbeJob(QRunnable):
    """Разовая загрузка объявления-образца, чтобы показать превью в диалоге."""

    def __init__(self, fetcher, url: str, signals: _ProbeSignals) -> None:
        super().__init__()
        self._fetcher = fetcher
        self._url = url
        self._signals = signals

    def run(self) -> None:
        try:
            self._signals.done.emit(fetch_item_info(self._fetcher, self._url))
        except Exception as exc:
            self._signals.failed.emit(str(exc))


class TaskDialog(QDialog):
    def __init__(self, parent, fetcher, task: Task | None = None,
                 default_interval: int = 300) -> None:
        super().__init__(parent)
        self._fetcher = fetcher
        self._task = task
        self._info: ItemInfo | None = None
        self._pool = QThreadPool(self)
        self._pool.setMaxThreadCount(1)

        self.setWindowTitle("Изменить задачу" if task else "Новая задача")
        self.setMinimumWidth(560)
        self._build()
        self._load(task, default_interval)

    # --- сборка интерфейса ------------------------------------------------
    def _build(self) -> None:
        layout = QVBoxLayout(self)
        layout.setSpacing(12)

        kind_box = QGroupBox("Что отслеживаем")
        kind_layout = QHBoxLayout(kind_box)
        self.radio_search = QRadioButton("Объявления по запросу")
        self.radio_similar = QRadioButton("Похожие на конкретное объявление")
        self.radio_search.setChecked(True)
        kind_layout.addWidget(self.radio_search)
        kind_layout.addWidget(self.radio_similar)
        kind_layout.addStretch(1)
        layout.addWidget(kind_box)

        self.stack = QStackedWidget()
        self.stack.addWidget(self._build_search_page())
        self.stack.addWidget(self._build_similar_page())
        layout.addWidget(self.stack)

        self.radio_search.toggled.connect(
            lambda on: self.stack.setCurrentIndex(0 if on else 1)
        )

        common = QGroupBox("Общее")
        form = QFormLayout(common)
        self.name_edit = QLineEdit()
        self.name_edit.setPlaceholderText("Название задачи (подставится автоматически)")
        form.addRow("Название:", self.name_edit)

        self.interval_combo = QComboBox()
        for title, seconds in INTERVALS:
            self.interval_combo.addItem(title, seconds)
        form.addRow("Проверять раз в:", self.interval_combo)

        self.exclude_edit = QLineEdit()
        self.exclude_edit.setPlaceholderText("запчасти, на разбор, битый — через запятую")
        form.addRow("Исключать слова:", self.exclude_edit)

        self.notify_existing = QCheckBox(
            "Уведомить обо всех объявлениях уже при первой проверке"
        )
        self.notify_existing.setToolTip(
            "По умолчанию первая проверка молча запоминает текущую выдачу, "
            "и уведомления приходят только о том, что появилось потом."
        )
        form.addRow("", self.notify_existing)

        self.enabled_check = QCheckBox("Задача включена")
        self.enabled_check.setChecked(True)
        form.addRow("", self.enabled_check)
        layout.addWidget(common)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.button(QDialogButtonBox.Ok).setText("Сохранить")
        buttons.button(QDialogButtonBox.Cancel).setText("Отмена")
        buttons.accepted.connect(self._on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _build_search_page(self) -> QWidget:
        page = QWidget()
        outer = QVBoxLayout(page)
        outer.setContentsMargins(0, 0, 0, 0)

        hint = QLabel(
            "Задайте запрос и город — или настройте фильтры прямо на Авито "
            "и вставьте сюда ссылку из адресной строки: так доступны все фильтры сайта."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#8a93a0;")
        outer.addWidget(hint)

        form = QFormLayout()
        self.query_edit = QLineEdit()
        self.query_edit.setPlaceholderText("например: iphone 13 128")
        form.addRow("Запрос:", self.query_edit)

        self.region_combo = QComboBox()
        for title, slug in REGIONS:
            self.region_combo.addItem(title, slug)
        form.addRow("Город:", self.region_combo)

        prices = QHBoxLayout()
        self.price_min = QSpinBox()
        self.price_min.setRange(0, 1_000_000_000)
        self.price_min.setSingleStep(1000)
        self.price_min.setGroupSeparatorShown(True)
        self.price_min.setSpecialValueText("без ограничения")
        self.price_max = QSpinBox()
        self.price_max.setRange(0, 1_000_000_000)
        self.price_max.setSingleStep(1000)
        self.price_max.setGroupSeparatorShown(True)
        self.price_max.setSpecialValueText("без ограничения")
        prices.addWidget(QLabel("от"))
        prices.addWidget(self.price_min, 1)
        prices.addWidget(QLabel("до"))
        prices.addWidget(self.price_max, 1)
        prices.addWidget(QLabel("₽"))
        form.addRow("Цена:", prices)

        self.url_edit = QLineEdit()
        self.url_edit.setPlaceholderText(
            "https://www.avito.ru/moskva/telefony?q=iphone — необязательно"
        )
        form.addRow("Готовая ссылка:", self.url_edit)
        outer.addLayout(form)

        self.search_preview = QLabel()
        self.search_preview.setWordWrap(True)
        self.search_preview.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.search_preview.setStyleSheet("color:#6f7885;font-size:11px;")
        outer.addWidget(self.search_preview)

        for widget in (self.query_edit, self.url_edit):
            widget.textChanged.connect(self._update_search_preview)
        self.region_combo.currentIndexChanged.connect(self._update_search_preview)
        self.price_min.valueChanged.connect(self._update_search_preview)
        self.price_max.valueChanged.connect(self._update_search_preview)
        outer.addStretch(1)
        return page

    def _build_similar_page(self) -> QWidget:
        page = QWidget()
        outer = QVBoxLayout(page)
        outer.setContentsMargins(0, 0, 0, 0)

        hint = QLabel(
            "Вставьте ссылку на объявление-образец. Приложение разберёт его "
            "и будет искать новые объявления того же рода — в той же категории "
            "и городе, с похожим названием и близкой ценой."
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color:#8a93a0;")
        outer.addWidget(hint)

        row = QHBoxLayout()
        self.item_url_edit = QLineEdit()
        self.item_url_edit.setPlaceholderText(
            "https://www.avito.ru/moskva/telefony/iphone_13_128gb-1234567890"
        )
        self.probe_button = QPushButton("Проверить")
        self.probe_button.clicked.connect(self._probe)
        row.addWidget(self.item_url_edit, 1)
        row.addWidget(self.probe_button)
        outer.addLayout(row)

        self.item_preview = QLabel("Объявление ещё не проверено.")
        self.item_preview.setWordWrap(True)
        self.item_preview.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.item_preview.setStyleSheet(
            "background:#1e2228;border:1px solid #2e343d;border-radius:8px;padding:10px;"
        )
        outer.addWidget(self.item_preview)

        form = QFormLayout()
        sim_row = QHBoxLayout()
        self.similarity_slider = QSlider(Qt.Horizontal)
        self.similarity_slider.setRange(10, 95)
        self.similarity_slider.setValue(45)
        self.similarity_label = QLabel("45%")
        self.similarity_label.setMinimumWidth(44)
        self.similarity_slider.valueChanged.connect(
            lambda v: self.similarity_label.setText(f"{v}%")
        )
        sim_row.addWidget(self.similarity_slider, 1)
        sim_row.addWidget(self.similarity_label)
        form.addRow("Порог похожести:", sim_row)

        price_row = QHBoxLayout()
        self.tolerance_slider = QSlider(Qt.Horizontal)
        self.tolerance_slider.setRange(0, 100)
        self.tolerance_slider.setValue(40)
        self.tolerance_label = QLabel("±40%")
        self.tolerance_label.setMinimumWidth(44)
        self.tolerance_slider.valueChanged.connect(
            lambda v: self.tolerance_label.setText(f"±{v}%" if v else "любая")
        )
        price_row.addWidget(self.tolerance_slider, 1)
        price_row.addWidget(self.tolerance_label)
        form.addRow("Разброс цены:", price_row)
        outer.addLayout(form)

        note = QLabel(
            "Чем выше порог, тем строже отбор: 30–40% ловит широкий круг похожих "
            "товаров, 60% и выше — почти те же модели."
        )
        note.setWordWrap(True)
        note.setStyleSheet("color:#6f7885;font-size:11px;")
        outer.addWidget(note)
        outer.addStretch(1)
        return page

    # --- данные -----------------------------------------------------------
    def _load(self, task: Task | None, default_interval: int) -> None:
        interval = task.interval if task else default_interval
        index = self.interval_combo.findData(interval)
        self.interval_combo.setCurrentIndex(index if index >= 0 else 2)

        if task is None:
            self._update_search_preview()
            return

        self.name_edit.setText(task.name)
        self.enabled_check.setChecked(task.enabled)
        self.exclude_edit.setText(str(task.params.get("exclude", "")))
        self.notify_existing.setChecked(bool(task.params.get("notify_existing")))

        if task.kind == KIND_SIMILAR:
            self.radio_similar.setChecked(True)
            self.stack.setCurrentIndex(1)
            self.item_url_edit.setText(task.url)
            self.similarity_slider.setValue(task.similarity)
            self.tolerance_slider.setValue(task.price_tolerance)
            if task.source_title:
                self._show_item_summary(task.source_title, task.source_price,
                                        task.params.get("derived_url", ""))
        else:
            self.radio_search.setChecked(True)
            self.query_edit.setText(task.query)
            region_index = self.region_combo.findData(task.region)
            if region_index >= 0:
                self.region_combo.setCurrentIndex(region_index)
            self.price_min.setValue(task.price_min or 0)
            self.price_max.setValue(task.price_max or 0)
            if task.params.get("manual_url"):
                self.url_edit.setText(task.url)
            self._update_search_preview()

    def _built_search_url(self) -> str:
        manual = self.url_edit.text().strip()
        if manual:
            return normalize_search_url(manual)

        query = self.query_edit.text().strip()
        if not query:
            return ""
        params = [("q", query)]
        if self.price_min.value():
            params.append(("pmin", str(self.price_min.value())))
        if self.price_max.value():
            params.append(("pmax", str(self.price_max.value())))
        params.append(("s", "104"))
        region = self.region_combo.currentData()
        return f"{BASE}/{region}?{urllib.parse.urlencode(params)}"

    def _update_search_preview(self) -> None:
        url = self._built_search_url()
        self.search_preview.setText(f"Ссылка для проверки: {url}" if url else "")

    def _show_item_summary(self, title: str, price, derived_url: str) -> None:
        price_text = f"{price:,}".replace(",", " ") + " ₽" if price else "не указана"
        query = similarity.build_query(title)
        self.item_preview.setText(
            f"<b>{title}</b><br>Цена: {price_text}<br>"
            f"<span style='color:#8a93a0;'>Поисковый запрос: {query or '—'}<br>"
            f"{derived_url}</span>"
        )

    # --- проверка образца -------------------------------------------------
    def _probe(self) -> None:
        url = self.item_url_edit.text().strip()
        if not looks_like_item_url(url):
            QMessageBox.warning(
                self, "Проверьте ссылку",
                "Нужна ссылка на страницу конкретного объявления — она "
                "заканчивается числовым номером, например …_1234567890.",
            )
            return
        self.probe_button.setEnabled(False)
        self.probe_button.setText("Загружаю…")
        self.item_preview.setText("Загружаю объявление с Авито…")

        signals = _ProbeSignals(self)
        signals.done.connect(self._on_probe_done)
        signals.failed.connect(self._on_probe_failed)
        self._pool.start(_ProbeJob(self._fetcher, url, signals))

    def _on_probe_done(self, info: ItemInfo) -> None:
        self._info = info
        self.probe_button.setEnabled(True)
        self.probe_button.setText("Проверить")
        derived = similarity.build_search_url(info, self.tolerance_slider.value())
        self._show_item_summary(info.title, info.price, derived)
        if not self.name_edit.text().strip():
            self.name_edit.setText(f"Похожие: {info.title[:40]}")

    def _on_probe_failed(self, error: str) -> None:
        self.probe_button.setEnabled(True)
        self.probe_button.setText("Проверить")
        self.item_preview.setText(f"<span style='color:#ff7a5c;'>{error}</span>")

    # --- сохранение -------------------------------------------------------
    def _on_accept(self) -> None:
        if self.radio_similar.isChecked():
            url = self.item_url_edit.text().strip()
            if not looks_like_item_url(url):
                QMessageBox.warning(
                    self, "Нужна ссылка на объявление",
                    "Вставьте ссылку на конкретное объявление Авито.",
                )
                return
        elif not self._built_search_url():
            QMessageBox.warning(
                self, "Нечего отслеживать",
                "Введите поисковый запрос или вставьте готовую ссылку с Авито.",
            )
            return
        self.accept()

    def result_task(self) -> Task:
        task = self._task or Task()
        task.interval = int(self.interval_combo.currentData())
        task.enabled = self.enabled_check.isChecked()
        params = dict(task.params)
        params["exclude"] = self.exclude_edit.text().strip()
        params["notify_existing"] = self.notify_existing.isChecked()

        if self.radio_similar.isChecked():
            url = self.item_url_edit.text().strip()
            changed = task.kind != KIND_SIMILAR or task.url != url
            task.kind = KIND_SIMILAR
            task.url = url
            params["similarity"] = self.similarity_slider.value()
            params["price_tolerance"] = self.tolerance_slider.value()
            if self._info is not None:
                params["source_title"] = self._info.title
                params["source_price"] = self._info.price
                params["source_id"] = self._info.item_id
                params["derived_url"] = similarity.build_search_url(
                    self._info, self.tolerance_slider.value()
                )
            elif changed:
                # Ссылку поиска вычислит движок при первой проверке.
                params.pop("derived_url", None)
                params.pop("baseline_done", None)
            default_name = params.get("source_title") or "Похожие объявления"
        else:
            manual = self.url_edit.text().strip()
            task.kind = KIND_SEARCH
            task.url = self._built_search_url()
            params["manual_url"] = bool(manual)
            params["query"] = self.query_edit.text().strip()
            params["region"] = self.region_combo.currentData()
            params["price_min"] = self.price_min.value() or None
            params["price_max"] = self.price_max.value() or None
            default_name = params["query"] or "Поиск на Авито"

        task.params = params
        task.name = self.name_edit.text().strip() or str(default_name)[:60]
        return task
