// Host unit test for the pure tap/swipe classifier (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/gesture_test tests/gesture_test.cpp && ./build/gesture_test
#include "../engine/wpeqt/gesture.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    assert(classifyGesture(0, -300, 200) == Gesture::SwipeUp);    // finger up   = next page
    assert(classifyGesture(10, 300, 200) == Gesture::SwipeDown);  // finger down = prev page
    assert(classifyGesture(5, 5, 120)    == Gesture::Tap);        // small move, short dwell
    assert(classifyGesture(5, 5, 2000)   == Gesture::None);       // long press is NOT a tap
    assert(classifyGesture(300, 300, 200) == Gesture::None);      // diagonal -> nothing
    assert(classifyGesture(0, 100, 200)   == Gesture::None);      // short vertical -> nothing
    printf("gesture tests OK\n");
    return 0;
}
