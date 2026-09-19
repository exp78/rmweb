#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QMap>
#include <QVector>
#include <optional>

namespace rmweb {
struct AuthLaunch {
    QUrl initialUrl;
    QUrl callbackUrl;
    QString state;
    QString deviceCode;
};
enum class AuthNavigation { Blocked, Web, Callback, LocalBlank };
// The caller supplies an HTTPS URL and owns authorization state and completion.
// A redirect_uri/state pair opts into a canonical HTTP loopback callback;
// without that pair, HTTP navigation is never allowed. Device codes are display
// hints for ordinary HTTPS launches, separate from callback authorization.
std::optional<AuthLaunch> parseAuthLaunch(const QByteArray &url, const QString &deviceCode = {});
// LocalBlank covers only exact about:blank/about:srcdoc child documents; the
// driver rejects either top-level commit. Neither proves OAuth/enrollment success.
AuthNavigation authNavigation(const AuthLaunch &launch, const QByteArray &url);

struct AuthKey {
    quint32 code;
    quint32 value; // XKB/WPE keysym, not a Qt key enum.
    quint32 modifiers; // WPE: control=1, shift=2, alt=4.
};
// Exact AppLoad v0.5.3 default.layout.json codes; modifier-only packets have
// no character. Never reads or stores a form field or synthesizes DOM mutations.
std::optional<AuthKey> authKey(int apploadCode);
struct AuthKeyEvent { AuthKey key; bool pressed; };
class AuthKeyboard {
public:
    QVector<AuthKeyEvent> event(int apploadCode, bool pressed);
    QVector<AuthKeyEvent> cancel();
private:
    QMap<quint32, AuthKey> m_pressed;
};
} // namespace rmweb
