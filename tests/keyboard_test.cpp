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
    const int W = 1620, H = 2160, TOP = 1340, U = W / 10;
    const auto keys = buildKeyboard(W, H, TOP);                    // letters page, unshifted
    CHECK(keys.size() == 46);                                      // 10 + 10 + (9+Del) + (Shift+9) + 6

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
    { const Key* sp = find(keys, "space"); CHECK(sp && sp->kind == KeyKind::Char && sp->insert == " "); }

    // The old crippled layout had none of these — pin that they live on the symbols page now.
    CHECK(find(keys, ":") == nullptr && find(keys, "?") == nullptr && find(keys, "=") == nullptr);

    // Shift + page-switch keys sit at fixed action positions.
    { const Key* sh = find(keys, "Shift"); CHECK(sh && sh->kind == KeyKind::Shift); }
    { const Key* sym = find(keys, "?123"); CHECK(sym && sym->kind == KeyKind::Sym); }

    // Action-row geometry: ?123 (1u) | Cancel (2u) | .com (1u) | space (3u) | / (1u) | Go (2u).
    { const Key* sym = find(keys, "?123");  CHECK(sym && sym->x == 0   && sym->w == U); }
    { const Key* c = find(keys, "Cancel");  CHECK(c   && c->x   == U   && c->w   == 2*U); }
    { const Key* sp = find(keys, "space");  CHECK(sp  && sp->x  == 4*U && sp->w  == 3*U); }
    { const Key* sl = find(keys, "/");      CHECK(sl  && sl->x  == 7*U && sl->w  == U); }
    { const Key* go = find(keys, "Go");     CHECK(go  && go->x  == 8*U && go->w  == W - 8*U); }

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

    // --- Shift: letter labels+inserts uppercase, digits/punctuation untouched, geometry identical. ---
    { const auto up = buildKeyboard(W, H, TOP, /*shifted=*/true);
      CHECK(up.size() == keys.size());
      const Key* Q = find(up, "Q"); CHECK(Q && Q->kind == KeyKind::Char && Q->insert == "Q");
      const Key* Z = find(up, "Z"); CHECK(Z && Z->insert == "Z");
      CHECK(find(up, "q") == nullptr && find(up, "z") == nullptr);
      const Key* d1 = find(up, "1");  CHECK(d1 && d1->insert == "1");
      const Key* dt = find(up, ".");  CHECK(dt && dt->insert == ".");
      const Key* sh = find(up, "Shift"); CHECK(sh && sh->kind == KeyKind::Shift);
      const Key* q0 = find(keys, "q"); CHECK(Q && q0 && Q->x == q0->x && Q->w == q0->w); }

    // --- Symbols page: RFC-3986 set + digits kept on top row; action row flips to ABC. ---
    { const auto sym = buildKeyboard(W, H, TOP, false, /*symPage=*/true);
      CHECK(sym.size() == 46);
      for (const auto& k : sym) { CHECK(k.y >= TOP && k.y + k.h <= H && k.x >= 0 && k.x + k.w <= W); }
      const char* want[] = {":","?","=","&","_","%","~","#","+","@","!","$","'","\"",
                            "(",")","*",",",";","^","\\","|","<",">","[","]","{","}","`"};
      for (const char* s : want) { const Key* k = find(sym, s); CHECK(k && k->insert == s); }
      const Key* d0 = find(sym, "0");   CHECK(d0 && d0->insert == "0");      // digits kept
      const Key* del = find(sym, "Del"); CHECK(del && del->kind == KeyKind::Backspace);
      const Key* abc = find(sym, "ABC"); CHECK(abc && abc->kind == KeyKind::Sym);
      CHECK(find(sym, "?123") == nullptr && find(sym, "q") == nullptr);
      const Key* sp = find(sym, "space"); CHECK(sp && sp->insert == " ");
      const Key* go = find(sym, "Go");    CHECK(go && go->x == 8*U && go->w == W - 8*U); }

    if (fails == 0) std::printf("keyboard_test: OK (%zu keys)\n", keys.size());
    return fails ? 1 : 0;
}
