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

} // namespace rmweb
