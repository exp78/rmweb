// Pure link-hint label generator (qutebrowser-style) — no Qt, no device deps, so it is unit-tested
// off-device (tests/hintlabels_test.cpp). Produces N short, unique, type- or tap-able labels from an
// alphabet, each of the minimal uniform length needed (>= minLen). Home-row default = easy on a HW
// keyboard and readable as a tappable badge on e-ink. Consumed by the link-hint overlay (Phase D).
#pragma once
#include <string>
#include <vector>
namespace rmweb {

struct HintConfig {
    std::string chars = "asdfghjkl";  // alphabet for labels (home row)
    int minLen = 1;                   // minimum label length floor
};

inline std::vector<std::string> hintLabels(int n, const HintConfig& c = {}) {
    std::vector<std::string> out;
    if (n <= 0 || c.chars.empty()) return out;
    const int a = static_cast<int>(c.chars.size());
    if (a == 1 && n > 1) return out;          // a 1-char alphabet can't produce >1 unique label
    int L = 1;
    if (a >= 2) { long cap = a; while (cap < n) { cap *= a; ++L; } }  // smallest L with a^L >= n
    if (L < c.minLen) L = c.minLen;                                   // (a==1 here implies n==1)
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        std::string s(static_cast<size_t>(L), c.chars[0]);
        int v = i;
        for (int pos = L - 1; pos >= 0; --pos) { s[pos] = c.chars[v % a]; v /= a; }  // odometer
        out.push_back(s);
    }
    return out;
}

} // namespace rmweb
