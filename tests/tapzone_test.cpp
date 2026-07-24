// Host unit test for the pure tap-zone classifier (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/tapzone_test tests/tapzone_test.cpp && ./build/tapzone_test
#include "../engine/wpeqt/tapzone.h"
#include <cstdio>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

int main() {
    const int W = 1620, H = 2160;                 // rMPP panel
    // edgeFrac 0.15 -> left x<=243, right x>=1377 ; topStrip y<=172.8
    CHECK(classifyTap(W/2, H/2, W, H) == TapAction::Content);       // center -> link/content
    CHECK(classifyTap(10,   H/2, W, H) == TapAction::Prev);         // left edge
    CHECK(classifyTap(W-10, H/2, W, H) == TapAction::Next);         // right edge
    CHECK(classifyTap(W/2,  5,   W, H) == TapAction::SummonChrome); // top strip
    CHECK(classifyTap(5,    5,   W, H) == TapAction::SummonChrome); // top-left: top strip wins
    CHECK(classifyTap(10,   H-10,W, H) == TapAction::Prev);         // bottom-left = prev (no top)
    CHECK(classifyTap(243,  H/2, W, H) == TapAction::Prev);         // boundary <= edge -> prev
    CHECK(classifyTap(244,  H/2, W, H) == TapAction::Content);      // just inside center
    CHECK(classifyTap(10,   10,  0, 0) == TapAction::Content);      // zero-size guard

    // Zone seams, verified against the double math in tapzone.h at this panel size:
    // 0.15*1620 == 243.0, (1-0.15)*1620 == 1377.0, 0.08*2160 == 172.8 (all exact)
    CHECK(classifyTap(1376, H/2, W, H) == TapAction::Content);      // last center px before right zone
    CHECK(classifyTap(1377, H/2, W, H) == TapAction::Next);         // boundary >= right edge -> next
    CHECK(classifyTap(W/2, 172, W, H) == TapAction::SummonChrome); // 172 <= 172.8 -> top strip
    CHECK(classifyTap(W/2, 173, W, H) == TapAction::Content);      // first row below the strip

    // Extreme panel pixels
    CHECK(classifyTap(0,    H/2, W, H) == TapAction::Prev);         // leftmost column
    CHECK(classifyTap(W-1,  H/2, W, H) == TapAction::Next);         // rightmost column
    CHECK(classifyTap(0,    H-1, W, H) == TapAction::Prev);         // bottom-left corner
    CHECK(classifyTap(W-1,  H-1, W, H) == TapAction::Next);         // bottom-right corner

    if (fails == 0) std::printf("tapzone_test: OK\n");
    return fails ? 1 : 0;
}
