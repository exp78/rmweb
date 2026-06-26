// engine/wpeqt/main.cpp — Phase 4: WPE WebKit -> Qt6 -> (offscreen PNG | epaper e-ink) + paged reading.
//
// WpeEngine drives WPE WebKit headless on its own worker thread (own GMainContext + GMainLoop) and on each
// buffer-rendered deep-copies the BGRA buffer into a QImage emitted to the GUI thread. Scrolling = PAGED:
// a swipe runs window.scrollBy(0, ±one screen) in the page and WebKit repaints; we show the new frame.
//
// Correctness rules verified against WPE 2.48.5 source — docs/research/wpe-rendering-protocol.md:
//   1. The view must be MAPPED or WebKit suspends painting (IsVisible tracks wpe_view_get_mapped()). Resize
//      the toplevel first (headless default 0x0), then force set_visible(FALSE)->(TRUE), then verify mapped.
//   2. NEVER call wpe_view_buffer_released()/_rendered() with an embedded WebKitWebView — double-free crash.
//   3. Scrolled frames need single-threaded CPU Skia (launcher: WEBKIT_SKIA_CPU_PAINTING_THREADS=0).
//
// INPUT — verified against the epaper QPA + Qt source (docs/research/remarkable-touch-input.md): the epaper
// QPA posts finger touch with a NULL target window, so Qt drops it (no QTouchEvent/QMouseEvent ever reaches a
// QtQuick item), and that same null-window path crashes WebKit on touch. So we read the finger digitizer
// directly from evdev (TouchReader: node "Elan touch input" = event3, EVIOCGRAB'd — the grab also silences the
// QPA's broken touch dispatch, fixing the crash) and detect page-turn swipes ourselves.
//
//   * save mode  (argv[2] = out.png): save the 2nd painted frame and quit  — headless proof.
//   * display mode (no argv[2])     : show the page; swipe up = next page, swipe down = previous.
//
// Timing: every milestone logs "[t] ... @Xms" (ms since engine start) so we can see where time goes.
// NOTE: launcher sets JSC_useJIT=0 (interpreter) — a JS page segfaulted with the JIT on this device.
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
#include <jsc/jsc.h>
#include <glib.h>

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <execinfo.h>

// Finger digitizer raw range (Elan, verified on device) -> 1620x2160 panel; swipe thresholds in panel px.
static const int kPanelW = 1620, kPanelH = 2160, kTouchRawW = 2064, kTouchRawH = 2832;
static const int kSwipeMinDy = 240;   // vertical travel to count as a page turn (~11% of height)
static const int kSwipeMaxDx = 200;   // keep the gesture roughly vertical
static const double kPageStepPx = 2000.0;   // ~one screen of scroll per page turn (with a little overlap)

// Milliseconds elapsed since a monotonic timestamp (for the "[t] ... @Xms" instrumentation).
static inline double msSince(gint64 us) { return (g_get_monotonic_time() - us) / 1000.0; }

// Print a native backtrace on a fatal signal (to stderr -> the persistent device log), then re-raise.
// Our binary is unstripped, so addr2line on build/rmweb-wpeqt resolves the rmweb frames.
extern "C" void crashHandler(int sig) {
    void *bt[64];
    const int n = backtrace(bt, 64);
    fprintf(stderr, "\n[CRASH] signal %d — backtrace (%d frames):\n", sig, n);
    backtrace_symbols_fd(bt, n, fileno(stderr));
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

// A small self-contained test page (no network needed; HTTPS waits on glib-networking). Taller than the
// 2160 px viewport so there is something to page through; the lines are injected by JS to also exercise JSC.
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
    "<p>Rendered by WPE (Skia CPU) on a Qt worker thread, software GL. aarch64. Swipe up for next page.</p>"
    "<div class='box r'>RED</div><div class='box g'>GREEN</div>"
    "<div id='lines'></div>"
    "<script>var h='';for(var i=1;i<=60;i++){h+='<p>Line '+i+' &mdash; the quick brown fox jumps over the lazy dog. 0123456789.</p>';}"
    "document.getElementById('lines').innerHTML=h;</script>"
    "</body></html>";

// ---------------------------------------------------------------------------
// WpeEngine — owns all WPE/WebKit objects on its worker thread.
// ---------------------------------------------------------------------------
class WpeEngine : public QObject {
    Q_OBJECT
public:
    WpeEngine(QString url, int w, int h)
        : m_url(std::move(url)), m_w(w), m_h(h),
          m_ctx(g_main_context_new()), m_loop(g_main_loop_new(m_ctx, FALSE)) {}

Q_SIGNALS:
    void frameReady(const QImage &img, int frame);

public Q_SLOTS:
    void start() {
        g_main_context_push_thread_default(m_ctx);
        m_startUs = g_get_monotonic_time();

        GError *err = nullptr;
        WPEDisplay *display = wpe_display_headless_new();
        if (!display || !wpe_display_connect(display, &err)) {
            qWarning() << "[wpe] display connect failed:" << (err ? err->message : "?");
            g_clear_error(&err);
            return;
        }
        qInfo("[t] display connected @%.0fms", msSince(m_startUs));

        m_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, nullptr));
        WPEView *wpeView = webkit_web_view_get_wpe_view(m_view);
        // Size the toplevel first (headless default is 0x0 -> empty paints), then force a real
        // visible FALSE->TRUE transition so the view MAPS (WebKit only keeps painting while mapped).
        if (WPEToplevel *top = wpe_view_get_toplevel(wpeView))
            wpe_toplevel_resize(top, m_w, m_h);
        wpe_view_resized(wpeView, m_w, m_h);
        g_signal_connect(wpeView, "buffer-rendered", G_CALLBACK(&WpeEngine::onBuffer), this);
        wpe_view_set_visible(wpeView, FALSE);
        wpe_view_set_visible(wpeView, TRUE);
        qInfo("[t] view mapped=%d size=%dx%d @%.0fms", wpe_view_get_mapped(wpeView),
              wpe_view_get_width(wpeView), wpe_view_get_height(wpeView), msSince(m_startUs));
        g_signal_connect(m_view, "load-changed", G_CALLBACK(&WpeEngine::onLoadChanged), this);

        if (m_url.isEmpty())
            webkit_web_view_load_html(m_view, kTestPage, nullptr);
        else
            webkit_web_view_load_uri(m_view, m_url.toUtf8().constData());
        qInfo("[t] load dispatched @%.0fms", msSince(m_startUs));

        g_main_loop_run(m_loop);  // pumps WPE on this thread until stop()
    }

    void stop() {
        g_main_context_invoke(m_ctx, [](gpointer l) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(l)); return G_SOURCE_REMOVE; }, m_loop);
    }

    // Scroll by dy device px (called from the GUI thread on a swipe). Marshalled onto the worker thread;
    // WebKit repaints at the new offset (mapped view + single-threaded Skia) and clamps the scroll for us.
    // m_ctx is valid from the ctor, so this is safe to call cross-thread.
    void pageBy(double dy) {
        auto *msg = new PageMsg{ this, dy };
        g_main_context_invoke_full(m_ctx, G_PRIORITY_DEFAULT, &WpeEngine::onPage, msg,
                                   [](gpointer d) { delete static_cast<PageMsg*>(d); });
    }

private:
    struct PageMsg { WpeEngine *self; double dy; };
    static gboolean onPage(gpointer d) {
        auto *m = static_cast<PageMsg*>(d);
        WpeEngine *self = m->self;
        if (self->m_view) {
            self->m_pageUs = g_get_monotonic_time();
            // Scroll, then force an immediate repaint: a bare scroll changes scrollY but does NOT emit a
            // buffer (the screen would only update on the next ~6 s periodic render). A tiny DOM mutation
            // forces WebKit to repaint the viewport now. We reuse one hidden marker node (no accumulation).
            gchar *js = g_strdup_printf(
                "window.scrollBy(0,%d);"
                "var m=document.getElementById('__r')||document.body.appendChild(document.createElement('span'));"
                "m.id='__r';m.textContent=((+m.textContent||0)+1);window.scrollY",
                static_cast<int>(m->dy));
            webkit_web_view_evaluate_javascript(self->m_view, js, -1, nullptr, nullptr, nullptr,
                                                &WpeEngine::onJsDone, self);
            g_free(js);
            qInfo("[t] pageBy(%d) @%.0fms", static_cast<int>(m->dy), msSince(self->m_startUs));
        }
        return G_SOURCE_REMOVE;
    }

    static void onJsDone(GObject *obj, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        GError *err = nullptr;
        JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
        double scrollY = -1;
        if (v) { if (jsc_value_is_number(v)) scrollY = jsc_value_to_double(v); g_object_unref(v); }
        else g_clear_error(&err);
        if (self) qInfo("[t] page JS done scrollY=%.0f @%.0fms", scrollY, msSince(self->m_startUs));
    }

    static void onLoadChanged(WebKitWebView *, WebKitLoadEvent ev, gpointer data) {
        if (ev != WEBKIT_LOAD_FINISHED) return;
        auto *self = static_cast<WpeEngine*>(data);
        qInfo("[t] load finished @%.0fms", msSince(self->m_startUs));
    }

    static void onBuffer(WPEView *, WPEBuffer *buffer, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        const gint64 tIn = g_get_monotonic_time();
        const int w = wpe_buffer_get_width(buffer);
        const int h = wpe_buffer_get_height(buffer);
        if (w <= 0 || h <= 0) return;
        self->m_frames++;

        // Read the SHM buffer directly: wpe_buffer_import_to_pixels() returns a garbage size on
        // incrementally-rendered (scrolled) frames, whereas the SHM getters give the real data + row
        // stride. wpe_buffer_shm_get_data is (transfer none) — owned by the buffer, valid for this
        // callback only; copy now, do NOT unref. We also never release the buffer (rule 2).
        if (!WPE_IS_BUFFER_SHM(buffer)) {
            qWarning("[t] onBuffer #%d: non-SHM (%s) — skipped", self->m_frames, G_OBJECT_TYPE_NAME(buffer));
            return;
        }
        WPEBufferSHM *shm = WPE_BUFFER_SHM(buffer);
        const int stride = static_cast<int>(wpe_buffer_shm_get_stride(shm));
        gsize size = 0;
        GBytes *bytes = wpe_buffer_shm_get_data(shm);
        const uchar *pix = bytes ? static_cast<const uchar*>(g_bytes_get_data(bytes, &size)) : nullptr;
        if (!pix || stride < w * 4 || size < static_cast<gsize>(stride) * static_cast<gsize>(h)) {
            qWarning("[t] onBuffer #%d: bad SHM geom stride=%d size=%zu — skipped", self->m_frames,
                     stride, static_cast<size_t>(size));
            return;
        }
        // WPE SHM memory order is B,G,R,A (ARGB8888 little-endian) == QImage::Format_ARGB32.
        QImage img(pix, w, h, stride, QImage::Format_ARGB32);
        Q_EMIT self->frameReady(img.copy(), self->m_frames);

        const double dt   = self->m_lastBufUs ? (tIn - self->m_lastBufUs) / 1000.0 : 0.0;
        const double flip = self->m_pageUs    ? (tIn - self->m_pageUs)    / 1000.0 : -1.0;
        qInfo("[t] frame %d @%.0fms  build=%.1fms  dt=%.1fms  flip-latency=%.1fms  %dx%d",
              self->m_frames, msSince(self->m_startUs), msSince(tIn), dt, flip, w, h);
        self->m_lastBufUs = tIn;
        self->m_pageUs = 0;
    }

    QString m_url;
    int m_w, m_h, m_frames = 0;
    GMainContext *m_ctx = nullptr;
    GMainLoop *m_loop = nullptr;
    WebKitWebView *m_view = nullptr;
    gint64 m_startUs = 0;     // monotonic origin (set in start) — all "@Xms" timings are relative to it
    gint64 m_lastBufUs = 0;   // previous buffer-rendered time — gives the inter-frame interval
    gint64 m_pageUs = 0;      // last page-flip dispatch time — gives swipe -> rendered-frame latency
};

// ---------------------------------------------------------------------------
// WpeView — a full-screen QtQuick item that just paints the latest WPE frame (input comes from TouchReader,
// not Qt: the epaper QPA does not deliver finger touch to QtQuick items here).
// ---------------------------------------------------------------------------
class WpeView : public QQuickPaintedItem {
    Q_OBJECT
public:
    explicit WpeView(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent) {}
    void paint(QPainter *p) override {
        if (m_img.isNull()) return;
        const gint64 t = g_get_monotonic_time();
        p->drawImage(QRectF(0, 0, width(), height()), m_img);
        qInfo("[t][gui] paint drawImage %.1fms", (g_get_monotonic_time() - t) / 1000.0);
    }
public Q_SLOTS:
    void setImage(const QImage &img) { m_img = img; update(); }
private:
    QImage m_img;
};

// ---------------------------------------------------------------------------
// TouchReader — reads the finger digitizer straight from evdev on its own thread (the epaper QPA drops touch
// into a null window, so Qt never delivers it, and that path crashes WebKit). Resolves the node by NAME
// ("Elan touch input"), EVIOCGRABs it (the grab also silences the QPA's broken touch dispatch), decodes
// kernel multitouch Protocol-B for the first finger, and emits swipe(+1 = next page / -1 = previous).
// See docs/research/remarkable-touch-input.md.
// ---------------------------------------------------------------------------
class TouchReader : public QObject {
    Q_OBJECT
public:
    void requestStop() { m_stop.store(true); }
Q_SIGNALS:
    void swipe(int dir);
public Q_SLOTS:
    void run() {
        int fd = openByName("Elan touch input");
        if (fd < 0) { qWarning("[touch] 'Elan touch input' node not found"); return; }
        if (ioctl(fd, EVIOCGRAB, reinterpret_cast<void*>(1)) != 0)
            qWarning("[touch] EVIOCGRAB failed (device held elsewhere)");
        else
            qInfo("[touch] grabbed 'Elan touch input' — reading finger touch directly");

        int curSlot = 0, x = 0, y = 0, sx = 0, sy = 0;
        bool down = false;
        struct input_event ev[64];
        while (!m_stop.load()) {
            struct pollfd pfd { fd, POLLIN, 0 };
            if (poll(&pfd, 1, 200) <= 0) continue;
            const ssize_t n = read(fd, ev, sizeof ev);
            if (n < static_cast<ssize_t>(sizeof(struct input_event))) continue;
            for (size_t i = 0; i < n / sizeof(struct input_event); ++i) {
                const struct input_event &p = ev[i];
                if (p.type != EV_ABS) continue;
                if (p.code == ABS_MT_SLOT) { curSlot = p.value; continue; }
                if (curSlot != 0) continue;                                  // first finger only
                if (p.code == ABS_MT_POSITION_X)      x = p.value * kPanelW / kTouchRawW;
                else if (p.code == ABS_MT_POSITION_Y) y = p.value * kPanelH / kTouchRawH;
                else if (p.code == ABS_MT_TRACKING_ID) {
                    if (p.value >= 0) { down = true; sx = x; sy = y; }       // new contact -> down
                    else { if (down) emitSwipe(x - sx, y - sy); down = false; }   // -1 -> lifted
                }
            }
        }
        ioctl(fd, EVIOCGRAB, reinterpret_cast<void*>(0));
        close(fd);
    }
private:
    static int openByName(const char *want) {
        DIR *dir = opendir("/dev/input");
        if (!dir) return -1;
        struct dirent *e; char path[64], name[256]; int found = -1;
        while ((e = readdir(dir))) {
            if (strncmp(e->d_name, "event", 5) != 0) continue;
            snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            name[0] = 0;
            if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strcmp(name, want) == 0) { found = fd; break; }
            close(fd);
        }
        closedir(dir);
        return found;
    }
    void emitSwipe(int dx, int dy) {
        if (qAbs(dx) >= kSwipeMaxDx) return;                 // too diagonal
        if (qAbs(dy) < kSwipeMinDy) return;                  // too short
        const gint64 now = g_get_monotonic_time();
        if (m_lastSwipeUs && now - m_lastSwipeUs < 800000) return;   // debounce: <=1 turn / 0.8s
        m_lastSwipeUs = now;
        if (dy < 0) { qInfo("[touch] swipe up -> next");   Q_EMIT swipe(+1); }
        else        { qInfo("[touch] swipe down -> prev"); Q_EMIT swipe(-1); }
    }
    std::atomic<bool> m_stop { false };
    gint64 m_lastSwipeUs = 0;
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
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    QGuiApplication app(argc, argv);
    const QString url      = (argc > 1) ? QString::fromUtf8(argv[1]) : QString();
    const QString savePath = (argc > 2) ? QString::fromUtf8(argv[2]) : QString();

    QThread thread;
    WpeEngine engine(url, 1620, 2160);
    engine.moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, &engine, &WpeEngine::start);

    QThread touchThread;
    TouchReader touchReader;

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
            const gint64 t = g_get_monotonic_time();
            view->setImage(img);
            qInfo("[t][gui] frame %d -> setImage %.1fms  %dx%d", frame,
                  (g_get_monotonic_time() - t) / 1000.0, img.width(), img.height());
        });

        // Direct evdev touch -> page turns (queued onto the GUI thread; pageBy then marshals to the worker).
        touchReader.moveToThread(&touchThread);
        QObject::connect(&touchThread, &QThread::started, &touchReader, &TouchReader::run);
        QObject::connect(&touchReader, &TouchReader::swipe, &app, [&engine](int dir) {
            engine.pageBy(dir > 0 ? kPageStepPx : -kPageStepPx);
        });
        touchThread.start();
    }

    thread.start();
    const int rc = app.exec();
    touchReader.requestStop();
    touchThread.quit();
    touchThread.wait(1000);
    engine.stop();
    thread.quit();
    thread.wait(3000);
    return rc;
}
