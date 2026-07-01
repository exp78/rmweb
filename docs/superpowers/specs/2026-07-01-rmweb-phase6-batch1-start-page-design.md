# rmweb Phase 6 / Batch 1 — Bookmarks, History, Settings, Start Page (design)

> Status: **approved 2026-07-01** (brainstorming). Next: implementation plan via writing-plans.
> Scope locked with the user: bookmarks, history, settings-persistence, a start page. **No tabs.** Start
> page = tiles only (typing uses the existing address bar). Forms + cookies are the next batch, not this one.

## Goal

A start page ("home") that shows bookmarks and recent history as tappable tiles; persistent bookmarks and
history; and display settings (page zoom, reader font, User-Agent) that survive relaunch. On launch with no
URL argument, rmweb opens the start page.

## Architecture

The start page is a **generated local HTML file** loaded like any web page — tapping a tile navigates via
the existing link-following path, and styling is plain CSS. Persistence is a small **line-based text store**
(zero external dependency, so the pure-logic parts unit-test on the host with `clang++`, exactly like the
existing `keyboard.h` / `tapzone.h`). User data lives in a **separate profile directory** outside the app
bundle, so reinstalling or OTA-updating the app never wipes bookmarks/history/settings.

## Storage

Profile directory: **`/home/root/.rmweb/`** (default; override with env `RMWEB_PROFILE`). Created on first
run. It is deliberately OUTSIDE `/home/root/rmweb` (the bundle) so an app reinstall/clean-install leaves user
data intact. Files:

- `bookmarks.txt` — one bookmark per line: `url\ttitle`. Newest-added first.
- `history.txt` — one entry per line: `ts\turl\ttitle` (ts = unix seconds). Most-recent first, deduped by
  url (re-visiting moves the entry to the front and updates ts+title), capped at **300** lines.
- `settings.txt` — `key=value` per line: `zoom=1.0`, `readerFont=38`, `ua=` (empty = WPE default; `mobile`
  = mobile UA; any other string = literal UA).
- `home.html` — the generated start page (rewritten by `goHome()`).

Titles are sanitized before storage (tabs/newlines/control chars → spaces) so the line format can't be
corrupted by a page title. Writes are **atomic** (write `<file>.tmp`, then `rename()` over the target).
A missing or malformed file parses to empty/defaults — never a crash.

## Components (each isolated, with a well-defined interface)

### 1. `engine/wpeqt/profile.h` — pure C++ store (no Qt/WebKit)
Value types + free functions, all `std::` only, so it compiles into a host unit test.

```cpp
namespace rmweb {
struct Bookmark { std::string url, title; };
struct HistoryEntry { std::string url, title; long ts; };
struct Settings { double zoom = 1.0; int readerFont = 38; std::string ua; };

std::vector<Bookmark>     loadBookmarks(const std::string& dir);
void                      saveBookmarks(const std::string& dir, const std::vector<Bookmark>&);
std::vector<HistoryEntry> loadHistory(const std::string& dir);
void                      saveHistory(const std::string& dir, const std::vector<HistoryEntry>&);
Settings                  loadSettings(const std::string& dir);
void                      saveSettings(const std::string& dir, const Settings&);

// helpers (pure, operate on vectors so they are unit-testable without files):
bool toggleBookmark(std::vector<Bookmark>&, const std::string& url, const std::string& title); // ret: now-bookmarked
bool isBookmarked(const std::vector<Bookmark>&, const std::string& url);
void addHistory(std::vector<HistoryEntry>&, const std::string& url, const std::string& title, long ts); // dedupe+front+cap 300
std::string sanitizeField(const std::string&); // strip \t \n and control chars
}
```
- `addHistory`: if `url` already present, remove it, then push to front (updated title+ts); truncate to 300.
- Load functions skip malformed lines (lenient). Save functions write atomically (`.tmp` + rename).

### 2. `engine/wpeqt/startpage.h` — pure HTML generator (no Qt/WebKit)
```cpp
namespace rmweb {
std::string buildStartPage(const std::vector<Bookmark>& bookmarks,
                           const std::vector<HistoryEntry>& recent);   // recent already trimmed by caller
std::string htmlEscape(const std::string&);   // & < > " ' -> entities
}
```
- Output: a self-contained HTML doc, inline CSS tuned for e-ink (pure black on white, large tap targets,
  no JavaScript). Sections: an app title; a **Bookmarks** grid of tiles, each `<a href="<url>">title</a>`;
  a **Recent** list of `<a href="<url>">title</a>` (caller passes ~15 most recent); a footer link
  `<a href="rmweb:clear-history">Clear recent</a>`. Empty states render a friendly hint ("No bookmarks yet
  — tap ★ on a page to save it").
- Every `url`/`title` is `htmlEscape`d.

### 3. `WpeEngine` integration (`engine/wpeqt/main.cpp`)
- Holds `m_profileDir` (`getenv("RMWEB_PROFILE")` else `/home/root/.rmweb`), created on start; and in-memory
  `m_bookmarks`, `m_history`, `m_settings` loaded once at start.
- **Start:** load settings → apply: `m_zoom = m_settings.zoom` (+ `webkit_web_view_set_zoom_level`),
  `m_readerFont = m_settings.readerFont`, UA from `m_settings.ua` (seeded from `RMWEB_UA` on first run).
  If the initial argv URL is empty → `goHome()`.
- **`goHome()`:** `buildStartPage(m_bookmarks, firstN(m_history,15))` → atomic-write `m_profileDir/home.html`
  → `loadUrl("file://" + m_profileDir + "/home.html")`. On write failure, load a hardcoded minimal HTML via
  a `data:` URL.
- **History recording:** in the existing `LOAD_FINISHED` handler, when the committed URL is a real page
  (starts `http://`/`https://` — NOT the `file://…/home.html` start page, NOT a reader transform),
  `addHistory(m_history, url, webkit_web_view_get_title(view), time(nullptr))` → `saveHistory`.
- **`toggleBookmark()`:** `toggleBookmark(m_bookmarks, currentUrl, currentTitle)` → `saveBookmarks` → emit
  `bookmarkedChanged(bool)` for the star.
- **`isCurrentBookmarked()`** drives the star state (recomputed on `urlChanged`).
- **Settings save:** `zoomBy()` already mutates `m_zoom`/`m_readerFont`; after mutation, update `m_settings`
  and `saveSettings`. (Runtime UA changes are out of scope this batch — `ua` is env-seeded + persisted.)
- **`rmweb:` action scheme:** add a WebKit **decide-policy** handler (`decide-policy` signal,
  `WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION`). If the request URI starts with `rmweb:`, handle the
  action (`rmweb:clear-history` → `m_history.clear()` + `saveHistory` + `goHome()`) and
  `webkit_policy_decision_ignore()` the navigation. Other schemes fall through unchanged.

### 4. Chrome (`WpeView`) — two new buttons
- **Home** (house icon) in the left cluster, after Reload → engine `goHome()`.
- **Bookmark star** in the right cluster, before Reader → engine `toggleBookmark()`; drawn filled ★ when the
  current page is bookmarked, outline ☆ otherwise (fed by `bookmarkedChanged`).
- Extend `enum Hit` with `Home` and `Bookmark`; add `kHomeW`/`kStarW`; re-flow `hitChrome()` and
  `drawChromeBar()` (the address field yields the extra width); add `iconHome()` + `iconStar(bool filled)`
  vector helpers (same style as `iconReader`/`iconPower`). The toolbar gets denser — exact widths tuned on
  device. Tap router in `main()` gains `Home`/`Bookmark` cases.

## Data flow

```
launch (no URL) → load settings → apply zoom/readerFont/UA → goHome():
    read bookmarks + recent history → buildStartPage → write home.html → load file://…/home.html
tap a tile         → link-follow (existing) → navigate to the page
page LOAD_FINISHED → addHistory(url,title,now) → saveHistory
tap ★              → toggleBookmark(currentUrl,title) → saveBookmarks → star updates
tap Home           → goHome() (regenerated from the latest store)
tap "Clear recent" → rmweb:clear-history → decide-policy ignores nav → clear + saveHistory + goHome
A-/A+ (zoom/font)  → mutate m_settings → saveSettings   (persists across relaunch)
```

## Error handling
- Malformed/missing store file → parsed as empty/defaults; bad individual lines skipped. Launch never
  breaks on a bad profile.
- Atomic writes (`.tmp` + `rename`) so a crash mid-save cannot corrupt the store.
- `home.html` write failure → fall back to a hardcoded minimal start page via a `data:` URL.
- Title/URL are HTML-escaped in the start page and tab/newline-sanitized in the store (no injection, no
  format corruption).

## Testing
- **Host unit tests** (`clang++`, no device — wired via the existing `tests/*_test.cpp` loop in
  `scripts/run-tests.sh`):
  - `tests/profile_test.cpp`: bookmark toggle + `isBookmarked`; `addHistory` dedupe-to-front + 300 cap +
    order; `sanitizeField` strips tab/newline; a file round-trip (save→load equality) in a temp dir; a
    malformed file loads to empty.
  - `tests/startpage_test.cpp`: `buildStartPage` output contains each bookmark's and each recent entry's
    `href`; `htmlEscape` turns `<>&"'` into entities (a title `"<script>"` appears escaped, not raw); the
    `rmweb:clear-history` link is present; the empty-bookmarks/empty-history state renders the hint text.
- **On-device:** launch with no URL → start page shows bookmarks + recent → tap a tile navigates → ★
  adds/removes (star reflects it) → Home returns to the start page → **relaunch preserves bookmarks,
  history, and settings** (zoom/reader-font stick) → "Clear recent" empties the recent list.
- Then, per the working agreement: **code-review subagent → simplify subagent**.

## Out of scope (this batch)
Tabs, forms (in-page text input), cookies, downloads. No search box on the start page (the address bar is
used). No per-item history/bookmark delete (only "Clear recent" for history; a bookmark is removed by
re-tapping ★ on its page). No runtime UA-toggle UI (UA is env-seeded and persisted).

## Global constraints (inherited)
- Everything the app writes stays under `/home` (`/home/root/.rmweb` for user data; the bundle under
  `/home/root/rmweb`); nothing in `/etc`/rootfs.
- Pure-logic modules (`profile.h`, `startpage.h`) are `std`-only headers so host tests compile without Qt.
- Chrome is hand-painted into the WPE frame + hit-tested in C++ (QtQuick doesn't composite over it).
- `.env` never committed; commits guard with `git check-ignore -q .env` and stage specific files only.
