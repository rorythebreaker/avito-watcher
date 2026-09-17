"""Хранилище SQLite: задачи, найденные объявления, журнал просмотренных id."""
from __future__ import annotations

import json
import sqlite3
import threading
import time
from typing import Iterable

from .models import Listing, Task
from .paths import db_path

SCHEMA = """
CREATE TABLE IF NOT EXISTS tasks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT    NOT NULL,
    kind        TEXT    NOT NULL,
    enabled     INTEGER NOT NULL DEFAULT 1,
    interval    INTEGER NOT NULL DEFAULT 300,
    url         TEXT    NOT NULL DEFAULT '',
    params      TEXT    NOT NULL DEFAULT '{}',
    created_at  REAL    NOT NULL,
    last_check  REAL    NOT NULL DEFAULT 0,
    status      TEXT    NOT NULL DEFAULT 'idle',
    last_error  TEXT    NOT NULL DEFAULT '',
    found_total INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS seen (
    task_id  INTEGER NOT NULL,
    item_id  TEXT    NOT NULL,
    seen_at  REAL    NOT NULL,
    PRIMARY KEY (task_id, item_id)
);

CREATE TABLE IF NOT EXISTS listings (
    row_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id     TEXT    NOT NULL,
    task_id     INTEGER NOT NULL,
    title       TEXT    NOT NULL DEFAULT '',
    url         TEXT    NOT NULL DEFAULT '',
    price       INTEGER,
    price_text  TEXT    NOT NULL DEFAULT '',
    location    TEXT    NOT NULL DEFAULT '',
    date_text   TEXT    NOT NULL DEFAULT '',
    seller      TEXT    NOT NULL DEFAULT '',
    description TEXT    NOT NULL DEFAULT '',
    image_url   TEXT    NOT NULL DEFAULT '',
    score       INTEGER NOT NULL DEFAULT 100,
    first_seen  REAL    NOT NULL,
    notified    INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_listings_seen ON listings(first_seen DESC);
CREATE INDEX IF NOT EXISTS idx_listings_task ON listings(task_id);
CREATE INDEX IF NOT EXISTS idx_seen_at ON seen(seen_at);
"""


class Database:
    """Потокобезопасная обёртка: одно соединение под общим замком."""

    def __init__(self, path=None) -> None:
        self._lock = threading.RLock()
        self._conn = sqlite3.connect(
            str(path or db_path()), check_same_thread=False, timeout=15
        )
        self._conn.row_factory = sqlite3.Row
        with self._lock:
            self._conn.executescript(SCHEMA)
            self._conn.commit()

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    # --- задачи -----------------------------------------------------------
    def _row_to_task(self, row: sqlite3.Row) -> Task:
        try:
            params = json.loads(row["params"])
        except Exception:
            params = {}
        task = Task(
            id=row["id"],
            name=row["name"],
            kind=row["kind"],
            enabled=bool(row["enabled"]),
            interval=row["interval"],
            url=row["url"],
            params=params,
            created_at=row["created_at"],
            last_check=row["last_check"],
            status=row["status"],
            last_error=row["last_error"],
            found_total=row["found_total"],
        )
        task.next_check = task.last_check + task.interval
        return task

    def list_tasks(self) -> list[Task]:
        with self._lock:
            rows = self._conn.execute(
                "SELECT * FROM tasks ORDER BY created_at"
            ).fetchall()
        return [self._row_to_task(r) for r in rows]

    def get_task(self, task_id: int) -> Task | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT * FROM tasks WHERE id = ?", (task_id,)
            ).fetchone()
        return self._row_to_task(row) if row else None

    def add_task(self, task: Task) -> Task:
        with self._lock:
            cur = self._conn.execute(
                """INSERT INTO tasks
                   (name, kind, enabled, interval, url, params, created_at,
                    last_check, status, last_error, found_total)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?)""",
                (
                    task.name, task.kind, int(task.enabled), task.interval,
                    task.url, json.dumps(task.params, ensure_ascii=False),
                    task.created_at, task.last_check, task.status,
                    task.last_error, task.found_total,
                ),
            )
            self._conn.commit()
            task.id = int(cur.lastrowid)
        return task

    def update_task(self, task: Task) -> None:
        with self._lock:
            self._conn.execute(
                """UPDATE tasks SET name=?, kind=?, enabled=?, interval=?, url=?,
                       params=?, last_check=?, status=?, last_error=?, found_total=?
                   WHERE id=?""",
                (
                    task.name, task.kind, int(task.enabled), task.interval,
                    task.url, json.dumps(task.params, ensure_ascii=False),
                    task.last_check, task.status, task.last_error,
                    task.found_total, task.id,
                ),
            )
            self._conn.commit()

    def set_task_status(self, task_id: int, status: str, error: str = "") -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE tasks SET status=?, last_error=? WHERE id=?",
                (status, error, task_id),
            )
            self._conn.commit()

    def delete_task(self, task_id: int) -> None:
        with self._lock:
            self._conn.execute("DELETE FROM tasks WHERE id=?", (task_id,))
            self._conn.execute("DELETE FROM seen WHERE task_id=?", (task_id,))
            self._conn.execute("DELETE FROM listings WHERE task_id=?", (task_id,))
            self._conn.commit()

    # --- дедупликация -----------------------------------------------------
    def filter_new(self, task_id: int, item_ids: Iterable[str]) -> set[str]:
        """Возвращает те id, которых ещё не видели по этой задаче."""
        ids = [i for i in item_ids if i]
        if not ids:
            return set()
        with self._lock:
            placeholders = ",".join("?" * len(ids))
            rows = self._conn.execute(
                f"SELECT item_id FROM seen WHERE task_id=? AND item_id IN ({placeholders})",
                (task_id, *ids),
            ).fetchall()
        known = {r["item_id"] for r in rows}
        return {i for i in ids if i not in known}

    def mark_seen(self, task_id: int, item_ids: Iterable[str]) -> None:
        now = time.time()
        rows = [(task_id, i, now) for i in item_ids if i]
        if not rows:
            return
        with self._lock:
            self._conn.executemany(
                "INSERT OR IGNORE INTO seen (task_id, item_id, seen_at) VALUES (?,?,?)",
                rows,
            )
            self._conn.commit()

    def seen_count(self, task_id: int) -> int:
        with self._lock:
            row = self._conn.execute(
                "SELECT COUNT(*) AS n FROM seen WHERE task_id=?", (task_id,)
            ).fetchone()
        return int(row["n"])

    # --- лента ------------------------------------------------------------
    def add_listings(self, listings: list[Listing]) -> None:
        if not listings:
            return
        with self._lock:
            self._conn.executemany(
                """INSERT INTO listings
                   (item_id, task_id, title, url, price, price_text, location,
                    date_text, seller, description, image_url, score, first_seen, notified)
                   VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                [
                    (
                        l.item_id, l.task_id, l.title, l.url, l.price, l.price_text,
                        l.location, l.date_text, l.seller, l.description,
                        l.image_url, l.score, l.first_seen, int(l.notified),
                    )
                    for l in listings
                ],
            )
            self._conn.commit()

    def recent_listings(self, limit: int = 300, task_id: int | None = None) -> list[Listing]:
        sql = """SELECT l.*, t.name AS task_name FROM listings l
                 LEFT JOIN tasks t ON t.id = l.task_id"""
        args: list = []
        if task_id is not None:
            sql += " WHERE l.task_id = ?"
            args.append(task_id)
        sql += " ORDER BY l.first_seen DESC LIMIT ?"
        args.append(limit)
        with self._lock:
            rows = self._conn.execute(sql, args).fetchall()
        return [
            Listing(
                item_id=r["item_id"], task_id=r["task_id"], title=r["title"],
                url=r["url"], price=r["price"], price_text=r["price_text"],
                location=r["location"], date_text=r["date_text"], seller=r["seller"],
                description=r["description"], image_url=r["image_url"],
                score=r["score"], first_seen=r["first_seen"],
                notified=bool(r["notified"]), task_name=r["task_name"] or "",
            )
            for r in rows
        ]

    def clear_listings(self, task_id: int | None = None) -> None:
        with self._lock:
            if task_id is None:
                self._conn.execute("DELETE FROM listings")
            else:
                self._conn.execute("DELETE FROM listings WHERE task_id=?", (task_id,))
            self._conn.commit()

    def prune(self, keep_days: int, max_items: int) -> None:
        """Чистит старые записи, чтобы база не росла бесконечно."""
        cutoff = time.time() - keep_days * 86400
        with self._lock:
            self._conn.execute("DELETE FROM seen WHERE seen_at < ?", (cutoff,))
            self._conn.execute(
                """DELETE FROM listings WHERE row_id NOT IN (
                       SELECT row_id FROM listings ORDER BY first_seen DESC LIMIT ?
                   )""",
                (max_items,),
            )
            self._conn.commit()
