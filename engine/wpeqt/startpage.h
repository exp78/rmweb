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
