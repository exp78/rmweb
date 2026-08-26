// Pure finger-gesture classifier for the reading shell — no Qt, no device deps, so it is unit-tested
// off-device (tests/gesture_test.cpp). TouchReader feeds it the contact's travel + dwell and turns the
// result into a page-turn (swipe) or a tap / long-press (link peek) for the C++ tap router in main().
#pragma once
namespace rmweb {

enum class Gesture { None, SwipeUp, SwipeDown, Tap, LongPress };

struct GestureParams {
    int swipeMinDy    = 240;  // vertical travel (panel px) to count as a page turn (~11% of height)
    int swipeMaxDx    = 200;  // keep a swipe roughly vertical (reject diagonals)
    int tapMaxMove    = 40;   // max travel (panel px) for a contact to still be a tap
    int tapMaxDwellMs = 700;  // max contact duration (ms) for a tap — longer is a long-press (link peek)
};

// dx,dy = lift - down position (panel px); dwellMs = contact duration. A near-stationary, short contact
// is a Tap; a near-stationary LONG one is a LongPress (link peek); a long, mostly-vertical contact is a
// Swipe; everything else (diagonal, tiny drift over a long hold past the move cap) is None.
inline Gesture classifyGesture(int dx, int dy, int dwellMs, const GestureParams& p = {}) {
    const int adx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    if (adx <= p.tapMaxMove && ady <= p.tapMaxMove)
        return dwellMs <= p.tapMaxDwellMs ? Gesture::Tap : Gesture::LongPress;
    if (adx < p.swipeMaxDx && ady >= p.swipeMinDy)
        return dy < 0 ? Gesture::SwipeUp : Gesture::SwipeDown;
    return Gesture::None;
}

} // namespace rmweb
