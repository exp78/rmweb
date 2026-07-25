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

// Only http(s) URLs may become links. Any other scheme smuggled into the store (javascript:,
// data:, file:, ...) renders as plain text — an <a> without href is inert. The only non-http(s)
// link on the page is the generator's own hardcoded rmweb:clear-history below.
inline bool isSafeLinkUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// `recent` is the already-trimmed most-recent slice (the caller passes ~15). `tabs` is the
// tabs-lite session list (MRU first); each row pairs the page link with a "✕" close command.
// `readerDark` only flips the label of the settings toggle — the theme itself applies on the
// next Reader activation.
inline std::string buildStartPage(const std::vector<Bookmark>& bookmarks,
                                  const std::vector<HistoryEntry>& recent,
                                  const std::vector<Tab>& tabs = {},
                                  bool readerDark = false) {
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
        ".tab{display:flex;align-items:stretch;}"
        ".tab .page{flex:1;}"
        ".tab .x{display:block;font-size:30px;padding:16px 20px;border-bottom:1px solid #bbb;"
        "color:#666;text-decoration:none;}"
        ".u{color:#666;font-size:22px;} .empty{color:#666;font-size:28px;padding:16px 0;}"
        ".clear{display:inline-block;margin-top:28px;font-size:26px;color:#666;}"
        "</style></head><body><h1>rmweb</h1>";
    if (!tabs.empty()) {
        // Tabs-lite: the pages this session remembers, newest first. "✕" drops one from the list
        // (rmweb:close-tab:, honoured only from this page — see the decide-policy guard).
        h += "<h2>Open tabs</h2>";
        for (const auto& t : tabs) {
            const std::string label = t.title.empty() ? t.url : t.title;
            h += "<div class='tab'><a class='page'";
            if (isSafeLinkUrl(t.url)) h += " href='" + htmlEscape(t.url) + "'";
            h += ">" + htmlEscape(label) + "<span class='u'> \xE2\x80\x94 " + htmlEscape(t.url) + "</span></a>"
               + "<a class='x' href='rmweb:close-tab:" + htmlEscape(t.url) + "'>\xE2\x9C\x95</a></div>";
        }
    }
    h += "<h2>Bookmarks</h2>";
    if (bookmarks.empty()) {
        h += "<div class='empty'>No bookmarks yet \xE2\x80\x94 tap \xE2\x98\x85 on a page to save it.</div>";
    } else {
        h += "<div class='tiles'>";
        for (const auto& b : bookmarks) {
            const std::string t = b.title.empty() ? b.url : b.title;
            h += "<a class='tile'";
            if (isSafeLinkUrl(b.url)) h += " href='" + htmlEscape(b.url) + "'";
            h += ">" + htmlEscape(t) + "</a>";
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
            h += "<a";
            if (isSafeLinkUrl(e.url)) h += " href='" + htmlEscape(e.url) + "'";
            h += ">" + htmlEscape(t)
               + "<span class='u'> \xE2\x80\x94 " + htmlEscape(e.url) + "</span></a>";
        }
        h += "</div><a class='clear' href='rmweb:clear-history'>Clear recent</a>";
    }
    h += "<h2>Settings</h2><div class='recent'><a href='rmweb:toggle-dark'>Reader theme: ";
    h += readerDark ? "dark" : "light";
    h += "</a></div></body></html>";
    return h;
}

} // namespace rmweb
