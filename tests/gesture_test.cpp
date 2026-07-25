// Host unit test for the pure tap/swipe classifier (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/gesture_test tests/gesture_test.cpp && ./build/gesture_test
#include "../engine/wpeqt/gesture.h"
#include <cstdio>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

int main() {
    CHECK(classifyGesture(0, -300, 200) == Gesture::SwipeUp);    // finger up   = next page
    CHECK(classifyGesture(10, 300, 200) == Gesture::SwipeDown);  // finger down = prev page
    CHECK(classifyGesture(5, 5, 120)    == Gesture::Tap);        // small move, short dwell
    CHECK(classifyGesture(5, 5, 2000)   == Gesture::LongPress);  // long stationary hold = peek
    CHECK(classifyGesture(300, 300, 200) == Gesture::None);      // diagonal -> nothing
    CHECK(classifyGesture(0, 100, 200)   == Gesture::None);      // short vertical -> nothing

    // Exact swipe boundaries (defaults: swipeMinDy=240 inclusive, swipeMaxDx=200 exclusive)
    CHECK(classifyGesture(0, 240, 200)   == Gesture::SwipeDown); // dy == swipeMinDy -> swipe
    CHECK(classifyGesture(0, 239, 200)   == Gesture::None);      // dy one px short
    CHECK(classifyGesture(0, -240, 200)  == Gesture::SwipeUp);   // same boundary upwards
    CHECK(classifyGesture(199, 300, 200) == Gesture::SwipeDown); // dx < swipeMaxDx: vertical enough
    CHECK(classifyGesture(200, 300, 200) == Gesture::None);      // dx == swipeMaxDx: diagonal, rejected

    // Exact tap boundaries (tapMaxMove=40 and tapMaxDwellMs=700, both inclusive)
    CHECK(classifyGesture(40, 40, 700) == Gesture::Tap);         // all maxima hit exactly
    CHECK(classifyGesture(41, 0, 100)  == Gesture::None);        // 1px over tap move, short of a swipe
    CHECK(classifyGesture(0, 0, 701)   == Gesture::LongPress);   // 1ms over tap dwell -> long-press

    // Dwell gates only taps, not swipes
    CHECK(classifyGesture(0, 300, 5000) == Gesture::SwipeDown);  // slow swipe still turns the page

    if (fails == 0) std::printf("gesture_test: OK\n");
    return fails ? 1 : 0;
}
