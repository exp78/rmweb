#include "../engine/wpeqt/startpage.h"
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

    // scheme whitelist: non-http(s) URLs from a poisoned store render as inert text, never href
    std::vector<Bookmark> badBm = {{"javascript:alert(1)", "BadBm"}, {"file:///etc/passwd", "BadFile"}};
    std::vector<HistoryEntry> badH = {{"javascript:alert(1)", "BadH", 1}, {"data:text/html,x", "BadD", 2}};
    std::string bad = buildStartPage(badBm, badH);
    CHECK(!has(bad, "href='javascript:"));                // no working script link
    CHECK(!has(bad, "href='data:"));
    CHECK(!has(bad, "href='file:"));
    CHECK(has(bad, "BadBm") && has(bad, "BadH"));         // entries still rendered as plain text
    CHECK(has(bad, "href='rmweb:clear-history'"));        // generator's own rmweb: link unaffected

    // href-attribute escaping: ' and " inside a URL must not break out of href='...'
    std::vector<Bookmark> quoteBm = {{"https://ex.com/a'b\"c", "Quoted"}};
    std::string q = buildStartPage(quoteBm, {});
    CHECK(has(q, "href='https://ex.com/a&#39;b&quot;c'")); // quotes escaped inside the attribute
    CHECK(!has(q, "a'b\"c"));                             // raw quotes must NOT appear anywhere

    // empty title falls back to the URL as the visible label (startpage.h bookmark/history loops)
    std::vector<Bookmark> noTitleBm = {{"http://ex.com/notitle", ""}};
    std::vector<HistoryEntry> noTitleH = {{"http://ex.com/h2", "", 5}};
    std::string nt = buildStartPage(noTitleBm, noTitleH);
    CHECK(has(nt, "<span class='n'>http://ex.com/notitle</span>")); // bookmark tile label = url
    CHECK(has(nt, ">http://ex.com/h2<span"));             // history label = url (before the url span)

    // avatar chips: first letter uppercased; multibyte UTF-8 kept whole (never a sliced continuation byte)
    CHECK(utf8First("alpha") == "A");
    CHECK(utf8First("") == "?");
    CHECK(utf8First("\xD0\x91\xD0\xBB\xD0\xBE\xD0\xB3") == "\xD0\x91");   // "Блог" -> "Б"
    CHECK(hostFromUrl("https://ex.com/path?q=1") == "ex.com");
    CHECK(has(nt, "<span class='chip'>H</span>"));        // history row chip from the url fallback label

    // tabs-lite section: rendered only when tabs exist; page link + close command, both escaped
    CHECK(!has(html, "Open tabs"));                       // no tabs -> no section
    std::vector<Tab> tabs = {{"http://ex.com/t1", "Tab One"}, {"javascript:alert(1)", "Evil"}};
    std::string tp = buildStartPage({}, {}, tabs);
    CHECK(has(tp, "Open tabs"));
    CHECK(has(tp, "href='http://ex.com/t1'"));            // tab page link
    CHECK(has(tp, "href='rmweb:close-tab:http://ex.com/t1'")); // its close command
    CHECK(!has(tp, "href='javascript:"));                 // poisoned tab: no working link...
    CHECK(has(tp, "href='rmweb:close-tab:javascript:alert(1)'")); // ...but close still offered (inert
                                                                  //  until tapped; decide-policy guards it)
    // settings line: theme toggle label reflects the flag
    CHECK(has(buildStartPage({}, {}, {}, false), "Reader theme: light"));
    CHECK(has(buildStartPage({}, {}, {}, true),  "Reader theme: dark"));
    CHECK(has(html, "rmweb:toggle-dark"));                // toggle present on every start page

    // address-bar search: case-insensitive substring on title/url, results page renders matches
    CHECK(containsCI("E-Reader Wiki", "reader"));
    CHECK(!containsCI("E-Reader Wiki", "xyz"));
    std::vector<HistoryEntry> sh = searchStore(std::vector<HistoryEntry>{{"https://a.com/x", "Alpha page", 1},
                                                  {"https://b.com/y", "Beta", 2}}, "alpha");
    CHECK(sh.size() == 1 && sh[0].url == "https://a.com/x");
    std::vector<Bookmark> sb = searchStore(std::vector<Bookmark>{{"https://a.com", "Alpha"}, {"https://b.com", "Beta"}}, "b.com");
    CHECK(sb.size() == 1 && sb[0].title == "Beta");       // url substring matches too
    std::string sr = buildSearchResults("wiki e ink", sb, sh);
    CHECK(has(sr, "Search: wiki e ink"));
    CHECK(has(sr, "href='https://html.duckduckgo.com/html/?q=wiki%20e%20ink'")); // web-search link, encoded
    CHECK(has(sr, "href='https://a.com/x'"));             // history match row
    CHECK(!has(sr, "No local matches"));                  // matches exist -> no empty hint
    CHECK(has(buildSearchResults("zzz", {}, {}), "No local matches"));

    if (fails == 0) std::printf("startpage_test: OK\n");
    return fails ? 1 : 0;
}
