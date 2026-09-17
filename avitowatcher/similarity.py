"""Поиск объявлений, похожих на заданное.

Как это работает. По ссылке на объявление-образец забираем его заголовок,
цену, регион и категорию. Из заголовка строим поисковый запрос и получаем
обычную ленту Авито в той же категории и том же регионе, отсортированную по
дате. Дальше каждое новое объявление оценивается по двум признакам:
совпадение слов заголовка и близость цены. Всё, что не дотянуло до порога,
отбрасывается — в уведомления идут только действительно похожие.
"""
from __future__ import annotations

import re
import urllib.parse

from .parser import BASE, ItemInfo

# Слова, которые ничего не говорят о товаре и только мешают поиску.
STOPWORDS = {
    "в", "во", "и", "на", "с", "со", "за", "от", "до", "из", "по", "для", "под",
    "или", "не", "но", "а", "к", "у", "о", "об", "при", "как", "the", "a", "an",
    "новый", "новая", "новое", "новые", "продам", "продаю", "продается",
    "продаётся", "срочно", "торг", "идеальном", "отличном", "хорошем",
    "состоянии", "состояние", "бу", "б", "у", "оригинал", "недорого", "дёшево",
    "дешево", "цена", "руб", "рублей", "шт", "комплект", "есть", "нет",
}

_WORD = re.compile(r"[a-zа-я0-9]+", re.IGNORECASE)


def normalize(text: str) -> list[str]:
    """Приводит заголовок к списку значащих слов."""
    text = text.lower().replace("ё", "е")
    words = _WORD.findall(text)
    return [w for w in words if len(w) > 1 and w not in STOPWORDS]


def build_query(title: str, max_words: int = 4) -> str:
    """Сжимает заголовок до короткого поискового запроса.

    Берём первые значащие слова в исходном порядке: продавцы начинают
    заголовок с типа товара и марки, а в хвосте идут подробности вроде цвета
    и размера. Длинный запрос приводит к выдаче из одного-единственного
    объявления — того самого, которое мы взяли за образец.
    """
    return " ".join(normalize(title)[:max_words])


def build_search_url(info: ItemInfo, price_tolerance: int, query: str = "") -> str:
    """Собирает ссылку на ленту Авито с объявлениями того же рода."""
    region = info.region or "rossiya"
    path = f"/{region}"
    if info.category:
        path += f"/{info.category}"

    params: list[tuple[str, str]] = []
    query = query or build_query(info.title)
    if query:
        params.append(("q", query))
    if info.price and price_tolerance > 0:
        low = int(info.price * (1 - price_tolerance / 100))
        high = int(info.price * (1 + price_tolerance / 100))
        params.append(("pmin", str(max(0, low))))
        params.append(("pmax", str(high)))
    params.append(("s", "104"))  # сортировка по дате, сначала свежие

    return f"{BASE}{path}?{urllib.parse.urlencode(params)}"


def title_score(reference: str, candidate: str) -> int:
    """Насколько заголовок кандидата совпадает с образцом, 0..100."""
    ref = set(normalize(reference))
    cand = set(normalize(candidate))
    if not ref or not cand:
        return 0
    common = ref & cand
    coverage = len(common) / len(ref)          # сколько слов образца нашлось
    jaccard = len(common) / len(ref | cand)    # штраф за лишние слова
    return round(100 * (0.7 * coverage + 0.3 * jaccard))


def price_matches(reference: int | None, candidate: int | None, tolerance: int) -> bool:
    """Укладывается ли цена кандидата в заданный разброс.

    Тот же разброс уходит в ссылку поиска параметрами pmin/pmax, поэтому здесь
    он тоже работает жёстким условием, а не смягчающей поправкой: ползунок
    «±40%» должен значить ровно то, что на нём написано.
    """
    if not reference or not candidate or tolerance <= 0:
        return True
    return abs(candidate - reference) / reference * 100 <= tolerance


def score(
    reference_title: str,
    reference_price: int | None,
    candidate_title: str,
    candidate_price: int | None,
    price_tolerance: int,
) -> int:
    """Итоговая оценка похожести объявления, 0..100.

    Цена вне разброса — сразу 0, такое объявление в ленту не попадёт.
    """
    if not price_matches(reference_price, candidate_price, price_tolerance):
        return 0
    return title_score(reference_title, candidate_title)
