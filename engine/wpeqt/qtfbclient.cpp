#include "qtfbclient.h"
#include <QFile>
#include <QSocketNotifier>
#include <QThread>
#include <QtEndian>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef Q_OS_LINUX
#error "QTFB client requires Linux SOCK_SEQPACKET and POSIX shared memory"
#endif

namespace {
// Wire layout verified against asivery/rm-appload v0.5.3, commit
// 5bb34a362f09f753f18bd6261558f8e2737aacdb, src/qtfb/common.h/fbmanagement.cpp.
// Its 24/32-byte little-endian ABI is unchanged in d58c3476, which adds optional
// rotation messages. Explicit offsets avoid C++ padding and upstream client bugs.
constexpr size_t ClientPacketBytes = 24;
constexpr size_t ServerPacketBytes = 32;
constexpr int DeadlineMs = 1500;
constexpr int MaxPacketsPerWake = 32;
constexpr int Initialize = 0, Update = 1, CustomInitialize = 2, Terminate = 3;
constexpr int UserInput = 4, StateChanged = 7, StateInitial = 8;
constexpr int Rgba8888 = 2;

using Packet = std::array<char, ClientPacketBytes>;
Packet packet(int type) {
    Packet bytes{};
    bytes[0] = char(type);
    return bytes;
}
void writeInt(Packet &bytes, int offset, int value) {
    qToLittleEndian<qint32>(value, bytes.data() + offset);
}
int readInt(const char *bytes, int offset) {
    return qFromLittleEndian<qint32>(bytes + offset);
}
}

namespace rmweb {

QtfbClient::QtfbClient(QObject *parent) : QObject(parent) {
    m_handshakeDeadline.setSingleShot(true);
    m_writeDeadline.setSingleShot(true);
    connect(&m_handshakeDeadline, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("AppLoad framebuffer initialization timed out"));
    });
    connect(&m_writeDeadline, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("AppLoad framebuffer stopped accepting updates"));
    });
}

QtfbClient::~QtfbClient() { cleanup(); }

bool QtfbClient::startFromEnvironment() {
    const QByteArray value = qgetenv("QTFB_KEY");
    bool valid = !value.isEmpty() && value.size() <= 10;
    for (const char c : value) valid = valid && c >= '0' && c <= '9';
    bool number = false;
    const int key = value.toInt(&number);
    if (!valid || !number || key < 0) {
        fail(QStringLiteral("A valid AppLoad QTFB_KEY is required"));
        return false;
    }
    return start(key);
}

bool QtfbClient::start(int key, const QString &socketPath, QSize requestedSize) {
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_state != State::Idle) return false;
    const QByteArray path = QFile::encodeName(socketPath);
    sockaddr_un address{};
    if (key < 0 || requestedSize.width() < 1 || requestedSize.height() < 1
        || requestedSize.width() > 1620 || requestedSize.height() > 2160
        || path.isEmpty() || path[0] != '/' || path.contains('\0')
        || size_t(path.size()) >= sizeof address.sun_path) {
        fail(QStringLiteral("Invalid AppLoad framebuffer connection parameters"));
        return false;
    }
    m_key = key;
    m_size = requestedSize;
    m_shmSize = size_t(m_size.width()) * size_t(m_size.height()) * 4;
    m_state = State::Connecting;
    m_socket = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_socket < 0) {
        fail(QStringLiteral("Could not create AppLoad framebuffer socket"));
        return false;
    }
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.constData(), size_t(path.size()) + 1);
    const int result = ::connect(m_socket, reinterpret_cast<sockaddr *>(&address), sizeof address);
    // For local sockets EAGAIN means the accept backlog is full, not an in-flight
    // connect. Fail clearly instead of treating it as a connected descriptor.
    if (result < 0 && errno != EINPROGRESS) {
        fail(QStringLiteral("Could not connect to AppLoad framebuffer"));
        return false;
    }
    if (result == 0) m_state = State::Initializing;
    m_readNotifier = new QSocketNotifier(m_socket, QSocketNotifier::Read, this);
    m_writeNotifier = new QSocketNotifier(m_socket, QSocketNotifier::Write, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, [this] { readable(); });
    connect(m_writeNotifier, &QSocketNotifier::activated, this, [this] { writable(); });
    m_handshakeDeadline.start(DeadlineMs);
    flush();
    return m_state != State::Closed;
}

bool QtfbClient::isReady() const { return m_state == State::Ready; }

bool QtfbClient::submitImage(const QImage &image) {
    Q_ASSERT(thread() == QThread::currentThread());
    if (!isReady() || image.isNull() || image.size() != m_size) return false;
    // Bound conversion and queued storage to one <=14 MiB frame. All transport
    // I/O is nonblocking; image conversion and copy do bounded CPU work only.
    m_pendingImage = image.convertToFormat(QImage::Format_RGBA8888);
    if (m_pendingImage.isNull()) return false;
    flush();
    return isReady();
}

void QtfbClient::writable() {
    if (m_state == State::Connecting) {
        int socketError = 0;
        socklen_t size = sizeof socketError;
        if (getsockopt(m_socket, SOL_SOCKET, SO_ERROR, &socketError, &size) != 0 || socketError != 0) {
            fail(QStringLiteral("AppLoad framebuffer connection failed"));
            return;
        }
        m_state = State::Initializing;
    }
    flush();
}

bool QtfbClient::sendPacket(const char *data, size_t length) {
    const ssize_t written = ::send(m_socket, data, length, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (written == ssize_t(length)) {
        m_writeDeadline.stop();
        return true;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        if (!m_writeDeadline.isActive()) m_writeDeadline.start(DeadlineMs);
        m_writeNotifier->setEnabled(true);
        return false;
    }
    fail(QStringLiteral("AppLoad framebuffer update could not be sent"));
    return false;
}

void QtfbClient::flush() {
    if (m_state == State::Connecting || m_state == State::Closed || m_state == State::Idle) return;
    if (!m_initSent) {
        Packet request = packet(CustomInitialize);
        writeInt(request, 4, m_key);
        request[8] = char(Rgba8888);
        qToLittleEndian<quint16>(quint16(m_size.width()), request.data() + 10);
        qToLittleEndian<quint16>(quint16(m_size.height()), request.data() + 12);
        if (!sendPacket(request.data(), request.size())) return;
        m_initSent = true;
    }
    if (isReady() && !m_pendingImage.isNull()) {
        const size_t rowBytes = size_t(m_size.width()) * 4;
        for (int row = 0; row < m_size.height(); ++row) {
            memcpy(static_cast<char *>(m_shm) + size_t(row) * rowBytes,
                   m_pendingImage.constScanLine(row), rowBytes);
        }
        Packet update = packet(Update); // UPDATE_ALL=0; the packet is zero-filled.
        if (!sendPacket(update.data(), update.size())) return;
        m_pendingImage = {};
        // Disable before the signal so a new frame submitted from the signal
        // can arm it again if that send encounters backpressure.
        m_writeNotifier->setEnabled(false);
        Q_EMIT frameSubmitted();
        return;
    }
    m_writeNotifier->setEnabled(false);
}

bool QtfbClient::receiveInitialization(const char *bytes) {
    if (m_state != State::Initializing || !m_initSent) {
        fail(QStringLiteral("Unexpected AppLoad framebuffer initialization reply"));
        return false;
    }
    const int key = readInt(bytes, 8);
    const quint64 size = qFromLittleEndian<quint64>(bytes + 16);
    if (key < 0 || size != m_shmSize) {
        fail(QStringLiteral("AppLoad framebuffer size does not match the request"));
        return false;
    }
    const QByteArray name = QByteArray("/qtfb_") + QByteArray::number(key);
    m_shmFd = shm_open(name.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0);
    struct stat info{};
    if (m_shmFd < 0 || fstat(m_shmFd, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_uid != geteuid() || info.st_size != off_t(m_shmSize)) {
        fail(QStringLiteral("AppLoad framebuffer shared memory is invalid"));
        return false;
    }
    void *mapped = mmap(nullptr, m_shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_shmFd, 0);
    if (mapped == MAP_FAILED) {
        fail(QStringLiteral("Could not map AppLoad framebuffer shared memory"));
        return false;
    }
    m_shm = mapped;
    m_handshakeDeadline.stop();
    m_state = State::Ready;
    Q_EMIT initialized(m_size);
    return isReady();
}

void QtfbClient::receiveInput(const char *bytes) {
    const int type = readInt(bytes, 8), id = readInt(bytes, 12);
    const int x = readInt(bytes, 16), y = readInt(bytes, 20);
    if (type == 0x13) {
        if (id == 0 && x == 0 && y == 0 && readInt(bytes, 24) == 0)
            Q_EMIT touchesCancelled();
        return;
    }
    if (type == 0x40 || type == 0x41) {
        // v0.5.3 default.layout.json is the key-code source of truth. The
        // common.h special-key constants omit Backspace and disagree with it.
        constexpr int modifiers = 0x700000;
        const int key = x & ~modifiers;
        const bool known = key == 9 || key == 13 || key == 27
            || (key >= 32 && key <= 96) || (key >= 123 && key <= 136)
            || (key == 0 && (x & modifiers) != 0);
        if (id == 0 && y == 0 && readInt(bytes, 24) == 0 && x >= 0 && known)
            Q_EMIT keyEvent(x, type == 0x40);
        return; // Key codes and modifier bits are not framebuffer coordinates.
    }
    if (id < 0 || x < 0 || y < 0 || x >= m_size.width() || y >= m_size.height()) return;
    if (type >= 0x20 && type <= 0x22) {
        const int pressure = readInt(bytes, 24);
        // AppLoad reports one pen (device 0), pressure 0..100, and 0 on release.
        if (id != 0 || pressure < 0 || pressure > 100 || (type == 0x21 && pressure != 0)) return;
    }
    switch (type) {
        case 0x10: Q_EMIT touchPressed(id, x, y); break;
        case 0x11: Q_EMIT touchReleased(id, x, y); break;
        case 0x12: Q_EMIT touchMoved(id, x, y); break;
        case 0x20: Q_EMIT penPressed(x, y); break;
        case 0x21: Q_EMIT penReleased(x, y); break;
        case 0x22: Q_EMIT penMoved(x, y); break;
        default: break; // Physical buttons are not pointer events.
    }
}

void QtfbClient::readable() {
    if (m_state == State::Connecting) writable();
    for (int count = 0; count < MaxPacketsPerWake && m_socket >= 0; ++count) {
        std::array<char, ServerPacketBytes> bytes{};
        iovec buffer{bytes.data(), bytes.size()};
        msghdr message{};
        message.msg_iov = &buffer;
        message.msg_iovlen = 1;
        const ssize_t received = recvmsg(m_socket, &message, MSG_DONTWAIT);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (received < 0 && errno == EINTR) continue;
        if (received == 0) { close(); return; }
        if (received != ssize_t(ServerPacketBytes) || (message.msg_flags & MSG_TRUNC)) {
            fail(QStringLiteral("Invalid AppLoad framebuffer packet length"));
            return;
        }
        const int type = static_cast<unsigned char>(bytes[0]);
        if (type == Initialize) {
            if (!receiveInitialization(bytes.data())) return;
        } else if (!isReady()) {
            fail(QStringLiteral("AppLoad framebuffer data arrived before initialization"));
            return;
        } else if (type == UserInput) {
            receiveInput(bytes.data());
        } else if (type == StateChanged || type == StateInitial) {
            const int reason = readInt(bytes.data(), 8), rotation = readInt(bytes.data(), 12);
            if (reason != 0 || rotation < 0 || rotation > 3) {
                fail(QStringLiteral("Invalid AppLoad framebuffer rotation"));
                return;
            }
            Q_EMIT rotationChanged(rotation);
        } else if (type == Terminate) {
            close();
            return;
        } else {
            fail(QStringLiteral("Unknown AppLoad framebuffer packet"));
            return;
        }
    }
}

void QtfbClient::cleanup() {
    m_handshakeDeadline.stop();
    m_writeDeadline.stop();
    for (QSocketNotifier **notifier : {&m_readNotifier, &m_writeNotifier}) {
        if (*notifier) {
            (*notifier)->setEnabled(false);
            (*notifier)->deleteLater();
            *notifier = nullptr;
        }
    }
    if (m_socket >= 0) {
        const Packet terminate = packet(Terminate);
        (void) ::send(m_socket, terminate.data(), terminate.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        ::close(m_socket);
        m_socket = -1;
    }
    if (m_shm) { munmap(m_shm, m_shmSize); m_shm = nullptr; }
    if (m_shmFd >= 0) { ::close(m_shmFd); m_shmFd = -1; }
    m_pendingImage = {};
    m_state = State::Closed;
}

void QtfbClient::close() {
    if (m_state == State::Closed) return;
    cleanup();
    Q_EMIT connectionClosed();
}

void QtfbClient::fail(const QString &message) {
    if (m_state == State::Closed) return;
    cleanup();
    Q_EMIT error(message);
    Q_EMIT connectionClosed();
}
}
