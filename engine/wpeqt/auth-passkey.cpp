#include "auth-passkey.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <syslog.h>

namespace rmweb {
namespace {
constexpr qsizetype MaxLine = 64 * 1024;
constexpr qsizetype MaxOutput = 512 * 1024;
bool decode(const QJsonValue &value, QByteArray &out, qsizetype minimum, qsizetype maximum) {
    if (!value.isString()) return false;
    const auto text = value.toString();
    static const QRegularExpression alphabet(QStringLiteral("\\A[A-Za-z0-9_-]*\\z"));
    if (text.size() > maximum * 2 || !alphabet.match(text).hasMatch()) return false;
    const auto encoded = text.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded,
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() < minimum || decoded.decoded.size() > maximum
        || decoded.decoded.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) != encoded)
        return false;
    out = decoded.decoded;
    return true;
}
}
AuthPasskey::AuthPasskey(QObject *parent)
    : AuthPasskey(QCoreApplication::applicationDirPath() + QStringLiteral("/rmweb-auth-passkey"), {}, parent) {}
AuthPasskey::AuthPasskey(QString executable, QStringList arguments, QObject *parent)
    : QObject(parent), m_executable(std::move(executable)), m_arguments(std::move(arguments)) {
    qRegisterMetaType<AuthPasskeyPrompt>();
    qRegisterMetaType<AuthPasskeyAssertion>();
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(125000);
    m_killTimer.setSingleShot(true);
    m_killTimer.setInterval(5000);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_deadline");
        fail();
    });
    connect(&m_killTimer, &QTimer::timeout, this, [this] { m_process.kill(); });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &AuthPasskey::readOutput);
    connect(&m_process, &QProcess::started, this, [] {
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_started");
    });
    connect(&m_process, &QProcess::finished, this, &AuthPasskey::finish);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_start_failed");
        else if (!m_failed)
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_process_error");
        fail();
        if (error == QProcess::FailedToStart) finish(-1, QProcess::CrashExit);
    });
}
AuthPasskey::~AuthPasskey() { shutdown(); }
void AuthPasskey::publishPrompt() { Q_EMIT promptChanged(m_prompt); }
void AuthPasskey::start(quint64 requestId, const QString &relyingParty, const QJsonObject &request) {
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    static const QRegularExpression domain(QStringLiteral("\\A[a-zA-Z0-9][a-zA-Z0-9.-]{0,251}[a-zA-Z0-9]\\z"));
    if (!requestId || m_requestId || m_process.state() != QProcess::NotRunning
        || bytes.size() > 128 * 1024 || !domain.match(relyingParty).hasMatch()) {
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: request_rejected");
        Q_EMIT completed(requestId, false, {});
        return;
    }
    m_requestId = requestId;
    m_buffer.clear(); m_outputBytes = 0;
    m_failed = false; m_terminal = false; m_haveAssertion = false; m_assertion = {};
    m_reportedStatuses = 0;
    m_prompt = {true, relyingParty, QStringLiteral("Preparing phone sign-in"), {}};
    publishPrompt();
    m_deadline.start();
    syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_start_requested");
    m_process.start(m_executable, m_arguments, QIODevice::ReadWrite);
    if (m_process.write(bytes) != bytes.size()) {
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_input_failed");
        fail(); return;
    }
    m_process.closeWriteChannel();
}
void AuthPasskey::cancel(quint64 requestId) {
    if (m_requestId && requestId == m_requestId) {
        if (!m_failed) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_cancelled");
        fail();
    }
}
void AuthPasskey::fail() {
    if (!m_requestId) return;
    m_failed = true; m_assertion = {}; m_haveAssertion = false;
    m_prompt = {}; publishPrompt();
    m_deadline.stop();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_killTimer.isActive()) m_killTimer.start();
    }
}
void AuthPasskey::rejectOutput() {
    if (!m_failed) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_output_rejected");
    fail();
}
void AuthPasskey::readOutput() {
    if (!m_requestId) { m_process.readAllStandardOutput(); return; }
    while (m_process.bytesAvailable() > 0) {
        const auto bytes = m_process.read(qMin<qint64>(m_process.bytesAvailable(), MaxLine + 1));
        m_outputBytes += bytes.size();
        if (m_failed) return;
        m_buffer.append(bytes);
        if (m_outputBytes > MaxOutput) { rejectOutput(); continue; }
        qsizetype newline;
        while ((newline = m_buffer.indexOf('\n')) >= 0) {
            if (newline > MaxLine || m_terminal) { rejectOutput(); break; }
            const auto line = m_buffer.left(newline);
            m_buffer.remove(0, newline + 1);
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject() || !message(document.object())) {
                rejectOutput(); break;
            }
        }
        if (m_buffer.size() > MaxLine) rejectOutput();
    }
}
bool AuthPasskey::message(const QJsonObject &object) {
    const auto type = object.value("type").toString();
    if (type == "qr") {
        const auto sizeValue = object.value("size");
        const int size = sizeValue.toInt();
        const auto modules = object.value("modules").toString();
        if (!sizeValue.isDouble() || sizeValue.toDouble() != size || size < 21 || size > 177
            || (size - 21) % 4 || modules.size() != size * size || !m_prompt.qr.isNull()) return false;
        QImage qr(size + 8, size + 8, QImage::Format_RGB32);
        qr.fill(Qt::white);
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            const auto c = modules.at(y * size + x);
            if (c != u'0' && c != u'1') return false;
            if (c == u'1') qr.setPixelColor(x + 4, y + 4, Qt::black);
        }
        m_prompt.qr = qr;
        m_prompt.status = QStringLiteral("Scan with your phone");
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: qr_ready");
        publishPrompt();
        return true;
    }
    if (type == "status") {
        const auto state = object.value("state").toString();
        QString label;
        // Report each known state once per ceremony. The helper can repeat
        // progress messages, but no helper-provided string enters syslog.
        unsigned status = 0;
        if (state == "waiting_for_phone") {
            label = QStringLiteral("Scan with your phone"); status = 1;
            if (!(m_reportedStatuses & status)) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: waiting_for_phone");
        } else if (state == "connecting") {
            label = QStringLiteral("Connecting to your phone"); status = 2;
            if (!(m_reportedStatuses & status)) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: connecting");
        } else if (state == "verifying") {
            label = QStringLiteral("Verifying phone connection"); status = 4;
            if (!(m_reportedStatuses & status)) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: verifying");
        } else if (state == "connected") {
            label = QStringLiteral("Continue on your phone"); status = 8;
            if (!(m_reportedStatuses & status)) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: connected");
        } else if (state == "confirm_on_phone") {
            label = QStringLiteral("Continue on your phone"); status = 16;
            if (!(m_reportedStatuses & status)) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: confirm_on_phone");
        }
        else return false;
        m_reportedStatuses |= status;
        m_prompt.status = label; publishPrompt();
        return true;
    }
    if (type == "result") {
        AuthPasskeyAssertion result;
        if (!decode(object.value("credentialId"), result.credentialId, 1, 1024)
            || !decode(object.value("authenticatorData"), result.authenticatorData, 37, 16384)
            || !decode(object.value("signature"), result.signature, 1, 4096)
            || (object.contains("userHandle") && !object.value("userHandle").isNull()
                && !decode(object.value("userHandle"), result.userHandle, 1, 64))) return false;
        m_assertion = std::move(result); m_haveAssertion = true; m_terminal = true;
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: assertion_received");
        return true;
    }
    if (type == "error") {
        const auto name = object.value("name").toString();
        if (name != "NotAllowedError" && name != "NotSupportedError" && name != "OperationError") return false;
        if (name == "NotAllowedError") syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: not_allowed");
        else if (name == "NotSupportedError") syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: not_supported");
        else syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: operation_error");
        m_terminal = true;
        return true;
    }
    return false;
}
void AuthPasskey::finish(int exitCode, QProcess::ExitStatus exitStatus) {
    if (!m_requestId) return;
    readOutput();
    const auto id = m_requestId;
    const bool success = !m_failed && m_terminal && m_buffer.isEmpty() && m_haveAssertion
        && exitStatus == QProcess::NormalExit && exitCode == 0;
    if (success) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_exit_success");
    else syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_exit_failure");
    const auto assertion = success ? m_assertion : AuthPasskeyAssertion{};
    m_requestId = 0; m_buffer.clear(); m_assertion = {};
    m_deadline.stop(); m_killTimer.stop();
    m_prompt = {}; publishPrompt();
    Q_EMIT completed(id, success, assertion);
}
void AuthPasskey::shutdown() {
    if (!m_requestId && m_process.state() == QProcess::NotRunning) return;
    if (!m_failed) syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: helper_shutdown");
    fail();
    if (m_process.state() != QProcess::NotRunning && !m_process.waitForFinished(5000)) {
        m_process.kill(); m_process.waitForFinished(1000);
    }
}
} // namespace rmweb
