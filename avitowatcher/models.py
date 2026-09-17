"""Модели данных."""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any

KIND_SEARCH = "search"
KIND_SIMILAR = "similar"

KIND_TITLES = {
    KIND_SEARCH: "По запросу",
    KIND_SIMILAR: "Похожие объявления",
}

STATUS_IDLE = "idle"
STATUS_OK = "ok"
STATUS_RUNNING = "running"
STATUS_BLOCKED = "blocked"
STATUS_ERROR = "error"

STATUS_TITLES = {
    STATUS_IDLE: "Ожидание",
    STATUS_OK: "Работает",
    STATUS_RUNNING: "Проверка…",
    STATUS_BLOCKED: "Авито блокирует",
    STATUS_ERROR: "Ошибка",
}


@dataclass
class Task:
    id: int = 0
    name: str = ""
    kind: str = KIND_SEARCH
    enabled: bool = True
    interval: int = 300
    url: str = ""                 # ссылка на поиск или на объявление-образец
    params: dict[str, Any] = field(default_factory=dict)
    created_at: float = field(default_factory=time.time)
    last_check: float = 0.0
    next_check: float = 0.0
    status: str = STATUS_IDLE
    last_error: str = ""
    found_total: int = 0

    # --- параметры поиска -------------------------------------------------
    @property
    def query(self) -> str:
        return str(self.params.get("query", ""))

    @property
    def region(self) -> str:
        return str(self.params.get("region", "rossiya"))

    @property
    def price_min(self) -> int | None:
        return self.params.get("price_min")

    @property
    def price_max(self) -> int | None:
        return self.params.get("price_max")

    @property
    def keywords_exclude(self) -> list[str]:
        raw = self.params.get("exclude", "")
        return [w.strip().lower() for w in str(raw).split(",") if w.strip()]

    # --- параметры «похожих» ----------------------------------------------
    @property
    def similarity(self) -> int:
        return int(self.params.get("similarity", 45))

    @property
    def price_tolerance(self) -> int:
        return int(self.params.get("price_tolerance", 40))

    @property
    def source_title(self) -> str:
        return str(self.params.get("source_title", ""))

    @property
    def source_price(self) -> int | None:
        return self.params.get("source_price")

    @property
    def search_url(self) -> str:
        """Для «похожих» — вычисленная ссылка поиска, для обычной задачи — url."""
        if self.kind == KIND_SIMILAR:
            return str(self.params.get("derived_url", ""))
        return self.url

    def status_title(self) -> str:
        return STATUS_TITLES.get(self.status, self.status)

    def kind_title(self) -> str:
        return KIND_TITLES.get(self.kind, self.kind)


@dataclass
class Listing:
    """Одно объявление в ленте."""

    item_id: str = ""
    task_id: int = 0
    title: str = ""
    url: str = ""
    price: int | None = None
    price_text: str = ""
    location: str = ""
    date_text: str = ""
    seller: str = ""
    description: str = ""
    image_url: str = ""
    score: int = 100              # степень похожести для задач вида «похожие»
    first_seen: float = field(default_factory=time.time)
    notified: bool = False
    task_name: str = ""

    def price_label(self) -> str:
        if self.price_text:
            return self.price_text
        if self.price is not None:
            return f"{self.price:,}".replace(",", " ") + " \u20bd"
        return "Цена не указана"
