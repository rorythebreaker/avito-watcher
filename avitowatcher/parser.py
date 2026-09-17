"""Разбор страниц Авито: выдача поиска и карточка объявления.

Вёрстка Авито меняется, поэтому каждое поле ищется несколькими способами:
сперва по атрибутам data-marker, потом по микроразметке itemprop, потом
регулярками по тексту. Если разметку опять перекроят, отвалится одно поле,
а не весь разбор.
"""
from __future__ import annotations

import html as html_mod
import json
import re
import urllib.parse
from dataclasses import dataclass

from bs4 import BeautifulSoup

from .models import Listing

BASE = "https://www.avito.ru"

# В адресе объявления номер отделён дефисом: /moskva/telefony/iphone_13-1234567890
_ID_FROM_URL = re.compile(r"[-_](\d{6,})(?:[/?#]|$)")
_DIGITS = re.compile(r"\d+")


def _soup(html: str) -> BeautifulSoup:
    """Разбирает страницу, страхуясь от обрыва разбора.

    lxml быстрее, но на больших страницах выдачи Авито (больше двух мегабайт)
    libxml2 молча обрывается на первых же тегах и разбор даёт ноль карточек.
    Поэтому сверяем число найденных атрибутов data-marker с тем, сколько их в
    сырой разметке, и при недоборе перечитываем встроенным разборщиком —
    он медленнее, зато дочитывает страницу до конца.
    """
    expected = html.count('data-marker="')
    try:
        soup = BeautifulSoup(html, "lxml")
        if expected == 0 or len(soup.select("[data-marker]")) >= expected:
            return soup
    except Exception:
        pass
    return BeautifulSoup(html, "html.parser")


def _text(node) -> str:
    if node is None:
        return ""
    return re.sub(r"\s+", " ", node.get_text(" ", strip=True)).strip()


def _first(root, selectors: list[str]):
    for sel in selectors:
        node = root.select_one(sel)
        if node is not None:
            return node
    return None


def _abs_url(href: str) -> str:
    if not href:
        return ""
    if href.startswith("http"):
        return href
    return urllib.parse.urljoin(BASE, href)


def _price_from_text(text: str) -> int | None:
    digits = "".join(_DIGITS.findall(text.replace(" ", "").replace(" ", "")))
    if not digits:
        return None
    try:
        value = int(digits)
    except ValueError:
        return None
    return value if 0 < value < 10**12 else None


# В карточках ниже первого экрана Авито не отдаёт тег <img> вовсе, но адрес
# фотографии остаётся в атрибуте data-marker слайдера.
_SLIDER_PREFIX = "slider-image/image-"


def _from_srcset(srcset: str) -> str:
    """Из набора «адрес 208w, адрес 416w» берёт самую широкую картинку."""
    best_url, best_width = "", -1
    for part in srcset.split(","):
        chunks = part.strip().split()
        if not chunks:
            continue
        width = _price_from_text(chunks[1]) or 0 if len(chunks) > 1 else 0
        if width > best_width:
            best_url, best_width = chunks[0], width
    return best_url


def _best_image(card) -> str:
    img = _first(card, ["img[itemprop='image']", "img[data-marker='item-photo']", "img"])
    if img is not None:
        url = _from_srcset(img.get("srcset") or "")
        url = url or img.get("src") or img.get("data-src") or ""
        if url and not url.startswith("data:"):
            return _abs_url(url)

    slider = card.select_one("[data-marker^='slider-image/image-']")
    if slider is not None:
        marker = slider.get("data-marker") or ""
        if marker.startswith(_SLIDER_PREFIX):
            return _abs_url(marker[len(_SLIDER_PREFIX):])
    return ""


# --- выдача поиска --------------------------------------------------------
def parse_search(html: str) -> list[Listing]:
    """Вытаскивает карточки объявлений из HTML страницы результатов."""
    soup = _soup(html)
    cards = soup.select("div[data-marker='item'], div[data-marker='item'][data-item-id]")
    if not cards:
        cards = soup.select("[data-item-id]")

    listings: list[Listing] = []
    for card in cards:
        link = _first(card, [
            "a[data-marker='item-title']",
            "a[itemprop='url']",
            "h3 a",
            "a[href*='_']",
        ])
        if link is None:
            continue
        url = _abs_url(link.get("href", ""))
        if not url:
            continue

        item_id = str(card.get("data-item-id") or "").strip()
        if not item_id:
            match = _ID_FROM_URL.search(url)
            item_id = match.group(1) if match else ""
        if not item_id:
            continue

        # В атрибуте title Авито дописывает город — текст ссылки чище.
        title = _text(link) or (link.get("title") or "").strip()

        price = None
        price_text = ""
        meta_price = _first(card, [
            "meta[itemprop='price']",
            "[data-marker='item-price'][content]",
        ])
        if meta_price is not None:
            price = _price_from_text(meta_price.get("content", ""))
        price_node = _first(card, [
            "[data-marker='item-price']",
            "[data-marker='item-price-value']",
            "[itemprop='offers'] [itemprop='price']",
            "strong[class*='price']",
            "p[class*='price']",
        ])
        if price_node is not None:
            price_text = _text(price_node)
            if price is None:
                price = _price_from_text(price_text)

        location = _text(_first(card, [
            "[data-marker='item-location']",
            "[data-marker='item-address']",
            "div[class*='geo-root']",
            "span[class*='geo-address']",
        ]))
        date_text = _text(_first(card, [
            "[data-marker='item-date']",
            "p[data-marker='item-date']",
            "div[class*='date-text']",
        ]))
        seller = _text(_first(card, [
            "[data-marker='seller-info/name']",
            "[data-marker='seller-link/link']",
            "div[class*='seller-info-name']",
        ]))
        description = _text(_first(card, [
            "[data-marker='item-description']",
            "[data-marker='item-specific-params']",
            "meta[itemprop='description']",
            "div[class*='item-description']",
        ]))[:400]

        listings.append(Listing(
            item_id=item_id,
            title=title,
            url=url.split("?")[0],
            price=price,
            price_text=price_text,
            location=location,
            date_text=date_text,
            seller=seller,
            description=description,
            image_url=_best_image(card),
        ))

    # Одно и то же объявление может встретиться дважды (например, в блоке
    # рекламы и в основной ленте) — оставляем первое вхождение.
    unique: dict[str, Listing] = {}
    for item in listings:
        unique.setdefault(item.item_id, item)
    return list(unique.values())


def total_found(html: str) -> int | None:
    """Сколько всего объявлений нашёл Авито по запросу (для проверки задачи)."""
    soup = _soup(html)
    node = _first(soup, [
        "[data-marker='page-title/count']",
        "span[class*='page-title-count']",
    ])
    if node is None:
        return None
    return _price_from_text(_text(node))


# --- карточка объявления --------------------------------------------------
@dataclass
class ItemInfo:
    item_id: str = ""
    title: str = ""
    price: int | None = None
    price_text: str = ""
    region: str = ""
    category: str = ""
    location: str = ""
    image_url: str = ""
    url: str = ""


# Разметка, по которой видно настоящую страницу объявления.
ITEM_MARKER = 'data-marker="item-view/title-info"'

# Снятое объявление Авито подменяет страницей-подборкой «похожие».
_REMOVED_SIGNS = ("снят с", "снято с продажи", "подобрали похожие", "объявление удалено")


def is_removed_item(html: str) -> bool:
    """Объявление сняли с публикации, вместо него показана подборка похожих."""
    if ITEM_MARKER in html:
        return False
    soup = _soup(html)
    heading = _text(soup.select_one("h1")).lower()
    return any(sign in heading for sign in _REMOVED_SIGNS)


def parse_item(html: str, url: str) -> ItemInfo:
    """Собирает данные объявления-образца для задачи «похожие»."""
    soup = _soup(html)
    info = ItemInfo(url=url.split("?")[0])

    match = _ID_FROM_URL.search(url)
    info.item_id = match.group(1) if match else ""

    title_node = _first(soup, [
        "h1[data-marker='item-view/title-info']",
        "[data-marker='item-view/title-info']",
        "h1[itemprop='name']",
        "h1",
    ])
    info.title = _text(title_node)
    if not info.title:
        og = soup.select_one("meta[property='og:title']")
        if og is not None:
            info.title = html_mod.unescape(og.get("content", "")).strip()

    # Цену ищем строго внутри блока самого объявления. К странице снизу
    # подклеена карусель «похожие объявления» с такой же разметкой, и без
    # этой привязки в образец попадала цена чужого товара.
    price_scope = _first(soup, [
        "[data-marker='item-view/item-price-container']",
        "[data-marker='item-view/item-price']",
    ])
    if price_scope is not None:
        node = _first(price_scope, ["[itemprop='price'][content]", "meta[itemprop='price']", "[content]"])
        if node is not None:
            info.price = _price_from_text(node.get("content", ""))
        info.price_text = _text(price_scope)
        if info.price is None:
            info.price = _price_from_text(info.price_text)
    else:
        # Разметку блока цены перекроили — берём то, что помечено микроразметкой.
        node = _first(soup, ["span[itemprop='price'][content]", "meta[itemprop='price']"])
        if node is not None:
            info.price = _price_from_text(node.get("content", ""))

    info.location = _text(_first(soup, [
        "[data-marker='item-view/item-address']",
        "div[class*='item-address__string']",
        "span[class*='item-address']",
    ]))

    og_image = soup.select_one("meta[property='og:image']")
    if og_image is not None:
        info.image_url = og_image.get("content", "")

    # Регион и категория берём из адреса объявления:
    # /moskva/telefony/iphone_13-1234567890
    parts = [p for p in urllib.parse.urlparse(info.url).path.split("/") if p]
    if len(parts) >= 2:
        info.region = parts[0]
        info.category = parts[1]

    # Хлебные крошки точнее — последняя ссылка ведёт в самую узкую категорию.
    crumbs = soup.select("[data-marker='breadcrumbs/link'], .js-breadcrumbs a, nav a[href^='/']")
    for crumb in crumbs:
        crumb_parts = [p for p in (crumb.get("href") or "").split("?")[0].split("/") if p]
        if len(crumb_parts) >= 2 and crumb_parts[0] == info.region:
            info.category = crumb_parts[1]
    return info


def extract_initial_data(html: str) -> dict | None:
    """Достаёт window.__initialData__ — иногда там есть то, чего нет в разметке."""
    match = re.search(r'__initialData__\s*=\s*"(.*?)"\s*;', html, re.S)
    if not match:
        return None
    try:
        return json.loads(urllib.parse.unquote(match.group(1)))
    except Exception:
        return None
