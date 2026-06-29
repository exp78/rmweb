// Host unit test for the pure tap-zone classifier (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/tapzone_test tests/tapzone_test.cpp && ./build/tapzone_test
#include "../engine/wpeqt/tapzone.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    const int W = 1620, H = 2160;                 // rMPP panel
    // edgeFrac 0.22 -> left x<=356.4, right x>=1263.6 ; topStrip y<=172.8
    assert(classifyTap(W/2, H/2, W, H) == TapAction::Content);       // center -> link/content
    assert(classifyTap(10,   H/2, W, H) == TapAction::Prev);         // left edge
    assert(classifyTap(W-10, H/2, W, H) == TapAction::Next);         // right edge
    assert(classifyTap(W/2,  5,   W, H) == TapAction::SummonChrome); // top strip
    assert(classifyTap(5,    5,   W, H) == TapAction::SummonChrome); // top-left: top strip wins
    assert(classifyTap(10,   H-10,W, H) == TapAction::Prev);         // bottom-left = prev (no top)
    assert(classifyTap(356,  H/2, W, H) == TapAction::Prev);         // boundary <= edge -> prev
    assert(classifyTap(357,  H/2, W, H) == TapAction::Content);      // just inside center
    assert(classifyTap(10,   10,  0, 0) == TapAction::Content);      // zero-size guard
    printf("tapzone tests OK\n");
    return 0;
}
