// Pure on-screen-keyboard layout + hit-test for URL entry — no Qt, no device deps, so it is unit-tested
// off-device (tests/keyboard_test.cpp). The reading shell DRAWS these keys into the WPE frame (B2: a
// QtQuick keyboard does not composite under the epaper QPA) and routes finger taps through hitKey().
//
// Two pages on a 10-column grid, 5 equal rows filling [topY, panelH):
//   letters : digits / qwertyuiop / asdfghjkl+Del / Shift zxcvbnm . - / ?123 Cancel .com space / Go
//   symbols : digits / : ? = & _ % ~ # + @ / ! $ ' " ( ) * , ; ^ / \ | < > [ ] { } ` + Del / ABC Cancel .com space / Go
// `shifted` uppercases the letter keys (label + insert) on the letters page — one-shot Shift in the shell.
#pragma once
#include <string>
#include <vector>
namespace rmweb {

enum class KeyKind { Char, Backspace, Go, Cancel, Shift, Sym };

struct Key {
    int x, y, w, h;        // rect in panel px
    std::string label;     // text drawn on the key
    std::string insert;    // text appended to the edit buffer on tap (Char keys only)
    KeyKind kind;
};

// Build the URL keyboard: 5 equal-height rows filling [topY, panelH) across panelW.
inline std::vector<Key> buildKeyboard(int panelW, int panelH, int topY,
                                      bool shifted = false, bool symPage = false) {
    std::vector<Key> keys;
    if (panelW <= 0 || panelH <= topY) return keys;          // guard degenerate geometry
    const int rows = 5;
    const int rowH = (panelH - topY) / rows;
    const int u = panelW / 10;                               // 10-column base unit

    auto up = [](std::string s) {                            // ASCII-uppercase single-letter labels
        for (char &c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        return s;
    };
    auto charRow = [&](int r, const std::vector<std::string>& cells, bool shiftable) {
        const int y = topY + r * rowH, n = (int)cells.size();
        for (int i = 0; i < n; ++i) {
            const int x = i * u, w = (i == n - 1) ? (panelW - x) : u;   // last cell absorbs any remainder
            const std::string s = (shiftable && shifted) ? up(cells[i]) : cells[i];
            keys.push_back({ x, y, w, rowH, s, s, KeyKind::Char });
        }
    };
    // 9 char cells + a Del key absorbing the rest of the row.
    auto charRowWithDel = [&](int r, const std::vector<std::string>& cells, bool shiftable) {
        const int y = topY + r * rowH, n = (int)cells.size();
        for (int i = 0; i < n; ++i) {
            const std::string s = (shiftable && shifted) ? up(cells[i]) : cells[i];
            keys.push_back({ i*u, y, u, rowH, s, s, KeyKind::Char });
        }
        keys.push_back({ n*u, y, panelW - n*u, rowH, "Del", "", KeyKind::Backspace });
    };

    charRow(0, {"1","2","3","4","5","6","7","8","9","0"}, /*shiftable=*/false);
    if (!symPage) {
        charRow(1, {"q","w","e","r","t","y","u","i","o","p"}, /*shiftable=*/true);
        charRowWithDel(2, {"a","s","d","f","g","h","j","k","l"}, /*shiftable=*/true);
        {   // Shift | z x c v b n m | . | -
            const int y = topY + 3 * rowH;
            keys.push_back({ 0, y, u, rowH, "Shift", "", KeyKind::Shift });
            const std::vector<std::string> cs = {"z","x","c","v","b","n","m",".","-"};
            for (int i = 0; i < (int)cs.size(); ++i) {
                const int x = (i + 1) * u, w = (i == (int)cs.size() - 1) ? (panelW - x) : u;
                const std::string s = shifted ? up(cs[i]) : cs[i];
                keys.push_back({ x, y, w, rowH, s, s, KeyKind::Char });
            }
        }
    } else {
        // RFC-3986-oriented symbol set (ports, query strings, escapes) + common punctuation.
        charRow(1, {":","?","=","&","_","%","~","#","+","@"}, /*shiftable=*/false);
        charRow(2, {"!","$","'","\"","(",")","*",",",";","^"}, /*shiftable=*/false);
        charRowWithDel(3, {"\\","|","<",">","[","]","{","}","`"}, /*shiftable=*/false);
    }
    {   // action row (both pages): ?123/ABC (1u) | Cancel (2u) | .com (1u) | space (3u) | / (1u) | Go (2u)
        const int y = topY + 4 * rowH;
        keys.push_back({ 0,   y, u,             rowH, symPage ? "ABC" : "?123", "",     KeyKind::Sym });
        keys.push_back({ u,   y, 2*u,           rowH, "Cancel",                 "",     KeyKind::Cancel });
        keys.push_back({ 3*u, y, u,             rowH, ".com",                   ".com", KeyKind::Char });
        keys.push_back({ 4*u, y, 3*u,           rowH, "space",                  " ",    KeyKind::Char });
        keys.push_back({ 7*u, y, u,             rowH, "/",                      "/",    KeyKind::Char });
        keys.push_back({ 8*u, y, panelW - 8*u,  rowH, "Go",                     "",     KeyKind::Go });
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
