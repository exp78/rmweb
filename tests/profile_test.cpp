#include "../engine/wpeqt/profile.h"
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

    // file round-trip in a temp dir for history
    std::string dir = "/tmp/rmweb-profile-test";
    std::string mk = "mkdir -p " + dir; (void)std::system(mk.c_str());
    saveHistory(dir, h);
    auto h2 = loadHistory(dir);
    CHECK(h2.size() == 300);
    CHECK(h2[0].url == h[0].url);
    CHECK(h2[0].ts == h[0].ts);
    CHECK(h2[0].title == h[0].title);

    // sanitizeField strips tab/newline
    CHECK(sanitizeField("a\tb\nc") == "a b c");

    // file round-trip in a temp dir
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
    CHECK(s3.zoom == 1.0); CHECK(s3.readerFont == 30); CHECK(s3.ua.empty());

    if (fails == 0) std::printf("profile_test: OK\n");
    return fails ? 1 : 0;
}
