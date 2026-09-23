# Avito Watcher

A desktop app for Windows that watches [Avito](https://www.avito.ru) for new
listings and tells you the moment one shows up — in the app itself, in Telegram
and by email.

The interface is in Russian, because Avito is a Russian classifieds site and
that is who this is for.

> **Note on ethics and terms.** This is a personal tool that polls public search
> pages at a human pace, no faster than once a minute per task, one request at a
> time. Do not turn it into a scraper: Avito's terms forbid bulk collection, and
> hammering the site will get your address blocked.

![Built for Windows](https://img.shields.io/badge/platform-Windows%2010%2F11-0078d4)
![Written in C++20](https://img.shields.io/badge/C%2B%2B-20-00599c)
![No dependencies](https://img.shields.io/badge/dependencies-none-4ade80)

## What it does

**Watch a search.** Give it a query, a city and a price range — or set the
filters up on Avito itself and paste the address bar into the app, which gets
you every filter the site offers. The first page of results, sorted by date, is
re-read on a schedule and anything that was not there before is reported.

**Watch for listings similar to one you like.** Paste a link to a listing. The
app reads its title, price, category and city, and turns that into a search.
The query width is tuned once, when the task is created: a full title as a
query usually matches exactly one listing — the very one you started from — so
the app shortens it until neighbours appear. Every result is then scored from 0
to 100 on how much of the sample's wording it repeats, and only what clears your
threshold and fits the price band is reported.

**Tell you about it.** A Windows notification and a feed inside the app, a
Telegram message with the photo, price and link, and an email. Each channel is
independent and each has a test button in the settings.

## Install

Download `AvitoWatcher.exe` from the
[latest release](https://github.com/rorythebreaker/avito-watcher/releases/latest)
and run it. There is nothing to install: it is a single executable of about
764 KB with no runtime, no frameworks and no DLLs beyond what Windows already
has.

Settings and data live in `%APPDATA%\AvitoWatcher`.

## Getting started

1. Press **Новая задача** (New task) and pick what to watch.
2. For a search, type the query and pick the city, or paste a ready Avito link.
   For similar listings, paste a link to the sample and press **Проверить**
   (Check) — the app shows you exactly what it read from it.
3. Choose how often to check. Once every 5 minutes is a sensible default.
4. Press **Начать слежение** (Start watching).

The first check is silent: the app memorises what is on Avito right now and only
reports what appears afterwards. A per-task checkbox turns that off if you want
to be told about the current page too.

## Telegram notifications

1. Message **@BotFather** in Telegram, send `/newbot` and follow the prompts.
   It hands you a token.
2. Paste the token into **Настройки → Telegram**.
3. Open your new bot in Telegram and press **Start** — without this a bot is not
   allowed to message you.
4. Press **Определить chat_id** (Detect chat id), then send a test message.

## Email notifications

In **Настройки → Почта** enter your address; the server and port fill
themselves in for Gmail, Yandex, Mail.ru, Outlook and Rambler.

Your normal mailbox password will not work — mail providers require a separate
**app password**, created in your mailbox security settings.

The bot token and the mail password are encrypted in `settings.json` with
Windows DPAPI, so they can only be read by your account on this machine.

## How it treats Avito

Avito throttles frequent requests, so the app is deliberately restrained:

- requests go strictly one at a time, with a randomised pause of a few seconds;
- a task is never polled more often than once a minute;
- only the first page of results is read, sorted by date.

A plain HTTPS request is tried first. When Avito answers with its "Доступ
ограничен" stub, the app loads the same page once through the browser already
installed on the machine — Edge ships with Windows 11 — started headless and
driven over the Chrome DevTools protocol. No Chromium is bundled, which is why
the executable stays small, but the fallback needs a Chromium browser to exist.

If blocks persist, raise the check interval, take a break, or set a proxy in
**Настройки → Сеть**.

## Building

Requires Visual Studio 2022 Build Tools (the "Desktop development with C++"
workload) and CMake 3.20 or newer. Nothing else — there are no third-party
libraries to fetch.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The result is `build\bin\AvitoWatcher.exe`.

Running `AvitoWatcher.exe --tray` starts it minimised to the tray; that is also
what gets written to the Windows Run key when you enable autostart.

### Self-test

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build
build\bin\AvitoWatcherSelfTest.exe              # offline checks
build\bin\AvitoWatcherSelfTest.exe <dir>        # also parse saved Avito pages
build\bin\AvitoWatcherSelfTest.exe <dir> --live # also hit Avito for real
```

Saved pages are not kept in the repository. Point the test at a directory with
`real.html`, `http_block.html`, `live_item.html`, `item.html` and `avito.html`
captured from Avito to run the parsing checks against genuine markup.

## How it is put together

Written in C++20 against the Win32 API directly. Every part that would normally
be a dependency is a thin module against something Windows already provides:
WinHTTP for HTTPS, Schannel for the TLS that SMTP needs, DPAPI for secrets, GDI
and GDI+ for drawing, and a small hand-written WebSocket client for the DevTools
protocol.

| Path | Responsibility |
|---|---|
| `src/util/` | strings, JSON, paths, logging |
| `src/core/` | settings, secrets, task and feed storage, autostart |
| `src/net/` | HTTPS, TLS, WebSocket, the browser fallback, page fetching |
| `src/avito/` | parsing, similarity scoring, the watching loop |
| `src/notify/` | Telegram and email |
| `src/ui/` | window, feed, dialogs, tray icon, theme |

There is no database: tasks, the feed and the set of already-seen listings are
three plain files in `%APPDATA%`.

### Parsing

Avito's result pages are two megabytes of generated markup, and the app needs a
handful of fields per card, so there is no DOM. The parser scans for the
`data-marker` attributes Avito puts on its elements and slices out the element
carrying them. Each field is looked up through several markers in turn, so when
the layout changes one field stops resolving instead of the whole page.

Two things that page turned out to need special care, both covered by the
self-test:

- Cards below the fold arrive with no `<img>` tag at all. The photo address is
  still there, inside the slider's `data-marker` value.
- A listing page has a strip of suggested listings built from the very same
  markup, so the price has to be read from the listing's own block or the sample
  ends up carrying a neighbour's price.

## Troubleshooting

The log is at `%APPDATA%\AvitoWatcher\watcher.log`, and recent events are shown
in the window, bottom right.

If the app stops finding anything, Avito has most likely changed its markup.
Run the self-test against a freshly saved page to see which field broke; the
fix is usually one selector in `src/avito/parser.cpp`.

## History

This started as a Python application using PySide6 and curl_cffi, and was
rewritten in C++ to drop the runtime and the 56 MB bundle. The Python version is
in the git history, up to commit `5c0b5e1`.

Settings carry over between the two — `settings.json` uses the same keys. Tasks
do not: the Python version kept them in SQLite, the C++ one keeps them in
`tasks.json`, so watch tasks have to be created again.

## License

MIT — see [LICENSE](LICENSE).
