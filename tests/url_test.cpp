// Host unit test for the pure URL normalizer (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/url_test tests/url_test.cpp && ./build/url_test
#include "../engine/wpeqt/url.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    assert(normalizeUrl("example.com")        == "https://example.com");  // bare host -> https
    assert(normalizeUrl("  example.com  ")    == "https://example.com");  // trimmed first
    assert(normalizeUrl("http://x.org")       == "http://x.org");         // explicit scheme kept
    assert(normalizeUrl("https://y.org/path") == "https://y.org/path");   // path preserved
    assert(normalizeUrl("")                   == "");                     // empty stays empty
    assert(normalizeUrl("   ")                == "");                     // all-space -> empty
    printf("url tests OK\n");
    return 0;
}
