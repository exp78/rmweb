#pragma once
// Pure, std-only persistent store for rmweb (no Qt/WebKit -> host-unit-testable). Line-based text files so
// there is zero JSON/serialization dependency; writes are atomic (.tmp + rename); reads are lenient.
#include <string>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

namespace rmweb {

struct Bookmark { std::string url, title; };
struct HistoryEntry { std::string url, title; long ts = 0; };
struct Settings { double zoom = 1.0; int readerFont = 30; std::string ua; bool readerDark = false;
                  std::string autofillEmail, autofillUser, autofillName; };   // learn-as-you-type autofill
struct ScrollEntry { std::string url; int pos = 0; };   // per-URL reading position (page offset, CSS px)
struct Tab { std::string url, title; };                 // tabs-lite: one entry per open page, MRU first

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

// Case-insensitive substring (ASCII fold — good enough for title/url search on this device).
inline bool containsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

// Address-bar search over the local stores: case-insensitive substring on title OR url, capped.
inline std::vector<HistoryEntry> searchHistory(const std::vector<HistoryEntry>& h, const std::string& q, size_t cap = 20) {
    std::vector<HistoryEntry> out;
    for (const auto& e : h) {
        if (out.size() >= cap) break;
        if (containsCI(e.title, q) || containsCI(e.url, q)) out.push_back(e);
    }
    return out;
}
inline std::vector<Bookmark> searchBookmarks(const std::vector<Bookmark>& bm, const std::string& q, size_t cap = 20) {
    std::vector<Bookmark> out;
    for (const auto& b : bm) {
        if (out.size() >= cap) break;
        if (containsCI(b.title, q) || containsCI(b.url, q)) out.push_back(b);
    }
    return out;
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
// Record a reading position (MRU front, cap 200). pos <= 0 = the reader is back at the top of the
// page -> drop the entry, so a later visit starts at the top instead of jumping to a stale offset.
inline void upsertScroll(std::vector<ScrollEntry>& v, const std::string& url, int pos) {
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->url == url) {
            if (pos > 0) it->pos = pos;
            else v.erase(it);
            return;
        }
    }
    if (pos > 0) {
        v.insert(v.begin(), ScrollEntry{url, pos});
        if (v.size() > 200) v.resize(200);
    }
}
inline int scrollPosFor(const std::vector<ScrollEntry>& v, const std::string& url) {
    for (const auto& e : v) if (e.url == url) return e.pos;
    return 0;
}
// Tabs-lite (one shared WebKitWebView, so a "tab" is a remembered page the start page can switch
// back to): upsert MRU-first with a hard cap of 8; revisiting a URL refreshes its title + position.
inline void upsertTab(std::vector<Tab>& tabs, const std::string& url, const std::string& title) {
    for (auto it = tabs.begin(); it != tabs.end(); ++it) {
        if (it->url == url) {
            Tab t = *it;
            if (!title.empty()) t.title = sanitizeField(title);
            tabs.erase(it);
            tabs.insert(tabs.begin(), t);
            return;
        }
    }
    tabs.insert(tabs.begin(), Tab{url, sanitizeField(title)});
    if (tabs.size() > 8) tabs.resize(8);
}
inline bool removeTab(std::vector<Tab>& tabs, const std::string& url) {
    for (auto it = tabs.begin(); it != tabs.end(); ++it)
        if (it->url == url) { tabs.erase(it); return true; }
    return false;
}

// --- line-based file I/O ------------------------------------------------------
namespace detail {
// Atomic replace: full write -> fsync -> close -> rename -> directory fsync. The rename happens
// ONLY after a fully successful write, so ENOSPC/EIO can never replace a good file with a
// truncated .tmp (the .tmp is unlinked instead and the old file is kept). Logs + returns false
// on any failure; POSIX only, no Qt/WebKit.
inline bool atomicWrite(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        std::fprintf(stderr, "[profile] atomicWrite: open %s failed: %s\n", tmp.c_str(), std::strerror(errno));
        return false;
    }
    bool ok = true;
    for (size_t off = 0; off < data.size();) {                       // write(2) may be partial
        const ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        off += static_cast<size_t>(n);
    }
    int savedErr = errno;
    if (ok && fsync(fd) != 0) { ok = false; savedErr = errno; }      // flush file data to storage
    if (close(fd) != 0 && ok) { ok = false; savedErr = errno; }
    if (!ok) {
        std::fprintf(stderr, "[profile] atomicWrite: write %s failed: %s — old file kept\n",
                     tmp.c_str(), std::strerror(savedErr));
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr, "[profile] atomicWrite: rename %s -> %s failed: %s\n",
                     tmp.c_str(), path.c_str(), std::strerror(errno));
        std::remove(tmp.c_str());
        return false;
    }
    // fsync the containing directory so the rename itself survives a power cut (best effort).
    std::string dir = path;
    const size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) dir = "."; else if (slash == 0) dir = "/"; else dir.erase(slash);
    const int dfd = open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        if (fsync(dfd) != 0)
            std::fprintf(stderr, "[profile] atomicWrite: dir fsync %s failed: %s\n", dir.c_str(), std::strerror(errno));
        close(dfd);
    }
    return true;
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
inline bool saveBookmarks(const std::string& dir, const std::vector<Bookmark>& bm) {
    std::string s; for (const auto& b : bm) s += sanitizeField(b.url) + "\t" + sanitizeField(b.title) + "\n";
    return detail::atomicWrite(dir + "/bookmarks.txt", s);
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
inline bool saveHistory(const std::string& dir, const std::vector<HistoryEntry>& h) {
    std::string s;
    for (const auto& e : h) s += std::to_string(e.ts) + "\t" + sanitizeField(e.url) + "\t" + sanitizeField(e.title) + "\n";
    return detail::atomicWrite(dir + "/history.txt", s);
}
inline std::vector<ScrollEntry> loadScroll(const std::string& dir) {
    std::vector<ScrollEntry> out;
    for (const auto& ln : detail::readLines(dir + "/scroll.txt")) {
        auto t = ln.find('\t'); if (t == std::string::npos) continue;
        const int pos = std::atoi(ln.substr(t + 1).c_str());
        if (pos <= 0) continue;                                   // corrupt/stale -> skip
        out.push_back(ScrollEntry{ln.substr(0, t), pos});
    }
    return out;
}
inline bool saveScroll(const std::string& dir, const std::vector<ScrollEntry>& v) {
    std::string s;
    for (const auto& e : v) s += sanitizeField(e.url) + "\t" + std::to_string(e.pos) + "\n";
    return detail::atomicWrite(dir + "/scroll.txt", s);
}
// tabs.txt shares the bookmarks.txt line format (url \t title).
inline std::vector<Tab> loadTabs(const std::string& dir) {
    std::vector<Tab> out;
    for (const auto& ln : detail::readLines(dir + "/tabs.txt")) {
        auto t = ln.find('\t'); if (t == std::string::npos) continue;
        out.push_back(Tab{ln.substr(0, t), ln.substr(t + 1)});
    }
    return out;
}
inline bool saveTabs(const std::string& dir, const std::vector<Tab>& tabs) {
    std::string s; for (const auto& t : tabs) s += sanitizeField(t.url) + "\t" + sanitizeField(t.title) + "\n";
    return detail::atomicWrite(dir + "/tabs.txt", s);
}
inline Settings loadSettings(const std::string& dir) {
    Settings s;
    for (const auto& ln : detail::readLines(dir + "/settings.txt")) {
        auto eq = ln.find('='); if (eq == std::string::npos) continue;
        const std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
        if (k == "zoom") s.zoom = std::strtod(v.c_str(), nullptr);
        else if (k == "readerFont") s.readerFont = std::atoi(v.c_str());
        else if (k == "ua") s.ua = sanitizeField(v);
        else if (k == "readerDark") s.readerDark = (v == "1");
        else if (k == "afEmail") s.autofillEmail = sanitizeField(v);
        else if (k == "afUser") s.autofillUser = sanitizeField(v);
        else if (k == "afName") s.autofillName = sanitizeField(v);
    }
    if (!(s.zoom >= 0.5 && s.zoom <= 3.0)) s.zoom = 1.0;                 // clamp corrupt values
    if (!(s.readerFont >= 14 && s.readerFont <= 96)) s.readerFont = 30;
    return s;
}
inline bool saveSettings(const std::string& dir, const Settings& s) {
    std::string out = "zoom=" + std::to_string(s.zoom) + "\n"
                    + "readerFont=" + std::to_string(s.readerFont) + "\n"
                    + "ua=" + sanitizeField(s.ua) + "\n"
                    + "readerDark=" + std::string(s.readerDark ? "1" : "0") + "\n"
                    + "afEmail=" + sanitizeField(s.autofillEmail) + "\n"
                    + "afUser=" + sanitizeField(s.autofillUser) + "\n"
                    + "afName=" + sanitizeField(s.autofillName) + "\n";
    return detail::atomicWrite(dir + "/settings.txt", out);
}

} // namespace rmweb
