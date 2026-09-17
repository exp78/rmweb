// Exercise the actual upstream socket loop and actual QtQuick framebuffer item.
// Each scenario is a separate bounded process, so a baseline deadlock cannot
// strand a test runner. No global listener, device nodes or stock UI are used.
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QPaintEngine>
#include <QPaintDevice>
#include <QThread>
#include <atomic>
#include <csignal>
#include <functional>
#include <iostream>
#include <thread>
#include "fbmanagement.cpp"

#define REQUIRE(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition << std::endl; std::_Exit(2); \
} } while (false)

static bool waitFor(const std::function<bool()> &predicate, bool events = true) {
    QElapsedTimer clock; clock.start();
    while (!predicate() && clock.elapsed() < 1200) {
        if (events) QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

struct Session {
    int peer = -1;
    int shmKey = -1;
    std::atomic<bool> done{false};
    std::thread worker;
    explicit Session(int key) {
        int fds[2]; REQUIRE(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);
        peer = fds[0];
        timeval timeout{1, 0};
        REQUIRE(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == 0);
        worker = std::thread([this, fd = fds[1]] { managementClientThread(fd); done = true; });
        qtfb::ClientMessage message{};
        message.type = MESSAGE_CUSTOM_INITIALIZE;
        message.customInit.framebufferKey = key;
        message.customInit.framebufferType = FBFMT_RMPP_RGBA8888;
        message.customInit.width = 32; message.customInit.height = 48;
        sendMessage(message);
        qtfb::ServerMessage response{};
        REQUIRE(recv(peer, &response, sizeof response, 0) == sizeof response);
        REQUIRE(response.type == MESSAGE_INITIALIZE && response.init.shmSize == 32 * 48 * 4);
        shmKey = response.init.shmKeyDefined;
    }
    void sendMessage(const qtfb::ClientMessage &message) {
        REQUIRE(send(peer, &message, sizeof message, MSG_NOSIGNAL) == sizeof message);
    }
    void closePeer() { if (peer >= 0) { close(peer); peer = -1; } }
    void join(bool events = true) {
        REQUIRE(waitFor([&] { return done.load(); }, events));
        worker.join();
    }
    ~Session() { closePeer(); if (worker.joinable()) join(); }
};

static bool sharedMemoryExists(int key) {
    FORMAT_SHM(name, key);
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd >= 0) { close(fd); return true; }
    REQUIRE(errno == ENOENT);
    return false;
}
static void associate(FBController &controller, int key) {
    controller.setWidth(32); controller.setHeight(48);
    controller.setFramebufferID(key);
}

// QPainter passes its actual source image into this engine. Hold it while the
// GUI detaches and the worker deletes its backend; then read the retained bytes.
class BlockingDevice : public QPaintDevice {
public:
    class Engine : public QPaintEngine {
    public:
        Engine() : QPaintEngine(QPaintEngine::AllFeatures) {}
        std::atomic<bool> entered{false}, resume{false};
        QImage retained;
        bool begin(QPaintDevice *) override { return true; }
        bool end() override { return true; }
        Type type() const override { return QPaintEngine::User; }
        void updateState(const QPaintEngineState &) override {}
        void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override { REQUIRE(false); }
        void drawImage(const QRectF &, const QImage &image, const QRectF &, Qt::ImageConversionFlags) override {
            retained = image;
            entered = true;
            while (!resume.load()) QThread::msleep(1);
            REQUIRE(retained.constBits()[0] == 255);
        }
    };
    mutable Engine engine;
    QPaintEngine *paintEngine() const override { return &engine; }
    int metric(PaintDeviceMetric m) const override {
        switch (m) {
        case PdmWidth: return 32;
        case PdmHeight: return 48;
        case PdmDepth: return 32;
        case PdmDpiX: case PdmDpiY: case PdmPhysicalDpiX: case PdmPhysicalDpiY: return 96;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return 65536;
        default: return 0;
        }
    }
};

class InputController : public FBController {
public:
    using FBController::touchEvent;
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    REQUIRE(argc == 2);
    const std::string scenario(argv[1]);
    const int key = 1024 + int(getpid());
    InputController controller;
    associate(controller, key);

    if (scenario == "touch-lifecycle") {
        Session session(key);
        REQUIRE(waitFor([&] { return controller.active(); }));
        for (auto type : {QEvent::TouchBegin, QEvent::TouchCancel}) {
            const QList<QEventPoint> points = type == QEvent::TouchBegin
                ? QList<QEventPoint>{QEventPoint(7, QEventPoint::State::Pressed, {}, {})}
                : QList<QEventPoint>{};
            QTouchEvent event(type, nullptr, Qt::NoModifier, points);
            controller.touchEvent(&event);
            qtfb::ServerMessage response{};
            REQUIRE(recv(session.peer, &response, sizeof response, 0) == sizeof response);
            REQUIRE(response.type == MESSAGE_USERINPUT);
            REQUIRE(response.userInput.inputType == 0x13);
            REQUIRE(response.userInput.devId == 0 && response.userInput.x == 0
                && response.userInput.y == 0 && response.userInput.d == 0);
            if (type == QEvent::TouchBegin) {
                REQUIRE(recv(session.peer, &response, sizeof response, 0) == sizeof response);
                REQUIRE(response.type == MESSAGE_USERINPUT);
                REQUIRE(response.userInput.inputType == INPUT_TOUCH_PRESS);
                REQUIRE(response.userInput.devId == 7);
            }
        }
        QTouchEvent stationary(QEvent::TouchUpdate, nullptr, Qt::NoModifier,
            {QEventPoint(1, QEventPoint::State::Stationary, {}, {})});
        controller.touchEvent(&stationary);
        qtfb::ServerMessage unexpected{};
        REQUIRE(recv(session.peer, &unexpected, sizeof unexpected, MSG_DONTWAIT) == -1);
        REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK);
    } else if (scenario == "initial-window") {
        int activated = 0, deactivated = 0;
        QObject::connect(&controller, &FBController::activeChanged, &app, [&] {
            if (controller.active()) ++activated; else ++deactivated;
        });
        QCoreApplication::processEvents();
        // AppLoad's real window closes on an inactive notification. Registering
        // its initially empty framebuffer must not send that notification.
        REQUIRE(activated == 0 && deactivated == 0);
        Session session(key);
        REQUIRE(waitFor([&] { return controller.active(); }));
        REQUIRE(activated == 1 && deactivated == 0);
        session.closePeer(); session.join();
        REQUIRE(waitFor([&] { return !controller.active(); }));
        REQUIRE(activated == 1 && deactivated == 1);
    } else if (scenario == "pending-close") {
        Session session(key);
        REQUIRE(waitFor([&] { return controller.active(); }));
        controller.markedUpdate(); // Schedule a paint without ever painting.
        session.closePeer();
        session.join(false); // Worker must not wait for the GUI's pending paint.
        qtfb::UserInputContents input{INPUT_TOUCH_RELEASE, 1, 12, 16, 0};
        qtfb::management::forwardUserInput(key, &input); // Must not block on a leaked mutex.
        REQUIRE(waitFor([&] { return !controller.active(); }));
        REQUIRE(!sharedMemoryExists(session.shmKey));
    } else if (scenario == "retained-paint") {
        Session session(key);
        REQUIRE(waitFor([&] { return controller.active(); }));
        BlockingDevice device;
        std::thread painting([&] { QPainter painter(&device); controller.paint(&painter); });
        REQUIRE(waitFor([&] { return device.engine.entered.load(); }));
        session.closePeer(); session.join();
        REQUIRE(waitFor([&] { return !controller.active(); }));
        REQUIRE(sharedMemoryExists(session.shmKey));
        device.engine.resume = true; painting.join();
        REQUIRE(device.engine.retained.constBits()[0] == 255);
        device.engine.retained = {};
        REQUIRE(!sharedMemoryExists(session.shmKey));
    } else if (scenario == "input-empty") {
        controller.setAllowScaling(true);
        REQUIRE(controller.convertPointToQTFBPixels({7, 9}) == QPoint(7, 9));
        controller.virtualKeyboardKeyDown(65);
        Session session(key);
        REQUIRE(waitFor([&] { return controller.active(); }));
        session.closePeer(); session.join();
        REQUIRE(waitFor([&] { return !controller.active(); }));
        REQUIRE(controller.convertPointToQTFBPixels({7, 9}) == QPoint(7, 9));
        controller.virtualKeyboardKeyUp(65);
    } else if (scenario == "repeated-open") {
        for (int i = 0; i < 100; ++i) {
            Session session(key);
            REQUIRE(waitFor([&] { return controller.active(); }));
            controller.markedUpdate();
            session.closePeer(); session.join();
            REQUIRE(waitFor([&] { return !controller.active(); }));
            REQUIRE(!sharedMemoryExists(session.shmKey));
        }
    } else if (scenario == "queued-reopen") {
        Session first(key);
        REQUIRE(waitFor([&] { return controller.active(); }));
        first.closePeer(); first.join(false); // Old detach is still queued.
        Session second(key); // Same ID must bind to the current backend.
        REQUIRE(waitFor([&] { return !sharedMemoryExists(first.shmKey); }));
        REQUIRE(controller.active());
        REQUIRE(sharedMemoryExists(second.shmKey));
        second.closePeer(); second.join();
        REQUIRE(waitFor([&] { return !controller.active(); }));
        REQUIRE(!sharedMemoryExists(second.shmKey));
    } else if (scenario == "repeated-initialize") {
        Session session(key);
        REQUIRE(waitFor([&] { return controller.active(); }));
        qtfb::ClientMessage message{};
        message.type = MESSAGE_CUSTOM_INITIALIZE;
        message.customInit.framebufferKey = key;
        message.customInit.framebufferType = FBFMT_RMPP_RGBA8888;
        message.customInit.width = 32; message.customInit.height = 48;
        session.sendMessage(message);
        session.join(); // Invalid second initialization closes and removes the connection.
        REQUIRE(waitFor([&] { return !controller.active(); }));
        qtfb::UserInputContents input{INPUT_TOUCH_RELEASE, 1, 12, 16, 0};
        qtfb::management::forwardUserInput(key, &input);
        REQUIRE(!sharedMemoryExists(session.shmKey));
    } else if (scenario == "negative-key") {
        int fds[2]; REQUIRE(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);
        std::atomic<bool> done{false};
        std::thread worker([&] { managementClientThread(fds[1]); done = true; });
        qtfb::ClientMessage message{};
        message.type = MESSAGE_CUSTOM_INITIALIZE;
        message.customInit.framebufferKey = -1;
        message.customInit.framebufferType = FBFMT_RMPP_RGBA8888;
        message.customInit.width = 32; message.customInit.height = 48;
        REQUIRE(send(fds[0], &message, sizeof message, MSG_NOSIGNAL) == sizeof message);
        REQUIRE(waitFor([&] { return done.load(); }));
        worker.join();
        qtfb::ServerMessage response{};
        REQUIRE(recv(fds[0], &response, sizeof response, 0) == 0);
        close(fds[0]);
    } else if (scenario == "sigpipe") {
        signal(SIGPIPE, SIG_DFL);
        int fds[2]; REQUIRE(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);
        close(fds[0]);
        qtfb::management::ClientConnection item; item.clientFD = fds[1];
        qtfb::management::ClientBackend backend; backend.connections.push_back(&item);
        { SYNCHRONIZE; qtfb::management::connections[key] = &backend; }
        qtfb::UserInputContents input{INPUT_TOUCH_RELEASE, 1, 12, 16, 0};
        qtfb::management::forwardUserInput(key, &input);
        { SYNCHRONIZE; qtfb::management::connections.erase(key); }
        close(fds[1]);
    } else { REQUIRE(false); }
    std::cout << "PASS " << scenario << std::endl;
    return 0;
}
