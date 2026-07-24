#include "../engine/wpeqt/profile.h"
#include <cstdio>
#include <string>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

// Unique temp dir per run (mkdtemp) + RAII cleanup: no stale state between runs and
// no race when two test binaries run in parallel. mkdtemp's charset is shell-safe.
struct TmpDir {
    std::string path;
    TmpDir() {
        char t[] = "/tmp/rmweb-profile-test-XXXXXX";
        const char* d = mkdtemp(t);
        if (d) path = d;
    }
    ~TmpDir() {
        if (!path.empty()) {
            const std::string cmd = "rm -rf " + path;
            (void)std::system(cmd.c_str());
        }
    }
};

int main() {
    TmpDir tmp;
    const std::string dir = tmp.path;
    CHECK(!dir.empty());

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

    // file round-trip in the temp dir for history
    saveHistory(dir, h);
    auto h2 = loadHistory(dir);
    CHECK(h2.size() == 300);
    CHECK(h2[0].url == h[0].url);
    CHECK(h2[0].ts == h[0].ts);
    CHECK(h2[0].title == h[0].title);

    // sanitizeField strips tab/newline
    CHECK(sanitizeField("a\tb\nc") == "a b c");

    // file round-trip in the temp dir
    std::vector<Bookmark> b2 = {{"http://x", "X title"}, {"http://y", "Y\ttab"}};
    saveBookmarks(dir, b2);
    auto b3 = loadBookmarks(dir);
    CHECK(b3.size() == 2); CHECK(b3[0].url == "http://x"); CHECK(b3[1].title == "Y tab"); // sanitized on save
    Settings s; s.zoom = 1.44; s.readerFont = 46; s.ua = "mobile";
    saveSettings(dir, s);
    Settings s2 = loadSettings(dir);
    CHECK(s2.readerFont == 46); CHECK(s2.ua == "mobile"); CHECK(s2.zoom > 1.43 && s2.zoom < 1.45);

    // corrupt/missing -> defaults
    Settings s3 = loadSettings(dir + "/missing-subdir");
    CHECK(s3.zoom == 1.0); CHECK(s3.readerFont == 30); CHECK(s3.ua.empty());

    // atomicWrite: a failed write returns false and must NOT clobber an existing good file
    CHECK(!detail::atomicWrite(dir + "/missing/x.txt", "data"));   // missing parent dir
    CHECK(detail::atomicWrite(dir + "/protect.txt", "original"));
    CHECK(!detail::atomicWrite(dir + "/protect.txt/nested", "x")); // parent is a file -> open fails
    auto kept = detail::readLines(dir + "/protect.txt");
    CHECK(kept.size() == 1 && kept[0] == "original");              // untouched

    // loadSettings: ua goes through sanitizeField — control chars in a hand-edited file (ssh)
    // must not survive into the User-Agent / HTTP headers
    CHECK(detail::atomicWrite(dir + "/settings.txt", "zoom=2.0\nua=evil\tagent\n"));
    Settings s6 = loadSettings(dir);
    CHECK(s6.ua == "evil agent");
    CHECK(s6.zoom > 1.99 && s6.zoom < 2.01);

    // ua round-trip: dirty value is sanitized on save and stays clean on load
    Settings s7; s7.ua = "a\tb\nc";
    CHECK(saveSettings(dir, s7));
    CHECK(loadSettings(dir).ua == "a b c");

    // loadSettings clamps out-of-range values back to defaults (zoom [0.5,3.0], readerFont [14,96])
    CHECK(detail::atomicWrite(dir + "/settings.txt", "zoom=99\nreaderFont=5\n"));
    Settings sc = loadSettings(dir);
    CHECK(sc.zoom == 1.0); CHECK(sc.readerFont == 30);
    CHECK(detail::atomicWrite(dir + "/settings.txt", "zoom=0.5\nreaderFont=96\n"));
    Settings se = loadSettings(dir);
    CHECK(se.zoom == 0.5); CHECK(se.readerFont == 96);      // range edges are kept, not clamped

    // loadBookmarks/loadHistory skip malformed lines (a line without the tab separator)
    CHECK(detail::atomicWrite(dir + "/bookmarks.txt", "http://good\tGood\nno-tab-line\nhttp://g2\tG2\n"));
    auto bl = loadBookmarks(dir);
    CHECK(bl.size() == 2); CHECK(bl[0].url == "http://good"); CHECK(bl[1].title == "G2");
    CHECK(detail::atomicWrite(dir + "/history.txt", "100\thttp://a\tA\nno-tab\n50\thttp://b\n"));
    auto hl = loadHistory(dir);
    CHECK(hl.size() == 1); CHECK(hl[0].url == "http://a"); CHECK(hl[0].ts == 100); CHECK(hl[0].title == "A");

    // Duplicate URLs in the store (hand-edited file): loadBookmarks keeps both — no dedupe on
    // load — and toggleBookmark removes only the FIRST match. Current behavior, pinned here.
    CHECK(detail::atomicWrite(dir + "/bookmarks.txt", "http://dup\tFirst\nhttp://dup\tSecond\n"));
    auto dup = loadBookmarks(dir);
    CHECK(dup.size() == 2);
    CHECK(toggleBookmark(dup, "http://dup", "x") == false);  // un-bookmarks one copy...
    CHECK(dup.size() == 1); CHECK(dup[0].title == "Second"); // ...the first one
    CHECK(isBookmarked(dup, "http://dup"));                  // the second copy is still bookmarked

    if (fails == 0) std::printf("profile_test: OK\n");
    return fails ? 1 : 0;
}
