#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

class AuthPasskeyTest;
namespace rmweb {
struct AuthPasskeyPrompt {
    bool active = false;
    QString relyingParty;
    QString status;
    QImage qr;
};
struct AuthPasskeyAssertion {
    QByteArray credentialId;
    QByteArray authenticatorData;
    QByteArray signature;
    QByteArray userHandle;
};

// One private helper process per browser ceremony. Diagnostics contain only
// fixed lifecycle labels, never request/response data or page JavaScript.
class AuthPasskey final : public QObject {
    Q_OBJECT
public:
    explicit AuthPasskey(QObject *parent = nullptr);
    ~AuthPasskey() override;
    void start(quint64 requestId, const QString &relyingParty, const QJsonObject &request);
    void cancel(quint64 requestId);
    void shutdown();
Q_SIGNALS:
    void promptChanged(rmweb::AuthPasskeyPrompt prompt);
    void completed(quint64 requestId, bool success, rmweb::AuthPasskeyAssertion assertion);
private:
    friend class ::AuthPasskeyTest;
    AuthPasskey(QString executable, QStringList arguments, QObject *parent = nullptr);
    void readOutput();
    bool message(const QJsonObject &object);
    void fail();
    void rejectOutput();
    void finish(int exitCode, QProcess::ExitStatus exitStatus);
    void publishPrompt();
    QProcess m_process;
    QTimer m_deadline;
    QTimer m_killTimer;
    QString m_executable;
    QStringList m_arguments;
    quint64 m_requestId = 0;
    QByteArray m_buffer;
    qsizetype m_outputBytes = 0;
    bool m_failed = false;
    bool m_terminal = false;
    bool m_haveAssertion = false;
    unsigned m_reportedStatuses = 0;
    AuthPasskeyPrompt m_prompt;
    AuthPasskeyAssertion m_assertion;
};
} // namespace rmweb
Q_DECLARE_METATYPE(rmweb::AuthPasskeyPrompt)
Q_DECLARE_METATYPE(rmweb::AuthPasskeyAssertion)
