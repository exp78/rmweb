#include "../engine/wpeqt/qtfbclient.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <QtTest>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// Independent byte fixtures for the observed aarch64 ABI: client=24 bytes;
// server=32 bytes, union starts at 8, size_t starts at 16 in an init reply.
static void put32(QByteArray &bytes, int offset, int value) {
    memcpy(bytes.data() + offset, &value, sizeof value);
}
static int get32(const QByteArray &bytes, int offset) {
    int value = 0;
    memcpy(&value, bytes.constData() + offset, sizeof value);
    return value;
}
static QByteArray packet(int type) {
    QByteArray bytes(32, '\0');
    bytes[0] = static_cast<char>(type);
    return bytes;
}

class Server {
public:
    Server() {
        path = directory.path() + QStringLiteral("/qtfb.sock");
        listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const QByteArray name = path.toLocal8Bit();
        memcpy(address.sun_path, name.constData(), size_t(name.size()) + 1);
        valid = listener >= 0 && bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0
            && listen(listener, 1) == 0;
    }
    ~Server() {
        if (peer >= 0) ::close(peer);
        if (listener >= 0) ::close(listener);
        if (shm >= 0) ::close(shm);
        if (!shmName.isEmpty()) shm_unlink(shmName.constData());
    }
    bool acceptClient() {
        peer = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        return peer >= 0;
    }
    QByteArray read() {
        char bytes[128];
        const ssize_t n = recv(peer, bytes, sizeof bytes, MSG_DONTWAIT);
        return n > 0 ? QByteArray(bytes, int(n)) : QByteArray();
    }
    bool send(const QByteArray &bytes) {
        return ::send(peer, bytes.constData(), size_t(bytes.size()), MSG_DONTWAIT | MSG_NOSIGNAL) == bytes.size();
    }
    QByteArray init(QSize size, qint64 reportedBytes = -1, qint64 actualBytes = -1) {
        static int nextKey = 0;
        const int key = int(getpid()) * 1000 + ++nextKey;
        shmName = QByteArray("/qtfb_") + QByteArray::number(key);
        shm = shm_open(shmName.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        const quint64 expected = quint64(size.width()) * quint64(size.height()) * 4;
        if (shm < 0 || ftruncate(shm, actualBytes < 0 ? off_t(expected) : actualBytes) != 0) return {};
        QByteArray bytes = packet(0);
        put32(bytes, 8, key);
        const quint64 advertised = reportedBytes < 0 ? expected : quint64(reportedBytes);
        memcpy(bytes.data() + 16, &advertised, sizeof advertised);
        return bytes;
    }
    bool initialize(rmweb::QtfbClient &client, QSize size = QSize(3, 2)) {
        if (!valid || !client.start(42, path, size) || !acceptClient()) return false;
        request = read();
        return request.size() == 24 && send(init(size));
    }
    QTemporaryDir directory;
    QString path;
    QByteArray shmName, request;
    int listener = -1, peer = -1, shm = -1;
    bool valid = false;
};

class QtfbClientTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void frameUsesExactWireLayoutAndRgbaBytes() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy ready(&client, &rmweb::QtfbClient::initialized);
        QSignalSpy sent(&client, &rmweb::QtfbClient::frameSubmitted);
        QVERIFY(server.initialize(client));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 500);
        QVERIFY(client.isReady());
        QCOMPARE(client.size(), QSize(3, 2));
        QCOMPARE(server.request[0], char(2)); // custom initialize
        QCOMPARE(get32(server.request, 4), 42);
        QCOMPARE(server.request[8], char(2)); // RMPP RGBA8888
        QCOMPARE(server.request.mid(10, 4), QByteArray::fromHex("03000200"));
        QImage image(3, 2, QImage::Format_ARGB32);
        image.fill(QColor(0x12, 0x34, 0x56, 0xff));
        image.setPixelColor(2, 1, QColor(0x78, 0x9a, 0xbc, 0xff));
        QVERIFY(client.submitImage(image));
        QCOMPARE(sent.size(), 1);
        const QByteArray update = server.read();
        QCOMPARE(update.size(), 24);
        QCOMPARE(update[0], char(1));
        QCOMPARE(get32(update, 4), 0); // full update
        char bytes[24]{};
        QCOMPARE(pread(server.shm, bytes, sizeof bytes, 0), ssize_t(sizeof bytes));
        QCOMPARE(QByteArray(bytes, 4), QByteArray::fromHex("123456ff"));
        QCOMPARE(QByteArray(bytes + 20, 4), QByteArray::fromHex("789abcff"));
        QVERIFY(!client.submitImage(QImage(2, 2, QImage::Format_RGB32)));
    }

    void touchSequenceResetIsBounded() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy resets(&client, &rmweb::QtfbClient::touchesCancelled);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        QByteArray reset = packet(4);
        put32(reset, 8, 0x13);
        QVERIFY(server.send(reset));
        QTRY_COMPARE_WITH_TIMEOUT(resets.size(), 1, 500);
        for (int offset : {12, 16, 20, 24}) {
            QByteArray malformed = reset;
            put32(malformed, offset, 1);
            QVERIFY(server.send(malformed));
        }
        QVERIFY(server.send(reset));
        QTRY_COMPARE_WITH_TIMEOUT(resets.size(), 2, 500);
    }

    void inputAndRotationAreDecoded() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy presses(&client, &rmweb::QtfbClient::touchPressed);
        QSignalSpy moves(&client, &rmweb::QtfbClient::touchMoved);
        QSignalSpy releases(&client, &rmweb::QtfbClient::touchReleased);
        QSignalSpy rotations(&client, &rmweb::QtfbClient::rotationChanged);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        for (int type : {0x10, 0x12, 0x11}) {
            QByteArray input = packet(4);
            put32(input, 8, type); put32(input, 12, 7);
            put32(input, 16, 2); put32(input, 20, 1);
            QVERIFY(server.send(input));
        }
        QByteArray rotation = packet(8);
        put32(rotation, 12, 3);
        QVERIFY(server.send(rotation));
        QTRY_COMPARE_WITH_TIMEOUT(releases.size(), 1, 500);
        QCOMPARE(presses.size(), 1); QCOMPARE(moves.size(), 1);
        QCOMPARE(presses.first(), QVariantList({7, 2, 1}));
        QTRY_COMPARE_WITH_TIMEOUT(rotations.size(), 1, 500);
        QCOMPARE(rotations.first(), QVariantList({3}));
        QByteArray invalid = packet(4);
        put32(invalid, 8, 0x10); put32(invalid, 16, 3);
        QVERIFY(server.send(invalid)); // Outside exact geometry.
        put32(invalid, 16, 0); put32(invalid, 12, -1);
        QVERIFY(server.send(invalid));
        put32(invalid, 12, 0); put32(invalid, 8, 0x20); // Pen, not finger.
        QVERIFY(server.send(invalid));
        QTest::qWait(20);
        QCOMPARE(presses.size(), 1);
    }

    void penPacketsUseSeparateSignals() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy presses(&client, &rmweb::QtfbClient::penPressed);
        QSignalSpy moves(&client, &rmweb::QtfbClient::penMoved);
        QSignalSpy releases(&client, &rmweb::QtfbClient::penReleased);
        QSignalSpy fingersDown(&client, &rmweb::QtfbClient::touchPressed);
        QSignalSpy fingersMoved(&client, &rmweb::QtfbClient::touchMoved);
        QSignalSpy fingersUp(&client, &rmweb::QtfbClient::touchReleased);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        for (int pressure : {0, 100}) {
            for (int type : {0x20, 0x22, 0x21}) {
                QByteArray input = packet(4);
                put32(input, 8, type); // AppLoad's pen, not touch, packet family.
                put32(input, 16, type == 0x20 ? 0 : 2);
                put32(input, 20, type == 0x20 ? 0 : 1);
                put32(input, 24, type == 0x21 ? 0 : pressure);
                QVERIFY(server.send(input));
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(releases.size(), 2, 500);
        QCOMPARE(presses.size(), 2);
        QCOMPARE(moves.size(), 2);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(presses.at(i), QVariantList({0, 0}));
            QCOMPARE(moves.at(i), QVariantList({2, 1}));
            QCOMPARE(releases.at(i), QVariantList({2, 1}));
        }
        QCOMPARE(fingersDown.size(), 0);
        QCOMPARE(fingersMoved.size(), 0);
        QCOMPARE(fingersUp.size(), 0);
        QVERIFY(client.isReady());
    }
    void malformedPenIgnored_data() {
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("id");
        QTest::addColumn<int>("x");
        QTest::addColumn<int>("y");
        QTest::addColumn<int>("pressure");
        for (int type : {0x20, 0x21, 0x22}) {
            const QByteArray prefix = QByteArray::number(type) + '-';
            QTest::newRow((prefix + "negative-device").constData()) << type << -1 << 0 << 0 << 0;
            QTest::newRow((prefix + "unexpected-device").constData()) << type << 1 << 0 << 0 << 0;
            QTest::newRow((prefix + "negative-x").constData()) << type << 0 << -1 << 0 << 0;
            QTest::newRow((prefix + "outside-x").constData()) << type << 0 << 3 << 0 << 0;
            QTest::newRow((prefix + "negative-y").constData()) << type << 0 << 0 << -1 << 0;
            QTest::newRow((prefix + "outside-y").constData()) << type << 0 << 0 << 2 << 0;
            QTest::newRow((prefix + "negative-pressure").constData()) << type << 0 << 0 << 0 << -1;
            QTest::newRow((prefix + "outside-pressure").constData()) << type << 0 << 0 << 0 << 101;
        }
        QTest::newRow("release-pressure") << 0x21 << 0 << 0 << 0 << 1;
    }
    void malformedPenIgnored() {
        QFETCH(int, type); QFETCH(int, id); QFETCH(int, x); QFETCH(int, y); QFETCH(int, pressure);
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy presses(&client, &rmweb::QtfbClient::penPressed);
        QSignalSpy moves(&client, &rmweb::QtfbClient::penMoved);
        QSignalSpy releases(&client, &rmweb::QtfbClient::penReleased);
        QSignalSpy fingers(&client, &rmweb::QtfbClient::touchPressed);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        QByteArray input = packet(4);
        put32(input, 8, type); put32(input, 12, id);
        put32(input, 16, x); put32(input, 20, y); put32(input, 24, pressure);
        QVERIFY(server.send(input));
        // A subsequent finger packet proves the preceding packet was consumed.
        input = packet(4);
        put32(input, 8, 0x10); put32(input, 12, 7);
        put32(input, 16, 2); put32(input, 20, 1);
        QVERIFY(server.send(input));
        QTRY_COMPARE_WITH_TIMEOUT(fingers.size(), 1, 500);
        QCOMPARE(fingers.first(), QVariantList({7, 2, 1}));
        QCOMPARE(presses.size(), 0);
        QCOMPARE(moves.size(), 0);
        QCOMPARE(releases.size(), 0);
        QVERIFY(client.isReady());
    }

    void virtualKeyboardUsesLayoutCodesOutsidePixelBounds() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy keys(&client, &rmweb::QtfbClient::keyEvent);
        QSignalSpy touches(&client, &rmweb::QtfbClient::touchPressed);
        QVERIFY(server.initialize(client)); // 3x2: codes are not pixel coordinates.
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        // Independent values from v0.5.3 default.layout.json. common.h's
        // special-key constants omit Backspace and are wrong after Delete.
        const QList<int> codes {9, 13, 27, 32, 33, 34, 35, 36, 37, 38, 39, 40,
            41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
            57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72,
            73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88,
            89, 90, 91, 92, 93, 94, 95, 96, 123, 124, 125, 126, 127, 128,
            129, 130, 131, 132, 133, 134, 135, 136};
        QList<QVariantList> expected;
        QList<int> packets;
        for (int mask = 0; mask <= 7; ++mask) {
            for (int code : codes) packets.append(code | (mask << 20));
            if (mask) packets.append(mask << 20);
        }
        for (int code : packets) {
            for (bool pressed : {true, false}) {
                QByteArray input = packet(4);
                put32(input, 8, pressed ? 0x40 : 0x41);
                put32(input, 16, code);
                QVERIFY(server.send(input));
                expected.append(QVariantList({code, pressed}));
                if (expected.size() % 32 == 0)
                    QTRY_COMPARE_WITH_TIMEOUT(keys.size(), expected.size(), 500);
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(keys.size(), expected.size(), 500);
        for (qsizetype i = 0; i < expected.size(); ++i) QCOMPARE(keys.at(i), expected.at(i));
        QCOMPARE(touches.size(), 0);
        QVERIFY(client.isReady());
    }
    void malformedKeyboardIgnored_data() {
        QTest::addColumn<int>("code");
        QTest::addColumn<int>("id");
        QTest::addColumn<int>("y");
        QTest::addColumn<int>("d");
        for (int code : {0, 8, 10, 26, 28, 31, 97, 122, 137, -1, 0x800000,
                         0x100100, 0x700008})
            QTest::newRow(qPrintable(QStringLiteral("code-%1").arg(code))) << code << 0 << 0 << 0;
        QTest::newRow("negative-device") << 65 << -1 << 0 << 0;
        QTest::newRow("unexpected-device") << 65 << 1 << 0 << 0;
        QTest::newRow("unexpected-y") << 65 << 0 << 1 << 0;
        QTest::newRow("unexpected-d") << 65 << 0 << 0 << 1;
    }
    void malformedKeyboardIgnored() {
        QFETCH(int, code); QFETCH(int, id); QFETCH(int, y); QFETCH(int, d);
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy keys(&client, &rmweb::QtfbClient::keyEvent);
        QSignalSpy touches(&client, &rmweb::QtfbClient::touchPressed);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        for (int type : {0x40, 0x41}) {
            QByteArray input = packet(4);
            put32(input, 8, type); put32(input, 12, id);
            put32(input, 16, code); put32(input, 20, y); put32(input, 24, d);
            QVERIFY(server.send(input));
        }
        QByteArray touch = packet(4);
        put32(touch, 8, 0x10); put32(touch, 12, 7); put32(touch, 16, 2); put32(touch, 20, 1);
        QVERIFY(server.send(touch));
        QTRY_COMPARE_WITH_TIMEOUT(touches.size(), 1, 500);
        QCOMPARE(touches.first(), QVariantList({7, 2, 1}));
        QCOMPARE(keys.size(), 0);
        QVERIFY(client.isReady());
    }
    void closeFromKeyboardSignalIsSafe() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy keys(&client, &rmweb::QtfbClient::keyEvent);
        QSignalSpy closed(&client, &rmweb::QtfbClient::connectionClosed);
        QObject::connect(&client, &rmweb::QtfbClient::keyEvent, &client,
                         [&client](int, bool) { client.close(); });
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        QByteArray input = packet(4);
        put32(input, 8, 0x40); put32(input, 16, 65);
        QVERIFY(server.send(input));
        put32(input, 8, 0x41);
        QVERIFY(server.send(input));
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 500);
        QCOMPARE(keys.size(), 1);
        QVERIFY(!client.isReady());
    }

    void malformedReplies_data() {
        QTest::addColumn<QByteArray>("reply");
        QTest::newRow("short") << QByteArray(31, '\0');
        QTest::newRow("oversized") << QByteArray(33, '\0');
        QTest::newRow("input-before-init") << packet(4);
        QTest::newRow("unknown-before-init") << packet(99);
        QByteArray bad = packet(0);
        put32(bad, 8, -1);
        QTest::newRow("invalid-shm-key") << bad;
        put32(bad, 8, 12345);
        QTest::newRow("zero-shm-size") << bad;
        const quint64 huge = 1024ull * 1024 * 1024;
        memcpy(bad.data() + 16, &huge, sizeof huge);
        QTest::newRow("oversized-shm") << bad;
    }
    void malformedReplies() {
        QFETCH(QByteArray, reply);
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QSignalSpy closed(&client, &rmweb::QtfbClient::connectionClosed);
        QVERIFY(client.start(42, server.path, QSize(3, 2)));
        QVERIFY(server.acceptClient());
        QCOMPARE(server.read().size(), 24);
        QVERIFY(server.send(reply));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 500);
        QCOMPARE(closed.size(), 1);
        QVERIFY(!client.isReady());
        QVERIFY(!client.submitImage(QImage(3, 2, QImage::Format_RGB32)));
        QVERIFY(!client.start(42, server.path)); // No implicit retry after failure.
    }

    void malformedReadyPackets_data() {
        QTest::addColumn<QByteArray>("reply");
        QTest::newRow("duplicate-init") << packet(0);
        QTest::newRow("unknown") << packet(99);
        QByteArray bad = packet(7);
        put32(bad, 12, 4);
        QTest::newRow("invalid-rotation") << bad;
        put32(bad, 12, 0); put32(bad, 8, 1);
        QTest::newRow("invalid-state-reason") << bad;
    }
    void malformedReadyPackets() {
        QFETCH(QByteArray, reply);
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        QVERIFY(server.send(reply));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 500);
        QVERIFY(!client.isReady());
    }

    void invalidSharedMemory_data() {
        QTest::addColumn<qint64>("actualSize");
        QTest::newRow("truncated") << qint64(23);
        QTest::newRow("oversized") << qint64(25);
    }
    void invalidSharedMemory() {
        QFETCH(qint64, actualSize);
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QVERIFY(client.start(42, server.path, QSize(3, 2)));
        QVERIFY(server.acceptClient());
        QCOMPARE(server.read().size(), 24);
        QVERIFY(server.send(server.init(QSize(3, 2), -1, actualSize)));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 500);
        QVERIFY(!client.isReady());
    }

    void missingSharedMemoryIsRejected() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QVERIFY(client.start(42, server.path, QSize(3, 2)));
        QVERIFY(server.acceptClient());
        QCOMPARE(server.read().size(), 24);
        QByteArray reply = server.init(QSize(3, 2));
        QVERIFY(shm_unlink(server.shmName.constData()) == 0);
        QVERIFY(server.send(reply));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 500);
        QVERIFY(!client.isReady());
    }

    void closeFromInitializedSignalIsSafe() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy closed(&client, &rmweb::QtfbClient::connectionClosed);
        connect(&client, &rmweb::QtfbClient::initialized, &client, &rmweb::QtfbClient::close);
        QVERIFY(server.initialize(client));
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 500);
        QVERIFY(!client.isReady());
        const QByteArray terminate = server.read();
        QCOMPARE(terminate.size(), 24);
        QCOMPARE(terminate[0], char(3));
    }

    void closeFromFrameSignalIsSafe() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy closed(&client, &rmweb::QtfbClient::connectionClosed);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        connect(&client, &rmweb::QtfbClient::frameSubmitted, &client, &rmweb::QtfbClient::close);
        QImage image(3, 2, QImage::Format_RGB32);
        image.fill(Qt::white);
        QVERIFY(!client.submitImage(image)); // The callback closed it before return.
        QCOMPARE(closed.size(), 1);
        QVERIFY(!client.isReady());
    }

    void handshakeTimeoutKeepsEventLoopResponsive() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY(client.start(42, server.path));
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(server.acceptClient());
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start();
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 2000);
        QVERIFY(ticks > 50);
        QVERIFY(elapsed.elapsed() >= 1000 && elapsed.elapsed() < 2200);
    }

    void pendingFrameCoalescesAfterBackpressure() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy sent(&client, &rmweb::QtfbClient::frameSubmitted);
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        QImage image(3, 2, QImage::Format_RGBA8888);
        image.fill(Qt::white);
        for (int i = 0; i < 2048; ++i) QVERIFY(client.submitImage(image));
        QVERIFY(sent.size() > 0 && sent.size() < 2048);
        const int delivered = sent.size();
        image.fill(QColor(9, 8, 7, 255));
        QVERIFY(client.submitImage(image));
        while (!server.read().isEmpty()) {}
        QTRY_COMPARE_WITH_TIMEOUT(sent.size(), delivered + 1, 500);
        QCOMPARE(server.read().size(), 24);
        char bytes[4]{};
        QCOMPARE(pread(server.shm, bytes, sizeof bytes, 0), ssize_t(4));
        QCOMPARE(QByteArray(bytes, 4), QByteArray::fromHex("090807ff"));
        QCOMPARE(errors.size(), 0);
    }

    void stalledPeerHasBoundedDeadline() {
        Server server;
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QVERIFY(server.initialize(client));
        QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
        QImage image(3, 2, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int i = 0; i < 2048; ++i) QVERIFY(client.submitImage(image));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 2000);
        QVERIFY(!client.isReady());
    }

    void disconnectIsCleanAndReleasesDescriptors() {
        const int before = QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        for (int i = 0; i < 12; ++i) {
            Server server;
            rmweb::QtfbClient client;
            QSignalSpy closed(&client, &rmweb::QtfbClient::connectionClosed);
            QSignalSpy errors(&client, &rmweb::QtfbClient::error);
            QVERIFY(server.initialize(client));
            QTRY_VERIFY_WITH_TIMEOUT(client.isReady(), 500);
            ::close(server.peer); server.peer = -1;
            QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 500);
            client.close();
            QCOMPARE(closed.size(), 1);
            QCOMPARE(errors.size(), 0);
        }
        QCOMPARE(QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), before);
    }

    void missingServerAndBadArgumentsFailImmediately() {
        QTemporaryDir directory;
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        QVERIFY(!client.start(1, directory.path() + QStringLiteral("/missing")));
        QCOMPARE(errors.size(), 1);
        for (const QSize size : {QSize(0, 10), QSize(1621, 10), QSize(10, 2161)}) {
            rmweb::QtfbClient invalid;
            QVERIFY(!invalid.start(1, directory.path(), size));
        }
        rmweb::QtfbClient invalidKey;
        QVERIFY(!invalidKey.start(-1));
        rmweb::QtfbClient invalidPath;
        QVERIFY(!invalidPath.start(1, QString(200, QLatin1Char('/'))));
    }

    void environmentKeyIsStrict_data() {
        QTest::addColumn<QByteArray>("key");
        for (const char *value : {"", "-1", " 12", "+12", "12x", "99999999999", "2147483648"})
            QTest::newRow(value[0] ? value : "empty") << QByteArray(value);
    }
    void environmentKeyIsStrict() {
        QFETCH(QByteArray, key);
        const QByteArray previous = qgetenv("QTFB_KEY");
        const bool existed = qEnvironmentVariableIsSet("QTFB_KEY");
        qputenv("QTFB_KEY", key);
        rmweb::QtfbClient client;
        QSignalSpy errors(&client, &rmweb::QtfbClient::error);
        const bool started = client.startFromEnvironment();
        if (existed) qputenv("QTFB_KEY", previous); else qunsetenv("QTFB_KEY");
        QVERIFY(!started);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("QTFB_KEY")));
    }
};

QTEST_GUILESS_MAIN(QtfbClientTest)
#include "qtfb_client_test.moc"
