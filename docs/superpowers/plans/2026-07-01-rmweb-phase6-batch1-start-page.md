# rmweb Phase 6 Batch 1 — Bookmarks, History, Settings, Start Page — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a start page (bookmarks + recent history as tappable tiles), persistent bookmarks/history, and display settings (zoom, reader font, UA) that survive relaunch.

**Architecture:** Two pure `std`-only headers — `profile.h` (line-based text store) and `startpage.h` (HTML generator) — carry all testable logic and unit-test on the host with `clang++`. `WpeEngine` (in `main.cpp`) loads/saves the store, generates + loads the start page as a local `file://` HTML (so tapping a tile reuses the existing link-follow), records history, toggles bookmarks, and intercepts a `rmweb:` action scheme. The B2 chrome gains Home + bookmark-star buttons. User data lives in `/home/root/.rmweb/` (outside the bundle → survives reinstall).

**Tech Stack:** C++17 (`std` only for the two headers; Qt6 + WebKitGTK/WPE for the engine), clang++ host tests, the reMarkable Paper Pro device.

## Global Constraints

- Pure-logic headers `profile.h` / `startpage.h` are `std`-only (no Qt, no WebKit) so `tests/*_test.cpp` compile with `clang++ -std=c++17` (the existing host-test harness).
- User data dir: `/home/root/.rmweb/` default, `RMWEB_PROFILE` override. Files: `bookmarks.txt` (`url\ttitle`, newest first), `history.txt` (`ts\turl\ttitle`, most-recent first, deduped by url, cap **300**), `settings.txt` (`key=value`: `zoom`,`readerFont`,`ua`), `home.html` (generated).
- Everything the app writes stays under `/home`; nothing in `/etc`/rootfs.
- Store fields are tab/newline-sanitized; start-page url/title are HTML-escaped; store writes are atomic (`.tmp` + `rename`); a missing/corrupt file → empty/defaults, never a crash.
- Chrome is hand-painted into the WPE frame + hit-tested in C++ (QtQuick does not composite over it). Quit path stays `std::_Exit(0)`.
- History excludes the `file://…/home.html` start page and reader transforms.
- Commit guard on EVERY commit: `if git check-ignore -q .env; then git add <specific files>; git commit ...; else echo ABORT; fi`. Never `git add -A`.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

## File Structure

| File | Responsibility |
|------|----------------|
| `engine/wpeqt/profile.h` (new) | Pure store: `Bookmark`/`HistoryEntry`/`Settings` types; load/save (line-based, atomic); `toggleBookmark`/`isBookmarked`/`addHistory`/`sanitizeField` vector ops. |
| `engine/wpeqt/startpage.h` (new) | Pure `buildStartPage(bookmarks, recent)` + `htmlEscape`. |
| `tests/profile_test.cpp` (new) | Host tests for `profile.h`. |
| `tests/startpage_test.cpp` (new) | Host tests for `startpage.h`. |
| `engine/wpeqt/main.cpp` (modify) | `WpeEngine`: profile state, `goHome()`, history recording, `toggleBookmark()`, settings apply/save, `rmweb:` decide-policy. `WpeView`: Home + star chrome buttons. `main()`: tap routes. |

---

### Task 1: `profile.h` — pure store + host tests

**Files:**
- Create: `engine/wpeqt/profile.h`, `tests/profile_test.cpp`

**Interfaces:**
- Produces: `namespace rmweb` with `struct Bookmark{std::string url,title;}`, `struct HistoryEntry{std::string url,title; long ts;}`, `struct Settings{double zoom=1.0; int readerFont=38; std::string ua;}`; and `sanitizeField`, `isBookmarked`, `toggleBookmark`, `addHistory`, `loadBookmarks`/`saveBookmarks`, `loadHistory`/`saveHistory`, `loadSettings`/`saveSettings` (all taking a `const std::string& dir`).

- [ ] **Step 1: Write the failing test** — `tests/profile_test.cpp`

```cpp
#include "../engine/wpeqt/profile.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

int main() {
    // toggleBookmark + isBookmarked
    std::vector<Bookmark> bm;
    CHECK(toggleBookmark(bm, "http://a", "A") == true);   // added
    CHECK(isBookmarked(bm, "http://a"));
    CHECK(toggleBookmark(bm, "http://a", "A") == false);  // removed
    CHECK(!isBookmarked(bm, "http://a"));

    // addHistory: dedupe-to-front, order, cap 300
    std::vector<HistoryEntry> h;
    addHistory(h, "http://1", "one", 100);
    addHistory(h, "http://2", "two", 200);
    addHistory(h, "http://1", "one-again", 300);          // revisit -> front, title/ts updated
    CHECK(h.size() == 2);
    CHECK(h[0].url == "http://1"); CHECK(h[0].title == "one-again"); CHECK(h[0].ts == 300);
    for (int i = 0; i < 400; ++i) addHistory(h, "http://x" + std::to_string(i), "t", i);
    CHECK(h.size() == 300);                                // capped

    // sanitizeField strips tab/newline
    CHECK(sanitizeField("a\tb\nc") == "a b c");

    // file round-trip in a temp dir
    std::string dir = "/tmp/rmweb-profile-test";
    std::string mk = "mkdir -p " + dir; (void)std::system(mk.c_str());
    std::vector<Bookmark> b2 = {{"http://x", "X title"}, {"http://y", "Y\ttab"}};
    saveBookmarks(dir, b2);
    auto b3 = loadBookmarks(dir);
    CHECK(b3.size() == 2); CHECK(b3[0].url == "http://x"); CHECK(b3[1].title == "Y tab"); // sanitized on save
    Settings s; s.zoom = 1.44; s.readerFont = 46; s.ua = "mobile";
    saveSettings(dir, s);
    Settings s2 = loadSettings(dir);
    CHECK(s2.readerFont == 46); CHECK(s2.ua == "mobile"); CHECK(s2.zoom > 1.43 && s2.zoom < 1.45);

    // corrupt/missing -> defaults
    Settings s3 = loadSettings("/tmp/rmweb-does-not-exist");
    CHECK(s3.zoom == 1.0); CHECK(s3.readerFont == 38); CHECK(s3.ua.empty());

    if (fails == 0) std::printf("profile_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `clang++ -std=c++17 -o build/profile_test tests/profile_test.cpp`
Expected: FAIL to compile — `engine/wpeqt/profile.h` does not exist.

- [ ] **Step 3: Write `engine/wpeqt/profile.h`**

```cpp
#pragma once
// Pure, std-only persistent store for rmweb (no Qt/WebKit -> host-unit-testable). Line-based text files so
// there is zero JSON/serialization dependency; writes are atomic (.tmp + rename); reads are lenient.
#include <string>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <cstdio>

namespace rmweb {

struct Bookmark { std::string url, title; };
struct HistoryEntry { std::string url, title; long ts = 0; };
struct Settings { double zoom = 1.0; int readerFont = 38; std::string ua; };

// Strip tab/newline/control chars so a value can't corrupt the line-based store.
inline std::string sanitizeField(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) o += (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ? ' ' : c;
    return o;
}

// --- pure vector ops (unit-tested without files) ------------------------------
inline bool isBookmarked(const std::vector<Bookmark>& bm, const std::string& url) {
    for (const auto& b : bm) if (b.url == url) return true;
    return false;
}
inline bool toggleBookmark(std::vector<Bookmark>& bm, const std::string& url, const std::string& title) {
    for (auto it = bm.begin(); it != bm.end(); ++it)
        if (it->url == url) { bm.erase(it); return false; }          // was present -> removed
    bm.insert(bm.begin(), Bookmark{url, sanitizeField(title)});      // newest first
    return true;                                                     // now bookmarked
}
inline void addHistory(std::vector<HistoryEntry>& h, const std::string& url, const std::string& title, long ts) {
    for (auto it = h.begin(); it != h.end(); ++it) if (it->url == url) { h.erase(it); break; }  // dedupe
    h.insert(h.begin(), HistoryEntry{url, sanitizeField(title), ts});                            // move to front
    if (h.size() > 300) h.resize(300);                                                           // cap
}

// --- line-based file I/O ------------------------------------------------------
namespace detail {
inline void atomicWrite(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << data; }
    std::rename(tmp.c_str(), path.c_str());
}
inline std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> out; std::ifstream f(path); std::string line;
    while (std::getline(f, line)) if (!line.empty()) out.push_back(line);
    return out;
}
} // namespace detail

inline std::vector<Bookmark> loadBookmarks(const std::string& dir) {
    std::vector<Bookmark> out;
    for (const auto& ln : detail::readLines(dir + "/bookmarks.txt")) {
        auto t = ln.find('\t'); if (t == std::string::npos) continue;
        out.push_back(Bookmark{ln.substr(0, t), ln.substr(t + 1)});
    }
    return out;
}
inline void saveBookmarks(const std::string& dir, const std::vector<Bookmark>& bm) {
    std::string s; for (const auto& b : bm) s += b.url + "\t" + sanitizeField(b.title) + "\n";
    detail::atomicWrite(dir + "/bookmarks.txt", s);
}
inline std::vector<HistoryEntry> loadHistory(const std::string& dir) {
    std::vector<HistoryEntry> out;
    for (const auto& ln : detail::readLines(dir + "/history.txt")) {
        auto t1 = ln.find('\t'); if (t1 == std::string::npos) continue;
        auto t2 = ln.find('\t', t1 + 1); if (t2 == std::string::npos) continue;
        HistoryEntry e;
        e.ts = std::strtol(ln.substr(0, t1).c_str(), nullptr, 10);
        e.url = ln.substr(t1 + 1, t2 - t1 - 1);
        e.title = ln.substr(t2 + 1);
        out.push_back(e);
    }
    return out;
}
inline void saveHistory(const std::string& dir, const std::vector<HistoryEntry>& h) {
    std::string s;
    for (const auto& e : h) s += std::to_string(e.ts) + "\t" + e.url + "\t" + sanitizeField(e.title) + "\n";
    detail::atomicWrite(dir + "/history.txt", s);
}
inline Settings loadSettings(const std::string& dir) {
    Settings s;
    for (const auto& ln : detail::readLines(dir + "/settings.txt")) {
        auto eq = ln.find('='); if (eq == std::string::npos) continue;
        const std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
        if (k == "zoom") s.zoom = std::strtod(v.c_str(), nullptr);
        else if (k == "readerFont") s.readerFont = std::atoi(v.c_str());
        else if (k == "ua") s.ua = v;
    }
    if (!(s.zoom >= 0.5 && s.zoom <= 3.0)) s.zoom = 1.0;                 // clamp corrupt values
    if (!(s.readerFont >= 16 && s.readerFont <= 96)) s.readerFont = 38;
    return s;
}
inline void saveSettings(const std::string& dir, const Settings& s) {
    std::string out = "zoom=" + std::to_string(s.zoom) + "\n"
                    + "readerFont=" + std::to_string(s.readerFont) + "\n"
                    + "ua=" + sanitizeField(s.ua) + "\n";
    detail::atomicWrite(dir + "/settings.txt", out);
}

} // namespace rmweb
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `clang++ -std=c++17 -o build/profile_test tests/profile_test.cpp && ./build/profile_test`
Expected: `profile_test: OK`.

- [ ] **Step 5: Run the full host suite**

Run: `bash scripts/run-tests.sh`
Expected: existing tests + `profile_test: OK`, ending `ALL HOST TESTS OK`.

- [ ] **Step 6: Commit**

```bash
if git check-ignore -q .env; then
  git add engine/wpeqt/profile.h tests/profile_test.cpp
  git commit -m "feat(profile): pure line-based store for bookmarks/history/settings + host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 2: `startpage.h` — pure HTML generator + host tests

**Files:**
- Create: `engine/wpeqt/startpage.h`, `tests/startpage_test.cpp`

**Interfaces:**
- Consumes: `rmweb::Bookmark`, `rmweb::HistoryEntry` from `profile.h`.
- Produces: `rmweb::htmlEscape(const std::string&)`, `rmweb::buildStartPage(const std::vector<Bookmark>&, const std::vector<HistoryEntry>& recent) -> std::string`.

- [ ] **Step 1: Write the failing test** — `tests/startpage_test.cpp`

```cpp
#include "../engine/wpeqt/startpage.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)
static bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

int main() {
    CHECK(htmlEscape("<script>&\"'") == "&lt;script&gt;&amp;&quot;&#39;");

    std::vector<Bookmark> bm = {{"http://ex.com/a", "Alpha"}};
    std::vector<HistoryEntry> hist = {{"http://ex.com/b", "<b>Beta</b>", 10}};
    std::string html = buildStartPage(bm, hist);
    CHECK(has(html, "href='http://ex.com/a'"));           // bookmark link present
    CHECK(has(html, "Alpha"));
    CHECK(has(html, "href='http://ex.com/b'"));           // history link present
    CHECK(has(html, "&lt;b&gt;Beta&lt;/b&gt;"));          // title escaped, not raw
    CHECK(!has(html, "<b>Beta</b>"));                     // raw tag must NOT appear
    CHECK(has(html, "href='rmweb:clear-history'"));       // clear link present

    std::string empty = buildStartPage({}, {});
    CHECK(has(empty, "No bookmarks yet"));                // empty-state hint
    CHECK(!has(empty, "rmweb:clear-history"));            // no clear link when history empty

    if (fails == 0) std::printf("startpage_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `clang++ -std=c++17 -o build/startpage_test tests/startpage_test.cpp`
Expected: FAIL to compile — `engine/wpeqt/startpage.h` does not exist.

- [ ] **Step 3: Write `engine/wpeqt/startpage.h`**

```cpp
#pragma once
// Pure, std-only generator for the rmweb start page (no Qt/WebKit). Produces a self-contained, JS-free,
// e-ink-friendly HTML document from the bookmark + recent-history lists. All url/title are HTML-escaped.
#include <string>
#include <vector>
#include "profile.h"

namespace rmweb {

inline std::string htmlEscape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
    }
    return o;
}

// `recent` is the already-trimmed most-recent slice (the caller passes ~15).
inline std::string buildStartPage(const std::vector<Bookmark>& bookmarks,
                                  const std::vector<HistoryEntry>& recent) {
    std::string h =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>rmweb</title>"
        "<style>"
        "body{font-family:sans-serif;color:#000;background:#fff;margin:0;padding:40px 48px;}"
        "h1{font-size:44px;margin:0 0 8px;}"
        "h2{font-size:30px;margin:40px 0 16px;border-bottom:3px solid #000;padding-bottom:8px;}"
        ".tiles{display:flex;flex-wrap:wrap;gap:20px;}"
        ".tile{display:block;border:3px solid #000;border-radius:14px;padding:22px 26px;font-size:30px;"
        "min-width:280px;text-decoration:none;color:#000;}"
        ".recent a{display:block;font-size:30px;padding:16px 4px;border-bottom:1px solid #bbb;"
        "text-decoration:none;color:#000;}"
        ".u{color:#666;font-size:22px;} .empty{color:#666;font-size:28px;padding:16px 0;}"
        ".clear{display:inline-block;margin-top:28px;font-size:26px;color:#666;}"
        "</style></head><body><h1>rmweb</h1><h2>Bookmarks</h2>";
    if (bookmarks.empty()) {
        h += "<div class='empty'>No bookmarks yet \xE2\x80\x94 tap \xE2\x98\x85 on a page to save it.</div>";
    } else {
        h += "<div class='tiles'>";
        for (const auto& b : bookmarks) {
            const std::string t = b.title.empty() ? b.url : b.title;
            h += "<a class='tile' href='" + htmlEscape(b.url) + "'>" + htmlEscape(t) + "</a>";
        }
        h += "</div>";
    }
    h += "<h2>Recent</h2>";
    if (recent.empty()) {
        h += "<div class='empty'>Nothing yet.</div>";
    } else {
        h += "<div class='recent'>";
        for (const auto& e : recent) {
            const std::string t = e.title.empty() ? e.url : e.title;
            h += "<a href='" + htmlEscape(e.url) + "'>" + htmlEscape(t)
               + "<span class='u'> \xE2\x80\x94 " + htmlEscape(e.url) + "</span></a>";
        }
        h += "</div><a class='clear' href='rmweb:clear-history'>Clear recent</a>";
    }
    h += "</body></html>";
    return h;
}

} // namespace rmweb
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `clang++ -std=c++17 -o build/startpage_test tests/startpage_test.cpp && ./build/startpage_test`
Expected: `startpage_test: OK`.

- [ ] **Step 5: Run the full host suite**

Run: `bash scripts/run-tests.sh`
Expected: `profile_test: OK`, `startpage_test: OK`, `ALL HOST TESTS OK`.

- [ ] **Step 6: Commit**

```bash
if git check-ignore -q .env; then
  git add engine/wpeqt/startpage.h tests/startpage_test.cpp
  git commit -m "feat(startpage): pure HTML start-page generator (escaped, JS-free) + host tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 3: Engine integration — profile, start page, history, bookmarks, settings, `rmweb:` scheme

**Files:**
- Modify: `engine/wpeqt/main.cpp` (the `WpeEngine` class + its signal wiring in `start()`)

**Interfaces:**
- Consumes: everything from `profile.h` + `startpage.h`; existing `WpeEngine::loadUrl`, `m_view`, `m_zoom`, `m_readerFont`, the `LOAD_FINISHED` load-changed handler, `zoomBy()`, the UA block in `start()`.
- Produces (used by Task 4): slots `void goHome()`, `void toggleBookmark()`; getter `bool isCurrentBookmarked() const`; signal `void bookmarkedChanged(bool)`.

This task is WebKit-coupled (no host unit test — consistent with the codebase); verification is a clean cross-compile now + on-device behavior in Task 5.

- [ ] **Step 1: Add includes + engine members**

At the top of `engine/wpeqt/main.cpp`, near the other project includes (e.g. alongside `keyboard.h`), add:
```cpp
#include "profile.h"
#include "startpage.h"
#include <ctime>
```
In the `WpeEngine` class private members (near `m_zoom`/`m_readerFont` around line 703), add:
```cpp
    std::string m_profileDir;                       // /home/root/.rmweb (or $RMWEB_PROFILE)
    std::vector<rmweb::Bookmark> m_bookmarks;
    std::vector<rmweb::HistoryEntry> m_history;
    rmweb::Settings m_settings;
    std::string m_curUrl, m_curTitle;               // current committed page (for history + bookmark)
```

- [ ] **Step 2: Declare the new signal + load the profile in the ctor/start**

In the `Q_SIGNALS:` block of `WpeEngine`, add:
```cpp
    void bookmarkedChanged(bool on);
```
Find where `start()` initializes engine state (the block that reads `RMWEB_READER_FONT` ~line 214 and sets the UA ~line 297). At the START of `start()` (before creating the web view), load the profile and settings:
```cpp
    if (const char* p = getenv("RMWEB_PROFILE"); p && *p) m_profileDir = p; else m_profileDir = "/home/root/.rmweb";
    { std::string mk = "mkdir -p '" + m_profileDir + "'"; (void)system(mk.c_str()); }
    m_bookmarks = rmweb::loadBookmarks(m_profileDir);
    m_history   = rmweb::loadHistory(m_profileDir);
    m_settings  = rmweb::loadSettings(m_profileDir);
    m_zoom = m_settings.zoom;                        // apply persisted display settings
    m_readerFont = m_settings.readerFont;
```
Then make the existing env-based `m_readerFont`/UA logic respect the persisted values: the `RMWEB_READER_FONT` env block should only override when the env var is set (it already guards on `getenv` — keep it, so env still wins for a one-off). For the UA block (~line 297), seed from settings when no env override:
```cpp
    // UA: env overrides; else persisted setting; else WPE default.
    std::string ua = m_settings.ua;
    if (const char* uaEnv = getenv("RMWEB_UA"); uaEnv && *uaEnv && std::string(uaEnv) != "off") ua = uaEnv;
    if (!ua.empty()) {
        const char* real = (ua == "mobile") ? kMobileUA : ua.c_str();
        webkit_settings_set_user_agent(webkit_web_view_get_settings(m_view), real);
    }
    m_settings.ua = ua;
```
After the web view + zoom are set up, apply the persisted zoom: `webkit_web_view_set_zoom_level(m_view, m_zoom);`

- [ ] **Step 3: `goHome()` + open the start page when no URL**

Add these methods to `WpeEngine` (public slots, near `loadUrl`):
```cpp
    void goHome() {
        const std::string html = rmweb::buildStartPage(m_bookmarks, firstN(m_history, 15));
        const std::string path = m_profileDir + "/home.html";
        rmweb::detail::atomicWrite(path, html);
        loadUrl(QString::fromStdString("file://" + path));
    }
```
Add a small helper (free function or static) used above:
```cpp
static std::vector<rmweb::HistoryEntry> firstN(const std::vector<rmweb::HistoryEntry>& v, size_t n) {
    return {v.begin(), v.begin() + std::min(n, v.size())};
}
```
Where `main()` decides the initial load (it currently loads `argv[1]` via the engine — find the initial `loadUrl` / start call, ~line 1146 `url` handling and where the engine is told to load), route an empty URL to home. Concretely: in `main()`, where it connects/loads the initial URL, if `url.isEmpty()` call `engine.goHome()` instead of loading an empty string. (If the engine's `start()` already auto-loads the argv url, add: after `engine.start(...)`, `if (url.isEmpty()) QMetaObject::invokeMethod(&engine, "goHome", Qt::QueuedConnection);` — use the engine's existing cross-thread invoke pattern, `marshalToCtx`, if `goHome` must run on the worker context.)

- [ ] **Step 4: Record history on load-finished**

Find the `LOAD_FINISHED` branch of the WebKit `load-changed` handler (search `LOAD_FINISHED`). It already has the committed URL + can get the title via `webkit_web_view_get_title(view)`. Add, when the URL is a real page:
```cpp
    {
        const char* u = webkit_web_view_get_uri(view);
        const char* t = webkit_web_view_get_title(view);
        std::string url = u ? u : "";
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {   // not file://home, not reader
            self->m_curUrl = url; self->m_curTitle = t ? t : "";
            rmweb::addHistory(self->m_history, url, self->m_curTitle, (long)time(nullptr));
            rmweb::saveHistory(self->m_profileDir, self->m_history);
            Q_EMIT self->bookmarkedChanged(rmweb::isBookmarked(self->m_bookmarks, url));
        }
    }
```
(Use the handler's actual `self`/`view` variable names.)

- [ ] **Step 5: `toggleBookmark()` + `isCurrentBookmarked()`**

Add to `WpeEngine`:
```cpp
    void toggleBookmark() {
        if (m_curUrl.empty()) return;
        const bool on = rmweb::toggleBookmark(m_bookmarks, m_curUrl, m_curTitle);
        rmweb::saveBookmarks(m_profileDir, m_bookmarks);
        Q_EMIT bookmarkedChanged(on);
    }
    bool isCurrentBookmarked() const { return rmweb::isBookmarked(m_bookmarks, m_curUrl); }
```

- [ ] **Step 6: Persist settings on zoom/reader change**

In `zoomBy()` (~line 366-377), after it mutates `m_zoom` / `m_readerFont`, add:
```cpp
    m_settings.zoom = m_zoom; m_settings.readerFont = m_readerFont;
    rmweb::saveSettings(m_profileDir, m_settings);
```

- [ ] **Step 7: `rmweb:` action scheme via decide-policy**

Where `start()` connects the web view's signals (near the `load-changed` / crash-handler `g_signal_connect`s), add:
```cpp
    g_signal_connect(m_view, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* dec,
                                       WebKitPolicyDecisionType type, gpointer data) -> gboolean {
        if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
        auto* self = static_cast<WpeEngine*>(data);
        auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(dec);
        WebKitNavigationAction* act = webkit_navigation_policy_decision_get_navigation_action(nav);
        WebKitURIRequest* req = webkit_navigation_action_get_request(act);
        const char* uri = webkit_uri_request_get_uri(req);
        if (uri && std::string(uri).rfind("rmweb:", 0) == 0) {
            if (std::string(uri) == "rmweb:clear-history") {
                self->m_history.clear();
                rmweb::saveHistory(self->m_profileDir, self->m_history);
                self->goHome();
            }
            webkit_policy_decision_ignore(dec);
            return TRUE;
        }
        return FALSE;
    }), this);
```
(`+[]` makes the lambda a plain C function pointer. `QT_NO_KEYWORDS` is set in this project — the code already uses `Q_EMIT`/`Q_SIGNALS`, keep that.)

- [ ] **Step 8: Cross-compile to verify it builds**

Run: `./scripts/build-wpeqt.sh`
Expected: clean build → `build/rmweb-wpeqt`, no warnings. If an anchor didn't match, read the relevant region of `engine/wpeqt/main.cpp` and adapt the placement (do not force a mismatch).

- [ ] **Step 9: Commit**

```bash
if git check-ignore -q .env; then
  git add engine/wpeqt/main.cpp
  git commit -m "feat(engine): persist bookmarks/history/settings + start page + rmweb: scheme

Loads the /home/root/.rmweb profile on start, applies persisted zoom/font/UA,
opens the generated start page when no URL is given, records history on load,
toggles bookmarks, and handles rmweb:clear-history via decide-policy.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 4: Chrome — Home + bookmark-star buttons

**Files:**
- Modify: `engine/wpeqt/main.cpp` (the `WpeView` class + the `main()` tap router)

**Interfaces:**
- Consumes: engine slots `goHome()`, `toggleBookmark()`, signal `bookmarkedChanged(bool)` from Task 3; the existing chrome (`enum Hit`, `hitChrome`, `drawChromeBar`, layout constants, tap router).
- Produces: two new toolbar controls with C++ hit-testing.

Chrome is hand-painted with no host unit test (consistent with the codebase). Verify via clean build now + on-device tap in Task 5.

- [ ] **Step 1: Extend the `Hit` enum + layout constants**

Find:
```cpp
    enum Hit { None, Back, Fwd, Reload, Address, ZoomOut, ZoomIn, Reader, Power };
```
Replace with:
```cpp
    enum Hit { None, Back, Fwd, Reload, Home, Address, ZoomOut, ZoomIn, Bookmark, Reader, Power };
```
Find:
```cpp
    static const int kBarH = 104, kBackX = 170, kFwdX = 340, kRelX = 560, kReaderW = 190, kZoomW = 120, kPowerW = 130;
```
Replace with:
```cpp
    static const int kBarH = 104, kBackX = 170, kFwdX = 340, kRelX = 560, kReaderW = 190, kZoomW = 120, kPowerW = 130;
    static const int kHomeW = 140, kStarW = 120;   // Home (left, after Reload) + bookmark star (right, before Reader)
```
Add a member to hold the star state (near `m_readerable` ~line 942):
```cpp
    bool m_bookmarked = false;   // current page is bookmarked -> filled star
```

- [ ] **Step 2: Update `hitChrome()`**

Find:
```cpp
        const int powerX  = int(width()) - kPowerW;         // right cluster:  A- | A+ | Reader | Power
        const int readerX = powerX - kReaderW;
        const int zInX = readerX - kZoomW, zOutX = zInX - kZoomW;
        if (x < kBackX)   return Back;
        if (x < kFwdX)    return Fwd;
        if (x < kRelX)    return Reload;
        if (x >= powerX)  return Power;
        if (x >= readerX) return Reader;
        if (x >= zInX)    return ZoomIn;
        if (x >= zOutX)   return ZoomOut;
        return Address;
```
Replace with:
```cpp
        const int powerX  = int(width()) - kPowerW;         // right: A- | A+ | ★ | Reader | Power
        const int readerX = powerX - kReaderW;
        const int starX   = readerX - kStarW;
        const int zInX = starX - kZoomW, zOutX = zInX - kZoomW;
        if (x < kBackX)         return Back;
        if (x < kFwdX)          return Fwd;
        if (x < kRelX)          return Reload;
        if (x < kRelX + kHomeW) return Home;                // Home sits just after Reload
        if (x >= powerX)        return Power;
        if (x >= readerX)       return Reader;
        if (x >= starX)         return Bookmark;
        if (x >= zInX)          return ZoomIn;
        if (x >= zOutX)         return ZoomOut;
        return Address;
```

- [ ] **Step 3: Draw Home + star in `drawChromeBar()`, add icons**

Find:
```cpp
        pen(true);      m_loading ? iconStop(p, (kFwdX + kRelX) / 2.0, cy) : iconReload(p, (kFwdX + kRelX) / 2.0, cy);
        const qreal readerX = w - kReaderW, zInX = readerX - kZoomW, zOutX = zInX - kZoomW;
```
Replace with:
```cpp
        pen(true);      m_loading ? iconStop(p, (kFwdX + kRelX) / 2.0, cy) : iconReload(p, (kFwdX + kRelX) / 2.0, cy);
        pen(true);      iconHome(p, kRelX + kHomeW / 2.0, cy);
        const qreal powerX = w - kPowerW, readerX = powerX - kReaderW, starX = readerX - kStarW;
        const qreal zInX = starX - kZoomW, zOutX = zInX - kZoomW;
```
Find the address-field draw (uses `kRelX + 20` as the left edge):
```cpp
        const QString a = p->fontMetrics().elidedText(addrText, elide, int(zOutX - kRelX - 40));
        p->drawText(QRectF(kRelX + 20, 0, zOutX - kRelX - 40, kBarH), Qt::AlignVCenter, a);
```
Replace with (address now starts after Home):
```cpp
        const int addrX = kRelX + kHomeW;
        const QString a = p->fontMetrics().elidedText(addrText, elide, int(zOutX - addrX - 40));
        p->drawText(QRectF(addrX + 20, 0, zOutX - addrX - 40, kBarH), Qt::AlignVCenter, a);
```
Find the end of `drawChromeBar` (the reader branch + the power button added in Phase 5):
```cpp
        } else { pen(m_readerable); iconReader(p, rcx, cy); }
        pen(true); iconPower(p, powerX + kPowerW / 2.0, cy);   // exit to the reMarkable menu
    }
```
Replace with:
```cpp
        } else { pen(m_readerable); iconReader(p, rcx, cy); }
        pen(true); iconStar(p, starX + kStarW / 2.0, cy, m_bookmarked);
        pen(true); iconPower(p, powerX + kPowerW / 2.0, cy);   // exit to the reMarkable menu
    }
    void iconHome(QPainter *p, qreal cx, qreal cy) const {                 // a simple house
        QPen pn = p->pen(); pn.setWidthF(4); pn.setJoinStyle(Qt::RoundJoin); p->setPen(pn); p->setBrush(Qt::NoBrush);
        const qreal w = 20, r = 16;
        QPolygonF roof; roof << QPointF(cx - w, cy) << QPointF(cx, cy - r) << QPointF(cx + w, cy);
        p->drawPolyline(roof);
        p->drawRect(QRectF(cx - w + 4, cy, 2 * (w - 4), r));
    }
    void iconStar(QPainter *p, qreal cx, qreal cy, bool filled) const {    // 5-point star, filled if bookmarked
        QPen pn = p->pen(); pn.setWidthF(4); pn.setJoinStyle(Qt::RoundJoin); p->setPen(pn);
        const qreal R = 18, r = 7.2; QPolygonF star;
        for (int i = 0; i < 10; ++i) {
            const double ang = -3.14159265 / 2 + i * 3.14159265 / 5;
            const double rad = (i % 2 == 0) ? R : r;
            star << QPointF(cx + rad * std::cos(ang), cy + rad * std::sin(ang));
        }
        if (filled) { p->setBrush(p->pen().color()); p->drawPolygon(star); p->setBrush(Qt::NoBrush); }
        else { p->setBrush(Qt::NoBrush); p->drawPolygon(star); }
    }
```

- [ ] **Step 4: Add a setter slot for the star state**

In the `public Q_SLOTS:` block of `WpeView` (near `setReaderable`), add:
```cpp
    void setBookmarked(bool v) { if (v != m_bookmarked) { m_bookmarked = v; schedule(); } }
```

- [ ] **Step 5: Route the new taps + wire the star signal in `main()`**

Find the tap-router switch cases:
```cpp
                    case WpeView::Reload:  view->isLoading() ? engine.stopLoading() : engine.reload(); return;
```
After that line add:
```cpp
                    case WpeView::Home:    engine.goHome();     return;
```
Find:
```cpp
                    case WpeView::Address: view->beginEdit();  return;   // open the on-screen URL keyboard
```
After that line add:
```cpp
                    case WpeView::Bookmark: engine.toggleBookmark(); return;
```
Find where engine→view chrome signals are connected (e.g. `connect(&engine, &WpeEngine::readerModeChanged, ...)`), and add:
```cpp
        QObject::connect(&engine, &WpeEngine::bookmarkedChanged, view,
                         [view](bool on){ view->setBookmarked(on); }, Qt::QueuedConnection);
```

- [ ] **Step 6: Cross-compile to verify it builds**

Run: `./scripts/build-wpeqt.sh`
Expected: clean build, no unhandled-enum warning for `Home`/`Bookmark` in the tap-router switch.

- [ ] **Step 7: Commit**

```bash
if git check-ignore -q .env; then
  git add engine/wpeqt/main.cpp
  git commit -m "feat(chrome): Home + bookmark-star toolbar buttons

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

### Task 5: On-device verification + review checkpoint

**Files:** none (verification + review). Requires the device online.

- [ ] **Step 1: Deploy the new binary**

```bash
./scripts/build-wpeqt.sh
scp build/rmweb-wpeqt root@10.11.99.1:/home/root/rmweb/bin/rmweb-wpeqt
```

- [ ] **Step 2: Start page on launch (no URL)**

```bash
ssh root@10.11.99.1 'nohup /home/root/rmweb/rmweb >/dev/null 2>&1 & sleep 8; grep -c "frame " /home/root/rmweb/rmweb.log; ls -l /home/root/.rmweb/home.html'
```
Expected: `/home/root/.rmweb/home.html` exists; frames rendered (the start page is on screen). Grab it with `RMWEB_GRAB_MS` if a visual is wanted. Then quit (tap ⏻, or kill the launcher).

- [ ] **Step 3: Bookmark + history + persistence (hands-on)**

- Launch with a URL: `/home/root/rmweb/rmweb https://en.wikipedia.org/wiki/E_Ink`; tap **★** → confirm `/home/root/.rmweb/bookmarks.txt` gains the line; tap **Home** → the start page shows the bookmark tile + the page under Recent.
- Tap the bookmark tile → navigates back to the page.
- Change zoom with **A+**, quit, relaunch → the zoom is preserved (`settings.txt` shows the new `zoom`).
- On the start page, tap **Clear recent** → the Recent list empties (`history.txt` shrinks).
- Verify `/home/root/.rmweb/` is separate from the bundle and survives (it is not under `/home/root/rmweb`).

- [ ] **Step 4: Code-review subagent**

Dispatch a `feature-dev:code-reviewer` over the Batch-1 diff (profile.h, startpage.h, the two tests, the `main.cpp` engine + chrome changes). Apply high-confidence fixes; re-run `bash scripts/run-tests.sh`.

- [ ] **Step 5: Simplify subagent**

Dispatch a `code-simplifier:code-simplifier` over the same diff (behavior-preserving; don't churn the hardware-verified paths). Re-run `bash scripts/run-tests.sh`.

- [ ] **Step 6: Update status**

Add a Phase 6 / Batch 1 note to `CLAUDE.md` and the task tracker.

```bash
if git check-ignore -q .env; then
  git add CLAUDE.md
  git commit -m "docs: Phase 6 Batch 1 (start page, bookmarks, history, settings) done + on-device notes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
else echo "ABORT: .env not gitignored"; fi
```

---

## Self-Review

**Spec coverage:**
- Store (bookmarks/history/settings, line-based, atomic, sanitize, cap 300, defaults) → Task 1. ✓
- Start-page generator (tiles, recent, escape, clear link, empty states) → Task 2. ✓
- Profile dir `/home/root/.rmweb` + `RMWEB_PROFILE`, load on start, apply settings → Task 3. ✓
- `goHome()` + open start page when no URL → Task 3. ✓
- History recording on load-finished (exclude home/reader) → Task 3. ✓
- `toggleBookmark` + star state signal → Tasks 3 (engine) + 4 (chrome). ✓
- Settings persist on zoom/reader change → Task 3. ✓
- `rmweb:clear-history` via decide-policy → Task 3. ✓
- Home + star chrome buttons + tap routes → Task 4. ✓
- Host tests + on-device verify + code-review + simplify → Tasks 1,2,5. ✓
- Out of scope (tabs/forms/cookies/downloads/search box) → not planned. ✓

**Placeholder scan:** engine hook points are described by existing symbol + line hint plus complete code to insert (the implementer reads `main.cpp` to place them — flagged explicitly, not a silent TBD). No "TODO"/vague steps.

**Type consistency:** `rmweb::Bookmark`/`HistoryEntry`/`Settings` and the free functions are defined in Task 1 and used verbatim in Tasks 2–3. `Hit::Home`/`Hit::Bookmark`, `kHomeW`/`kStarW`, `iconHome`/`iconStar`, `m_bookmarked`, `setBookmarked`, `bookmarkedChanged`, `goHome`, `toggleBookmark` are introduced and referenced consistently across Tasks 3–4. `firstN` helper is defined where used (Task 3).
