// Host unit test for the tap-probe protocol + JS string escaping (no Qt, no device):
//   clang++ -std=c++17 -o build/fieldprobe_test tests/fieldprobe_test.cpp && ./build/fieldprobe_test
#include "../engine/wpeqt/fieldprobe.h"
#include <cstdio>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

int main() {
    // link / none / unknown
    CHECK(parseTapProbe("link").hit == TapHit::Link);
    CHECK(parseTapProbe("none").hit == TapHit::None);
    CHECK(parseTapProbe("").hit == TapHit::None);
    CHECK(parseTapProbe("whatever").hit == TapHit::None);      // unexpected JS answer -> safe fallback
    CHECK(parseTapProbe("link\nx").hit == TapHit::None);        // exact match required

    // peek (long-press): everything after the first \n is the href
    CHECK(parseTapProbe("peek\nhttps://ex.com/a").hit == TapHit::Peek);
    CHECK(parseTapProbe("peek\nhttps://ex.com/a").value == "https://ex.com/a");
    CHECK(parseTapProbe("peek\n").value == "");

    // tick: everything after the first \n is the label (labels may contain anything)
    CHECK(parseTapProbe("tick\non").hit == TapHit::Tick);
    CHECK(parseTapProbe("tick\non").value == "on");
    CHECK(parseTapProbe("tick\n").value == "");
    CHECK(parseTapProbe("tick\nmulti\nline").value == "multi\nline");

    // field: flag + hint + value; the value may contain newlines (textarea)
    TapProbe f = parseTapProbe("field\n0\nemail \nhello");
    CHECK(f.hit == TapHit::Field); CHECK(!f.masked); CHECK(f.hint == "email "); CHECK(f.value == "hello");
    TapProbe pw = parseTapProbe("field\n1\npassword\ns3cret");
    CHECK(pw.masked); CHECK(pw.hint == "password"); CHECK(pw.value == "s3cret");
    TapProbe ta = parseTapProbe("field\n0\n\nline1\nline2");
    CHECK(ta.hint.empty()); CHECK(ta.value == "line1\nline2");    // value = after the THIRD \n
    TapProbe empty = parseTapProbe("field\n0\nname\n");
    CHECK(empty.hit == TapHit::Field); CHECK(empty.hint == "name"); CHECK(empty.value.empty());
    TapProbe mal = parseTapProbe("field\nonlyone");
    CHECK(mal.hit == TapHit::Field); CHECK(mal.value.empty());    // malformed: no flag line -> empty value
    TapProbe legacy = parseTapProbe("field\n0\nhello");           // legacy: no hint line -> empty hint
    CHECK(legacy.hit == TapHit::Field); CHECK(legacy.hint.empty()); CHECK(legacy.value == "hello");
    TapProbe wrongflag = parseTapProbe("field\n2\nx\nv");
    CHECK(!wrongflag.masked); CHECK(wrongflag.hint == "x"); CHECK(wrongflag.value == "v");  // only "1" means masked

    // classifyFieldHint (autofill): email / user / name, order matters ("username" has "name")
    CHECK(classifyFieldHint("email", false) == FieldKind::Email);
    CHECK(classifyFieldHint("E-Mail", false) == FieldKind::Email);
    CHECK(classifyFieldHint(" text mailaddr ", false) == FieldKind::Email);
    CHECK(classifyFieldHint("username", false) == FieldKind::User);
    CHECK(classifyFieldHint("login", false) == FieldKind::User);
    CHECK(classifyFieldHint("nickname", false) == FieldKind::User);
    CHECK(classifyFieldHint("name", false) == FieldKind::Name);
    CHECK(classifyFieldHint("given-name fname", false) == FieldKind::Name);
    CHECK(classifyFieldHint("FullName", false) == FieldKind::Name);
    CHECK(classifyFieldHint("search q", false) == FieldKind::None);
    CHECK(classifyFieldHint("", false) == FieldKind::None);
    CHECK(classifyFieldHint("email", true) == FieldKind::None);   // passwords never classify

    // jsStringEscape
    CHECK(jsStringEscape("plain") == "plain");
    CHECK(jsStringEscape("a\"b") == "a\\\"b");
    CHECK(jsStringEscape("a\\b") == "a\\\\b");
    CHECK(jsStringEscape("l1\nl2") == "l1\\nl2");
    CHECK(jsStringEscape("cr\rt") == "cr\\rt");
    CHECK(jsStringEscape("tab\tt") == "tab\\tt");
    CHECK(jsStringEscape("\x01\x1f") == "\\x01\\x1f");          // other control chars -> \xNN
    CHECK(jsStringEscape("');alert(1);//") == "');alert(1);//"); // ' is inert inside a "..." literal
    CHECK(jsStringEscape("кириллица ✓") == "кириллица ✓");      // UTF-8 passes through
    CHECK(jsStringEscape("") == "");

    if (fails == 0) std::printf("fieldprobe_test: OK\n");
    return fails ? 1 : 0;
}
