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
#include <QUrl>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QPainter>
#include <QElapsedTimer>
#include <qpa/qwindowsysteminterface.h>   // QWindowSystemInterface — inject taps into QtQuick's input path

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
#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <execinfo.h>
#include <dlfcn.h>

#include "gesture.h"   // pure tap/swipe classifier (unit-tested in tests/gesture_test.cpp)
#include "url.h"       // pure URL normalizer (unit-tested in tests/url_test.cpp)
#include "tapzone.h"   // pure tap-zone classifier (unit-tested in tests/tapzone_test.cpp)
using rmweb::Gesture;
using rmweb::classifyGesture;

// Finger digitizer raw range (Elan, verified on device) -> 1620x2160 panel; swipe thresholds in panel px.
static const int kPanelW = 1620, kPanelH = 2160, kTouchRawW = 2064, kTouchRawH = 2832;
static const double kPageStepPx = 2000.0;   // ~one screen of scroll per page turn (with a little overlap)
// (swipe/tap thresholds live in gesture.h GestureParams — single source of truth)

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

// WKContentRuleList (Safari/WebKit content-blocker JSON): drop third-party scripts/media/fonts — i.e. ads,
// trackers, analytics, and other heavy cross-origin JS — so the interpreter-only JSC isn't swamped. First-
// party content/CSS is kept, so articles still render. Compiled once at startup (see onFilterSaved).
static const char *kBlockRules =
    "[{\"trigger\":{\"url-filter\":\".*\",\"resource-type\":[\"script\"],\"load-type\":[\"third-party\"]},"
       "\"action\":{\"type\":\"block\"}},"
     "{\"trigger\":{\"url-filter\":\".*\",\"resource-type\":[\"media\",\"font\"],\"load-type\":[\"third-party\"]},"
       "\"action\":{\"type\":\"block\"}}]";

// --- Reader mode (Mozilla Readability, vendored under engine/wpeqt/reader) -------------------------------
// On "Reader" we inject Readability.js + the glue below: it parses the article off a DOM *clone* (Readability
// mutates what it's given) and replaces the page with ONE clean, reflowed column styled by kReaderCss — so it
// fits the panel width with no horizontal scroll, big serif text, lots of air. Toggling off just reloads the
// original page. The vendored JS ships to the device at RMWEB_READER_DIR. See docs/research/zoom-readability.md.

// Read a whole file into a string ("" on failure) — loads the vendored JS at runtime (cached by the caller).
static std::string slurp(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string out; char buf[1 << 16]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}
static std::string readerDir() {
    const char *d = getenv("RMWEB_READER_DIR");
    return (d && *d) ? std::string(d) : std::string("/home/root/rmweb/share/reader");
}
static void replaceAll(std::string &s, const std::string &from, const std::string &to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
        s.replace(p, from.size(), to);
}
// Reader stylesheet (one line, NO double-quotes/backslashes -> safe inside the JS double-quoted string below).
// Laid out in the DPR-scaled CSS viewport (panel/dpr ~= 810px), so __FS__px is a comfortable e-ink reading size.
static const char *kReaderCss =
    "html{background:#fff;-webkit-text-size-adjust:none}body{margin:0;background:#fff}"
    "#rmweb-reader{max-width:46em;margin:0 auto;padding:1.1em 1.1em 4em;"
        "font-family:Georgia,'Times New Roman',serif;font-size:__FS__px;line-height:1.6;color:#111;"
        "word-wrap:break-word;overflow-wrap:break-word}"
    "#rmweb-reader .rmweb-title{font-size:1.5em;line-height:1.2;margin:0 0 .3em;font-weight:700}"
    "#rmweb-reader .rmweb-byline{font-size:.7em;color:#555;font-style:italic;margin:0 0 1.4em}"
    "#rmweb-reader p{margin:0 0 .9em}#rmweb-reader li{margin:.25em 0}"
    "#rmweb-reader ul,#rmweb-reader ol{margin:0 0 .9em 1.2em;padding:0}"
    "#rmweb-reader img,#rmweb-reader figure,#rmweb-reader video{max-width:100%;height:auto}"
    "#rmweb-reader figure{margin:1em 0}#rmweb-reader figcaption{font-size:.7em;color:#555;text-align:center}"
    "#rmweb-reader h1,#rmweb-reader h2,#rmweb-reader h3{line-height:1.25;margin:1.1em 0 .4em}"
    "#rmweb-reader h2{font-size:1.25em}#rmweb-reader h3{font-size:1.1em}"
    "#rmweb-reader a{color:#111;text-decoration:underline}"
    "#rmweb-reader blockquote{margin:.8em 0;padding-left:.8em;border-left:4px solid #bbb;color:#333}"
    "#rmweb-reader pre{white-space:pre-wrap;word-wrap:break-word;background:#f3f3f3;padding:.6em;font-size:.8em}"
    "#rmweb-reader code{font-family:'DejaVu Sans Mono',monospace;font-size:.85em}"
    "#rmweb-reader hr{border:none;border-top:1px solid #ccc;margin:1.2em 0}"
    "#rmweb-reader table{max-width:100%;border-collapse:collapse}";
// Glue: assumes Readability (injected before it) is in scope; returns 'ok' / 'noarticle' / 'error:...'.
static const char *kReaderGlue = R"JS(
(function(){
  try{
    if(typeof Readability!=='function') return 'noReadability';
    var art=new Readability(document.cloneNode(true)).parse();
    if(!art||!art.content) return 'noarticle';
    function esc(s){return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
    var css="__CSS__";
    var h='<div id="rmweb-reader"><h1 class="rmweb-title">'+esc(art.title||document.title)+'</h1>';
    if(art.byline) h+='<p class="rmweb-byline">'+esc(art.byline)+'</p>';
    h+='<div class="rmweb-content">'+art.content+'</div></div>';
    document.documentElement.innerHTML='<head><meta charset="utf-8"><style>'+css+'</style></head><body>'+h+'</body>';
    document.documentElement.setAttribute('data-rmweb-reader','1');
    window.scrollTo(0,0);
    return 'ok';
  }catch(e){return 'error:'+(e&&e.message?e.message:e);}
})()
)JS";

// ---------------------------------------------------------------------------
// WpeEngine — owns all WPE/WebKit objects on its worker thread.
// ---------------------------------------------------------------------------
class WpeEngine : public QObject {
    Q_OBJECT
public:
    WpeEngine(QString url, int w, int h)
        : m_url(std::move(url)), m_w(w), m_h(h),
          m_ctx(g_main_context_new()), m_loop(g_main_loop_new(m_ctx, FALSE)),
          m_cancel(g_cancellable_new()) {}

    ~WpeEngine() {
        // main() joins the worker thread before destroying us, so the loop has exited and m_view is already
        // released (end of start()); just drop the loop/context/cancellable refs created in the ctor.
        if (m_cancel) g_object_unref(m_cancel);
        if (m_loop) g_main_loop_unref(m_loop);
        if (m_ctx)  g_main_context_unref(m_ctx);
    }

Q_SIGNALS:
    void frameReady(const QImage &img, int frame);
    void urlChanged(const QString &url);   // current page URI (toolbar address field)
    void canGoBack(bool ok);               // toolbar Back button enabled-state
    void canGoForward(bool ok);            // toolbar Forward button enabled-state
    void titleChanged(const QString &title);
    void loadProgressChanged(double fraction);     // 0..1 estimated load progress
    void loadingChanged(bool loading);
    void tlsChanged(bool ok, const QString &host); // false = TLS error -> broken-lock indicator
    void processCrashed();                         // WebProcess died (auto-reload attempted)
    void readerModeChanged(bool on);               // reader view applied/cleared -> toolbar button state
    void readerableChanged(bool can);              // current page looks like an article -> enable Reader

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

        m_ucm = webkit_user_content_manager_new();   // holds the content-blocking filter (added async below)
        m_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "display", display, "user-content-manager", m_ucm, nullptr));
        WPEView *wpeView = webkit_web_view_get_wpe_view(m_view);
        // Readability: lay the page out at device-pixel-ratio `dpr` so the CSS viewport is narrower
        // (panel/dpr) -> responsive sites reflow to a readable, fits-width layout. Toplevel sizes are
        // LOGICAL; the buffer is logical*dpr (~= the physical panel), so the display path is unchanged.
        // Tunable via RMWEB_DPR (default 1.0 = current behaviour; ~2.0 = readable). See zoom-readability.md.
        double dpr = qgetenv("RMWEB_DPR").toDouble(); if (dpr < 1.0 || dpr > 3.0) dpr = 2.0;
        const int logW = static_cast<int>(m_w / dpr), logH = static_cast<int>(m_h / dpr);
        // Size the toplevel first (headless default is 0x0 -> empty paints), then force a real
        // visible FALSE->TRUE transition so the view MAPS (WebKit only keeps painting while mapped).
        if (WPEToplevel *top = wpe_view_get_toplevel(wpeView)) {
            if (dpr != 1.0) wpe_toplevel_scale_changed(top, dpr);
            wpe_toplevel_resize(top, logW, logH);
        }
        wpe_view_resized(wpeView, logW, logH);
        qInfo("[t] dpr=%.2f logical=%dx%d", dpr, logW, logH);
        g_signal_connect(wpeView, "buffer-rendered", G_CALLBACK(&WpeEngine::onBuffer), this);
        wpe_view_set_visible(wpeView, FALSE);
        wpe_view_set_visible(wpeView, TRUE);
        qInfo("[t] view mapped=%d size=%dx%d @%.0fms", wpe_view_get_mapped(wpeView),
              wpe_view_get_width(wpeView), wpe_view_get_height(wpeView), msSince(m_startUs));
        g_signal_connect(m_view, "load-changed", G_CALLBACK(&WpeEngine::onLoadChanged), this);
        g_signal_connect(m_view, "notify::uri", G_CALLBACK(&WpeEngine::onUri), this);
        g_signal_connect(m_view, "notify::title", G_CALLBACK(&WpeEngine::onTitle), this);
        g_signal_connect(m_view, "notify::estimated-load-progress", G_CALLBACK(&WpeEngine::onProgress), this);
        g_signal_connect(m_view, "load-failed-with-tls-errors", G_CALLBACK(&WpeEngine::onTlsError), this);
        g_signal_connect(m_view, "web-process-terminated", G_CALLBACK(&WpeEngine::onWebProcessTerminated), this);

        // Content blocking (RMWEB_BLOCK!=0, default on): compile the WKContentRuleList, add it, THEN load —
        // so it applies to the very first resource loads. The compile is async; loadInitial() runs from its
        // callback. Off => load immediately. A compile failure still loads (unfiltered) so the page works.
        if (qgetenv("RMWEB_BLOCK") != "0") {
            WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new("/home/root/rmweb/cfstore");
            GBytes *src = g_bytes_new_static(kBlockRules, strlen(kBlockRules));
            webkit_user_content_filter_store_save(store, "rmweb-block", src, m_cancel, &WpeEngine::onFilterSaved, this);
            g_bytes_unref(src);
        } else {
            loadInitial();
        }

        // Diagnostic (RMWEB_DEBUG_NAV): drive a back/forward sequence so navigation + the canGo/url signals
        // can be verified before the toolbar/URL entry exist (Tasks 4/8). A(initial) -> B -> back -> forward.
        if (qEnvironmentVariableIntValue("RMWEB_DEBUG_NAV") > 0) {
            scheduleOnCtx(4000, [](gpointer d) -> gboolean {
                auto *self = static_cast<WpeEngine*>(d); qInfo("[nav][dbg] load B example.org");
                webkit_web_view_load_uri(self->m_view, "https://example.org"); return G_SOURCE_REMOVE; });
            scheduleOnCtx(8000, [](gpointer d) -> gboolean {
                auto *self = static_cast<WpeEngine*>(d); qInfo("[nav][dbg] goBack");
                if (webkit_web_view_can_go_back(self->m_view)) webkit_web_view_go_back(self->m_view); return G_SOURCE_REMOVE; });
            scheduleOnCtx(12000, [](gpointer d) -> gboolean {
                auto *self = static_cast<WpeEngine*>(d); qInfo("[nav][dbg] goForward");
                if (webkit_web_view_can_go_forward(self->m_view)) webkit_web_view_go_forward(self->m_view); return G_SOURCE_REMOVE; });
        }

        g_main_loop_run(m_loop);  // pumps WPE on this thread until stop()

        // Loop exited (engine.stop()): release the web view here, on its own thread, BEFORE ~WpeEngine —
        // this drops the buffer-rendered/load-changed handlers that capture `this`, so none can fire late.
        if (m_view) { g_object_unref(m_view); m_view = nullptr; }
        if (m_ucm)  { g_object_unref(m_ucm);  m_ucm  = nullptr; }
        g_main_context_pop_thread_default(m_ctx);
    }

    void stop() {
        g_cancellable_cancel(m_cancel);   // abort an in-flight content-filter save so its callback bails
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

    // Navigation — WebKit's own history/loading API, marshalled onto the worker GMainContext.
    // Safe to call from the GUI thread (g_main_context_invoke_full is MT-safe); QML calls these directly.
    void loadUrl(const QString &u) { const std::string s = rmweb::normalizeUrl(u.toStdString());
        marshalToCtx([this, s] { if (m_view) webkit_web_view_load_uri(m_view, s.c_str()); }); }
    void goBack()    { marshalToCtx([this] { if (m_view && webkit_web_view_can_go_back(m_view))    webkit_web_view_go_back(m_view); }); }
    void goForward() { marshalToCtx([this] { if (m_view && webkit_web_view_can_go_forward(m_view)) webkit_web_view_go_forward(m_view); }); }
    void reload()    { marshalToCtx([this] { if (m_view) webkit_web_view_reload(m_view); }); }
    void stopLoading() { marshalToCtx([this] { if (m_view) webkit_web_view_stop_loading(m_view); }); }
    void pageNext()  { pageBy(kPageStepPx); }   // façade page-turn (wraps the scroll+repaint in pageBy)
    void pagePrev()  { pageBy(-kPageStepPx); }
    // Reader mode: inject Readability + reflow the article into one clean column; toggle off = reload original.
    void toggleReader() {
        marshalToCtx([this] {
            if (!m_view) return;
            if (m_readerMode) { webkit_web_view_reload(m_view); return; }   // off: reload (COMMITTED clears state)
            applyReader();
        });
    }

private:
    // Run fn on the worker thread's GMainContext (g_main_context_invoke_full is MT-safe).
    void marshalToCtx(std::function<void()> fn) {
        auto *f = new std::function<void()>(std::move(fn));
        g_main_context_invoke_full(m_ctx, G_PRIORITY_DEFAULT,
            [](gpointer d) -> gboolean { (*static_cast<std::function<void()>*>(d))(); return G_SOURCE_REMOVE; },
            f, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
    }
    // Attach a one-shot timeout to the WORKER context. g_timeout_add* would attach to the GLOBAL default
    // context (which our worker loop never runs) and never fire — we must g_source_attach to m_ctx. Diag only.
    void scheduleOnCtx(guint ms, GSourceFunc fn) {
        GSource *s = g_timeout_source_new(ms);
        g_source_set_callback(s, fn, this, nullptr);
        g_source_attach(s, m_ctx);
        g_source_unref(s);
    }
    struct PageMsg { WpeEngine *self; double dy; };
    static gboolean onPage(gpointer d) {
        auto *m = static_cast<PageMsg*>(d);
        WpeEngine *self = m->self;
        if (self->m_view) {
            self->m_pageUs = g_get_monotonic_time();
            // Scroll one page and force exactly ONE repaint: a bare scrollBy moves scrollY but commits no
            // buffer, so we bump a hidden marker node to dirty the page → one composite. With llvmpipe that
            // lands in ~90 ms, so one frame per turn is enough. (The earlier requestAnimationFrame burst was a
            // workaround for softpipe's ~6 s composite; it also flooded the e-ink panel with ~20 presents/turn.)
            gchar *js = g_strdup_printf(
                "window.scrollBy(0,%d>0?Math.round(innerHeight*0.92):-Math.round(innerHeight*0.92));"
                "var m=document.getElementById('__r');"
                "if(!m){m=document.createElement('span');m.id='__r';"
                "m.style.cssText='position:fixed;left:-9999px;top:0';document.body.appendChild(m);}"
                "m.textContent=((+m.textContent||0)+1);window.scrollY",
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

    // Build the apply script: the vendored Readability lib + our glue, with the reader CSS (font size from
    // RMWEB_READER_FONT, default 38) inlined. Injected in one shot so all symbols share the same scope.
    std::string buildReaderApplyJs() {
        int fs = 38;
        if (const char *e = getenv("RMWEB_READER_FONT")) { const int v = atoi(e); if (v >= 16 && v <= 96) fs = v; }
        std::string css = kReaderCss;  replaceAll(css, "__FS__", std::to_string(fs));
        std::string glue = kReaderGlue; replaceAll(glue, "__CSS__", css);
        return m_readabilityJs + "\n" + glue;
    }
    void applyReader() {
        if (m_readabilityJs.empty()) m_readabilityJs = slurp(readerDir() + "/Readability.js");
        if (m_readabilityJs.empty()) { qWarning("[reader] Readability.js missing in %s", readerDir().c_str()); return; }
        const std::string js = buildReaderApplyJs();
        webkit_web_view_evaluate_javascript(m_view, js.c_str(), -1, nullptr, nullptr, nullptr,
                                            &WpeEngine::onReaderApplied, this);
    }
    static void onReaderApplied(GObject *obj, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        GError *err = nullptr;
        JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
        std::string st = "error";
        if (v) { if (jsc_value_is_string(v)) { char *s = jsc_value_to_string(v); st = s ? s : ""; g_free(s); } g_object_unref(v); }
        else { st = err ? err->message : "?"; g_clear_error(&err); }
        if (st == "ok") { self->m_readerMode = true; Q_EMIT self->readerModeChanged(true); qInfo("[reader] applied"); }
        else qWarning("[reader] not applied: %s", st.c_str());
    }
    // After each load: does the page look like an article? -> enable/disable the Reader button.
    void checkReaderable() {
        if (m_readerableJs.empty()) m_readerableJs = slurp(readerDir() + "/Readability-readerable.js");
        if (m_readerableJs.empty()) return;   // can't tell -> leave the button as it is
        const std::string js = m_readerableJs + "\nisProbablyReaderable(document);";
        webkit_web_view_evaluate_javascript(m_view, js.c_str(), -1, nullptr, nullptr, nullptr,
                                            &WpeEngine::onReaderableChecked, this);
    }
    static void onReaderableChecked(GObject *obj, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        GError *err = nullptr;
        JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
        bool can = false;
        if (v) { if (jsc_value_is_boolean(v)) can = jsc_value_to_boolean(v); g_object_unref(v); }
        else g_clear_error(&err);
        qInfo("[reader] readerable=%d", can);
        Q_EMIT self->readerableChanged(can);
    }

    void loadInitial() {
        if (m_url.isEmpty()) webkit_web_view_load_html(m_view, kTestPage, nullptr);
        else                 webkit_web_view_load_uri(m_view, m_url.toUtf8().constData());
        qInfo("[t] load dispatched @%.0fms", msSince(m_startUs));
    }
    // WKContentRuleList compiled -> add it to the UCM (now active for all loads), then kick off the page load.
    static void onFilterSaved(GObject *obj, GAsyncResult *res, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        GError *err = nullptr;
        WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(obj), res, &err);
        // Cancelled = engine is tearing down (stop() cancelled m_cancel): release and bail without
        // touching m_ucm or starting a load.
        if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            if (f) webkit_user_content_filter_unref(f);
            g_clear_error(&err); g_object_unref(obj); return;
        }
        if (f) { webkit_user_content_manager_add_filter(self->m_ucm, f); webkit_user_content_filter_unref(f);
                 qInfo("[block] content filter active"); }
        else   { qWarning("[block] filter compile failed: %s", err ? err->message : "?"); g_clear_error(&err); }
        g_object_unref(obj);   // the filter store
        self->loadInitial();
    }
    static void onUri(GObject *obj, GParamSpec *, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        const char *u = webkit_web_view_get_uri(WEBKIT_WEB_VIEW(obj));
        qInfo("[nav] uri=%s", u ? u : "");
        Q_EMIT self->urlChanged(QString::fromUtf8(u ? u : ""));
    }

    static void onLoadChanged(WebKitWebView *view, WebKitLoadEvent ev, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        if (ev == WEBKIT_LOAD_STARTED) {
            qInfo("[t] load started @%.0fms", msSince(self->m_startUs));
            Q_EMIT self->loadingChanged(true);
            Q_EMIT self->tlsChanged(true, QString());   // optimistic; onTlsError flips it on a cert failure
        }
        if (ev == WEBKIT_LOAD_FINISHED) {
            self->m_reloadAttempts = 0;                  // a good load refills the crash auto-reload budget
            Q_EMIT self->loadingChanged(false);
            self->checkReaderable();                     // article? -> enable/disable the Reader button
            qInfo("[t] load finished @%.0fms", msSince(self->m_startUs));
        }
        if (ev == WEBKIT_LOAD_COMMITTED) {
            // A real navigation/reload landed fresh original content -> any reader view is gone; reset its state.
            if (self->m_readerMode) { self->m_readerMode = false; Q_EMIT self->readerModeChanged(false); }
            // Passive TLS indicator: get_tls_info flags https + cert errors even when the page still
            // loaded (lock shows broken for a bad cert, plain for http), independent of tls-errors-policy.
            GTlsCertificate *cert = nullptr; GTlsCertificateFlags errs = (GTlsCertificateFlags)0;
            const gboolean secure = webkit_web_view_get_tls_info(view, &cert, &errs);
            const char *cu = webkit_web_view_get_uri(view);
            qInfo("[tls] secure=%d errs=0x%x", secure, (unsigned)errs);
            Q_EMIT self->tlsChanged(secure && errs == 0, QUrl(QString::fromUtf8(cu ? cu : "")).host());
        }
        // Refresh nav state on commit (snappy button enable/disable) and again on finish.
        if (ev == WEBKIT_LOAD_COMMITTED || ev == WEBKIT_LOAD_FINISHED) {
            const bool b = webkit_web_view_can_go_back(view);
            const bool f = webkit_web_view_can_go_forward(view);
            qInfo("[nav] back=%d fwd=%d", b, f);
            Q_EMIT self->canGoBack(b);
            Q_EMIT self->canGoForward(f);
        }
    }

    static void onTitle(GObject *obj, GParamSpec *, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        const char *t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(obj));
        qInfo("[meta] title=%s", t ? t : "");
        Q_EMIT self->titleChanged(QString::fromUtf8(t ? t : ""));
    }
    static void onProgress(GObject *obj, GParamSpec *, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        Q_EMIT self->loadProgressChanged(webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(obj)));
    }
    static gboolean onTlsError(WebKitWebView *, gchar *failing_uri, GTlsCertificate *,
                               GTlsCertificateFlags, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        qWarning("[tls] cert error: %s", failing_uri ? failing_uri : "?");
        Q_EMIT self->tlsChanged(false, QUrl(QString::fromUtf8(failing_uri ? failing_uri : "")).host());
        return FALSE;   // don't proceed — WebKit fails the load; the shell shows a broken-lock indicator
    }
    static void onWebProcessTerminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        qWarning("[crash] WebProcess terminated (reason=%d), attempts=%d", reason, self->m_reloadAttempts);
        Q_EMIT self->processCrashed();
        if (self->m_reloadAttempts < 2) { self->m_reloadAttempts++; webkit_web_view_reload(view); }
        else qWarning("[crash] giving up auto-reload after %d attempts", self->m_reloadAttempts);
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
        // Cheap sparse content fingerprint (FNV-1a over a pixel grid) — proves whether consecutive frames
        // actually differ in pixels (diagnosing "the panel refreshed but the image didn't change").
        unsigned sig = 2166136261u;
        for (int yy = 0; yy < h; yy += 40) {
            const uchar *row = pix + static_cast<gsize>(yy) * stride;
            for (int xx = 0; xx < w; xx += 40) sig = (sig ^ row[xx * 4]) * 16777619u;
        }
        // Skip identical frames: WebKit re-submits the same composited buffer on its idle heartbeat, and
        // the rAF page-turn pulse yields several identical ticks. Only repaint the e-ink when pixels change
        // — this kills the wasteful periodic re-present and any flicker from the nudge.
        const bool changed = (sig != self->m_lastSig);
        self->m_lastSig = sig;

        const double dt   = self->m_lastBufUs ? (tIn - self->m_lastBufUs) / 1000.0 : 0.0;
        const double flip = self->m_pageUs    ? (tIn - self->m_pageUs)    / 1000.0 : -1.0;
        qInfo("[t] frame %d @%.0fms  build=%.1fms  dt=%.1fms  flip-latency=%.1fms  sig=%08x %s  %dx%d",
              self->m_frames, msSince(self->m_startUs), msSince(tIn), dt, flip, sig,
              changed ? "NEW" : "dup", w, h);
        self->m_lastBufUs = tIn;
        if (!changed) return;   // nothing visually new — do not repaint the panel
        if (flip >= 0) self->m_pageUs = 0;   // count this changed frame as the page-turn's result

        // WPE SHM memory order is B,G,R,A (ARGB8888 little-endian) == QImage::Format_ARGB32.
        QImage img(pix, w, h, stride, QImage::Format_ARGB32);
        Q_EMIT self->frameReady(img.copy(), self->m_frames);
    }

    QString m_url;
    int m_w, m_h, m_frames = 0;
    GMainContext *m_ctx = nullptr;
    GMainLoop *m_loop = nullptr;
    GCancellable *m_cancel = nullptr;            // cancels an in-flight content-filter save on shutdown
    WebKitWebView *m_view = nullptr;
    WebKitUserContentManager *m_ucm = nullptr;   // owns the content-blocking filter
    gint64 m_startUs = 0;     // monotonic origin (set in start) — all "@Xms" timings are relative to it
    gint64 m_lastBufUs = 0;   // previous buffer-rendered time — gives the inter-frame interval
    gint64 m_pageUs = 0;      // last page-flip dispatch time — gives swipe -> rendered-frame latency
    unsigned m_lastSig = 0;   // fingerprint of the last emitted frame — to drop identical (dup) frames
    int m_reloadAttempts = 0; // WebProcess-crash auto-reload budget (reset on a successful load)
    bool m_readerMode = false;     // reader view currently applied (vs the original page)
    std::string m_readabilityJs;   // vendored Readability.js, lazily slurped + cached
    std::string m_readerableJs;    // vendored isProbablyReaderable, lazily slurped + cached
};

// ---------------------------------------------------------------------------
// ShellBridge — a GUI-thread proxy between QML and the worker-thread WpeEngine. QML cannot `Connections`-
// connect to a QObject living in another thread (it aborts), so the toolbar talks to this bridge: button
// taps forward to the engine's MT-safe nav slots, and engine signals are relayed here (queued worker->GUI)
// so QML can bind to them. Exposed to QML as the "engine" context property.
// ---------------------------------------------------------------------------
class ShellBridge : public QObject {
    Q_OBJECT
    // Angelfish-style façade: the QML chrome binds to these properties; engine signals feed them.
    Q_PROPERTY(QString url          READ url          NOTIFY urlChanged)
    Q_PROPERTY(QString title        READ title        NOTIFY titleChanged)
    Q_PROPERTY(bool    loading      READ loading      NOTIFY loadingChanged)
    Q_PROPERTY(qreal   loadProgress READ loadProgress NOTIFY loadProgressChanged)
    Q_PROPERTY(bool    canGoBack    READ canGoBack    NOTIFY canGoBackChanged)
    Q_PROPERTY(bool    canGoForward READ canGoForward NOTIFY canGoForwardChanged)
    Q_PROPERTY(bool    readerable   READ readerable   NOTIFY readerableChanged)
    Q_PROPERTY(bool    readerMode   READ readerMode   NOTIFY readerModeChanged)
    Q_PROPERTY(bool    tlsOk        READ tlsOk        NOTIFY tlsChanged)
    Q_PROPERTY(QString tlsHost      READ tlsHost      NOTIFY tlsChanged)
public:
    explicit ShellBridge(WpeEngine *e, QObject *parent = nullptr) : QObject(parent), m_engine(e) {}
    QString url() const          { return m_url; }
    QString title() const        { return m_title; }
    bool    loading() const      { return m_loading; }
    qreal   loadProgress() const { return m_loadProgress; }
    bool    canGoBack() const    { return m_canGoBack; }
    bool    canGoForward() const { return m_canGoForward; }
    bool    readerable() const   { return m_readerable; }
    bool    readerMode() const   { return m_readerMode; }
    bool    tlsOk() const        { return m_tlsOk; }
    QString tlsHost() const      { return m_tlsHost; }
public Q_SLOTS:
    // Chrome -> engine (forwarded to the MT-safe engine slots).
    void goBack()                  { m_engine->goBack(); }
    void goForward()               { m_engine->goForward(); }
    void reload()                  { m_engine->reload(); }
    void stopLoading()             { m_engine->stopLoading(); }
    void loadUrl(const QString &u) { m_engine->loadUrl(u); }
    void pageNext()                { m_engine->pageNext(); }
    void pagePrev()                { m_engine->pagePrev(); }
    void readerToggle()                  { m_engine->toggleReader(); }
    // Phase B/D stubs — present now so the QML contract is stable; wired in later phases.
    void setReaderStyle(const QString &) {}
    void findText(const QString &)       {}
    void findNext()                      {}
    void findPrev()                      {}
    void findClear()                     {}
    void setJsEnabled(bool)              {}
    void hintStart()                     {}
    void hintFollow(const QString &)     {}
    // Engine signals -> property updates (relayed worker->GUI, then NOTIFY for QML bindings).
    void onEngineUrl(const QString &u) { if (u != m_url) { m_url = u; Q_EMIT urlChanged(); } }
    void onEngineCanGoBack(bool b)     { if (b != m_canGoBack)    { m_canGoBack = b;    Q_EMIT canGoBackChanged(); } }
    void onEngineCanGoForward(bool b)  { if (b != m_canGoForward) { m_canGoForward = b; Q_EMIT canGoForwardChanged(); } }
    void onEngineTitle(const QString &t)        { if (t != m_title) { m_title = t; Q_EMIT titleChanged(); } }
    void onEngineLoadProgress(double p)         { if (p != m_loadProgress) { m_loadProgress = p; Q_EMIT loadProgressChanged(); } }
    void onEngineLoading(bool l)                { if (l != m_loading) { m_loading = l; Q_EMIT loadingChanged(); } }
    void onEngineTls(bool ok, const QString &h) { if (ok != m_tlsOk || h != m_tlsHost) { m_tlsOk = ok; m_tlsHost = h; Q_EMIT tlsChanged(); } }
    void onEngineCrashed()                      { Q_EMIT processCrashed(); }
    void onEngineReaderMode(bool on)            { if (on != m_readerMode)  { m_readerMode  = on;  Q_EMIT readerModeChanged(); } }
    void onEngineReaderable(bool can)           { if (can != m_readerable) { m_readerable = can; Q_EMIT readerableChanged(); } }
Q_SIGNALS:
    void urlChanged();
    void titleChanged();
    void loadingChanged();
    void loadProgressChanged();
    void canGoBackChanged();
    void canGoForwardChanged();
    void readerableChanged();
    void readerModeChanged();
    void tlsChanged();
    void processCrashed();
private:
    WpeEngine *m_engine;
    QString m_url, m_title, m_tlsHost;
    qreal   m_loadProgress = 0.0;
    bool    m_loading = false, m_canGoBack = false, m_canGoForward = false;
    bool    m_readerable = false, m_readerMode = false, m_tlsOk = true;
};

// ---------------------------------------------------------------------------
// WpeView — a full-screen QtQuick item that just paints the latest WPE frame (input comes from TouchReader,
// not Qt: the epaper QPA does not deliver finger touch to QtQuick items here).
// ---------------------------------------------------------------------------
class WpeView : public QQuickPaintedItem {
    Q_OBJECT
public:
    explicit WpeView(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent) {
        // Completion-gated present serializer. The vendor epaper present (EPRenderLoop swapBuffers)
        // DEADLOCKS if a 2nd present overlaps the 1st (the panel refresh holds the framebuffer mutex),
        // so we present the LATEST frame and never start the next until the previous one has finished:
        // we wait for QQuickWindow::frameSwapped (the present returned) PLUS a small waveform dwell
        // (swapBuffers can return before the e-ink refresh physically settles). This replaces the old
        // fixed 2 s gap, restoring ~120-250 ms turns. A fallback timer releases the gate if frameSwapped
        // never arrives (e.g. the backend doesn't emit it) -> degrades to the old cadence, never freezes.
        if (const int v = qEnvironmentVariableIntValue("RMWEB_PRESENT_DWELL"); v > 0) m_dwellMs = v;
        m_fallback.setSingleShot(true);
        connect(&m_fallback, &QTimer::timeout, this,
                [this]{ qInfo("[t][gui] present fallback-release (no frameSwapped)"); releaseGate(); });
    }
    void paint(QPainter *p) override {
        const qreal w = width(), h = height();
        if (!m_img.isNull()) p->drawImage(QRectF(0, 0, w, h), m_img);
        else                 p->fillRect(QRectF(0, 0, w, h), Qt::white);
        if (!m_chromeOn) return;
        // B2: the browser chrome is painted straight into the WpeView's frame — this is what reaches the
        // e-ink panel (QtQuick chrome does NOT composite over it; see docs/research/epaper-chrome-compositing.md).
        p->fillRect(QRectF(0, 0, w, kBarH), Qt::white);
        p->fillRect(QRectF(0, kBarH - 3, w, 3), Qt::black);
        QFont bf = p->font(); bf.setPixelSize(44); p->setFont(bf);
        auto btn = [&](qreal x0, qreal x1, const QString &t, bool on) {
            p->setPen(on ? Qt::black : QColor(170, 170, 170));
            p->drawText(QRectF(x0, 0, x1 - x0, kBarH), Qt::AlignCenter, t);
        };
        btn(0,      kBackX, "Back", m_canBack);
        btn(kBackX, kFwdX,  "Fwd",  m_canFwd);
        btn(kFwdX,  kRelX,  m_loading ? "Stop" : "Reload", true);
        const qreal rx = w - kReaderW;
        QFont af = p->font(); af.setPixelSize(34); p->setFont(af); p->setPen(Qt::black);
        const QString a = p->fontMetrics().elidedText(m_addr, Qt::ElideRight, int(rx - kRelX - 40));
        p->drawText(QRectF(kRelX + 20, 0, rx - kRelX - 40, kBarH), Qt::AlignVCenter, a);
        // Reader toggle: inverted (black fill, white text) when active; greyed when the page isn't an article.
        QFont rf = p->font(); rf.setPixelSize(40); p->setFont(rf);
        if (m_readerMode) { p->fillRect(QRectF(rx, 6, kReaderW - 6, kBarH - 12), Qt::black); p->setPen(Qt::white); }
        else              { p->setPen(m_readerable ? Qt::black : QColor(170, 170, 170)); }
        p->drawText(QRectF(rx, 0, kReaderW, kBarH), Qt::AlignCenter, "Reader");
    }
    // Hit-test a tap against the chrome bar (panel px); returns the control, or None (off / below the bar).
    enum Hit { None, Back, Fwd, Reload, Address, Reader };
    Hit hitChrome(int x, int y) const {
        if (!m_chromeOn || y >= kBarH) return None;
        const int rx = int(width()) - kReaderW;            // Reader button occupies the right edge of the bar
        if (x < kBackX) return Back;
        if (x < kFwdX)  return Fwd;
        if (x < kRelX)  return Reload;
        if (x >= rx)    return Reader;
        return Address;
    }
    bool chromeOn()  const { return m_chromeOn; }
    bool isLoading() const { return m_loading; }
public Q_SLOTS:
    void setImage(const QImage &img) { m_pending = img; m_hasPending = true; schedule(); }
    // Chrome state (fed by engine signals on the GUI thread). Each re-presents the current frame with the
    // new chrome via the SAME serializer — never a bare update() (that would risk an overlapping present).
    void setChromeOn(bool v)       { if (v != m_chromeOn) { m_chromeOn = v; schedule(); } }
    void setCanBack(bool v)        { if (v != m_canBack)  { m_canBack  = v; schedule(); } }
    void setCanFwd(bool v)         { if (v != m_canFwd)   { m_canFwd   = v; schedule(); } }
    void setLoading(bool v)        { if (v != m_loading)  { m_loading  = v; schedule(); } }
    void setAddr(const QString &s) { if (s != m_addr)     { m_addr     = s; schedule(); } }
    void setReaderMode(bool v)     { if (v != m_readerMode)  { m_readerMode  = v; schedule(); } }
    void setReaderable(bool v)     { if (v != m_readerable) { m_readerable = v; schedule(); } }
protected:
    void itemChange(ItemChange ch, const ItemChangeData &d) override {
        if (ch == ItemSceneChange && d.window)   // frameSwapped fires after the panel present returns
            connect(d.window, &QQuickWindow::frameSwapped, this, &WpeView::onFrameSwapped, Qt::UniqueConnection);
        QQuickPaintedItem::itemChange(ch, d);
    }
private Q_SLOTS:
    void onFrameSwapped() {
        if (!m_inFlight) return;
        const int ms = m_clock.isValid() ? int(m_clock.elapsed()) : 0;
        qInfo("[t][gui] frameSwapped @%dms (dwell=%d)", ms, m_dwellMs);
        const int wait = m_dwellMs - ms;         // hold the rest of the dwell so the next can't overlap
        if (wait > 0) QTimer::singleShot(wait, this, [this]{ releaseGate(); });
        else releaseGate();
    }
private:
    void schedule() { if (m_inFlight) m_dirty = true; else presentNext(); }
    void presentNext() {
        if (m_hasPending) { m_img = m_pending; m_hasPending = false; }   // newest frame (else re-present current)
        m_dirty = false; m_inFlight = true;
        m_clock.restart();
        update();                                // -> scene render -> EPRenderLoop present to panel
        m_fallback.start(kFallbackMs);
    }
    void releaseGate() {
        m_fallback.stop(); m_inFlight = false;
        if (m_hasPending || m_dirty) presentNext();   // newer frame or a chrome change queued -> present it
    }
    static const int kFallbackMs = 2500;         // release even if frameSwapped never fires (>= worst refresh)
    int m_dwellMs = 200;                         // min present spacing, ms (RMWEB_PRESENT_DWELL overrides)
    bool m_hasPending = false, m_inFlight = false, m_dirty = false;
    QImage m_img, m_pending;
    QElapsedTimer m_clock;
    QTimer m_fallback;
    // chrome state, painted into the frame (reader-first: shown on launch, hidden by a content tap).
    static const int kBarH = 104, kBackX = 170, kFwdX = 340, kRelX = 560, kReaderW = 190;
    bool m_chromeOn = true, m_canBack = false, m_canFwd = false, m_loading = false;
    bool m_readerMode = false, m_readerable = false;
    QString m_addr;
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
    void swipe(int dir);     // page turn (+1 next / -1 prev)
    void tap(int x, int y);  // tap at panel px -> touch->mouse bridge -> QtQuick Controls
public Q_SLOTS:
    void run() {
        int fd = openByName("Elan touch input");
        if (fd < 0) { qWarning("[touch] 'Elan touch input' node not found"); return; }
        if (ioctl(fd, EVIOCGRAB, reinterpret_cast<void*>(1)) != 0)
            qWarning("[touch] EVIOCGRAB failed (device held elsewhere)");
        else
            qInfo("[touch] grabbed 'Elan touch input' — reading finger touch directly");

        // Protocol-B, first finger. ABS_MT_TRACKING_ID (contact start / -1 lift) arrives BEFORE the
        // POSITION_X/Y of the same SYN frame, so latching the swipe-start at TRACKING_ID time would capture
        // the PREVIOUS frame's stale x/y. Flag down/lift instead and resolve at SYN_REPORT, where x/y are
        // coherent for the whole frame.
        int curSlot = 0, x = 0, y = 0, sx = 0, sy = 0;
        bool down = false, pendingDown = false, pendingLift = false;
        gint64 downUs = 0;   // contact-start time, for tap dwell
        struct input_event ev[64];
        while (!m_stop.load()) {
            struct pollfd pfd { fd, POLLIN, 0 };
            if (poll(&pfd, 1, 200) <= 0) continue;
            const ssize_t n = read(fd, ev, sizeof ev);
            if (n < static_cast<ssize_t>(sizeof(struct input_event))) continue;
            for (size_t i = 0; i < n / sizeof(struct input_event); ++i) {
                const struct input_event &p = ev[i];
                if (p.type == EV_SYN && p.code == SYN_REPORT) {
                    if (pendingDown) { down = true; sx = x; sy = y; downUs = g_get_monotonic_time(); pendingDown = false; }
                    if (pendingLift) { if (down) emitGesture(x - sx, y - sy, x, y, downUs); down = false; pendingLift = false; }
                    continue;
                }
                if (p.type != EV_ABS) continue;
                if (p.code == ABS_MT_SLOT) { curSlot = p.value; continue; }
                if (curSlot != 0) continue;                                  // first finger only
                if (p.code == ABS_MT_POSITION_X)      x = std::min(p.value * kPanelW / kTouchRawW, kPanelW - 1);
                else if (p.code == ABS_MT_POSITION_Y) y = std::min(p.value * kPanelH / kTouchRawH, kPanelH - 1);
                else if (p.code == ABS_MT_TRACKING_ID) {
                    if (p.value >= 0) pendingDown = true;                     // new contact -> latch pos at SYN
                    else              pendingLift = true;                     // -1 -> lifted -> emit at SYN
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
    // Classify the finished contact (gesture.h) and dispatch: a tap becomes a synthetic mouse click
    // (the touch->mouse bridge), a swipe turns the page. Each path is independently debounced.
    void emitGesture(int dx, int dy, int x, int y, gint64 downUs) {
        const gint64 now = g_get_monotonic_time();
        const int dwellMs = static_cast<int>((now - downUs) / 1000);
        switch (classifyGesture(dx, dy, dwellMs)) {
        case Gesture::Tap:
            if (m_lastTapUs && now - m_lastTapUs < 250000) return;       // debounce double-taps
            m_lastTapUs = now;
            qInfo("[touch] tap @ %d,%d", x, y);
            Q_EMIT tap(x, y);
            return;
        case Gesture::SwipeUp:
        case Gesture::SwipeDown:
            if (m_lastSwipeUs && now - m_lastSwipeUs < 800000) return;   // <=1 turn / 0.8 s
            m_lastSwipeUs = now;
            if (dy < 0) { qInfo("[touch] swipe up -> next");   Q_EMIT swipe(+1); }
            else        { qInfo("[touch] swipe down -> prev"); Q_EMIT swipe(-1); }
            return;
        case Gesture::None:
            return;
        }
    }
    std::atomic<bool> m_stop { false };
    gint64 m_lastSwipeUs = 0;
    gint64 m_lastTapUs = 0;
};

#include "main.moc"

// ---------------------------------------------------------------------------
// EpaperRefresh — drives the e-ink panel ourselves. The libqsgepaper scenegraph renders into the DRM dumb
// buffer fast (~ms) but defers the *panel present* to a coarse internal cadence (~6 s), and a fast waveform
// never develops grayscale/color on Gallery 3 until a full pass. With QSG_RENDER_LOOP=basic (no EPRenderLoop
// auto-present), we present each rendered frame ourselves from QQuickWindow::afterRendering:
//   * fast grayscale  (Mono, QualityFast, NoRefresh)      — every frame, so a page turn shows immediately;
//   * full colour flash (Color, QualityFull, CompleteRefresh) — every kFullEvery frames, develops colour +
//     clears ghosting ("grayscale now, colour catches up"). Symbols verified in libqsgepaper.so via readelf.
// ---------------------------------------------------------------------------
class EpaperRefresh {
public:
    bool init() {
        void *h = dlopen("/usr/lib/plugins/scenegraph/libqsgepaper.so", RTLD_NOW | RTLD_GLOBAL);
        if (!h) { qWarning("[refresh] dlopen failed: %s", dlerror()); return false; }
        m_instance = reinterpret_cast<InstanceFn>(dlsym(h, "_ZN13EPFramebuffer8instanceEv"));
        m_swap = reinterpret_cast<SwapFn>(
            dlsym(h, "_ZN13EPFramebuffer11swapBuffersE5QRect13EPContentType12EPScreenMode6QFlagsINS_10UpdateFlagEE"));
        if (!m_instance || !m_swap) {
            qWarning("[refresh] dlsym failed (instance=%p swap=%p)", (void*)m_instance, (void*)m_swap);
            return false;
        }
        m_fb = m_instance();
        // Full colour anti-ghost flash every N page-turns. Gallery 3 needs a full-screen flash to change
        // colour (= visible flicker), so for text reading we make N large (mostly grayscale, no flash).
        // Tunable live via RMWEB_FULL_EVERY (0/unset -> default). 0 disables the colour flash entirely.
        if (qEnvironmentVariableIsSet("RMWEB_FULL_EVERY"))
            m_fullEvery = qEnvironmentVariableIntValue("RMWEB_FULL_EVERY");
        qInfo("[refresh] EPFramebuffer ready (instance=%p) fullEvery=%d", m_fb, m_fullEvery);
        return m_fb != nullptr;
    }
    bool ok() const { return m_fb != nullptr; }
    // Present what the scenegraph just rendered. Enum values: EPContentType{Mono=0,Color=1},
    // EPScreenMode{QualityFast=1,QualityFull=4}, UpdateFlag{NoRefresh=0,CompleteRefresh=1}.
    void present() {
        if (!m_fb) return;
        // e-ink physically can't refresh faster than ~6 Hz; with llvmpipe the engine can emit frames far
        // faster, so rate-limit panel presents to protect the controller and avoid ghosting/flicker.
        const gint64 now = g_get_monotonic_time();
        if (m_lastPresentUs && (now - m_lastPresentUs) < 150000) return;   // >= ~150 ms between presents
        m_lastPresentUs = now;
        const QRect full(0, 0, kPanelW, kPanelH);
        ++m_frames;
        const bool isFull = (m_fullEvery > 0 && (m_frames % m_fullEvery) == 0);
        qInfo("[present] #%d swap enter full=%d", m_frames, isFull);
        if (isFull) m_swap(m_fb, full, 1, 4, 1);  // colour + anti-ghost flash
        else        m_swap(m_fb, full, 0, 1, 0);  // fast grayscale, no flash
        qInfo("[present] #%d swap done", m_frames);
    }
private:
    typedef void *(*InstanceFn)();
    // ABI of EPFramebuffer::swapBuffers(QRect, EPContentType, EPScreenMode, QFlags<UpdateFlag>): the implicit
    // `this` is the 1st arg; the two enums and the (int-sized) QFlags pass like ints on aarch64.
    typedef void (*SwapFn)(void *self, QRect, int, int, int);
    int m_fullEvery = 6;   // full colour flash every N presents (env RMWEB_FULL_EVERY; <=0 = grayscale only)
    InstanceFn m_instance = nullptr;
    SwapFn m_swap = nullptr;
    void *m_fb = nullptr;
    int m_frames = 0;
    gint64 m_lastPresentUs = 0;
};

// Reading-shell chrome: a top toolbar (Qt Quick Controls, Basic style = flat/high-contrast for e-ink)
// over the full-screen web view. Buttons call the engine's nav slots directly (engine is a context
// property); Connections reflect engine state (address text + Back/Forward enabled). Button glyphs are
// literal UTF-8 in the source (U+25C0/25B6 arrows, U+27F3 reload) — GCC reads UTF-8 and passes the bytes
// to QML. Tap (x,y)==scene coords, so the touch->mouse bridge routes taps to whichever item is under it.
static const char *kQml = R"QML(
import QtQuick
import QtQuick.Window
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.VirtualKeyboard
import rmweb 1.0
ApplicationWindow {
    id: win
    width: Screen.width; height: Screen.height
    visible: true; color: "white"
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        ToolBar {
            Layout.fillWidth: true; implicitHeight: 104
            background: Rectangle { color: "white"; border.color: "black"; border.width: 2 }
            RowLayout {
                anchors.fill: parent; anchors.margins: 6; spacing: 8
                Button { id: back; text: "◀"; font.pixelSize: 40; implicitWidth: 104; implicitHeight: 88; enabled: engine.canGoBack;    onClicked: engine.goBack() }
                Button { id: fwd;  text: "▶"; font.pixelSize: 40; implicitWidth: 104; implicitHeight: 88; enabled: engine.canGoForward; onClicked: engine.goForward() }
                Button { id: rel;  text: "⟳"; font.pixelSize: 40; implicitWidth: 104; implicitHeight: 88;                 onClicked: engine.reload() }
                TextField {
                    id: address; Layout.fillWidth: true; implicitHeight: 88; font.pixelSize: 34
                    verticalAlignment: TextInput.AlignVCenter
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    onAccepted: { engine.loadUrl(text); focus = false }   // load + dismiss keyboard
                }
                Button { id: clr; text: "✕"; font.pixelSize: 32; implicitWidth: 88; implicitHeight: 88
                    onClicked: { address.text = ""; address.forceActiveFocus() } }   // clear, keep focus + keyboard
            }
        }
        WpeView { objectName: "view"; Layout.fillWidth: true; Layout.fillHeight: true }
    }
    Connections {
        target: engine
        function onUrlChanged() { if (!address.activeFocus) address.text = engine.url }
    }
    InputPanel {
        id: inputPanel; z: 99
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        visible: active   // Qt Virtual Keyboard — shown only while a field has active focus
    }
}
)QML";

// DIAG (RMWEB_SIMPLE_QML): bare Window, no chrome — to bisect whether ApplicationWindow / Controls /
// VirtualKeyboard is what stalls the GUI event loop (timers + async frames stop firing after startup).
static const char *kQmlSimple = R"QML(
import QtQuick
import QtQuick.Window
import rmweb 1.0
Window {
    width: Screen.width; height: Screen.height
    visible: true; color: "white"
    WpeView { objectName: "view"; anchors.fill: parent }
}
)QML";

int main(int argc, char **argv) {
    // Line-buffer stderr: the launcher redirects it to a file (block-buffered by default), so a kill at
    // the end of a timed run would drop the last unflushed block — losing exactly the most recent events.
    setvbuf(stderr, nullptr, _IOLBF, 0);
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    QGuiApplication app(argc, argv);
    const QString url      = (argc > 1) ? QString::fromUtf8(argv[1]) : QString();
    const QString savePath = (argc > 2) ? QString::fromUtf8(argv[2]) : QString();

    QThread thread;
    WpeEngine engine(url, 1620, 2160);
    engine.moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, &engine, &WpeEngine::start);
    ShellBridge bridge(&engine);   // GUI-thread proxy for QML (QML can't connect across threads to engine)

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
        qmlEngine->rootContext()->setContextProperty("engine", &bridge);   // QML talks to the GUI-thread bridge
        QObject::connect(&engine, &WpeEngine::urlChanged,   &bridge, &ShellBridge::onEngineUrl);
        QObject::connect(&engine, &WpeEngine::canGoBack,    &bridge, &ShellBridge::onEngineCanGoBack);
        QObject::connect(&engine, &WpeEngine::canGoForward, &bridge, &ShellBridge::onEngineCanGoForward);
        QObject::connect(&engine, &WpeEngine::titleChanged,        &bridge, &ShellBridge::onEngineTitle);
        QObject::connect(&engine, &WpeEngine::loadProgressChanged, &bridge, &ShellBridge::onEngineLoadProgress);
        QObject::connect(&engine, &WpeEngine::loadingChanged,      &bridge, &ShellBridge::onEngineLoading);
        QObject::connect(&engine, &WpeEngine::tlsChanged,          &bridge, &ShellBridge::onEngineTls);
        QObject::connect(&engine, &WpeEngine::processCrashed,      &bridge, &ShellBridge::onEngineCrashed);
        QObject::connect(&engine, &WpeEngine::readerModeChanged,   &bridge, &ShellBridge::onEngineReaderMode);
        QObject::connect(&engine, &WpeEngine::readerableChanged,   &bridge, &ShellBridge::onEngineReaderable);
        auto *comp = new QQmlComponent(qmlEngine, qmlEngine);
        comp->setData(qEnvironmentVariableIsSet("RMWEB_SIMPLE_QML") ? kQmlSimple : kQml,
                      QUrl(QStringLiteral("inline.qml")));
        if (comp->status() != QQmlComponent::Ready) {
            qWarning() << "[qml]" << comp->errorString();
            return 2;
        }
        QObject *root = comp->create();
        auto *view = root ? root->findChild<WpeView*>("view") : nullptr;
        if (!view) { qWarning() << "[qml] WpeView not found"; return 3; }
        root->setParent(qmlEngine);   // engine owns the QML tree -> well-defined teardown order
        auto *win = qobject_cast<QQuickWindow*>(root);
        QObject::connect(&engine, &WpeEngine::frameReady, view,
                         [view](const QImage &img, int frame) {
            const gint64 t = g_get_monotonic_time();
            view->setImage(img);
            qInfo("[t][gui] frame %d -> setImage %.1fms  %dx%d", frame,
                  (g_get_monotonic_time() - t) / 1000.0, img.width(), img.height());
        });
        // Engine state -> the C++ chrome painted into the frame (queued worker->GUI).
        QObject::connect(&engine, &WpeEngine::canGoBack,      view, &WpeView::setCanBack);
        QObject::connect(&engine, &WpeEngine::canGoForward,   view, &WpeView::setCanFwd);
        QObject::connect(&engine, &WpeEngine::loadingChanged, view, &WpeView::setLoading);
        QObject::connect(&engine, &WpeEngine::urlChanged,     view, &WpeView::setAddr);
        QObject::connect(&engine, &WpeEngine::readerModeChanged, view, &WpeView::setReaderMode);
        QObject::connect(&engine, &WpeEngine::readerableChanged, view, &WpeView::setReaderable);

        // Drive the e-ink panel ourselves so page turns show immediately (needs QSG_RENDER_LOOP=basic so
        // afterRendering fires on the GUI thread and the EPRenderLoop's slow auto-present is out of the way).
        // The epaper QPA's own EPRenderLoop already presents the scene to the panel. Calling
        // EPFramebuffer::swapBuffers ourselves from afterRendering RE-ENTERS the framebuffer mutex that
        // EPRenderLoop holds across renderSceneGraph -> non-recursive self-DEADLOCK on the GUI thread
        // (the whole UI freezes after the first frame; confirmed by a backtrace). So let EPRenderLoop drive
        // the panel by default; opt back into manual present only with RMWEB_MANUAL_PRESENT (diagnostic).
        static EpaperRefresh epaper;
        if (win && qgetenv("QT_QPA_PLATFORM") == "epaper" && qEnvironmentVariableIsSet("RMWEB_MANUAL_PRESENT")) {
            if (epaper.init())
                QObject::connect(win, &QQuickWindow::afterRendering, win,
                                 [] { epaper.present(); }, Qt::DirectConnection);
        }

        // Direct evdev touch -> page turns (queued onto the GUI thread; pageBy then marshals to the worker).
        touchReader.moveToThread(&touchThread);
        QObject::connect(&touchThread, &QThread::started, &touchReader, &TouchReader::run);
        QObject::connect(&touchReader, &TouchReader::swipe, &app, [&engine](int dir) {
            if (dir > 0) engine.pageNext(); else engine.pagePrev();
        });
        // Reader-first tap routing (queued worker->GUI). The chrome is painted INTO the frame (B2), so we
        // hit-test it in C++: a tap on the bar runs its button; a tap on the page toggles chrome (hide when
        // shown -> read fullscreen, summon when hidden); with chrome hidden the tap-zones (tapzone.h) turn
        // pages at the edges. tap(x,y) is in panel px.
        QObject::connect(&touchReader, &TouchReader::tap, win ? win : qobject_cast<QObject*>(&app),
            [&engine, view](int x, int y) {
                switch (view->hitChrome(x, y)) {
                    case WpeView::Back:    engine.goBack();    return;
                    case WpeView::Fwd:     engine.goForward(); return;
                    case WpeView::Reload:  view->isLoading() ? engine.stopLoading() : engine.reload(); return;
                    case WpeView::Reader:  engine.toggleReader(); return;
                    case WpeView::Address: return;            // URL entry comes later (drawn OSK / HTML chrome)
                    case WpeView::None:    break;             // tap not on the bar
                }
                if (view->chromeOn()) { view->setChromeOn(false); return; }   // tap page -> hide chrome (read)
                switch (rmweb::classifyTap(x, y, kPanelW, kPanelH)) {         // chrome hidden -> tap-zones
                    case rmweb::TapAction::Next:         engine.pageNext(); break;
                    case rmweb::TapAction::Prev:         engine.pagePrev(); break;
                    case rmweb::TapAction::SummonChrome:
                    case rmweb::TapAction::Content:      view->setChromeOn(true); break;
                }
            }, Qt::QueuedConnection);
        touchThread.start();

        // DIAG: GUI event-loop heartbeat. If these "[gui] tick" lines stop, the GUI thread is blocked
        // (e.g. inside present()/swapBuffers) and queued frameReady deliveries stall -> content never paints.
        { auto *hb = new QTimer(&app);
          QObject::connect(hb, &QTimer::timeout, &app, []{ qInfo("[gui] tick"); });
          hb->start(2000); }

        // DIAG (RMWEB_GRAB_MS): grab the composited window to a PNG after N ms — captures exactly what Qt
        // presents (= what's on the e-ink), so we can SEE the result without catching the live screen.
        if (const int grabMs = qEnvironmentVariableIntValue("RMWEB_GRAB_MS"); grabMs > 0 && win) {
            QTimer::singleShot(grabMs, win, [win]{
                QImage g = win->grabWindow();
                if (!g.isNull() && g.save("/home/root/rmweb/grab.png")) qInfo("[grab] saved %dx%d", g.width(), g.height());
                else qInfo("[grab] FAILED null=%d", g.isNull());
            });
        }

        // DIAG (RMWEB_DEBUG_READER): auto-toggle reader mode once after N ms, so the reflow can be verified
        // (pair with RMWEB_GRAB_MS to capture the result) without a human tap on the Reader button.
        if (const int rdMs = qEnvironmentVariableIntValue("RMWEB_DEBUG_READER"); rdMs > 0) {
            QTimer::singleShot(rdMs, &app, [&engine]{ qInfo("[reader][dbg] toggleReader"); engine.toggleReader(); });
        }

        // Diagnostic: auto-page every RMWEB_AUTOPAGE_MS ms (alternating direction) through the exact same
        // pageBy() path as a real swipe, so page-turn latency can be measured without hand-swipe timing.
        if (const int autoMs = qEnvironmentVariableIntValue("RMWEB_AUTOPAGE_MS"); autoMs > 0) {
            auto *t = new QTimer(&app);
            QObject::connect(t, &QTimer::timeout, &app, [&engine, dir = 1]() mutable {
                engine.pageBy(dir > 0 ? kPageStepPx : -kPageStepPx);
                dir = -dir;
            });
            t->start(autoMs);
            qInfo("[t] auto-page every %d ms (diagnostic)", autoMs);
        }
    }

    thread.start();
    const int rc = app.exec();
    // Tear down in dependency order, with UNBOUNDED waits: a timed-out wait would let a thread that still
    // references `engine` (a stack object) run on past its destruction -> use-after-free -> device reboot.
    // Both threads exit promptly: TouchReader::run sees m_stop within one ~200 ms poll; the worker's
    // g_main_loop_run returns as soon as engine.stop() posts g_main_loop_quit.
    touchReader.requestStop();
    touchThread.quit();
    touchThread.wait();
    engine.stop();
    thread.quit();
    thread.wait();
    return rc;
}
