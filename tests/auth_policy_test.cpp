#include "auth-policy.h"
#include <QtTest>

namespace {
QByteArray authorizationUrl(const QByteArray &callback, const QString &state = QStringLiteral("abcdefghijklmnop1234567890")) {
    return "https://login.example.test:8443/authorize?state=" + QUrl::toPercentEncoding(state)
        + "&redirect_uri=" + QUrl::toPercentEncoding(QString::fromUtf8(callback));
}
}

class AuthPolicyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void onlyExactInternalDocumentsAreRecognizedWithoutBecomingLaunchUrls() {
        const auto launch = rmweb::parseAuthLaunch("https://auth.openai.com/codex/device");
        QVERIFY(launch);
        QCOMPARE(rmweb::authNavigation(*launch, "about:blank"), rmweb::AuthNavigation::LocalBlank);
        QCOMPARE(rmweb::authNavigation(*launch, "about:srcdoc"), rmweb::AuthNavigation::LocalBlank);
        QVERIFY(!rmweb::parseAuthLaunch("about:blank"));
        QVERIFY(!rmweb::parseAuthLaunch("about:srcdoc"));
        for (const QByteArray url : {"about:blank#fragment", "about:blank?query", "ABOUT:blank", "about:blank/",
                 "about:srcdoc#fragment", "about:srcdoc?query", "ABOUT:srcdoc", "about:srcdoc/",
                 "data:text/html,hello", "blob:https://example.com/id", "file:///missing"})
            QCOMPARE(rmweb::authNavigation(*launch, url), rmweb::AuthNavigation::Blocked);
    }
    void appLoadKeysUseNativeKeysymsWithoutFieldCapture() {
        QVERIFY(rmweb::authKey(65));
        QCOMPARE(rmweb::authKey(65)->value, quint32('a'));
        QCOMPARE(rmweb::authKey(65)->code, quint32(0x26));
        QCOMPARE(rmweb::authKey(13)->code, quint32(0x24));
        QCOMPARE(rmweb::authKey(9)->code, quint32(0x17));
        QCOMPARE(rmweb::authKey(27)->code, quint32(0x09));
        QCOMPARE(rmweb::authKey(128)->code, quint32(0x16));
        QCOMPARE(rmweb::authKey(129)->code, quint32(0x70));
        QCOMPARE(rmweb::authKey(33)->code, rmweb::authKey(49)->code);
        QCOMPARE(rmweb::authKey(123)->code, rmweb::authKey(91)->code);
        QCOMPARE(rmweb::authKey(65 | 0x100000)->value, quint32('A'));
        QCOMPARE(rmweb::authKey(33 | 0x100000)->value, quint32('!'));
        QCOMPARE(rmweb::authKey(65 | 0x300000)->modifiers, quint32(3));
        QCOMPARE(rmweb::authKey(65 | 0x400000)->modifiers, quint32(4));
        QCOMPARE(rmweb::authKey(128)->value, quint32(0xff08));
        QCOMPARE(rmweb::authKey(13)->value, quint32(0xff0d));
        QCOMPARE(rmweb::authKey(9)->value, quint32(0xff09));
        QCOMPARE(rmweb::authKey(133)->value, quint32(0xff51));
        QCOMPARE(rmweb::authKey(129)->value, quint32(0xff55));
        QCOMPARE(rmweb::authKey(130)->value, quint32(0xff56));
        for (int raw : {0, -1, 97, 122, 137, 0x800000, 0x100000, 0x700000})
            QVERIFY(!rmweb::authKey(raw));
    }
    void keyReleaseUsesTheOriginalPressEvenAfterModifierChanges() {
        rmweb::AuthKeyboard keyboard;
        const auto down = keyboard.event(65 | 0x100000, true);
        QCOMPARE(down.size(), 1);
        QVERIFY(down[0].pressed);
        QCOMPARE(down[0].key.value, quint32('A'));
        QVERIFY(keyboard.event(65 | 0x100000, true).isEmpty());
        const auto up = keyboard.event(65, false);
        QCOMPARE(up.size(), 1);
        QVERIFY(!up[0].pressed);
        QCOMPARE(up[0].key.value, quint32('A'));
        QVERIFY(keyboard.cancel().isEmpty());
        // Actual AppLoad layout: shifted 1 is + (not US Shift+1 = !).
        QCOMPARE(keyboard.event(43 | 0x100000, true).size(), 1);
        const auto changedSymbol = keyboard.event(49, false);
        QCOMPARE(changedSymbol.size(), 1);
        QCOMPARE(changedSymbol[0].key.value, quint32('+'));
        QVERIFY(!changedSymbol[0].pressed);
        QCOMPARE(keyboard.event(66, true).size(), 1);
        const auto cancelled = keyboard.cancel();
        QCOMPARE(cancelled.size(), 1);
        QVERIFY(!cancelled[0].pressed);
        QVERIFY(keyboard.event(66, false).isEmpty());
        QVERIFY(keyboard.event(0x100000, true).isEmpty());
        // Dedicated ! and 1 buttons must not be collapsed just because their
        // best-effort DOM physical-code mapping is the same US key.
        QCOMPARE(keyboard.event(33, true).size(), 1);
        QCOMPARE(keyboard.event(49, true).size(), 1);
        QCOMPARE(keyboard.cancel().size(), 2);
    }
    void enrollmentCallbackIsBoundToTheIssuedStateAndLoopback() {
        const QByteArray request = "https://auth.openai.com/oauth/authorize?response_type=code&state=abcdefghijklmnop1234567890&redirect_uri=http%3A%2F%2Flocalhost%3A1457%2Fauth%2Fcallback";
        const auto launch = rmweb::parseAuthLaunch(request);
        QVERIFY(launch);
        QCOMPARE(launch->callbackUrl, QUrl("http://localhost:1457/auth/callback"));
        QCOMPARE(rmweb::authNavigation(*launch, "http://localhost:1457/auth/callback?state=abcdefghijklmnop1234567890&code=one-time-code"), rmweb::AuthNavigation::Callback);
        for (const QByteArray url : {
                 "http://localhost:1455/auth/callback?state=abcdefghijklmnop1234567890&code=code",
                 "http://127.0.0.1:1457/auth/callback?state=abcdefghijklmnop1234567890&code=code",
                 "http://localhost:1457/auth/callback?state=wrong&code=code",
                 "http://localhost:1457/auth/callback?state=abcdefghijklmnop1234567890&state=wrong&code=code",
                 "http://localhost:1457/other?state=abcdefghijklmnop1234567890&code=code",
                 "http://localhost:1457/auth/callback?state=abcdefghijklmnop1234567890&code=code#fragment",
                 "http://example.com/auth/callback?state=abcdefghijklmnop1234567890&code=code",
                 "file:///etc/passwd", "javascript:alert(1)", "rmweb:tls-continue"})
            QCOMPARE(rmweb::authNavigation(*launch, url), rmweb::AuthNavigation::Blocked);
        QCOMPARE(rmweb::authNavigation(*launch, "https://accounts.example.org/sign-in"), rmweb::AuthNavigation::Web);
    }
    void genericHttpsLaunches_data() {
        QTest::addColumn<QByteArray>("url");
        for (const QByteArray url : {"https://auth.openai.com/codex/device",
                 "https://auth.openai.com:443/codex/device", "https://example.test",
                 "https://example.test:8443/passkeys?method=phone",
                 "https://another.example.test/any/path", "https://example.test/a%20path"})
            QTest::newRow(url.constData()) << url;
    }
    void genericHttpsLaunches() {
        QFETCH(QByteArray, url);
        const auto launch = rmweb::parseAuthLaunch(url);
        QVERIFY(launch);
        QCOMPARE(launch->initialUrl, QUrl::fromEncoded(url));
        QVERIFY(launch->callbackUrl.isEmpty());
        QVERIFY(launch->state.isEmpty());
        QCOMPARE(rmweb::authNavigation(*launch, "http://localhost:1455/auth/callback?code=test&state=test"), rmweb::AuthNavigation::Blocked);
    }
    void initialRequestsRejectMalformedDestinations_data() {
        QTest::addColumn<QByteArray>("url");
        for (const QByteArray url : {
                 "", "http://example.test/sign-in", "https://", "https:///path",
                 "https://example.test:65536/", "https://example.test:port/",
                 "https://user@example.test/sign-in", "https://@example.test/sign-in",
                 "https://example.test/sign-in#fragment", "https://example.test/sign-in#",
                 "https://example.test/sign-in\n", "https://example.test/a path",
                 "https://auth.openai.com/oauth/authorize?state=short&redirect_uri=http://localhost:1455/auth/callback",
                 "https://auth.openai.com/oauth/authorize?state=abcdefghijklmnop&redirect_uri=https://elsewhere.example/callback",
                 "https://auth.openai.com/oauth/authorize?state=abcdefghijklmnop&state=abcdefghijklmnop&redirect_uri=http://localhost:1455/auth/callback",
                 "https://auth.openai.com/oauth/authorize?state=abcdefghijklmnop&redirect_uri=http://localhost:1455/auth/callback&redirect_uri=http://localhost:1455/auth/callback",
                 "https://example.test/authorize?state=abcdefghijklmnop",
                 "https://example.test/authorize?redirect_uri=http://localhost:1455/auth/callback",
                 "https://example.test/authorize?state=&redirect_uri=",
                 "https://example.test/authorize?state=abcdefghijklmnop&redirect_uri=http://localhost:1455/auth/callback&%73tate=abcdefghijklmnop"})
            QTest::newRow(url.constData()) << url;
        QTest::newRow("oversized") << (QByteArray("https://example.test/authorize?") + QByteArray(8200, 'a'));
        QTest::newRow("oversized-state") << authorizationUrl("http://localhost:1455/auth/callback", QString(257, 'a'));
        QTest::newRow("invalid-state") << authorizationUrl("http://localhost:1455/auth/callback", QStringLiteral("abcdefghijklmno!"));
    }
    void initialRequestsRejectMalformedDestinations() {
        QFETCH(QByteArray, url);
        QVERIFY(!rmweb::parseAuthLaunch(url));
    }
    void genericLoopbackCallbacks_data() {
        QTest::addColumn<QByteArray>("callback");
        for (const QByteArray callback : {"http://localhost:1024/callback", "http://localhost:9999/finish",
                 "http://127.0.0.1:49152/oauth/return", "http://[::1]:65535/callback"})
            QTest::newRow(callback.constData()) << callback;
    }
    void genericLoopbackCallbacks() {
        QFETCH(QByteArray, callback);
        const auto launch = rmweb::parseAuthLaunch(authorizationUrl(callback));
        QVERIFY(launch);
        QCOMPARE(launch->callbackUrl.toEncoded(), callback);
        const auto response = callback + "?state=abcdefghijklmnop1234567890";
        QCOMPARE(rmweb::authNavigation(*launch, response + "&code=one-time-code"), rmweb::AuthNavigation::Callback);
        QCOMPARE(rmweb::authNavigation(*launch, response + "&error=access_denied"), rmweb::AuthNavigation::Callback);
        for (const QByteArray query : {"", "&code=", "&error=", "&code=a&code=b", "&error=a&error=b",
                 "&code=a&error=b", "&code=%0A", "&code=a&state=wrong", "&code=a#fragment"})
            QCOMPARE(rmweb::authNavigation(*launch, response + query), rmweb::AuthNavigation::Blocked);
        QVERIFY(!rmweb::parseAuthLaunch(authorizationUrl(callback), QStringLiteral("ABCD-1234")));
    }
    void callbackEndpointsRejectAmbiguousOrNonloopbackUrls_data() {
        QTest::addColumn<QByteArray>("callback");
        for (const QByteArray callback : {"http://localhost/callback", "http://localhost:80/callback",
                 "http://localhost:0/callback", "http://localhost:1023/callback", "http://localhost:65536/callback",
                 "https://localhost:1455/callback", "http://example.test:1455/callback",
                 "http://localhost.example.test:1455/callback", "http://127.0.0.2:1455/callback",
                 "http://127.1:1455/callback", "http://2130706433:1455/callback",
                 "http://[::ffff:127.0.0.1]:1455/callback", "http://user@localhost:1455/callback",
                 "http://@localhost:1455/callback", "http://LOCALHOST:1455/callback",
                 "http://localhost.:1455/callback", "http://localhost:01455/callback",
                 "HTTP://localhost:1455/callback", "http://localhost:1455",
                 "http://localhost:1455/callback?", "http://localhost:1455/callback?x=1",
                 "http://localhost:1455/callback#", "http://localhost:1455/callback#fragment",
                 "http://localhost:1455/%63allback", "http://localhost:1455/a%2Fcallback",
                 "http://localhost:1455/%252Fcallback", "http://localhost:1455/a/../callback",
                 "http://localhost:1455/./callback", "http://localhost:1455/callback\n",
                 "http%3A%2F%2Flocalhost%3A1455%2Fcallback"})
            QTest::newRow(callback.constData()) << callback;
    }
    void callbackEndpointsRejectAmbiguousOrNonloopbackUrls() {
        QFETCH(QByteArray, callback);
        QVERIFY(!rmweb::parseAuthLaunch(authorizationUrl(callback)));
    }
    void callbackNavigationRequiresTheExactEndpointSpelling() {
        const auto launch = rmweb::parseAuthLaunch(authorizationUrl("http://localhost:49152/callback"));
        QVERIFY(launch);
        for (const QByteArray endpoint : {"http://LOCALHOST:49152/callback", "http://localhost:049152/callback",
                 "http://localhost:49152/%63allback", "http://localhost:49152/a/../callback",
                 "http://127.0.0.1:49152/callback", "http://localhost:49153/callback",
                 "http://localhost:49152/another"})
            QCOMPARE(rmweb::authNavigation(*launch, endpoint + "?state=abcdefghijklmnop1234567890&code=code"), rmweb::AuthNavigation::Blocked);
    }
    void publicDeviceCodeStaysSeparateFromEnrollment() {
        const auto launch = rmweb::parseAuthLaunch("https://auth.openai.com/codex/device", "ABCD-12345");
        QVERIFY(launch);
        QCOMPARE(launch->deviceCode, QString("ABCD-12345"));
        QVERIFY(!rmweb::parseAuthLaunch("https://auth.openai.com/codex/device", "INVALID\nCODE"));
        QVERIFY(!rmweb::parseAuthLaunch("https://auth.openai.com/codex/device", QString(65, 'A')));
        QVERIFY(!rmweb::parseAuthLaunch("https://auth.openai.com/oauth/authorize?state=abcdefghijklmnop&redirect_uri=http://localhost:1455/auth/callback", "CODE"));
        const auto generic = rmweb::parseAuthLaunch("https://example.test:8443/link-device?method=code", "ABCD-12345");
        QVERIFY(generic);
        QCOMPARE(generic->deviceCode, QString("ABCD-12345"));
        QVERIFY(!rmweb::parseAuthLaunch("https://example.test/link-device", "lowercase"));
    }
    void openAiDeviceSignInRemainsAnOrdinaryHttpsLaunch() {
        const auto launch = rmweb::parseAuthLaunch("https://auth.openai.com/codex/device");
        QVERIFY(launch);
        QVERIFY(launch->callbackUrl.isEmpty());
        QCOMPARE(rmweb::authNavigation(*launch, "https://auth.openai.com/log-in"), rmweb::AuthNavigation::Web);
        QCOMPARE(rmweb::authNavigation(*launch, "http://localhost:1455/auth/callback?code=test&state=test"), rmweb::AuthNavigation::Blocked);
    }
};
QTEST_GUILESS_MAIN(AuthPolicyTest)
#include "auth_policy_test.moc"
