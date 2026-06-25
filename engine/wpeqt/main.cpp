// engine/wpeqt/main.cpp — Phase 3b: WPE WebKit -> Qt6 -> (offscreen PNG | epaper e-ink).
//
// WpeEngine drives WPE WebKit headless on its own worker thread (own GMainContext + GMainLoop),
// and on each buffer-rendered deep-copies the BGRA buffer into a QImage emitted to the GUI thread.
//   * save mode  (argv[2] = out.png): save the 2nd painted frame and quit  — headless proof (3b.1).
//   * display mode (no argv[2])     : paint frames into a full-screen QtQuick WpeView, shown via the
//                                     epaper QPA (3b.3). Window sized to Screen.width/height (Phase 1 cure).
#include <QGuiApplication>
#include <QThread>
#include <QImage>
#include <QTimer>
#include <QDebug>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QPainter>

#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <glib.h>

// A small self-contained test page (no network needed; HTTPS waits on glib-networking).
static const char *kTestPage =
    "<html><head><meta charset='utf-8'><style>"
    "html,body{margin:0;padding:0;font-family:sans-serif;color:#111}"
    ".bar{height:120px;background:#1565c0;color:#fff;display:flex;align-items:center;"
        "padding:0 40px;font-size:48px;font-weight:700}"
    "h1{font-size:64px;margin:40px}"
    "p{font-size:32px;margin:20px 40px}"
    ".box{width:300px;height:200px;margin:40px;border-radius:20px;display:inline-block;"
        "color:#fff;font-size:30px;padding:24px;box-sizing:border-box}"
    ".r{background:#e53935}.g{background:#2e7d32}"
    "</style></head><body>"
    "<div class='bar'>rmweb &mdash; WPE on Qt</div>"
    "<h1>Hello from WPE WebKit</h1>"
    "<p>Rendered by WPE (Skia CPU) on a Qt worker thread, software GL. aarch64.</p>"
    "<div class='box r'>RED</div><div class='box g'>GREEN</div>"
    "</body></html>";

// ---------------------------------------------------------------------------
// WpeEngine — owns all WPE/WebKit objects on its worker thread.
// ---------------------------------------------------------------------------
class WpeEngine : public QObject {
    Q_OBJECT
public:
    // m_ctx/m_loop are created here (GUI thread, before moveToThread) so stop() can read them
    // race-free; start() merely pushes the context thread-default and runs the loop on the worker.
    WpeEngine(QString url, int w, int h)
        : m_url(std::move(url)), m_w(w), m_h(h),
          m_ctx(g_main_context_new()), m_loop(g_main_loop_new(m_ctx, FALSE)) {}

Q_SIGNALS:
    void frameReady(const QImage &img, int frame);

public Q_SLOTS:
    void start() {
        g_main_context_push_thread_default(m_ctx);

        GError *err = nullptr;
        WPEDisplay *display = wpe_display_headless_new();
        if (!display || !wpe_display_connect(display, &err)) {
            qWarning() << "[wpe] display connect failed:" << (err ? err->message : "?");
            g_clear_error(&err);
            return;
        }
        qInfo() << "[wpe] headless display connected";

        m_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, nullptr));
        WPEView *wpeView = webkit_web_view_get_wpe_view(m_view);
        if (WPEToplevel *top = wpe_view_get_toplevel(wpeView))
            wpe_toplevel_resize(top, m_w, m_h);
        wpe_view_resized(wpeView, m_w, m_h);
        g_signal_connect(wpeView, "buffer-rendered", G_CALLBACK(&WpeEngine::onBuffer), this);

        if (m_url.isEmpty())
            webkit_web_view_load_html(m_view, kTestPage, nullptr);
        else
            webkit_web_view_load_uri(m_view, m_url.toUtf8().constData());

        g_main_loop_run(m_loop);  // pumps WPE on this thread until stop()
    }

    void stop() {
        // Safe to read m_ctx/m_loop from any thread (created in the constructor). invoke marshals
        // the quit onto the worker's context and wakes its loop.
        g_main_context_invoke(m_ctx, [](gpointer l) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(l)); return G_SOURCE_REMOVE; }, m_loop);
    }

private:
    static void onBuffer(WPEView *, WPEBuffer *buffer, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        const int w = wpe_buffer_get_width(buffer);
        const int h = wpe_buffer_get_height(buffer);
        if (w <= 0 || h <= 0) return;
        self->m_frames++;

        GError *err = nullptr;
        GBytes *bytes = wpe_buffer_import_to_pixels(buffer, &err);
        if (!bytes) { g_clear_error(&err); return; }
        gsize size = 0;
        const uchar *pix = static_cast<const uchar*>(g_bytes_get_data(bytes, &size));
        const int stride = static_cast<int>(size / static_cast<gsize>(h));
        // WPE buffer memory order is B,G,R,A (ARGB8888 little-endian) == QImage::Format_ARGB32.
        QImage img(pix, w, h, stride, QImage::Format_ARGB32);
        // img.copy() is an argument prvalue: fully deep-copied before frameReady is dispatched and
        // before g_bytes_unref runs — do NOT hoist the unref above this emit.
        Q_EMIT self->frameReady(img.copy(), self->m_frames);
        g_bytes_unref(bytes);
    }

    QString m_url;
    int m_w, m_h, m_frames = 0;
    GMainContext *m_ctx = nullptr;
    GMainLoop *m_loop = nullptr;
    WebKitWebView *m_view = nullptr;
};

// ---------------------------------------------------------------------------
// WpeView — a full-screen QtQuick item that paints the latest WPE frame.
// ---------------------------------------------------------------------------
class WpeView : public QQuickPaintedItem {
    Q_OBJECT
public:
    explicit WpeView(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent) {}
    void paint(QPainter *p) override {
        if (!m_img.isNull())
            p->drawImage(QRectF(0, 0, width(), height()), m_img);
    }
public Q_SLOTS:
    void setImage(const QImage &img) { m_img = img; update(); }
private:
    QImage m_img;
};

#include "main.moc"

static const char *kQml = R"QML(
import QtQuick
import QtQuick.Window
import rmweb 1.0
Window {
    width: Screen.width
    height: Screen.height
    visible: true
    color: "white"
    WpeView { objectName: "view"; anchors.fill: parent }
}
)QML";

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    const QString url      = (argc > 1) ? QString::fromUtf8(argv[1]) : QString();
    const QString savePath = (argc > 2) ? QString::fromUtf8(argv[2]) : QString();

    QThread thread;
    WpeEngine engine(url, 1620, 2160);
    engine.moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, &engine, &WpeEngine::start);

    if (!savePath.isEmpty()) {
        // --- save mode (headless proof): write the 2nd painted frame, then quit ---
        QObject::connect(&engine, &WpeEngine::frameReady, &app,
                         [savePath, &app, &engine, saved = false](const QImage &img, int frame) mutable {
            qInfo() << "[qt] frameReady" << frame << img.size();
            if (frame >= 2 && !saved) {
                saved = true;
                if (img.save(savePath)) qInfo() << "[qt] saved" << savePath;
                else                    qWarning() << "[qt] QImage::save FAILED" << savePath;
                engine.stop();
                QTimer::singleShot(300, &app, &QGuiApplication::quit);
            }
        });
    } else {
        // --- display mode: paint frames into a full-screen QtQuick item (epaper QPA) ---
        qmlRegisterType<WpeView>("rmweb", 1, 0, "WpeView");
        auto *qmlEngine = new QQmlEngine(&app);
        auto *comp = new QQmlComponent(qmlEngine, qmlEngine);
        comp->setData(kQml, QUrl(QStringLiteral("inline.qml")));
        if (comp->status() != QQmlComponent::Ready) {
            qWarning() << "[qml]" << comp->errorString();
            return 2;
        }
        QObject *root = comp->create();
        auto *view = root ? root->findChild<WpeView*>("view") : nullptr;
        if (!view) { qWarning() << "[qml] WpeView not found"; return 3; }
        root->setParent(qmlEngine);   // engine owns the QML tree -> well-defined teardown order
        QObject::connect(&engine, &WpeEngine::frameReady, view,
                         [view](const QImage &img, int frame) {
            qInfo() << "[qt] frame" << frame << img.size();
            view->setImage(img);
        });
    }

    thread.start();
    const int rc = app.exec();
    engine.stop();
    thread.quit();
    thread.wait(3000);
    return rc;
}
