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

// First UTF-8 codepoint of s as an uppercase-ish letter chip label (ASCII uppercased, multibyte
// kept whole — never slice a continuation byte). "?" when empty.
inline std::string utf8First(const std::string& s) {
    if (s.empty()) return "?";
    const unsigned char c = static_cast<unsigned char>(s[0]);
    size_t n = 1;
    if ((c & 0x80) == 0) n = 1;
    else if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    if (n > s.size()) n = 1;
    std::string f = s.substr(0, n);
    if (n == 1 && f[0] >= 'a' && f[0] <= 'z') f[0] = char(f[0] - 32);
    return f;
}

// Domain part of an http(s) URL for the grey subtitle / avatar fallback ("" when unparseable).
inline std::string urlHost(const std::string& url) {
    const size_t p = url.find("://");
    if (p == std::string::npos) return url;
    const size_t b = p + 3, e = url.find('/', b);
    return url.substr(b, e == std::string::npos ? std::string::npos : e - b);
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
        "*{box-sizing:border-box}"
        "body{font-family:sans-serif;color:#000;background:#fff;margin:0;padding:52px 60px;}"
        ".hero{font-size:64px;font-weight:800;letter-spacing:-1px;margin:0;}"
        ".tag{color:#666;font-size:26px;margin:8px 0 4px;}"
        "h2{font-size:24px;font-weight:700;letter-spacing:4px;color:#666;margin:48px 0 12px;"
        "border-bottom:2px solid #000;padding-bottom:10px;}"
        ".row{display:flex;align-items:center;padding:16px 0;border-bottom:1px solid #ddd;}"
        "a.page,a.rowlink{display:flex;align-items:center;flex:1;min-width:0;text-decoration:none;color:#000;}"
        ".t{flex:1;font-size:30px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        ".u{color:#888;font-size:22px;margin-left:16px;}"
        ".chip{flex:none;width:56px;height:56px;line-height:56px;text-align:center;background:#000;"
        "color:#fff;border-radius:12px;font-size:28px;font-weight:700;margin-right:20px;}"
        ".x{flex:none;color:#666;text-decoration:none;font-size:36px;padding:8px 16px;}"
        ".tiles{display:flex;flex-wrap:wrap;gap:22px;}"
        ".tile{display:block;width:208px;border:3px solid #000;border-radius:16px;padding:22px 16px;"
        "text-decoration:none;color:#000;text-align:center;}"
        ".tile .av{display:inline-block;width:76px;height:76px;line-height:76px;background:#000;color:#fff;"
        "border-radius:18px;font-size:38px;font-weight:700;}"
        ".tile .n{display:block;font-size:26px;margin-top:14px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        ".empty{color:#666;font-size:28px;padding:16px 0;}"
        ".clear{display:inline-block;margin-top:24px;font-size:26px;color:#666;}"
        "</style></head><body><div class='hero'>rmweb</div>"
        "<div class='tag'>read the web on e-ink</div>";
    if (!tabs.empty()) {
        // Tabs-lite: the pages this session remembers, newest first. "✕" drops one from the list
        // (rmweb:close-tab:, honoured only from this page — see the decide-policy guard).
        h += "<h2>Open tabs</h2>";
        for (const auto& t : tabs) {
            const std::string label = t.title.empty() ? t.url : t.title;
            h += "<div class='row'><a class='page'";
            if (isSafeLinkUrl(t.url)) h += " href='" + htmlEscape(t.url) + "'";
            h += "><span class='chip'>" + htmlEscape(utf8First(label)) + "</span>"
               + "<span class='t'>" + htmlEscape(label)
               + "<span class='u'>" + htmlEscape(urlHost(t.url)) + "</span></span></a>"
               + "<a class='x' href='rmweb:close-tab:" + htmlEscape(t.url) + "'>\xC3\x97</a></div>";   // × U+00D7 (the device font lacks U+2715)
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
            h += "><span class='av'>" + htmlEscape(utf8First(t)) + "</span>"
               + "<span class='n'>" + htmlEscape(t) + "</span></a>";
        }
        h += "</div>";
    }
    h += "<h2>Recent</h2>";
    if (recent.empty()) {
        h += "<div class='empty'>Nothing yet.</div>";
    } else {
        for (const auto& e : recent) {
            const std::string t = e.title.empty() ? e.url : e.title;
            h += "<div class='row'><a class='rowlink'";
            if (isSafeLinkUrl(e.url)) h += " href='" + htmlEscape(e.url) + "'";
            h += "><span class='chip'>" + htmlEscape(utf8First(t)) + "</span>"
               + "<span class='t'>" + htmlEscape(t)
               + "<span class='u'>" + htmlEscape(urlHost(e.url)) + "</span></span></a></div>";
        }
        h += "<a class='clear' href='rmweb:clear-history'>Clear recent</a>";
    }
    h += "<h2>Settings</h2><div class='row'><a class='rowlink' href='rmweb:toggle-dark'><span class='t'>Reader theme: ";
    h += readerDark ? "dark" : "light";
    h += "</span></a></div></body></html>";
    return h;
}

} // namespace rmweb
