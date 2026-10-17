#include "auth-policy.h"
#include <QUrlQuery>

namespace rmweb {
namespace {
std::optional<QUrl> webUrl(const QByteArray &encoded, int limit) {
    if (encoded.isEmpty() || encoded.size() > limit) return {};
    for (const unsigned char c : encoded) if (c <= 0x20 || c == 0x7f) return {};
    const QUrl url = QUrl::fromEncoded(encoded, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.authority().contains('@')) return {};
    return url;
}
std::optional<QUrl> loopbackCallback(const QString &address) {
    const auto encoded = address.toUtf8();
    const auto url = webUrl(encoded, 8192);
    if (!url || url->scheme() != "http" || url->port() < 1024 || url->port() > 65535
        || (url->host() != "localhost" && url->host() != "127.0.0.1" && url->host() != "::1")
        || !url->path().startsWith('/') || url->hasQuery() || url->hasFragment()) return {};
    // The query value has already been decoded once. Require a literal endpoint
    // so a second decoder, encoded separator, or normalized path cannot change
    // the caller's callback target. The outer redirect_uri may be URL-encoded.
    if (encoded.contains('%') || url->toEncoded(QUrl::FullyEncoded) != encoded
        || url->adjusted(QUrl::NormalizePathSegments).toEncoded(QUrl::FullyEncoded) != encoded) return {};
    return url;
}
}
std::optional<AuthLaunch> parseAuthLaunch(const QByteArray &encoded, const QString &deviceCode) {
    const auto url = webUrl(encoded, 8192);
    if (!url || url->scheme() != "https" || url->hasFragment()) return {};
    if (deviceCode.size() > 64) return {};
    for (const QChar c : deviceCode) {
        if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '-') return {};
    }
    const QUrlQuery query(*url);
    const auto states = query.allQueryItemValues("state", QUrl::FullyDecoded);
    const auto redirects = query.allQueryItemValues("redirect_uri", QUrl::FullyDecoded);
    if (states.isEmpty() && redirects.isEmpty()) return AuthLaunch{*url, {}, {}, deviceCode};
    if (!deviceCode.isEmpty() || states.size() != 1 || states.front().size() < 16 || states.front().size() > 256
        || redirects.size() != 1) return {};
    for (const QChar c : states.front()) {
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')
            && !(c >= '0' && c <= '9') && c != '-' && c != '_') return {};
    }
    const auto callback = loopbackCallback(redirects.front());
    if (!callback) return {};
    return AuthLaunch{*url, *callback, states.front(), {}};
}
AuthNavigation authNavigation(const AuthLaunch &launch, const QByteArray &encoded) {
    if (encoded == "about:blank" || encoded == "about:srcdoc") return AuthNavigation::LocalBlank;
    const auto url = webUrl(encoded, 16 * 1024);
    if (!url) return AuthNavigation::Blocked;
    if (url->scheme() == "https") return AuthNavigation::Web;
    if (launch.callbackUrl.isEmpty() || url->hasFragment()
        || url->adjusted(QUrl::RemoveQuery) != launch.callbackUrl
        || encoded.left(encoded.indexOf('?')) != launch.callbackUrl.toEncoded(QUrl::FullyEncoded)) return AuthNavigation::Blocked;
    const QUrlQuery query(*url);
    if (query.allQueryItemValues("state", QUrl::FullyDecoded) != QStringList{launch.state})
        return AuthNavigation::Blocked;
    const auto codes = query.allQueryItemValues("code", QUrl::FullyDecoded);
    const auto errors = query.allQueryItemValues("error", QUrl::FullyDecoded);
    // A denied authorization still belongs to the caller's callback handler. Neither
    // callback variant is treated here as proof of successful authentication.
    const auto values = errors.isEmpty() ? codes : errors;
    if ((!codes.isEmpty() && !errors.isEmpty()) || values.size() != 1
        || values.front().isEmpty() || values.front().size() > 8192) return AuthNavigation::Blocked;
    for (const QChar c : values.front()) if (c.isNull() || c.isLowSurrogate() || c.isHighSurrogate() || c.category() == QChar::Other_Control)
        return AuthNavigation::Blocked;
    return AuthNavigation::Callback;
}
std::optional<AuthKey> authKey(int raw) {
    if (raw < 0 || (raw & ~0x7000ff)) return {};
    const int base = raw & 0xfffff;
    quint32 value = 0;
    if ((base >= 32 && base <= 96) || (base >= 123 && base <= 126)) {
        value = quint32(base);
        if (base >= 'A' && base <= 'Z' && !(raw & 0x100000)) value += 'a' - 'A';
    } else {
        switch (base) {
            case 9: value = 0xff09; break;  // Tab
            case 13: value = 0xff0d; break; // Return
            case 27: value = 0xff1b; break; // Escape
            case 127: value = 0xffff; break;
            case 128: value = 0xff08; break; // Layout Backspace, not common.h PGUP.
            case 129: value = 0xff55; break;
            case 130: value = 0xff56; break;
            case 131: value = 0xff54; break;
            case 132: value = 0xff52; break;
            case 133: value = 0xff51; break;
            case 134: value = 0xff53; break;
            case 135: value = 0xff50; break;
            case 136: value = 0xff57; break;
            default: return {};
        }
    }
    const quint32 modifiers = ((raw & 0x200000) ? 1U : 0U)
        | ((raw & 0x100000) ? 2U : 0U) | ((raw & 0x400000) ? 4U : 0U);
    // WPE 2.48 uses XKB physical keycodes (evdev + 8) for DOM event.code.
    // AppLoad sends its own character/layout IDs, so forwarding base directly
    // would label Tab as Escape and Return as Digit4.
    quint32 code = 0;
    if (base >= 'A' && base <= 'Z') {
        static constexpr quint32 letters[] = {
            0x26, 0x38, 0x36, 0x28, 0x1a, 0x29, 0x2a, 0x2b, 0x1f,
            0x2c, 0x2d, 0x2e, 0x3a, 0x39, 0x20, 0x21, 0x18, 0x1b,
            0x27, 0x1c, 0x1e, 0x37, 0x19, 0x35, 0x1d, 0x34
        };
        code = letters[base - 'A'];
    } else if (base >= '1' && base <= '9') code = 0x0a + base - '1';
    else switch (base) {
        case '0': case ')': code = 0x13; break;
        case '!': code = 0x0a; break;
        case '@': code = 0x0b; break;
        case '#': code = 0x0c; break;
        case '$': code = 0x0d; break;
        case '%': code = 0x0e; break;
        case '^': code = 0x0f; break;
        case '&': code = 0x10; break;
        case '*': code = 0x11; break;
        case '(': code = 0x12; break;
        case '-': case '_': code = 0x14; break;
        case '=': case '+': code = 0x15; break;
        case '[': case '{': code = 0x22; break;
        case ']': case '}': code = 0x23; break;
        case ';': case ':': code = 0x2f; break;
        case '\'': case '"': code = 0x30; break;
        case '`': case '~': code = 0x31; break;
        case '\\': case '|': code = 0x33; break;
        case ',': case '<': code = 0x3b; break;
        case '.': case '>': code = 0x3c; break;
        case '/': case '?': code = 0x3d; break;
        case ' ': code = 0x41; break;
        case 9: code = 0x17; break;
        case 13: code = 0x24; break;
        case 27: code = 0x09; break;
        case 127: code = 0x77; break;
        case 128: code = 0x16; break;
        case 129: code = 0x70; break;
        case 130: code = 0x75; break;
        case 131: code = 0x74; break;
        case 132: code = 0x6f; break;
        case 133: code = 0x71; break;
        case 134: code = 0x72; break;
        case 135: code = 0x6e; break;
        case 136: code = 0x73; break;
    }
    return AuthKey{code, value, modifiers};
}
QVector<AuthKeyEvent> AuthKeyboard::event(int raw, bool pressed) {
    const auto key = authKey(raw);
    const auto identity = quint32(raw & 0xfffff);
    if (!pressed) {
        // AppLoad recomputes modifiers/alternate symbols at release. Keep the
        // original key identity; a lost/changed release cancels owned keys.
        if (!key || !m_pressed.contains(identity)) return cancel();
        const auto original = m_pressed.take(identity);
        return {{original, false}};
    }
    if (!key || m_pressed.contains(identity)) return {};
    if (m_pressed.size() >= 16) return cancel();
    m_pressed.insert(identity, *key);
    return {{*key, true}};
}
QVector<AuthKeyEvent> AuthKeyboard::cancel() {
    QVector<AuthKeyEvent> result;
    for (const auto &key : m_pressed) result.append({key, false});
    m_pressed.clear();
    return result;
}
} // namespace rmweb
