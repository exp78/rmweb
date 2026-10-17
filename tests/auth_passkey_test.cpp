#include "auth-passkey.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QFileInfo>
#include <QThread>
#include <QtTest>
#include <cstdarg>
#include <cstdio>
#include <syslog.h>

static QList<QPair<int, QByteArray>> diagnostics;
// Capture the real production syslog calls without changing their sink or
// allowing a production caller to supply diagnostic text.
extern "C" void syslog(int priority, const char *format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    diagnostics.append({priority, QByteArray(text, qBound(0, length, int(sizeof(text) - 1)))});
}

static QByteArray encoded(QByteArray bytes) {
    return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
static QJsonObject resultMessage() {
    return {{"type", "result"}, {"credentialId", QString::fromLatin1(encoded("credential"))},
        {"authenticatorData", QString::fromLatin1(encoded(QByteArray(37, 'a')))},
        {"signature", QString::fromLatin1(encoded("signature"))}};
}
static int fixture(const QString &mode) {
    QFile input; if (!input.open(stdin, QIODevice::ReadOnly)) return 8; input.readAll();
    QFile output; if (!output.open(stdout, QIODevice::WriteOnly)) return 8;
    const auto write = [&output](QJsonObject object) {
        output.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n'); output.flush();
    };
    if (mode == "oversize") { output.write(QByteArray(65538, 'x')); output.flush(); return 0; }
    if (mode == "invalid-qr") {
        write({{"type", "qr"}, {"size", 21}, {"modules", QString(440, '1')}}); return 0;
    }
    if (mode == "unknown-status") {
        write({{"type", "status"}, {"state", "synthetic-sensitive-status"}}); return 0;
    }
    write({{"type", "qr"}, {"size", 21}, {"modules", QString(441, '1')}});
    write({{"type", "status"}, {"state", "confirm_on_phone"}});
    if (mode == "diagnostics") {
        for (int i = 0; i < 20; ++i)
            for (const auto *state : {"waiting_for_phone", "connecting", "verifying", "connected", "confirm_on_phone"})
                write({{"type", "status"}, {"state", state}, {"extra", "synthetic-sensitive-status"}});
    }
    if (mode == "wait") { QThread::sleep(30); return 0; }
    auto result = resultMessage();
    if (mode == "diagnostics") {
        result["credentialId"] = QString::fromLatin1(encoded("synthetic-sensitive-credential"));
        result["signature"] = QString::fromLatin1(encoded("synthetic-sensitive-signature"));
    }
    if (mode == "invalid-base64") result["signature"] = "abc=";
    if (mode == "missing-credential") result.remove("credentialId");
    if (mode == "long-credential") result["credentialId"] = QString::fromLatin1(encoded(QByteArray(1025, 'a')));
    if (mode == "empty-user") result["userHandle"] = "";
    if (mode == "error") result = {{"type", "error"}, {"name", "NotAllowedError"}};
    if (mode == "not-supported") result = {{"type", "error"}, {"name", "NotSupportedError"}};
    if (mode == "operation-error") result = {{"type", "error"}, {"name", "OperationError"}};
    if (mode != "missing-result") write(result);
    if (mode == "duplicate") write(result);
    if (mode == "unterminated") output.write(" ");
    output.flush();
    return mode == "nonzero" ? 3 : 0;
}

class AuthPasskeyTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void init() { diagnostics.clear(); }
    void cleanup() {
        const QList<QByteArray> allowed {
            "request_rejected", "helper_start_requested", "helper_started", "helper_start_failed",
            "helper_process_error", "helper_input_failed", "helper_deadline", "helper_cancelled",
            "helper_shutdown", "helper_output_rejected", "qr_ready", "waiting_for_phone", "connecting",
            "verifying", "connected", "confirm_on_phone", "assertion_received", "not_allowed",
            "not_supported", "operation_error", "helper_exit_success", "helper_exit_failure"
        };
        const QByteArray prefix("rmweb-auth passkey: ");
        for (const auto &event : diagnostics) {
            QCOMPARE(event.first, LOG_AUTHPRIV | LOG_NOTICE);
            QVERIFY(event.second.startsWith(prefix));
            QVERIFY(allowed.contains(event.second.mid(prefix.size())));
            QVERIFY(!event.second.contains("synthetic-sensitive"));
        }
    }
    void fixedDiagnosticsExcludePayloadsAndBoundProgress() {
        rmweb::AuthPasskey session(QCoreApplication::applicationFilePath(), {"--helper-fixture", "diagnostics"});
        QSignalSpy done(&session, &rmweb::AuthPasskey::completed);
        session.start(987654321, "synthetic-sensitive.example.test", {{"clientDataHash", "synthetic-sensitive-hash"}});
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done[0][1].toBool());
        QList<QByteArray> events;
        for (const auto &event : diagnostics) events.append(event.second);
        QCOMPARE(events.count("rmweb-auth passkey: helper_start_requested"), 1);
        QCOMPARE(events.count("rmweb-auth passkey: helper_started"), 1);
        QCOMPARE(events.count("rmweb-auth passkey: qr_ready"), 1);
        for (const auto *state : {"waiting_for_phone", "connecting", "verifying", "connected", "confirm_on_phone"})
            QCOMPARE(events.count(QByteArray("rmweb-auth passkey: ") + state), 1);
        QCOMPARE(events.count("rmweb-auth passkey: assertion_received"), 1);
        QCOMPARE(events.count("rmweb-auth passkey: helper_exit_success"), 1);
        QCOMPARE(events.size(), 10);
    }
    void completeOnlyAfterValidCleanHelperExit() {
        rmweb::AuthPasskey session(QCoreApplication::applicationFilePath(), {"--helper-fixture", "valid"});
        QSignalSpy prompts(&session, &rmweb::AuthPasskey::promptChanged), done(&session, &rmweb::AuthPasskey::completed);
        session.start(41, "example.test", {{"version", 1}});
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done[0][0].toULongLong(), 41);
        QCOMPARE(done[0][1].toBool(), true);
        const auto result = qvariant_cast<rmweb::AuthPasskeyAssertion>(done[0][2]);
        QCOMPARE(result.credentialId, QByteArray("credential"));
        QCOMPARE(result.signature, QByteArray("signature"));
        bool sawQr = false;
        for (const auto &event : prompts) {
            const auto prompt = qvariant_cast<rmweb::AuthPasskeyPrompt>(event[0]);
            if (prompt.qr.isNull()) continue;
            sawQr = true;
            QCOMPARE(prompt.qr.size(), QSize(29, 29));
            QCOMPARE(prompt.qr.pixelColor(0, 0), QColor(Qt::white));
            QCOMPARE(prompt.qr.pixelColor(4, 4), QColor(Qt::black));
        }
        QVERIFY(sawQr);
        QVERIFY(!qvariant_cast<rmweb::AuthPasskeyPrompt>(prompts.last()[0]).active);
    }
    void malformedOutputFailsClosed_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"oversize", "invalid-qr", "invalid-base64", "missing-credential", "long-credential", "empty-user",
                                "duplicate", "unterminated", "nonzero", "missing-result", "error", "not-supported",
                                "operation-error", "unknown-status"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void malformedOutputFailsClosed() {
        QFETCH(QString, mode);
        rmweb::AuthPasskey session(QCoreApplication::applicationFilePath(), {"--helper-fixture", mode});
        QSignalSpy done(&session, &rmweb::AuthPasskey::completed);
        session.start(42, "example.test", {});
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][1].toBool());
        QVERIFY(qvariant_cast<rmweb::AuthPasskeyAssertion>(done[0][2]).credentialId.isEmpty());
        const auto reported = [](const char *label) {
            return diagnostics.contains({LOG_AUTHPRIV | LOG_NOTICE, QByteArray("rmweb-auth passkey: ") + label});
        };
        if (mode == "error") QVERIFY(reported("not_allowed"));
        if (mode == "not-supported") QVERIFY(reported("not_supported"));
        if (mode == "operation-error") QVERIFY(reported("operation_error"));
        if (mode == "unknown-status") QVERIFY(reported("helper_output_rejected"));
        QVERIFY(reported("helper_exit_failure"));
    }
    void cancellationRejectsStaleIdAndClearsPrompt() {
        rmweb::AuthPasskey session(QCoreApplication::applicationFilePath(), {"--helper-fixture", "wait"});
        QSignalSpy prompts(&session, &rmweb::AuthPasskey::promptChanged), done(&session, &rmweb::AuthPasskey::completed);
        session.start(43, "example.test", {});
        QTRY_VERIFY(prompts.size() >= 3);
        session.cancel(42);
        QVERIFY(done.isEmpty());
        QVERIFY(qvariant_cast<rmweb::AuthPasskeyPrompt>(prompts.last()[0]).active);
        session.cancel(43);
        QVERIFY(!qvariant_cast<rmweb::AuthPasskeyPrompt>(prompts.last()[0]).active);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][1].toBool());
        QVERIFY(diagnostics.contains({LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_cancelled"}));
    }
    void overlapNeverReplacesActiveHelper() {
        rmweb::AuthPasskey session(QCoreApplication::applicationFilePath(), {"--helper-fixture", "wait"});
        QSignalSpy done(&session, &rmweb::AuthPasskey::completed);
        session.start(44, "example.test", {});
        session.start(45, "example.test", {});
        QCOMPARE(done.size(), 1);
        QCOMPARE(done[0][0].toULongLong(), 45);
        session.shutdown();
        QCOMPARE(done.size(), 2);
        QCOMPARE(done[1][0].toULongLong(), 44);
    }
    void defaultHelperRunsFromApplicationDirectory() {
        const QString helper = QCoreApplication::applicationDirPath() + "/rmweb-auth-passkey";
        QVERIFY(!QFile::exists(helper));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), helper));
        const auto cleanup = qScopeGuard([helper] { QFile::remove(helper); });
        rmweb::AuthPasskey session;
        QSignalSpy done(&session, &rmweb::AuthPasskey::completed);
        session.start(47, "example.test", {});
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done[0][1].toBool());
        QCOMPARE(qvariant_cast<rmweb::AuthPasskeyAssertion>(done[0][2]).credentialId, QByteArray("credential"));
    }
    void failedExecutableCompletesOnce() {
        rmweb::AuthPasskey session("/nonexistent/rmweb-test-helper", {});
        QSignalSpy done(&session, &rmweb::AuthPasskey::completed);
        session.start(46, "example.test", {});
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done[0][1].toBool());
        QVERIFY(diagnostics.contains({LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_start_failed"}));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 1 && QFileInfo(QCoreApplication::applicationFilePath()).fileName() == "rmweb-auth-passkey")
        return fixture("success");
    if (argc == 3 && QByteArray(argv[1]) == "--helper-fixture") return fixture(QString::fromLatin1(argv[2]));
    AuthPasskeyTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "auth_passkey_test.moc"
