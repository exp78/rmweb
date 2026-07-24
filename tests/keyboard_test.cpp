// Host unit test for the pure URL-keyboard layout + hit-test (no Qt, no device). Build+run:
//   clang++ -std=c++17 -o build/keyboard_test tests/keyboard_test.cpp && ./build/keyboard_test
#include "../engine/wpeqt/keyboard.h"
#include <cstdio>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

static const Key* find(const std::vector<Key>& ks, const std::string& label) {
    for (const auto& k : ks) if (k.label == label) return &k;
    return nullptr;
}

int main() {
    const int W = 1620, H = 2160, TOP = 1340;
    const auto keys = buildKeyboard(W, H, TOP);
    CHECK(keys.size() == 43);                                   // 10 + 10 + 10 + 10 + 3

    // Every key sits inside the keyboard rect [TOP, H) x [0, W).
    for (const auto& k : keys) {
        CHECK(k.y >= TOP && k.y + k.h <= H);
        CHECK(k.x >= 0   && k.x + k.w <= W);
        CHECK(k.w > 0    && k.h > 0);
    }

    // Tap the center of the first key (digit '1', top-left) -> index 0.
    CHECK(hitKey(keys, keys[0].x + keys[0].w/2, keys[0].y + keys[0].h/2) == 0);
    // A tap above the keyboard hits nothing.
    CHECK(hitKey(keys, W/2, TOP - 5) == -1);

    // Char keys carry their insert text; special keys carry the right kind.
    { const Key* q = find(keys, "q");      CHECK(q && q->kind == KeyKind::Char && q->insert == "q"); }
    { const Key* dot = find(keys, ".");    CHECK(dot && dot->insert == "."); }
    { const Key* sl = find(keys, "/");     CHECK(sl && sl->insert == "/"); }
    { const Key* dash = find(keys, "-");   CHECK(dash && dash->insert == "-"); }
    { const Key* com = find(keys, ".com"); CHECK(com && com->kind == KeyKind::Char && com->insert == ".com"); }
    { const Key* del = find(keys, "Del");  CHECK(del && del->kind == KeyKind::Backspace && del->insert.empty()); }
    { const Key* go = find(keys, "Go");    CHECK(go && go->kind == KeyKind::Go); }
    { const Key* c = find(keys, "Cancel"); CHECK(c && c->kind == KeyKind::Cancel); }

    // Hitting the Go key's center resolves back to a Go key.
    { const Key* go = find(keys, "Go"); CHECK(go);
      const int i = hitKey(keys, go->x + go->w/2, go->y + go->h/2);
      CHECK(i >= 0 && keys[i].kind == KeyKind::Go); }

    // Key seams: rects are half-open [x, x+w) x [y, y+h), so the seam pixel
    // belongs to the NEXT key/row (hitKey: `x >= k.x && x < k.x + k.w`).
    CHECK(hitKey(keys, keys[0].x + keys[0].w,     keys[0].y + keys[0].h/2) == 1);  // '1'|'2' seam -> '2'
    CHECK(hitKey(keys, keys[0].x + keys[0].w - 1, keys[0].y + keys[0].h/2) == 0);  // last px still '1'
    { const Key* q = find(keys, "q"); CHECK(q);
      const int qi = (int)(q - keys.data());
      CHECK(hitKey(keys, q->x + 1, q->y)     == qi);  // row0/row1 seam -> row 1
      CHECK(hitKey(keys, q->x + 1, q->y - 1) == 0); } // one px above -> still row 0

    // First/last key and the extreme corners of the keyboard area.
    CHECK(keys.front().label == "1" && keys.back().label == "Go");
    CHECK(hitKey(keys, 0, TOP) == 0);                             // top-left corner -> first key
    CHECK(hitKey(keys, W - 1, H - 1) == (int)keys.size() - 1);    // bottom-right px -> last key (Go)

    // y outside all rows hits nothing.
    CHECK(hitKey(keys, W/2, TOP - 1) == -1);                      // just above the keyboard
    CHECK(hitKey(keys, W/2, H)       == -1);                      // just below the last row

    // Degenerate geometry -> no keys, no crash.
    CHECK(buildKeyboard(0, H, TOP).empty());
    CHECK(buildKeyboard(W, TOP, TOP).empty());

    if (fails == 0) std::printf("keyboard_test: OK (%zu keys)\n", keys.size());
    return fails ? 1 : 0;
}
