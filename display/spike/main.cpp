// rmweb display spike (QML) — proves arbitrary pixels reach the Paper Pro e-ink via the
// epaper QtQuick scenegraph (QsgEpaperPlugin, key "epaper"), from a standalone app.
//
// The cure: size the Window to Screen.width/height (the official recipe). Forcing geometry
// from C++ after creation gave a 0x0 first frame and only a partial update (white screen
// with a fragment). QtQuick ONLY — a QRasterWindow never reaches the panel.
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QTimer>
#include <QDebug>

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

    Component.onCompleted: console.log("[qml] size", root.width, "x", root.height)
}
)QML";

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);

    QQmlEngine engine;
    QQmlComponent comp(&engine);
    comp.setData(kQml, QUrl(QStringLiteral("inline.qml")));
    if (comp.status() != QQmlComponent::Ready) {
        qWarning() << "[qml] component not ready:" << comp.errorString();
        return 2;
    }
    QObject *root = comp.create();
    if (!root) {
        qWarning() << "[qml] create() returned null";
        return 3;
    }
    root->setParent(&engine);  // engine owns root → correct destruction order at exit
    if (auto *w = qobject_cast<QQuickWindow *>(root))
        qInfo() << "[main] window size" << w->size();

    const int seconds = (argc > 1) ? QString::fromLatin1(argv[1]).toInt() : 20;
    QTimer::singleShot(seconds * 1000, &app, &QGuiApplication::quit);
    return app.exec();
}
