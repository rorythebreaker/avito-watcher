"""Лента найденных объявлений: модель, отрисовка карточек и сам список."""
from __future__ import annotations

import time

from PySide6.QtCore import (
    QAbstractListModel,
    QModelIndex,
    QPoint,
    QRect,
    QSize,
    Qt,
    Signal,
)
from PySide6.QtGui import (
    QAction,
    QColor,
    QDesktopServices,
    QFont,
    QFontMetrics,
    QPainter,
    QPainterPath,
    QPen,
)
from PySide6.QtCore import QUrl
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QListView,
    QMenu,
    QStyle,
    QStyledItemDelegate,
)

from ..models import Listing
from .icons import placeholder_pixmap
from .images import THUMB_H, THUMB_W, ImageLoader

CARD_HEIGHT = THUMB_H + 24
PAD = 12

COL_BG = QColor("#1e2228")
COL_BG_HOVER = QColor("#252b33")
COL_BG_SEL = QColor("#243040")
COL_BORDER = QColor("#2e343d")
COL_ACCENT = QColor("#00aaff")
COL_TITLE = QColor("#e8ecf1")
COL_PRICE = QColor("#4ade80")
COL_MUTED = QColor("#8a93a0")
COL_BADGE = QColor("#323a45")


def _ago(timestamp: float) -> str:
    delta = max(0, int(time.time() - timestamp))
    if delta < 60:
        return "только что"
    if delta < 3600:
        return f"{delta // 60} мин назад"
    if delta < 86400:
        return f"{delta // 3600} ч назад"
    return time.strftime("%d.%m %H:%M", time.localtime(timestamp))


class FeedModel(QAbstractListModel):
    """Список объявлений, новые добавляются сверху."""

    ListingRole = Qt.UserRole + 1

    def __init__(self, parent=None, limit: int = 500) -> None:
        super().__init__(parent)
        self._items: list[Listing] = []
        self._limit = limit

    def set_limit(self, limit: int) -> None:
        self._limit = max(50, limit)
        self._trim()

    def rowCount(self, parent=QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self._items)

    def data(self, index: QModelIndex, role: int = Qt.DisplayRole):
        if not index.isValid():
            return None
        listing = self._items[index.row()]
        if role == self.ListingRole:
            return listing
        if role == Qt.DisplayRole:
            return listing.title
        if role == Qt.ToolTipRole:
            parts = [listing.title, listing.price_label()]
            if listing.description:
                parts.append(listing.description)
            parts.append(listing.url)
            return "\n".join(p for p in parts if p)
        return None

    def listing_at(self, index: QModelIndex) -> Listing | None:
        if not index.isValid():
            return None
        return self._items[index.row()]

    def set_items(self, listings: list[Listing]) -> None:
        self.beginResetModel()
        self._items = list(listings)[: self._limit]
        self.endResetModel()

    def prepend(self, listings: list[Listing]) -> None:
        if not listings:
            return
        self.beginInsertRows(QModelIndex(), 0, len(listings) - 1)
        self._items[:0] = listings
        self.endInsertRows()
        self._trim()

    def clear(self) -> None:
        self.beginResetModel()
        self._items = []
        self.endResetModel()

    def _trim(self) -> None:
        extra = len(self._items) - self._limit
        if extra <= 0:
            return
        start = self._limit
        self.beginRemoveRows(QModelIndex(), start, start + extra - 1)
        del self._items[start:]
        self.endRemoveRows()

    def row_for_url(self, url: str) -> list[int]:
        return [i for i, item in enumerate(self._items) if item.image_url == url]


class FeedDelegate(QStyledItemDelegate):
    """Рисует карточку объявления: фото, название, цена, откуда и когда."""

    def __init__(self, loader: ImageLoader, parent=None) -> None:
        super().__init__(parent)
        self._loader = loader
        self._placeholder = placeholder_pixmap(THUMB_W, THUMB_H)

    def sizeHint(self, option, index) -> QSize:
        return QSize(420, CARD_HEIGHT)

    def paint(self, painter: QPainter, option, index) -> None:
        listing: Listing = index.data(FeedModel.ListingRole)
        if listing is None:
            return
        painter.save()
        painter.setRenderHint(QPainter.Antialiasing)

        rect = option.rect.adjusted(6, 4, -6, -4)
        selected = bool(option.state & QStyle.State_Selected)
        hovered = bool(option.state & QStyle.State_MouseOver)
        background = COL_BG_SEL if selected else (COL_BG_HOVER if hovered else COL_BG)

        path = QPainterPath()
        path.addRoundedRect(rect, 10, 10)
        painter.fillPath(path, background)
        painter.setPen(QPen(COL_ACCENT if selected else COL_BORDER, 1))
        painter.drawPath(path)

        # Фотография.
        image_rect = QRect(rect.left() + 10, rect.top() + 10, THUMB_W, THUMB_H)
        pixmap = self._loader.request(listing.image_url) if listing.image_url else None
        clip = QPainterPath()
        clip.addRoundedRect(image_rect, 7, 7)
        painter.save()
        painter.setClipPath(clip)
        if pixmap is not None:
            source = pixmap.copy(0, 0, min(pixmap.width(), THUMB_W),
                                 min(pixmap.height(), THUMB_H))
            painter.drawPixmap(image_rect.topLeft(), source)
        else:
            painter.drawPixmap(image_rect.topLeft(), self._placeholder)
        painter.restore()

        left = image_rect.right() + PAD
        width = rect.right() - left - PAD

        # Плашки справа: задача и, для «похожих», степень совпадения.
        badge_right = rect.right() - PAD
        small = QFont(option.font)
        small.setPointSizeF(max(7.5, option.font.pointSizeF() - 1.5))
        painter.setFont(small)
        metrics = QFontMetrics(small)

        if listing.score < 100:
            text = f"{listing.score}%"
            badge_w = metrics.horizontalAdvance(text) + 14
            badge = QRect(badge_right - badge_w, rect.top() + 10, badge_w, 18)
            self._pill(painter, badge, text, COL_ACCENT, QColor("#0b1620"))
            badge_right = badge.left() - 6

        if listing.task_name:
            text = metrics.elidedText(listing.task_name, Qt.ElideRight, 150)
            badge_w = metrics.horizontalAdvance(text) + 14
            badge = QRect(badge_right - badge_w, rect.top() + 10, badge_w, 18)
            self._pill(painter, badge, text, COL_BADGE, COL_MUTED)
            badge_right = badge.left() - 6

        title_width = max(60, badge_right - left - 8)

        # Название.
        title_font = QFont(option.font)
        title_font.setBold(True)
        painter.setFont(title_font)
        painter.setPen(COL_TITLE)
        title_metrics = QFontMetrics(title_font)
        title = title_metrics.elidedText(
            listing.title or "Без названия", Qt.ElideRight, title_width
        )
        painter.drawText(
            QRect(left, rect.top() + 10, title_width, 20),
            Qt.AlignLeft | Qt.AlignVCenter, title,
        )

        # Цена.
        price_font = QFont(option.font)
        price_font.setBold(True)
        price_font.setPointSizeF(option.font.pointSizeF() + 2)
        painter.setFont(price_font)
        painter.setPen(COL_PRICE)
        painter.drawText(
            QRect(left, rect.top() + 34, width, 24),
            Qt.AlignLeft | Qt.AlignVCenter, listing.price_label(),
        )

        # Где и когда.
        painter.setFont(small)
        painter.setPen(COL_MUTED)
        meta = " · ".join(
            x for x in (listing.location, listing.date_text, listing.seller) if x
        )
        painter.drawText(
            QRect(left, rect.top() + 60, width, 18),
            Qt.AlignLeft | Qt.AlignVCenter,
            metrics.elidedText(meta, Qt.ElideRight, width),
        )
        painter.drawText(
            QRect(left, rect.top() + 78, width, 18),
            Qt.AlignLeft | Qt.AlignVCenter,
            f"найдено {_ago(listing.first_seen)}",
        )
        painter.restore()

    def _pill(self, painter: QPainter, rect: QRect, text: str,
              background: QColor, foreground: QColor) -> None:
        path = QPainterPath()
        path.addRoundedRect(rect, 9, 9)
        painter.fillPath(path, background)
        painter.setPen(foreground)
        painter.drawText(rect, Qt.AlignCenter, text)


class FeedView(QListView):
    """Список карточек с открытием объявления по двойному щелчку."""

    open_requested = Signal(object)

    def __init__(self, loader: ImageLoader, parent=None) -> None:
        super().__init__(parent)
        self._loader = loader
        self._model = FeedModel(self)
        self.setModel(self._model)
        self.setItemDelegate(FeedDelegate(loader, self))
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)
        self.setMouseTracking(True)
        self.setUniformItemSizes(True)
        self.setVerticalScrollMode(QAbstractItemView.ScrollPerPixel)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setContextMenuPolicy(Qt.CustomContextMenu)
        self.customContextMenuRequested.connect(self._context_menu)
        self.doubleClicked.connect(self._open_index)
        loader.ready.connect(self._on_image_ready)

    @property
    def feed_model(self) -> FeedModel:
        return self._model

    def _on_image_ready(self, url: str, _pixmap) -> None:
        for row in self._model.row_for_url(url):
            index = self._model.index(row, 0)
            self.update(index)

    def _open_index(self, index: QModelIndex) -> None:
        listing = self._model.listing_at(index)
        if listing is not None and listing.url:
            QDesktopServices.openUrl(QUrl(listing.url))

    def selected_listings(self) -> list[Listing]:
        return [
            self._model.listing_at(i)
            for i in self.selectedIndexes()
            if self._model.listing_at(i) is not None
        ]

    def _context_menu(self, position: QPoint) -> None:
        index = self.indexAt(position)
        listing = self._model.listing_at(index)
        if listing is None:
            return
        menu = QMenu(self)

        open_action = QAction("Открыть на Авито", menu)
        open_action.triggered.connect(lambda: self._open_index(index))
        menu.addAction(open_action)

        copy_action = QAction("Копировать ссылку", menu)
        copy_action.triggered.connect(
            lambda: QApplication.clipboard().setText(listing.url)
        )
        menu.addAction(copy_action)

        copy_all = QAction("Копировать ссылки выделенных", menu)
        copy_all.triggered.connect(
            lambda: QApplication.clipboard().setText(
                "\n".join(l.url for l in self.selected_listings())
            )
        )
        menu.addAction(copy_all)
        menu.exec(self.viewport().mapToGlobal(position))
