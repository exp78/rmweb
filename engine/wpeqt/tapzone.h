// Pure tap-location classifier for the reading shell — no Qt, no device deps, so it is unit-tested
// off-device (tests/tapzone_test.cpp). The shell maps the result: Next/Prev -> page turn,
// SummonChrome -> toggle bars, Content -> inject a mouse click (follow a link) via the touch bridge.
#pragma once
namespace rmweb {

enum class TapAction { Next, Prev, SummonChrome, Content };

// Edge-zone layout (fractions of panel w/h). Edges turn pages; the center stays tappable for links
// (a browser needs what a pure e-reader doesn't); the top strip summons chrome and wins at the corners.
struct TapZones {
    double topStripFrac = 0.08;  // top 8% summons chrome
    double edgeFrac     = 0.15;  // left/right 15% each = page turn (narrower so links stay tappable)
};

inline TapAction classifyTap(int x, int y, int w, int h, const TapZones& z = {}) {
    if (w <= 0 || h <= 0) return TapAction::Content;             // guard degenerate size
    if (y <= z.topStripFrac * h)        return TapAction::SummonChrome;
    if (x <= z.edgeFrac * w)            return TapAction::Prev;
    if (x >= (1.0 - z.edgeFrac) * w)    return TapAction::Next;
    return TapAction::Content;
}

} // namespace rmweb
