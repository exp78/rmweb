#pragma once
#include <QByteArray>
#include <QRegularExpression>
#include <QUrl>
namespace acceptance {
inline bool validOrigin(const QByteArray &origin) {
    const QUrl url = QUrl::fromEncoded(origin, QUrl::StrictMode);
    static const QRegularExpression dns(QStringLiteral("\\A[a-z0-9]+(?:[.-][a-z0-9]+)*\\z"));
    static const QRegularExpression numeric(QStringLiteral("\\A[0-9.]+\\z"));
    return origin.size() <= 300 && url.isValid() && url.scheme() == QStringLiteral("https")
        && url.host().size() <= 253 && url.host().contains('.') && dns.match(url.host()).hasMatch()
        && !numeric.match(url.host()).hasMatch() && url.userInfo().isEmpty() && url.path().isEmpty()
        && url.port() != 443 && url.port() != 0 && !url.hasQuery() && !url.hasFragment() && url.toEncoded(QUrl::FullyEncoded) == origin;
}
inline bool allowed(const QByteArray &origin, const QByteArray &url) {
    if (!validOrigin(origin)) return false;
    if (url == origin + "/verify") return true;
    const QByteArray prefix = origin + "/verify#join=";
    if (!url.startsWith(prefix)) return false;
    const QByteArray code = url.mid(prefix.size());
    if (code.size() < 32 || code.size() > 128) return false;
    for (const char c : code)
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z')
            && !(c >= '0' && c <= '9') && c != '_' && c != '-') return false;
    return true;
}
}
