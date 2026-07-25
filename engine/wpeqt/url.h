// Pure URL normalizer for the address bar — no Qt, no device deps, so it is unit-tested off-device
// (tests/url_test.cpp). Trims whitespace and, for a bare host (no scheme), defaults to https://.
#pragma once
#include <string>
#include <algorithm>
#include <cctype>
namespace rmweb {

inline std::string normalizeUrl(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));          // ltrim
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());   // rtrim
    if (s.empty()) return s;
    if (s.find("://") == std::string::npos) s = "https://" + s;             // bare host -> https
    return s;
}

// Percent-decode ("a%20b" -> "a b") — used on rmweb: command payloads, whose non-ASCII bytes the
// WebKit URL parser percent-encodes when resolving the start-page link. '+' stays literal (these
// are URL bytes, not form data); an invalid/truncated % sequence passes through untouched.
inline std::string urlDecode(const std::string& s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const int hi = (i + 1 < s.size()) ? hex(s[i + 1]) : -1;
        const int lo = (i + 2 < s.size()) ? hex(s[i + 2]) : -1;
        if (s[i] == '%' && hi >= 0 && lo >= 0) { o += char(hi * 16 + lo); i += 2; }
        else o += s[i];
    }
    return o;
}

} // namespace rmweb
