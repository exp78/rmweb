// Pure protocol helpers for the content-tap probe — no Qt, no device deps, unit-tested off-device
// (tests/fieldprobe_test.cpp). The engine's tap JS (main.cpp) classifies what a content tap hit and
// answers ONE of these line-protocol strings:
//   "none"                    — empty area (GUI falls back to the chrome toggle)
//   "link"                    — a link/button was followed (navigation proceeds on its own)
//   "tick\n<label>"           — a checkbox/radio toggled or a <select> cycled; label = new state text
//   "field\n<0|1>\n<value>"   — a text field was focused; flag 1 = password (mask the echo),
//                               value = the field's current content (may itself contain newlines)
#pragma once
#include <string>
#include <cstdio>
namespace rmweb {

enum class TapHit { None, Link, Tick, Field };

struct TapProbe {
    TapHit hit = TapHit::None;
    bool masked = false;     // Field only: password input -> echo '*' in the input line
    std::string value;       // Tick: state/option label. Field: current value.
};

inline TapProbe parseTapProbe(const std::string& s) {
    if (s == "link") return {TapHit::Link, false, {}};
    if (s.rfind("tick\n", 0) == 0) return {TapHit::Tick, false, s.substr(5)};
    if (s.rfind("field\n", 0) == 0) {
        const size_t p = s.find('\n', 6);
        if (p == std::string::npos) return {TapHit::Field, false, {}};   // malformed: no flag line
        return {TapHit::Field, s.substr(6, p - 6) == "1", s.substr(p + 1)};
    }
    return {TapHit::None, false, {}};
}

// Escape a C++ string for embedding inside a JS double-quoted string literal (the commit-text eval).
// UTF-8 bytes pass through (the eval source is UTF-8); control chars become \xNN so a value with
// newlines can't break the string or smuggle code.
inline std::string jsStringEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\x%02x", static_cast<unsigned char>(c));
                    o += b;
                } else o += c;
        }
    }
    return o;
}

} // namespace rmweb
