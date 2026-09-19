#pragma once

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

class QSocketNotifier;

namespace rmweb {

// One connection to AppLoad's 64-bit Linux QTFB ABI. No display/input devices or
// QPA are opened here. Call methods on this object's thread; no automatic retry.
class QtfbClient final : public QObject {
    Q_OBJECT
public:
    explicit QtfbClient(QObject *parent = nullptr);
    ~QtfbClient() override;

    bool startFromEnvironment();
    bool start(int key, const QString &socketPath = QStringLiteral("/tmp/qtfb.sock"),
               QSize size = QSize(1620, 2160));
    bool isReady() const;
    QSize size() const { return m_size; }

    // Accept the latest exact-size frame, replacing any unsent frame. False
    // means invalid/not ready. frameSubmitted is a socket send, NOT panel ACK.
    bool submitImage(const QImage &image);
    void close();

Q_SIGNALS:
    void initialized(QSize size);
    void frameSubmitted();
    void touchPressed(int id, int x, int y);
    void touchMoved(int id, int x, int y);
    void touchReleased(int id, int x, int y);
    // AppLoad touch lifecycle extension: a fresh sequence or cancellation.
    void touchesCancelled();
    // Separate from finger contacts; callers explicitly opt into pen input.
    void penPressed(int x, int y);
    void penMoved(int x, int y);
    void penReleased(int x, int y);
    // Default AppLoad layout code plus its 0x700000 modifier bits.
    void keyEvent(int code, bool pressed);
    void rotationChanged(int rotation);
    void connectionClosed();
    void error(const QString &message);

private:
    enum class State { Idle, Connecting, Initializing, Ready, Closed };
    void readable();
    void writable();
    void flush();
    void fail(const QString &message);
    void cleanup();
    bool receiveInitialization(const char *packet);
    void receiveInput(const char *packet);
    bool sendPacket(const char *data, size_t length);

    State m_state = State::Idle;
    int m_socket = -1;
    int m_shmFd = -1;
    int m_key = -1;
    void *m_shm = nullptr;
    size_t m_shmSize = 0;
    QSize m_size;
    bool m_initSent = false;
    QImage m_pendingImage;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QTimer m_handshakeDeadline;
    QTimer m_writeDeadline;
};

} // namespace rmweb
