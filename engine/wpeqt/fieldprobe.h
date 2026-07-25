// Pure protocol helpers for the content-tap probe — no Qt, no device deps, unit-tested off-device
// (tests/fieldprobe_test.cpp). The engine's tap JS (main.cpp) classifies what a content tap hit and
// answers ONE of these line-protocol strings:
//   "none"                    — empty area (GUI falls back to the chrome toggle)
//   "link"                    — a link/button was followed (navigation proceeds on its own)
//   "peek\n<href>"            — long-press peek: a link was hit but NOT followed; href = its target
//   "tick\n<label>"           — a checkbox/radio toggled or a <select> cycled; label = new state text
//   "field\n<0|1>\n<hint>\n<value>" — a text field was focused; flag 1 = password (mask the echo),
//                               hint = one line of identity clues (autocomplete/name/id/placeholder,
//                               whitespace-folded, may be empty) for autofill classification,
//                               value = the field's current content (may itself contain newlines)
#pragma once
#include <string>
#include <cstdio>
namespace rmweb {

enum class TapHit { None, Link, Peek, Tick, Field };

struct TapProbe {
    TapHit hit = TapHit::None;
    bool masked = false;     // Field only: password input -> echo '*' in the input line
    std::string value;       // Tick: state/option label. Field: current value. Peek: link href.
    std::string hint;        // Field only: autocomplete/name/id/placeholder clues (single line)
};

inline TapProbe parseTapProbe(const std::string& s) {
    if (s == "link") return {TapHit::Link, false, {}, {}};
    if (s.rfind("peek\n", 0) == 0) return {TapHit::Peek, false, s.substr(5), {}};
    if (s.rfind("tick\n", 0) == 0) return {TapHit::Tick, false, s.substr(5), {}};
    if (s.rfind("field\n", 0) == 0) {
        const size_t p = s.find('\n', 6);
        if (p == std::string::npos) return {TapHit::Field, false, {}, {}};   // malformed: no flag line
        const size_t q = s.find('\n', p + 1);
        if (q == std::string::npos)                                          // legacy: no hint line
            return {TapHit::Field, s.substr(6, p - 6) == "1", s.substr(p + 1), {}};
        return {TapHit::Field, s.substr(6, p - 6) == "1", s.substr(q + 1), s.substr(p + 1, q - p - 1)};
    }
    return {TapHit::None, false, {}, {}};
}

// Autofill kinds a text field can be classified into (learn-as-you-type; passwords never classify).
enum class FieldKind { None, Email, User, Name };

// Classify a field from its hint line — lower-cased ASCII substring match over
// "autocomplete name id placeholder". Order matters: "username" contains "name", so the
// user/login check runs first. Masked (password) fields are never classified.
inline FieldKind classifyFieldHint(const std::string& hint, bool masked) {
    if (masked) return FieldKind::None;
    std::string h; h.reserve(hint.size());
    for (char c : hint) h += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
    if (h.empty()) return FieldKind::None;
    if (h.find("mail") != std::string::npos) return FieldKind::Email;     // email / e-mail / mail
    if (h.find("user") != std::string::npos || h.find("login") != std::string::npos ||
        h.find("nick") != std::string::npos || h.find("account") != std::string::npos)
        return FieldKind::User;
    if (h.find("name") != std::string::npos) return FieldKind::Name;      // name / fullname / given-name…
    return FieldKind::None;
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
