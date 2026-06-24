// rmweb display spike (QML) — present a test pattern on the Paper Pro e-ink via the
// epaper QtQuick scenegraph (QsgEpaperPlugin, key "epaper").
//
// Findings that shaped this (see docs/research-reuse.md):
//  - QtQuick ONLY (QRasterWindow/Widgets never reach the panel — no sendUpdate path).
//  - Size the Window via Screen.width/height (the official recipe). Forcing geometry
//    from C++ after creation made the first frame 0x0, then only a partial update —
//    which on the color (Gallery 3/ACeP2) panel left the screen white with a fragment.
//  - The scenegraph auto-calls EPFrameBuffer::sendUpdate per dirty region, but color
//    content needs a FULL refresh; EPFrameBuffer::setForceFull(true) is the lever.
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QTimer>
#include <QDebug>
#include <dlfcn.h>

// libqsgepaper is already loaded (QT_QUICK_BACKEND=epaper); resolve EPFrameBuffer's
// static methods by symbol. Harmless no-op if not exported. Mangled (Itanium) names:
//   EPFrameBuffer::setForceFull(bool) -> _ZN12EPFrameBuffer12setForceFullEb
static void epaperForceFull() {
    using BoolFn = void (*)(bool);
    auto setForceFull =
        reinterpret_cast<BoolFn>(dlsym(RTLD_DEFAULT, "_ZN12EPFrameBuffer12setForceFullEb"));
    qInfo() << "[epfb] setForceFull symbol:" << reinterpret_cast<void *>(setForceFull);
    if (setForceFull) setForceFull(true);
}

static const char *kQml = R"QML(
import QtQuick
import QtQuick.Window
Window {
    id: root
    width: Screen.width
    height: Screen.height
    visible: true
    color: "white"

    // Grayscale bars (left half) — verify distinct e-ink levels.
    Column {
        Repeater {
            model: 16
            Rectangle {
                width: root.width / 2
                height: root.height / 16
                color: Qt.rgba(index / 15, index / 15, index / 15, 1)
            }
        }
    }

    // Border — verify full coverage / geometry.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: "black"
        border.width: 6
    }

    // Unmistakable text so it can't be confused with the xochitl UI.
    Text {
        anchors.centerIn: parent
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: 72
        color: "black"
        text: "RMWEB DISPLAY OK\n" + root.width + " x " + root.height + "\nepaper QML"
    }

    // Dirty the scene shortly after show so the scenegraph emits a fresh full update
    // (full because of setForceFull) over real content.
    property bool nudged: false
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: 90; height: 90
        color: root.nudged ? "black" : "white"
    }
    Timer { interval: 400; running: true; onTriggered: root.nudged = true }

    Component.onCompleted: console.log("[qml] size", root.width, "x", root.height,
                                       "Screen", Screen.width, Screen.height)
}
)QML";

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);

    QQmlEngine engine;
    QQmlComponent comp(&engine);
    comp.setData(kQml, QUrl(QStringLiteral("inline.qml")));
    if (comp.isError()) {
        qWarning() << "[qml] errors:" << comp.errorString();
        return 2;
    }
    QObject *root = comp.create();
    if (!root) {
        qWarning() << "[qml] create() returned null";
        return 3;
    }

    if (auto *w = qobject_cast<QQuickWindow *>(root)) {
        qInfo() << "[main] window size" << w->size();
        auto *frames = new int(0);
        QObject::connect(w, &QQuickWindow::frameSwapped, w, [frames, w] {
            if ((*frames)++ < 4) qInfo() << "[main] frameSwapped" << *frames << w->size();
        });
    }

    // Force full-refresh once the window is up so color content develops on the panel.
    QTimer::singleShot(100, &app, epaperForceFull);

    const int seconds = (argc > 1) ? QString::fromLatin1(argv[1]).toInt() : 20;
    QTimer::singleShot(seconds * 1000, &app, &QGuiApplication::quit);
    return app.exec();
}
