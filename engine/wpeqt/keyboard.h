// Pure on-screen-keyboard layout + hit-test for URL entry — no Qt, no device deps, so it is unit-tested
// off-device (tests/keyboard_test.cpp). The reading shell DRAWS these keys into the WPE frame (B2: a
// QtQuick keyboard does not composite under the epaper QPA) and routes finger taps through hitKey().
// Layout: 4 character rows (digits / qwerty / a..l+Del / z..m+./-/) + an action row (Cancel / .com / Go),
// 5 equal rows filling [topY, panelH) across panelW.
#pragma once
#include <string>
#include <vector>
namespace rmweb {

enum class KeyKind { Char, Backspace, Go, Cancel };

struct Key {
    int x, y, w, h;        // rect in panel px
    std::string label;     // text drawn on the key
    std::string insert;    // text appended to the edit buffer on tap (Char keys only)
    KeyKind kind;
};

// Build the URL keyboard: 5 equal-height rows filling [topY, panelH) across panelW.
inline std::vector<Key> buildKeyboard(int panelW, int panelH, int topY) {
    std::vector<Key> keys;
    if (panelW <= 0 || panelH <= topY) return keys;          // guard degenerate geometry
    const int rows = 5;
    const int rowH = (panelH - topY) / rows;
    const int u = panelW / 10;                               // 10-column base unit
    auto charRow = [&](int r, const std::vector<std::string>& cells) {
        const int y = topY + r * rowH, n = (int)cells.size();
        for (int i = 0; i < n; ++i) {
            const int x = i * u, w = (i == n - 1) ? (panelW - x) : u;   // last cell absorbs any remainder
            keys.push_back({ x, y, w, rowH, cells[i], cells[i], KeyKind::Char });
        }
    };
    charRow(0, {"1","2","3","4","5","6","7","8","9","0"});
    charRow(1, {"q","w","e","r","t","y","u","i","o","p"});
    {   // a..l (9) + Del
        const int r = 2, y = topY + r * rowH;
        const std::vector<std::string> cs = {"a","s","d","f","g","h","j","k","l"};
        for (int i = 0; i < 9; ++i) keys.push_back({ i*u, y, u, rowH, cs[i], cs[i], KeyKind::Char });
        keys.push_back({ 9*u, y, panelW - 9*u, rowH, "Del", "", KeyKind::Backspace });
    }
    charRow(3, {"z","x","c","v","b","n","m",".","-","/"});
    {   // action row: Cancel (3u) | .com (3u) | Go (rest)
        const int r = 4, y = topY + r * rowH;
        keys.push_back({ 0,    y, 3*u,          rowH, "Cancel", "",     KeyKind::Cancel });
        keys.push_back({ 3*u,  y, 3*u,          rowH, ".com",   ".com", KeyKind::Char });
        keys.push_back({ 6*u,  y, panelW - 6*u, rowH, "Go",     "",     KeyKind::Go });
    }
    return keys;
}

// Hit-test a tap (panel px) against the keys; returns the index, or -1 if none (e.g. above the keyboard).
inline int hitKey(const std::vector<Key>& keys, int x, int y) {
    for (int i = 0; i < (int)keys.size(); ++i) {
        const Key& k = keys[i];
        if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) return i;
    }
    return -1;
}

} // namespace rmweb
