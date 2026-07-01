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
    CHECK(has(empty, "Пока нет закладок"));                // empty-state hint
    CHECK(!has(empty, "rmweb:clear-history"));            // no clear link when history empty

    if (fails == 0) std::printf("startpage_test: OK\n");
    return fails ? 1 : 0;
}
