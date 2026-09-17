"""Движок слежения: фоновый поток, который обходит задачи по расписанию.

Один поток на всё приложение — так запросы к Авито идут строго по очереди
с паузами между ними. Результаты передаются в интерфейс через сигналы Qt,
они безопасно пересекают границу потоков.
"""
from __future__ import annotations

import logging
import queue
import re
import threading
import time
import urllib.parse

from PySide6.QtCore import QObject, Signal

from . import notify, similarity
from .config import SettingsStore
from .db import Database
from .fetcher import BlockedError, Fetcher, RateLimiter, is_empty_result
from .models import (
    KIND_SEARCH,
    KIND_SIMILAR,
    STATUS_BLOCKED,
    STATUS_ERROR,
    STATUS_OK,
    STATUS_RUNNING,
    Listing,
    Task,
)
from .parser import ItemInfo, is_removed_item, parse_item, parse_search

log = logging.getLogger(__name__)

MIN_INTERVAL = 60  # чаще раза в минуту Авито дёргать не будем


def normalize_search_url(url: str) -> str:
    """Приводит ссылку поиска к виду «сначала свежие, первая страница»."""
    url = url.strip()
    if not url:
        return ""
    if not url.startswith("http"):
        url = "https://" + url.lstrip("/")
    parts = urllib.parse.urlsplit(url)
    params = urllib.parse.parse_qsl(parts.query, keep_blank_values=True)
    params = [(k, v) for k, v in params if k != "p"]  # всегда первая страница
    if not any(k == "s" for k, _ in params):
        params.append(("s", "104"))  # сортировка по дате
    return urllib.parse.urlunsplit(
        (parts.scheme, parts.netloc, parts.path, urllib.parse.urlencode(params), "")
    )


_ITEM_URL = re.compile(r"[-_](\d{6,})/?$")


def looks_like_item_url(url: str) -> bool:
    """Ссылка на конкретное объявление (в конце числовой номер)?"""
    path = urllib.parse.urlsplit(url.strip()).path
    return bool(_ITEM_URL.search(path)) and len([p for p in path.split("/") if p]) >= 2


def fetch_item_info(fetcher: Fetcher, url: str) -> ItemInfo:
    """Загружает объявление-образец и разбирает его (для задачи «похожие»)."""
    html = fetcher.get(url)
    if is_removed_item(html):
        raise RuntimeError(
            "Это объявление снято с публикации, взять его за образец нельзя. "
            "Выберите другое, похожее объявление."
        )
    info = parse_item(html, url)
    if not info.title:
        raise RuntimeError(
            "Не удалось прочитать объявление. Проверьте ссылку — нужна ссылка "
            "на страницу конкретного объявления."
        )
    return info


class WatcherEngine(QObject):
    """Планировщик проверок. Запускается и останавливается из интерфейса."""

    task_started = Signal(int)                  # task_id
    task_finished = Signal(int, str, str)       # task_id, статус, текст ошибки
    new_listings = Signal(int, list)            # task_id, список Listing
    notify_errors = Signal(list)                # проблемы с каналами уведомлений
    message = Signal(str)                       # строка для журнала в интерфейсе
    running_changed = Signal(bool)

    def __init__(self, db: Database, settings_store: SettingsStore) -> None:
        super().__init__()
        self._db = db
        self._settings = settings_store
        self._limiter = RateLimiter()
        self._fetcher = Fetcher(settings_store.current, self._limiter)
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._wake = threading.Event()
        self._manual: queue.Queue[int] = queue.Queue()
        self._next_run: dict[int, float] = {}
        self._last_prune = 0.0

    # --- управление -------------------------------------------------------
    @property
    def is_running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self) -> None:
        if self.is_running:
            return
        self._stop.clear()
        self._fetcher.update_settings(self._settings.current)
        self._thread = threading.Thread(target=self._loop, name="watcher", daemon=True)
        self._thread.start()
        self.running_changed.emit(True)
        self.message.emit("Слежение запущено")

    def stop(self, timeout: float = 5.0) -> None:
        if not self.is_running:
            return
        self._stop.set()
        self._wake.set()
        if self._thread is not None:
            self._thread.join(timeout=timeout)
        self._thread = None
        self.running_changed.emit(False)
        self.message.emit("Слежение остановлено")

    def shutdown(self) -> None:
        self.stop(timeout=3.0)
        self._fetcher.close()

    def settings_changed(self) -> None:
        self._fetcher.update_settings(self._settings.current)
        self._wake.set()

    def check_now(self, task_id: int) -> None:
        """Внеочередная проверка задачи."""
        self._manual.put(task_id)
        self._wake.set()

    def schedule_soon(self, task_id: int, delay: float = 2.0) -> None:
        self._next_run[task_id] = time.time() + delay
        self._wake.set()

    def forget_task(self, task_id: int) -> None:
        self._next_run.pop(task_id, None)

    @property
    def fetcher(self) -> Fetcher:
        return self._fetcher

    # --- основной цикл ----------------------------------------------------
    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                self._tick()
            except Exception:
                log.exception("Сбой в цикле слежения")
            self._wake.wait(timeout=2.0)
            self._wake.clear()

    def _tick(self) -> None:
        now = time.time()

        # Внеочередные проверки — вперёд расписания.
        while not self._manual.empty():
            try:
                task_id = self._manual.get_nowait()
            except queue.Empty:
                break
            task = self._db.get_task(task_id)
            if task is not None:
                self._run_task(task, manual=True)
            if self._stop.is_set():
                return

        for task in self._db.list_tasks():
            if self._stop.is_set():
                return
            if not task.enabled:
                continue
            interval = max(MIN_INTERVAL, task.interval)
            due = self._next_run.get(task.id, task.last_check + interval)
            if now >= due:
                self._run_task(task)

        if now - self._last_prune > 3600:
            self._last_prune = now
            settings = self._settings.current
            try:
                self._db.prune(settings.keep_history_days, settings.max_feed_items * 4)
            except Exception:
                log.exception("Не удалось почистить базу")

    # --- проверка одной задачи -------------------------------------------
    def _run_task(self, task: Task, manual: bool = False) -> None:
        settings = self._settings.current
        self._db.set_task_status(task.id, STATUS_RUNNING)
        self.task_started.emit(task.id)

        status, error = STATUS_OK, ""
        try:
            self._check_task(task, settings)
        except BlockedError as exc:
            status, error = STATUS_BLOCKED, str(exc)
            log.warning("Задача %s: блокировка — %s", task.name, exc)
        except Exception as exc:
            status, error = STATUS_ERROR, str(exc)
            log.exception("Задача %s: ошибка", task.name)

        task.last_check = time.time()
        task.status = status
        task.last_error = error
        self._db.update_task(task)
        self._next_run[task.id] = task.last_check + max(MIN_INTERVAL, task.interval)
        self.task_finished.emit(task.id, status, error)
        if error:
            self.message.emit(f"«{task.name}»: {error}")

    def _check_task(self, task: Task, settings) -> None:
        if task.kind == KIND_SIMILAR:
            self._ensure_similar_ready(task)
        search_url = normalize_search_url(task.search_url)
        if not search_url:
            raise RuntimeError("У задачи не задана ссылка для поиска")

        html = self._fetcher.get(
            search_url, stop_event=self._stop, require='data-marker="item"'
        )
        log.info(
            "Задача %s: страница получена (%s)", task.name, self._fetcher.last_transport
        )
        found = parse_search(html)
        if not found:
            if is_empty_result(html):
                # По запросу пока просто ничего нет — ждём дальше.
                task.params["baseline_done"] = True
                return
            raise RuntimeError(
                "Авито вернул страницу без объявлений. Проверьте ссылку "
                "или попробуйте позже."
            )

        candidates = [l for l in found if self._passes_filters(task, l)]
        first_run = not task.params.get("baseline_done")
        new_ids = self._db.filter_new(task.id, [l.item_id for l in candidates])

        # Первый проход — запоминаем текущую выдачу молча, иначе пользователь
        # получит полсотни уведомлений о давно висящих объявлениях.
        if first_run and not task.params.get("notify_existing"):
            self._db.mark_seen(task.id, [l.item_id for l in found])
            task.params["baseline_done"] = True
            self.message.emit(
                f"«{task.name}»: запомнил {len(candidates)} текущих объявлений, "
                f"жду новые"
            )
            return

        task.params["baseline_done"] = True
        fresh = [l for l in candidates if l.item_id in new_ids]
        self._db.mark_seen(task.id, [l.item_id for l in found])
        if not fresh:
            return

        for listing in fresh:
            listing.task_id = task.id
            listing.task_name = task.name
        task.found_total += len(fresh)

        self._db.add_listings(fresh)
        self.new_listings.emit(task.id, fresh)
        self.message.emit(f"«{task.name}»: новых объявлений — {len(fresh)}")
        log.info("Задача %s: новых объявлений %d", task.name, len(fresh))

        errors = notify.dispatch(settings, fresh, task.name)
        if errors:
            self.notify_errors.emit(errors)

    # Сколько слов заголовка пробуем оставить в запросе, от точного к широкому.
    QUERY_WIDTHS = (4, 3, 2)
    ENOUGH_NEIGHBOURS = 3

    def _ensure_similar_ready(self, task: Task) -> None:
        """Для задачи «похожие» один раз вычисляет поисковую ссылку."""
        if task.params.get("derived_url"):
            return
        info = fetch_item_info(self._fetcher, task.url)
        task.params["source_title"] = info.title
        task.params["source_price"] = info.price
        task.params["source_id"] = info.item_id
        task.params["derived_url"] = self._pick_search_url(task, info)
        self._db.update_task(task)
        self.message.emit(
            f"«{task.name}»: образец — {info.title} "
            f"(запрос «{task.params.get('derived_query', '')}»)"
        )

    def _pick_search_url(self, task: Task, info: ItemInfo) -> str:
        """Подбирает ширину запроса, чтобы в выдаче были не только образец.

        Полный заголовок объявления в качестве запроса часто находит ровно
        одно объявление — то самое, с которого мы начали. Поэтому один раз,
        при заведении задачи, пробуем запрос покороче, пока в выдаче не
        появятся соседи.
        """
        fallback_url, fallback_query = "", ""
        for width in self.QUERY_WIDTHS:
            query = similarity.build_query(info.title, width)
            if not query:
                continue
            url = similarity.build_search_url(info, task.price_tolerance, query)
            if not fallback_url:
                fallback_url, fallback_query = url, query
            try:
                html = self._fetcher.get(
                    url, stop_event=self._stop, require='data-marker="item"'
                )
            except Exception as exc:
                log.info("Проба запроса «%s» не удалась: %s", query, exc)
                continue
            neighbours = [
                item for item in parse_search(html)
                if item.item_id != info.item_id
            ]
            log.info("Запрос «%s»: соседей %d", query, len(neighbours))
            if len(neighbours) >= self.ENOUGH_NEIGHBOURS:
                task.params["derived_query"] = query
                return url
            fallback_url, fallback_query = url, query
        task.params["derived_query"] = fallback_query
        return fallback_url

    def _passes_filters(self, task: Task, listing: Listing) -> bool:
        title = (listing.title or "").lower()
        for word in task.keywords_exclude:
            if word in title:
                return False

        if task.kind == KIND_SEARCH:
            if task.price_min is not None and listing.price is not None:
                if listing.price < task.price_min:
                    return False
            if task.price_max is not None and listing.price is not None:
                if listing.price > task.price_max:
                    return False
            return True

        # «Похожие»: само объявление-образец в ленту не пускаем.
        if listing.item_id and listing.item_id == str(task.params.get("source_id", "")):
            return False
        listing.score = similarity.score(
            task.source_title, task.source_price,
            listing.title, listing.price, task.price_tolerance,
        )
        return listing.score >= task.similarity
