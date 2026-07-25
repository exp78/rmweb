// Host unit test for the pure URL normalizer (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/url_test tests/url_test.cpp && ./build/url_test
#include "../engine/wpeqt/url.h"
#include <cstdio>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

int main() {
    CHECK(normalizeUrl("example.com")        == "https://example.com");  // bare host -> https
    CHECK(normalizeUrl("  example.com  ")    == "https://example.com");  // trimmed first
    CHECK(normalizeUrl("http://x.org")       == "http://x.org");         // explicit scheme kept
    CHECK(normalizeUrl("https://y.org/path") == "https://y.org/path");   // path preserved
    CHECK(normalizeUrl("")                   == "");                     // empty stays empty
    CHECK(normalizeUrl("   ")                == "");                     // all-space -> empty
    CHECK(normalizeUrl("a:b")                == "https://a:b");          // ':' without "://" is still a bare host
    CHECK(normalizeUrl("ftp://x.org/f")      == "ftp://x.org/f");        // any "://" scheme kept as-is

    // urlDecode (rmweb: command payloads)
    CHECK(urlDecode("http://ex.com/a%20b")   == "http://ex.com/a b");    // basic %20
    CHECK(urlDecode("%D0%BF%D1%80")          == "\xD0\xBF\xD1\x80");     // UTF-8 bytes pass through decoded
    CHECK(urlDecode("a+b")                   == "a+b");                  // '+' is NOT a space here
    CHECK(urlDecode("100%")                  == "100%");                 // trailing '%' untouched
    CHECK(urlDecode("a%2")                   == "a%2");                  // truncated sequence untouched
    CHECK(urlDecode("%zz")                   == "%zz");                  // invalid hex untouched
    CHECK(urlDecode("")                      == "");

    // looksLikeUrl (address bar: URL vs search query)
    CHECK(looksLikeUrl("example.com"));                    // bare host
    CHECK(looksLikeUrl("https://x.org/p"));                // scheme'd
    CHECK(looksLikeUrl("  ex.com  "));                     // trims first
    CHECK(!looksLikeUrl("wiki e ink"));                    // spaces -> query
    CHECK(!looksLikeUrl("localhost"));                     // no dot, no scheme -> query
    CHECK(!looksLikeUrl(""));                              // empty
    CHECK(!looksLikeUrl("   "));                           // all-space

    // urlEncode (search URLs)
    CHECK(urlEncode("wiki e ink") == "wiki%20e%20ink");
    CHECK(urlEncode("a+b&c")      == "a%2Bb%26c");
    CHECK(urlEncode("plain-ok_1.~") == "plain-ok_1.~");    // unreserved untouched
    CHECK(urlEncode("")           == "");

    // hostFromUrl (per-site stores: passwords)
    CHECK(hostFromUrl("https://example.com/a/b?x") == "example.com");
    CHECK(hostFromUrl("https://example.com:8443/a") == "example.com:8443");
    CHECK(hostFromUrl("http://ex.com") == "ex.com");
    CHECK(hostFromUrl("https://u:p@ex.com/") == "ex.com");   // userinfo stripped
    CHECK(hostFromUrl("ex.com/path") == "ex.com");           // scheme-less
    CHECK(hostFromUrl("") == "");
    CHECK(hostFromUrl("https://") == "");
    if (fails == 0) std::printf("url_test: OK\n");
    return fails ? 1 : 0;
}
