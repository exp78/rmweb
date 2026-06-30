// Host unit test for the pure URL-keyboard layout + hit-test (no Qt, no device). Build+run:
//   clang++ -std=c++17 -o build/keyboard_test tests/keyboard_test.cpp && ./build/keyboard_test
#include "../engine/wpeqt/keyboard.h"
#include <cassert>
#include <cstdio>
using namespace rmweb;

static const Key* find(const std::vector<Key>& ks, const std::string& label) {
    for (const auto& k : ks) if (k.label == label) return &k;
    return nullptr;
}

int main() {
    const int W = 1620, H = 2160, TOP = 1340;
    const auto keys = buildKeyboard(W, H, TOP);
    assert(keys.size() == 43);                                  // 10 + 10 + 10 + 10 + 3

    // Every key sits inside the keyboard rect [TOP, H) x [0, W).
    for (const auto& k : keys) {
        assert(k.y >= TOP && k.y + k.h <= H);
        assert(k.x >= 0   && k.x + k.w <= W);
        assert(k.w > 0    && k.h > 0);
    }

    // Tap the center of the first key (digit '1', top-left) -> index 0.
    assert(hitKey(keys, keys[0].x + keys[0].w/2, keys[0].y + keys[0].h/2) == 0);
    // A tap above the keyboard hits nothing.
    assert(hitKey(keys, W/2, TOP - 5) == -1);

    // Char keys carry their insert text; special keys carry the right kind.
    { const Key* q = find(keys, "q");      assert(q && q->kind == KeyKind::Char && q->insert == "q"); }
    { const Key* dot = find(keys, ".");    assert(dot && dot->insert == "."); }
    { const Key* sl = find(keys, "/");     assert(sl && sl->insert == "/"); }
    { const Key* dash = find(keys, "-");   assert(dash && dash->insert == "-"); }
    { const Key* com = find(keys, ".com"); assert(com && com->kind == KeyKind::Char && com->insert == ".com"); }
    { const Key* del = find(keys, "Del");  assert(del && del->kind == KeyKind::Backspace && del->insert.empty()); }
    { const Key* go = find(keys, "Go");    assert(go && go->kind == KeyKind::Go); }
    { const Key* c = find(keys, "Cancel"); assert(c && c->kind == KeyKind::Cancel); }

    // Hitting the Go key's center resolves back to a Go key.
    { const Key* go = find(keys, "Go"); assert(go);
      const int i = hitKey(keys, go->x + go->w/2, go->y + go->h/2);
      assert(i >= 0 && keys[i].kind == KeyKind::Go); }

    // Degenerate geometry -> no keys, no crash.
    assert(buildKeyboard(0, H, TOP).empty());
    assert(buildKeyboard(W, TOP, TOP).empty());

    printf("keyboard tests OK (%zu keys)\n", keys.size());
    return 0;
}
