#pragma once
// Pure, std-only generator for the rmweb start page (no Qt/WebKit). Produces a self-contained, JS-free,
// e-ink-friendly HTML document from the bookmark + recent-history lists. All url/title are HTML-escaped.
#include <string>
#include <vector>
#include "profile.h"
#include "url.h"

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

// Inline Lucide icons (lucide.dev, ISC) for our generated pages — stroke=currentColor, so they pick
// up the row/header colour. Static markup, no user data ever interpolates into it.
namespace icon {
    inline const char* globe =
        "<circle cx='12' cy='12' r='10'/>"
        "<path d='M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20'/><path d='M2 12h20'/>";
    inline const char* gear =
        "<path d='M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 "
        "4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 "
        "0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 "
        "2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915'/><circle cx='12' cy='12' r='3'/>";
    inline const char* sunMoon =
        "<path d='M12 2v2'/><path d='M14.837 16.385a6 6 0 1 1-7.223-7.222c.624-.147.97.66.715 1.248a4 4 0 "
        "0 0 5.26 5.259c.589-.255 1.396.09 1.248.715'/><path d='M16 12a4 4 0 0 0-4-4'/>"
        "<path d='m19 5-1.256 1.256'/><path d='M20 12h2'/>";
    inline const char* smartphone =
        "<rect width='14' height='20' x='5' y='2' rx='2' ry='2'/><path d='M12 18h.01'/>";
    inline const char* shield =
        "<path d='M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 "
        "4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z'/>";
    inline const char* maximize =
        "<path d='M15 3h6v6'/><path d='m21 3-7 7'/><path d='m3 21 7-7'/><path d='M9 21H3v-6'/>";
    inline const char* refreshCw =
        "<path d='M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8'/><path d='M21 3v5h-5'/>"
        "<path d='M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16'/><path d='M8 16H3v5'/>";
    inline const char* eraser =
        "<path d='M21 21H8a2 2 0 0 1-1.42-.587l-3.994-3.999a2 2 0 0 1 0-2.828l10-10a2 2 0 0 1 2.829 "
        "0l5.999 6a2 2 0 0 1 0 2.828L12.834 21'/><path d='m5.082 11.09 8.828 8.828'/>";
    inline const char* trash =
        "<path d='M10 11v6'/><path d='M14 11v6'/><path d='M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6'/>"
        "<path d='M3 6h18'/><path d='M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2'/>";
}
inline std::string svgIcon(const char* inner, int px) {
    return "<svg width='" + std::to_string(px) + "' height='" + std::to_string(px)
         + "' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2'"
           " stroke-linecap='round' stroke-linejoin='round'>" + inner + "</svg>";
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

// Domain part of an http(s) URL for the grey subtitle / avatar fallback — hostFromUrl (url.h).

// `recent` is the already-trimmed most-recent slice (the caller passes ~15). `tabs` is the
// tabs-lite session list (MRU first); each row pairs the page link with a "✕" close command.
// Settings live on their own page now — the last row here is just the link to it.
inline std::string buildStartPage(const std::vector<Bookmark>& bookmarks,
                                  const std::vector<HistoryEntry>& recent,
                                  const std::vector<Tab>& tabs = {}) {
    std::string h =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>rmweb</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:sans-serif;color:#000;background:#fff;margin:0;padding:52px 60px;}"
        ".hero{display:flex;align-items:center;gap:20px;font-size:64px;font-weight:800;letter-spacing:-1px;margin:0;}"
        ".tag{color:#666;font-size:26px;margin:8px 0 4px;}"
        "h2{font-size:24px;font-weight:700;letter-spacing:4px;color:#666;margin:48px 0 12px;"
        "border-bottom:2px solid #000;padding-bottom:10px;}"
        ".row{display:flex;align-items:center;padding:16px 0;border-bottom:1px solid #ddd;}"
        "a.page,a.rowlink{display:flex;align-items:center;flex:1;min-width:0;text-decoration:none;color:#000;}"
        ".ic{flex:none;display:inline-flex;margin-right:22px;}"
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
        "</style></head><body><div class='hero'>" + svgIcon(icon::globe, 60) + "<span>rmweb</span></div>"
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
               + "<span class='u'>" + htmlEscape(hostFromUrl(t.url)) + "</span></span></a>"
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
               + "<span class='u'>" + htmlEscape(hostFromUrl(e.url)) + "</span></span></a></div>";
        }
        h += "<a class='clear' href='rmweb:clear-history'>Clear recent</a>";
    }
    h += "<h2>Settings</h2><div class='row'><a class='rowlink' href='rmweb:settings'>"
         "<span class='ic'>" + svgIcon(icon::gear, 34) + "</span>"
         "<span class='t'>Open settings</span>"
         "<span class='u'>theme, sites, blocking, refresh &#187;</span></a></div></body></html>";
    return h;
}

// The settings page: one row per lever, tap cycles/toggles it (rmweb: commands, honoured only from
// our own generated pages — see the decide-policy guard). Current values render into the labels, so
// the page is regenerated after every change (the handler navigates back here). JS-free, same design
// language as the start page.
inline std::string buildSettingsPage(const Settings& s) {
    const char* onoff[] = { "off", "on" };
    std::string ar;
    switch (s.autoRefreshSec) {
        case -1: ar = "blocked (never)"; break;
        case 0:  ar = "allowed (no limit)"; break;
        default: ar = "every " + std::to_string(s.autoRefreshSec) + " s"; break;
    }
    std::string h =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>rmweb settings</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:sans-serif;color:#000;background:#fff;margin:0;padding:52px 60px;}"
        ".hero{display:flex;align-items:center;gap:20px;font-size:64px;font-weight:800;letter-spacing:-1px;margin:0;}"
        ".tag{color:#666;font-size:26px;margin:8px 0 4px;}"
        "h2{font-size:24px;font-weight:700;letter-spacing:4px;color:#666;margin:48px 0 12px;"
        "border-bottom:2px solid #000;padding-bottom:10px;}"
        ".row{display:flex;align-items:center;padding:22px 0;border-bottom:1px solid #ddd;}"
        "a.rowlink{display:flex;align-items:center;flex:1;min-width:0;text-decoration:none;color:#000;}"
        ".ic{flex:none;display:inline-flex;margin-right:22px;}"
        ".t{flex:1;font-size:30px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        ".v{color:#000;font-size:26px;font-weight:700;margin-left:16px;padding:6px 18px;"
        "border:2px solid #000;border-radius:10px;}"
        ".hint{color:#888;font-size:22px;padding:6px 0 0;}"
        ".back{display:inline-block;margin-top:40px;font-size:28px;color:#000;text-decoration:none;"
        "border:3px solid #000;border-radius:14px;padding:16px 26px;}"
        "</style></head><body><div class='hero'>" + svgIcon(icon::gear, 56) + "<span>Settings</span></div>"
        "<div class='tag'>tap a row to change &mdash; applies immediately</div>";
    auto row = [&h](const char* ic, const char* cmd, const std::string& label, const std::string& val) {
        h += "<div class='row'><a class='rowlink' href='";
        h += cmd;
        h += "'><span class='ic'>" + svgIcon(ic, 36) + "</span><span class='t'>" + label + "</span><span class='v'>" + val + "</span></a></div>";
    };
    h += "<h2>Reading</h2>";
    row(icon::sunMoon, "rmweb:toggle-dark", "Reader theme", s.readerDark ? "dark" : "light");
    h += "<div class='hint'>Font size: the A- / A+ buttons while reading.</div>";
    h += "<h2>Sites</h2>";
    row(icon::smartphone, "rmweb:toggle-ua", "Site version", s.ua == "mobile" ? "mobile (lighter)" : "desktop");
    row(icon::shield, "rmweb:toggle-block", "Ad &amp; tracker blocking", onoff[s.block ? 1 : 0]);
    h += "<div class='hint'>Blocking off renders heavy sites fully, but slowly.</div>";
    row(icon::maximize, "rmweb:toggle-sitecss", "Fit pages to screen", onoff[s.siteCss ? 1 : 0]);
    row(icon::refreshCw, "rmweb:autorefresh-next", "Site auto-refresh", ar);
    h += "<div class='hint'>Blocks pages reloading themselves while you read.</div>";
    h += "<h2>Data</h2>";
    row(icon::eraser, "rmweb:clear-autofill", "Clear remembered form fields", "clear");
    row(icon::trash, "rmweb:clear-history", "Clear recent history", "clear");
    h += "<a class='back' href='rmweb:home'>&#171; Start page</a></body></html>";
    return h;
}

// Address-bar search results (typed words that aren't a URL): a web-search link on top, then the
// matching bookmarks + history rows. Same design language as the start page, still JS-free.
inline std::string buildSearchResults(const std::string& query,
                                      const std::vector<Bookmark>& bm,
                                      const std::vector<HistoryEntry>& hist) {
    std::string h =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>rmweb</title>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:sans-serif;color:#000;background:#fff;margin:0;padding:52px 60px;}"
        ".hero{font-size:44px;font-weight:800;margin:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        "h2{font-size:24px;font-weight:700;letter-spacing:4px;color:#666;margin:44px 0 12px;"
        "border-bottom:2px solid #000;padding-bottom:10px;}"
        ".row{display:flex;align-items:center;padding:16px 0;border-bottom:1px solid #ddd;}"
        "a.rowlink{display:flex;align-items:center;flex:1;min-width:0;text-decoration:none;color:#000;}"
        ".t{flex:1;font-size:30px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        ".u{color:#888;font-size:22px;margin-left:16px;}"
        ".chip{flex:none;width:56px;height:56px;line-height:56px;text-align:center;background:#000;"
        "color:#fff;border-radius:12px;font-size:28px;font-weight:700;margin-right:20px;}"
        ".web{display:flex;align-items:center;border:3px solid #000;border-radius:16px;padding:22px 26px;"
        "text-decoration:none;color:#000;font-size:30px;font-weight:700;margin-top:22px;}"
        ".web .q{color:#666;font-weight:400;margin-left:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        ".empty{color:#666;font-size:28px;padding:16px 0;}"
        "</style></head><body><div class='hero'>Search: " + htmlEscape(query) + "</div>"
        "<a class='web' href='https://html.duckduckgo.com/html/?q=" + urlEncode(query) + "'>"
        "<span class='chip'>&#187;</span>Search the web<span class='q'>" + htmlEscape(query) + "</span></a>";
    auto rows = [&](const char* head, auto begin, auto end, auto labelOf, auto urlOf) {
        if (begin == end) return;
        h += std::string("<h2>") + head + "</h2>";
        for (auto it = begin; it != end; ++it) {
            const std::string url = urlOf(*it);
            const std::string t = labelOf(*it).empty() ? url : labelOf(*it);
            h += "<div class='row'><a class='rowlink'";
            if (isSafeLinkUrl(url)) h += " href='" + htmlEscape(url) + "'";
            h += "><span class='chip'>" + htmlEscape(utf8First(t)) + "</span>"
               + "<span class='t'>" + htmlEscape(t)
               + "<span class='u'>" + htmlEscape(hostFromUrl(url)) + "</span></span></a></div>";
        }
    };
    rows("Bookmarks", bm.begin(), bm.end(),
         [](const Bookmark& b) -> const std::string& { return b.title; },
         [](const Bookmark& b) -> const std::string& { return b.url; });
    rows("History", hist.begin(), hist.end(),
         [](const HistoryEntry& e) -> const std::string& { return e.title; },
         [](const HistoryEntry& e) -> const std::string& { return e.url; });
    if (bm.empty() && hist.empty())
        h += "<div class='empty'>No local matches \xE2\x80\x94 try the web search above.</div>";
    h += "</body></html>";
    return h;
}

} // namespace rmweb
