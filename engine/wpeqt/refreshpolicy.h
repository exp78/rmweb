// Pure e-ink waveform policy for the present controller — no Qt, no device deps, so it is unit-tested
// off-device (tests/refreshpolicy_test.cpp). The controller calls decide() once per present and uses
// the result to pick a fast (grayscale, ~150 ms) vs full (GC16 colour flash, ~1-1.5 s) refresh.
#pragma once
namespace rmweb {

enum class Waveform { Fast, Full };
// What triggered this present. Motion = a frame mid swipe/scroll; PageTurn = a settled page after a
// turn; Navigation = new URL / reader toggle; Idle = the ~1-2 s idle ghost-clear; Manual = user asked.
enum class PresentKind { Navigation, PageTurn, Motion, Idle, Manual };

struct RefreshPolicy {
    int  fullEveryN     = 12;     // full flash every N page turns to clear ghosting (0 = never)
    bool grayscaleMode  = false;  // user "grayscale reading mode": suppress colour-driven full refresh
    int  turnsSinceFull = 0;      // fast page turns accumulated since the last full refresh

    Waveform decide(PresentKind kind, bool hasColorContent) {
        switch (kind) {
            case PresentKind::Motion:
                return Waveform::Fast;                      // in-flight: always fast, no counter change
            case PresentKind::Navigation:
            case PresentKind::Manual:
                turnsSinceFull = 0; return Waveform::Full;  // new page / explicit: clean full
            case PresentKind::Idle:
                if (turnsSinceFull == 0) return Waveform::Fast;   // nothing to ghost-clear
                turnsSinceFull = 0; return Waveform::Full;
            case PresentKind::PageTurn: {
                ++turnsSinceFull;
                const bool everyN = fullEveryN > 0 && turnsSinceFull >= fullEveryN;
                const bool color  = hasColorContent && !grayscaleMode;
                if (everyN || color) { turnsSinceFull = 0; return Waveform::Full; }
                return Waveform::Fast;
            }
        }
        return Waveform::Fast;
    }
};

} // namespace rmweb
