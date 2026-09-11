#pragma once
// Register a downloaded PDF/EPUB in the xochitl (reMarkable) library, so the document shows up in
// the file manager once xochitl restarts (rmweb exits). A xochitl document is a set of files with a
// shared lowercase UUIDv4 stem in the store dir: <uuid>.<ext> (the payload), <uuid>.metadata (JSON,
// what the library lists) and <uuid>.content (JSON reading state — a dummy is fine, xochitl rebuilds
// the rest: .pagedata, thumbnails). Format per community docs; the store dir is overridable via
// RMWEB_XOCHITL_DIR (tests). Pure std/POSIX (no Qt/WebKit/glib) -> host-unit-testable
// (tests/library_test.cpp); reuses profile.h's sanitizeField + detail::atomicWrite.
#include <string>
#include <cstdio>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <chrono>
#include <random>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "profile.h"   // rmweb::sanitizeField, rmweb::detail::atomicWrite

namespace rmweb {

// The xochitl library store; RMWEB_XOCHITL_DIR overrides (used by the tests).
inline std::string xochitlDir() {
    const char *e = std::getenv("RMWEB_XOCHITL_DIR");
    return (e && *e) ? std::string(e) : std::string("/home/root/.local/share/remarkable/xochitl");
}

// Lowercased extension of a path ("" when there is no dot).
inline std::string lowerExt(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot + 1);
    for (auto &c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    return e;
}
// A download worth importing into the library (the e-ink reader formats).
inline bool isDocumentFile(const std::string &path) {
    const std::string e = lowerExt(path);
    return e == "pdf" || e == "epub";
}

// Minimal JSON string escaping (quotes, backslash, control chars) — the project has htmlEscape for
// markup but no JSON escaper, so this tiny local one covers the single interpolated JSON value.
inline std::string jsonEscape(const std::string &s) {
    std::string o; o.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += char(c);
        }
    }
    return o;
}

// RFC 4122 UUIDv4 (8-4-4-4-12, lowercase hex, version/variant bits set) — the xochitl document id.
inline std::string makeUuidV4() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned> nib(0, 15);
    static const char *hex = "0123456789abcdef";
    std::string u; u.reserve(36);
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { u += '-'; continue; }
        unsigned n = nib(gen);
        if (i == 14)      n = 4;                  // version 4
        else if (i == 19) n = (n & 0x3) | 0x8;    // variant 10xx
        u += hex[n];
    }
    return u;
}

namespace detail {
// mkdir -p, POSIX-only (profile.h's atomicWrite does not create parent dirs; glib is off-limits here).
inline bool mkdirs(const std::string &dir, std::string *err) {
    std::string cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] != '/' && i + 1 != dir.size()) continue;
        if (cur.empty() || cur == "/") continue;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            if (err) *err = "mkdir " + cur + " failed: " + std::strerror(errno);
            return false;
        }
    }
    return true;
}
// Chunked binary copy src -> dst with the same atomicity model as atomicWrite (tmp + fsync +
// rename, byte-count verified). The payload can be tens of MB, so it is NOT slurped into memory.
inline bool copyFileAtomic(const std::string &dst, const std::string &src, std::string *err) {
    const std::string tmp = dst + ".tmp";
    const int in = open(src.c_str(), O_RDONLY);
    if (in < 0) { if (err) *err = "open " + src + " failed: " + std::strerror(errno); return false; }
    struct stat st {};
    const bool haveSize = fstat(in, &st) == 0;
    const int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        if (err) *err = "open " + tmp + " failed: " + std::strerror(errno);
        close(in);
        return false;
    }
    bool ok = true;
    long long total = 0;
    char buf[1 << 16];
    for (;;) {
        const ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        for (ssize_t off = 0; off < n;) {                 // write(2) may be partial
            const ssize_t w = write(out, buf + off, static_cast<size_t>(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
        total += n;
    }
    const int savedErr = errno;
    if (ok && fsync(out) != 0) ok = false;
    if (close(out) != 0 && ok) ok = false;
    close(in);
    if (ok && haveSize && total != static_cast<long long>(st.st_size)) ok = false;   // truncated copy
    if (!ok) {
        if (err) *err = "copy " + src + " -> " + dst + " failed: " + std::strerror(savedErr);
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), dst.c_str()) != 0) {
        if (err) *err = "rename " + tmp + " -> " + dst + " failed: " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}
} // namespace detail

// Import srcPath (a finished download) into the xochitl store at xochitlDir. Returns false with a
// human-readable *err on any failure (no exceptions escape); on a mid-way failure every file it
// already wrote is unlinked again, so a retry starts clean. Each import mints a fresh UUID, so it
// never clashes with (or overwrites) an existing library document.
inline bool importDocument(const std::string &xochitlDir, const std::string &srcPath,
                           const std::string &visibleName, std::string *err) {
    auto fail = [err](const std::string &m) { if (err) *err = m; return false; };
    if (!isDocumentFile(srcPath)) return fail("not a pdf/epub: " + srcPath);
    const std::string ext = lowerExt(srcPath);
    struct stat st {};
    if (stat(srcPath.c_str(), &st) != 0) return fail("source not readable: " + srcPath);
    if (!detail::mkdirs(xochitlDir, err)) return false;

    const std::string uuid = makeUuidV4();
    const std::string base = xochitlDir + "/" + uuid;
    const std::string payload = base + "." + ext;
    const std::string contentF = base + ".content";
    const std::string metadataF = base + ".metadata";
    auto cleanup = [&] { std::remove(payload.c_str()); std::remove(contentF.c_str()); std::remove(metadataF.c_str()); };

    if (!detail::copyFileAtomic(payload, srcPath, err)) return fail(err ? *err : "copy failed");

    // visibleName: no control chars (profile.h sanitize), capped (a title bar is narrow), never empty.
    std::string name = sanitizeField(visibleName);
    if (name.size() > 120) name.resize(120);
    if (name.empty()) name = "download";

    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string ms = std::to_string(nowMs);
    // Reading-state dummy — xochitl tolerates it and fills in the real per-document state itself.
    const std::string content =
        "{\"coverPageNumber\":0,\"documentMetadata\":{},\"extraMetadata\":{},\"fileType\":\"" + ext +
        "\",\"fontName\":\"\",\"pageTags\":[],\"tags\":[],\"textAlignment\":\"justify\",\"textScale\":1,"
        "\"zoomMode\":\"bestFit\"}\n";
    if (!detail::atomicWrite(contentF, content)) { cleanup(); return fail("write " + contentF + " failed"); }
    // Written LAST: the .metadata file is what makes xochitl list the document.
    const std::string metadata =
        "{\"createdTime\":\"" + ms + "\",\"lastModified\":\"" + ms + "\",\"lastOpened\":\"0\","
        "\"lastOpenedPage\":0,\"parent\":\"\",\"pinned\":false,\"type\":\"DocumentType\","
        "\"visibleName\":\"" + jsonEscape(name) + "\"}\n";
    if (!detail::atomicWrite(metadataF, metadata)) { cleanup(); return fail("write " + metadataF + " failed"); }
    return true;
}

} // namespace rmweb
