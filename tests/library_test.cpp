#include "../engine/wpeqt/library.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include <dirent.h>
using namespace rmweb;

static int fails = 0;
#define CHECK(c) do { if(!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while(0)

// Unique temp dir per run (mkdtemp) + RAII cleanup: no stale state between runs and
// no race when two test binaries run in parallel. mkdtemp's charset is shell-safe.
struct TmpDir {
    std::string path;
    TmpDir() {
        char t[] = "/tmp/rmweb-library-test-XXXXXX";
        const char* d = mkdtemp(t);
        if (d) path = d;
    }
    ~TmpDir() {
        if (!path.empty()) {
            const std::string cmd = "rm -rf " + path;
            (void)std::system(cmd.c_str());
        }
    }
};

static std::string slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::vector<std::string> listDir(const std::string &dir) {
    std::vector<std::string> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    while (struct dirent *e = readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    closedir(d);
    return out;
}
// RFC 4122 UUIDv4 shape: 8-4-4-4-12 lowercase hex, version nibble 4, variant 8/9/a/b.
static bool isUuidV4(const std::string &u) {
    if (u.size() != 36) return false;
    for (size_t i = 0; i < u.size(); ++i) {
        const char c = u[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; continue; }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return u[14] == '4' && (u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
}
// The one UUID stem shared by all files currently in the store ("" if none/many).
static std::string onlyStem(const std::string &dir) {
    std::set<std::string> stems;
    for (const auto &n : listDir(dir)) {
        const size_t dot = n.find_last_of('.');
        if (dot == std::string::npos) continue;
        stems.insert(n.substr(0, dot));
    }
    return stems.size() == 1 ? *stems.begin() : std::string();
}

int main() {
    TmpDir tmp;
    CHECK(!tmp.path.empty());
    if (tmp.path.empty()) return 1;   // mkdtemp failed — do not touch the real cwd
    const std::string xo = tmp.path + "/xochitl";      // the (fake) xochitl store
    const std::string dl = tmp.path + "/downloads";    // fake download source

    // extension helpers
    CHECK(lowerExt("a/b/c.PDF") == "pdf");
    CHECK(lowerExt("noext").empty());
    CHECK(isDocumentFile("x.epub") && isDocumentFile("x.Pdf") && !isDocumentFile("x.txt"));

    // xochitlDir(): env override wins, default otherwise
    setenv("RMWEB_XOCHITL_DIR", "/tmp/somewhere-else", 1);
    CHECK(xochitlDir() == "/tmp/somewhere-else");
    unsetenv("RMWEB_XOCHITL_DIR");
    CHECK(xochitlDir() == "/home/root/.local/share/remarkable/xochitl");

    // fake download
    const std::string pdfBytes = "%PDF-1.4\n% fake pdf body \x01\x02\x03\n%%EOF\n";
    CHECK(detail::atomicWrite(dl + "/paper.pdf", pdfBytes) == false);   // parent missing -> fails (no mkdir there)
    CHECK(detail::mkdirs(dl, nullptr));
    CHECK(detail::atomicWrite(dl + "/paper.pdf", pdfBytes));

    // import a pdf
    std::string err;
    CHECK(importDocument(xo, dl + "/paper.pdf", "My Book", &err));
    CHECK(err.empty());
    const std::string uuid = onlyStem(xo);
    CHECK(isUuidV4(uuid));
    {   // exactly uuid.pdf + uuid.metadata + uuid.content, no leftovers
        std::set<std::string> got;
        for (const auto &n : listDir(xo)) { CHECK(n.find(".tmp") == std::string::npos); got.insert(n); }
        CHECK(got.count(uuid + ".pdf") && got.count(uuid + ".metadata") && got.count(uuid + ".content"));
        CHECK(got.size() == 3);
    }
    // payload is a byte-identical copy
    CHECK(slurp(xo + "/" + uuid + ".pdf") == pdfBytes);
    // metadata: minimal xochitl set, our visibleName
    const std::string md = slurp(xo + "/" + uuid + ".metadata");
    CHECK(md.find("\"type\":\"DocumentType\"") != std::string::npos);
    CHECK(md.find("\"visibleName\":\"My Book\"") != std::string::npos);
    CHECK(md.find("\"lastOpened\":\"0\"") != std::string::npos);
    CHECK(md.find("\"createdTime\":\"") != std::string::npos);
    CHECK(md.find("\"lastModified\":\"") != std::string::npos);
    // content: dummy reading state with the right fileType
    const std::string ct = slurp(xo + "/" + uuid + ".content");
    CHECK(ct.find("\"fileType\":\"pdf\"") != std::string::npos);

    // epub import + default/escaped names
    CHECK(detail::atomicWrite(dl + "/story.epub", "PK\x03\x04 fake epub\n"));
    CHECK(importDocument(xo, dl + "/story.epub", "", &err));            // empty name -> "download"
    {   // now two documents; find the new stem (the one that is not `uuid`)
        std::set<std::string> stems;
        for (const auto &n : listDir(xo)) stems.insert(n.substr(0, n.find_last_of('.')));
        CHECK(stems.size() == 2);
        stems.erase(uuid);
        const std::string u2 = *stems.begin();
        CHECK(isUuidV4(u2));
        const std::string md2 = slurp(xo + "/" + u2 + ".metadata");
        CHECK(md2.find("\"visibleName\":\"download\"") != std::string::npos);
        CHECK(slurp(xo + "/" + u2 + ".content").find("\"fileType\":\"epub\"") != std::string::npos);
    }

    // visibleName JSON escaping: quotes and backslashes must not break the JSON
    CHECK(importDocument(xo, dl + "/paper.pdf", "A \"quoted\" \\book\\", &err));
    {   // third document now; its stem is the one we haven't seen
        std::set<std::string> stems;
        for (const auto &n : listDir(xo)) stems.insert(n.substr(0, n.find_last_of('.')));
        CHECK(stems.size() == 3);
        std::string u3;
        for (const auto &s : stems) if (s != uuid && isUuidV4(s)) {
            const std::string m3 = slurp(xo + "/" + s + ".metadata");
            if (m3.find("quoted") != std::string::npos) u3 = s;
        }
        CHECK(!u3.empty());
        const std::string md3 = slurp(xo + "/" + u3 + ".metadata");
        CHECK(md3.find("\"visibleName\":\"A \\\"quoted\\\" \\\\book\\\\\"") != std::string::npos);
    }

    // visibleName capped at ~120 chars
    CHECK(importDocument(xo, dl + "/paper.pdf", std::string(200, 'a'), &err));
    {   // the long-named document exists somewhere among the stems
        bool found = false;
        for (const auto &n : listDir(xo)) {
            if (n.size() < 10 || n.substr(n.size() - 9) != ".metadata") continue;
            const std::string m = slurp(xo + "/" + n);
            if (m.find(std::string(120, 'a') + "\"") != std::string::npos) found = true;
            CHECK(m.find(std::string(121, 'a')) == std::string::npos);   // nowhere longer than 120
        }
        CHECK(found);
    }

    // rejections: not a document, missing source
    CHECK(detail::atomicWrite(dl + "/notes.txt", "plain text\n"));
    err.clear();
    CHECK(!importDocument(xo, dl + "/notes.txt", "Notes", &err));
    CHECK(err.find("not a pdf/epub") != std::string::npos);
    err.clear();
    CHECK(!importDocument(xo, dl + "/no-such.pdf", "Ghost", &err));
    CHECK(!err.empty());

    if (fails == 0) std::printf("library_test: OK\n");
    return fails ? 1 : 0;
}
