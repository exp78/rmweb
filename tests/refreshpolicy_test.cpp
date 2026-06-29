// Host unit test for the pure e-ink refresh policy (no Qt, no device). Build+run on the dev host:
//   clang++ -std=c++17 -o build/refreshpolicy_test tests/refreshpolicy_test.cpp && ./build/refreshpolicy_test
#include "../engine/wpeqt/refreshpolicy.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;
int main() {
    // in-flight motion is always Fast and never resets the counter
    { RefreshPolicy p; assert(p.decide(PresentKind::Motion, false) == Waveform::Fast);
      assert(p.turnsSinceFull == 0); }
    // navigation = clean Full, counter reset
    { RefreshPolicy p; p.turnsSinceFull = 5;
      assert(p.decide(PresentKind::Navigation, false) == Waveform::Full);
      assert(p.turnsSinceFull == 0); }
    // manual full refresh = Full + reset
    { RefreshPolicy p; p.turnsSinceFull = 2;
      assert(p.decide(PresentKind::Manual, false) == Waveform::Full); assert(p.turnsSinceFull == 0); }
    // every-N: full flash on the Nth grayscale page turn, then back to fast
    { RefreshPolicy p; p.fullEveryN = 3;
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast);  // 1
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast);  // 2
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Full);  // 3 -> flush
      assert(p.turnsSinceFull == 0);
      assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast); } // 1 again
    // fullEveryN=0 disables the count path (only color/nav/manual force Full)
    { RefreshPolicy p; p.fullEveryN = 0;
      for (int i = 0; i < 50; ++i) assert(p.decide(PresentKind::PageTurn, false) == Waveform::Fast); }
    // color content forces Full each turn (ghosts harder) unless grayscale mode is on
    { RefreshPolicy p; p.fullEveryN = 0;
      assert(p.decide(PresentKind::PageTurn, true) == Waveform::Full); assert(p.turnsSinceFull == 0); }
    { RefreshPolicy p; p.fullEveryN = 0; p.grayscaleMode = true;
      assert(p.decide(PresentKind::PageTurn, true) == Waveform::Fast); } // color suppressed
    // idle ghost-clear: Full only if fast frames accumulated, else a no-op Fast
    { RefreshPolicy p; assert(p.decide(PresentKind::Idle, false) == Waveform::Fast); }
    { RefreshPolicy p; p.fullEveryN = 0; p.decide(PresentKind::PageTurn, false); // turnsSinceFull=1
      assert(p.decide(PresentKind::Idle, false) == Waveform::Full); assert(p.turnsSinceFull == 0); }
    printf("refreshpolicy tests OK\n");
    return 0;
}
