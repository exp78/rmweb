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
// NOTE: launcher runs JSC in the interpreter (JSC_useJIT=0) by default. RMWEB_JIT=1 enables the JIT, which
// works here via JSC_usePollingTraps=1 (the earlier "JIT segfault" was a signal conflict — see the launcher).
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
#include <QPolygonF>
#include <QElapsedTimer>
#include <cmath>
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
#include "keyboard.h"  // pure on-screen-keyboard layout + hit-test (unit-tested in tests/keyboard_test.cpp)
#include "profile.h"   // persistent store: bookmarks / history / settings
#include "startpage.h" // start-page HTML generator
#include <ctime>
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

// Optional mobile User-Agent (opt-in via RMWEB_UA=mobile): makes heavy JS-app sites (e.g. a heavy SPA site) serve their
// lighter MOBILE layout, which renders where the desktop one stays blank. NOT the default — a mobile UA makes
// server-rendered content sites (Wikipedia & co.) serve a JS-only mobile skin that paints blank on this CPU
// engine. iPhone Safari is an honest fit (WPE is WebKit/Safari-family).
static const char *kMobileUA =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1";

// A tiny USER stylesheet injected into every page (raw browsing): keep wide media/tables from overflowing the
// narrow e-ink viewport, so nothing forces a horizontal scroll. User-level !important beats the site's author
// rules. (Reader mode is the fuller answer for article layout.) Toggle off with RMWEB_SITECSS=0.
static const char *kSiteCss =
    "img,video,iframe,table,pre,figure,canvas{max-width:100%!important}"
    "img,video{height:auto!important}"
    "html,body{overflow-x:hidden!important}";

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
    "#rmweb-reader code{font-family:monospace;font-size:.85em}"
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
          m_cancel(g_cancellable_new()) {
        if (const char *e = getenv("RMWEB_READER_FONT")) { const int v = atoi(e); if (v >= 16 && v <= 96) m_readerFont = v; }
    }

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
    void renderFailed(bool failed);                // load finished but the page rendered ~blank (heavy SPA)
    void linkMissed();                             // a content tap hit no link -> GUI falls back to chrome toggle
    void bookmarkedChanged(bool on);               // current page bookmark state changed

public Q_SLOTS:
    void start() {
        g_main_context_push_thread_default(m_ctx);
        m_startUs = g_get_monotonic_time();

        // Load persistent profile (bookmarks, history, settings) before any WebKit activity.
        if (const char* p = getenv("RMWEB_PROFILE"); p && *p) m_profileDir = p; else m_profileDir = "/home/root/.rmweb";
        { std::string mk = "mkdir -p '" + m_profileDir + "'"; (void)system(mk.c_str()); }
        m_bookmarks = rmweb::loadBookmarks(m_profileDir);
        m_history   = rmweb::loadHistory(m_profileDir);
        m_settings  = rmweb::loadSettings(m_profileDir);
        m_zoom = m_settings.zoom;
        m_readerFont = m_settings.readerFont;
        // RMWEB_READER_FONT env wins over persisted value (same guard as ctor, re-applied after settings load).
        if (const char *e = getenv("RMWEB_READER_FONT")) { const int v = atoi(e); if (v >= 16 && v <= 96) m_readerFont = v; }

        GError *err = nullptr;
        WPEDisplay *display = wpe_display_headless_new();
        if (!display || !wpe_display_connect(display, &err)) {
            qWarning() << "[wpe] display connect failed:" << (err ? err->message : "?");
            g_clear_error(&err);
            return;
        }
        qInfo("[t] display connected @%.0fms", msSince(m_startUs));

        m_ucm = webkit_user_content_manager_new();   // holds the content-blocking filter (added async below)
        // Readability user stylesheet (kSiteCss): keep wide media/tables from forcing horizontal scroll on the
        // narrow viewport. Applies to every page; reader mode replaces the DOM so it's harmless there too.
        if (qgetenv("RMWEB_SITECSS") != "0") {
            WebKitUserStyleSheet *ss = webkit_user_style_sheet_new(
                kSiteCss, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, nullptr, nullptr);
            webkit_user_content_manager_add_style_sheet(m_ucm, ss);
            webkit_user_style_sheet_unref(ss);
        }
        m_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "display", display, "user-content-manager", m_ucm, nullptr));
        WPEView *wpeView = webkit_web_view_get_wpe_view(m_view);
        // Readability: lay the page out at device-pixel-ratio `dpr` so the CSS viewport is narrower
        // (panel/dpr) -> responsive sites reflow to a readable, fits-width layout. Toplevel sizes are
        // LOGICAL; the buffer is logical*dpr (~= the physical panel), so the display path is unchanged.
        // Tunable via RMWEB_DPR (default 1.0 = current behaviour; ~2.0 = readable). See zoom-readability.md.
        double dpr = qgetenv("RMWEB_DPR").toDouble(); if (dpr < 1.0 || dpr > 3.0) dpr = 2.0;
        m_dpr = dpr;   // panel-px -> CSS-px factor, for elementFromPoint link hit-testing on a tap
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
        g_signal_connect(m_view, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* dec,
                                           WebKitPolicyDecisionType type, gpointer data) -> gboolean {
            if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
            auto* self = static_cast<WpeEngine*>(data);
            auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(dec);
            WebKitNavigationAction* act = webkit_navigation_policy_decision_get_navigation_action(nav);
            WebKitURIRequest* req = webkit_navigation_action_get_request(act);
            const char* uri = webkit_uri_request_get_uri(req);
            if (uri && std::string(uri).rfind("rmweb:", 0) == 0) {
                if (std::string(uri) == "rmweb:clear-history") {
                    self->m_history.clear();
                    rmweb::saveHistory(self->m_profileDir, self->m_history);
                    self->goHome();
                }
                webkit_policy_decision_ignore(dec);
                return TRUE;
            }
            return FALSE;
        }), this);

        // User-Agent: env overrides; else persisted setting; else WPE default.
        // RMWEB_UA=mobile opts into lighter mobile layout for heavy JS-app sites; any other non-empty value =
        // that exact string; "off" = use WPE default (also clears any persisted UA).
        {
            std::string ua = m_settings.ua;
            if (const char *uaEnv = getenv("RMWEB_UA"); uaEnv && *uaEnv && std::string(uaEnv) != "off") ua = uaEnv;
            else if (uaEnv && std::string(uaEnv) == "off") ua = "";
            if (!ua.empty()) {
                const char* real = (ua == "mobile") ? kMobileUA : ua.c_str();
                webkit_settings_set_user_agent(webkit_web_view_get_settings(m_view), real);
                qInfo("[ua] %s", real);
            }
            m_settings.ua = ua;
        }
        // Apply persisted zoom (must be done after the view is fully set up).
        webkit_web_view_set_zoom_level(m_view, m_zoom);

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
    void goHome() {
        // Marshalled: reads/writes m_bookmarks and m_history, which the worker-thread LOAD_FINISHED also touches.
        marshalToCtx([this] {
            const std::string html = rmweb::buildStartPage(m_bookmarks, firstN(m_history, 15));
            const std::string path = m_profileDir + "/home.html";
            rmweb::detail::atomicWrite(path, html);
            loadUrl(QString::fromStdString("file://" + path));
        });
    }
    void toggleBookmark() {
        // Marshalled: reads/writes m_bookmarks, which the worker-thread LOAD_FINISHED also touches.
        marshalToCtx([this] {
            if (m_curUrl.empty()) return;
            const bool on = rmweb::toggleBookmark(m_bookmarks, m_curUrl, m_curTitle);
            rmweb::saveBookmarks(m_profileDir, m_bookmarks);
            Q_EMIT bookmarkedChanged(on);
        });
    }
    void goBack()    { marshalToCtx([this] { if (m_view && webkit_web_view_can_go_back(m_view))    webkit_web_view_go_back(m_view); }); }
    void goForward() { marshalToCtx([this] { if (m_view && webkit_web_view_can_go_forward(m_view)) webkit_web_view_go_forward(m_view); }); }
    void reload()    { marshalToCtx([this] { if (m_view) webkit_web_view_reload(m_view); }); }
    void stopLoading() { marshalToCtx([this] { if (m_view) webkit_web_view_stop_loading(m_view); }); }
    // Follow a link at panel (x,y) if there is one (probe via elementFromPoint at CSS px = panel/dpr); else emit
    // linkMissed so the GUI falls back to the chrome toggle. This is what makes us a browser: tap a link to go.
    void tapLink(int x, int y) {
        marshalToCtx([this, x, y] {
            if (!m_view) return;
            // Probe the tap point + a small neighbourhood (finger taps on tiny inline links are imprecise on e-ink).
            gchar *js = g_strdup_printf(
                "(function(x,y){var p=[[0,0],[0,-12],[0,12],[-12,0],[12,0],[-22,0],[22,0],[0,-22],[0,22]];"
                "for(var i=0;i<p.length;i++){var e=document.elementFromPoint(x+p[i][0],y+p[i][1]);"
                "var a=e&&e.closest?e.closest('a[href]'):0;if(a&&a.href){location.href=a.href;return true;}}return false;})(%d,%d)",
                int(x / m_dpr), int(y / m_dpr));
            webkit_web_view_evaluate_javascript(m_view, js, -1, nullptr, nullptr, m_cancel, &WpeEngine::onTapLink, this);
            g_free(js);
        });
    }
    void pageNext()  { pageBy(kPageStepPx); }   // façade page-turn (wraps the scroll+repaint in pageBy)
    void pagePrev()  { pageBy(-kPageStepPx); }
    // Text size -/+ (the A-/A+ chrome buttons): page zoom in normal mode, reader font in reader mode.
    void zoomBy(int dir) {
        marshalToCtx([this, dir] {
            if (!m_view) return;
            if (m_readerMode) {
                m_readerFont = std::clamp(m_readerFont + (dir > 0 ? 4 : -4), 22, 64);   // reader column font px
                gchar *js = g_strdup_printf("var r=document.getElementById('rmweb-reader');if(r)r.style.fontSize='%dpx';", m_readerFont);
                webkit_web_view_evaluate_javascript(m_view, js, -1, nullptr, nullptr, m_cancel, nullptr, nullptr);
                g_free(js);
            } else {
                m_zoom = std::clamp(m_zoom * (dir > 0 ? 1.2 : 1.0 / 1.2), 0.5, 3.0);     // page zoom level
                webkit_web_view_set_zoom_level(m_view, m_zoom);
            }
            qInfo("[zoom] reader=%d zoom=%.2f font=%d", m_readerMode, m_zoom, m_readerFont);
            m_settings.zoom = m_zoom; m_settings.readerFont = m_readerFont;
            rmweb::saveSettings(m_profileDir, m_settings);
        });
    }
    // Reader mode: inject Readability + reflow the article into one clean column; toggle off = reload original.
    void toggleReader() {
        marshalToCtx([this] {
            if (!m_view || m_readerApplying) return;                        // ignore taps while a parse is in flight
            if (m_readerMode) { webkit_web_view_reload(m_view); return; }   // off: reload (COMMITTED clears state)
            applyReader();
        });
    }

private:
    static std::vector<rmweb::HistoryEntry> firstN(const std::vector<rmweb::HistoryEntry>& v, size_t n) {
        return { v.begin(), v.begin() + std::min(n, v.size()) };
    }
    // Run fn on the worker thread's GMainContext (g_main_context_invoke_full is MT-safe).
    void marshalToCtx(std::function<void()> fn) {
        auto *f = new std::function<void()>(std::move(fn));
        g_main_context_invoke_full(m_ctx, G_PRIORITY_DEFAULT,
            [](gpointer d) -> gboolean { (*static_cast<std::function<void()>*>(d))(); return G_SOURCE_REMOVE; },
            f, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
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
            webkit_web_view_evaluate_javascript(self->m_view, js, -1, nullptr, nullptr, self->m_cancel,
                                                &WpeEngine::onJsDone, self);
            g_free(js);
            qInfo("[t] pageBy(%d) @%.0fms", static_cast<int>(m->dy), msSince(self->m_startUs));
        }
        return G_SOURCE_REMOVE;
    }

    // Finish an evaluate_javascript call: returns the JSCValue (caller unrefs) or nullptr; sets *cancelled
    // when the engine is tearing down (m_cancel fired) so the caller touches nothing (self may be gone).
    static JSCValue *finishJsEval(GObject *obj, GAsyncResult *res, bool *cancelled) {
        *cancelled = false;
        GError *err = nullptr;
        JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
        if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { *cancelled = true; g_clear_error(&err); return nullptr; }
        if (!v) g_clear_error(&err);
        return v;
    }
    static void onJsDone(GObject *obj, GAsyncResult *res, gpointer data) {
        bool cancelled; JSCValue *v = finishJsEval(obj, res, &cancelled);
        if (cancelled) return;
        auto *self = static_cast<WpeEngine*>(data);
        double scrollY = -1;
        if (v) { if (jsc_value_is_number(v)) scrollY = jsc_value_to_double(v); g_object_unref(v); }
        if (self) qInfo("[t] page JS done scrollY=%.0f @%.0fms", scrollY, msSince(self->m_startUs));
    }
    static void onTapLink(GObject *obj, GAsyncResult *res, gpointer data) {
        bool cancelled; JSCValue *v = finishJsEval(obj, res, &cancelled);
        if (cancelled) return;
        auto *self = static_cast<WpeEngine*>(data);
        bool followed = false;
        if (v) { if (jsc_value_is_boolean(v)) followed = jsc_value_to_boolean(v); g_object_unref(v); }
        if (!self) return;
        qInfo("[link] followed=%d", followed);
        if (!followed) Q_EMIT self->linkMissed();
    }
    // Build the apply script: the vendored Readability lib + our glue, with the reader CSS (font size from
    // RMWEB_READER_FONT, default 38) inlined. Injected in one shot so all symbols share the same scope.
    std::string buildReaderApplyJs() {
        std::string css = kReaderCss;  replaceAll(css, "__FS__", std::to_string(m_readerFont));   // A-/A+ adjustable
        std::string glue = kReaderGlue; replaceAll(glue, "__CSS__", css);
        return m_readabilityJs + "\n" + glue;
    }
    void applyReader() {
        if (m_readabilityJs.empty()) m_readabilityJs = slurp(readerDir() + "/Readability.js");
        if (m_readabilityJs.empty()) { qWarning("[reader] Readability.js missing in %s", readerDir().c_str()); return; }
        const std::string js = buildReaderApplyJs();
        m_readerApplying = true;   // gate re-entrant Reader taps until onReaderApplied clears it
        // Pass m_cancel so a shutdown (stop() cancels it) aborts an in-flight eval — same pattern as onFilterSaved.
        webkit_web_view_evaluate_javascript(m_view, js.c_str(), -1, nullptr, nullptr, m_cancel,
                                            &WpeEngine::onReaderApplied, this);
    }
    static void onReaderApplied(GObject *obj, GAsyncResult *res, gpointer data) {
        GError *err = nullptr;
        JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(obj), res, &err);
        // Cancelled => engine is tearing down (m_cancel fired): self may be gone — touch nothing.
        if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { g_clear_error(&err); return; }
        auto *self = static_cast<WpeEngine*>(data);
        std::string st = "error";
        if (v) { if (jsc_value_is_string(v)) { char *s = jsc_value_to_string(v); st = s ? s : ""; g_free(s); } g_object_unref(v); }
        else { st = err ? err->message : "?"; g_clear_error(&err); }
        if (!self) return;
        self->m_readerApplying = false;
        if (st == "ok") { self->m_readerMode = true; Q_EMIT self->readerModeChanged(true); qInfo("[reader] applied"); }
        else qWarning("[reader] not applied: %s", st.c_str());
    }
    // After each load: does the page look like an article? -> enable/disable the Reader button.
    void checkReaderable() {
        if (m_readerableJs.empty()) m_readerableJs = slurp(readerDir() + "/Readability-readerable.js");
        if (m_readerableJs.empty()) return;   // can't tell -> leave the button as it is
        const std::string js = m_readerableJs + "\nisProbablyReaderable(document);";
        webkit_web_view_evaluate_javascript(m_view, js.c_str(), -1, nullptr, nullptr, m_cancel,
                                            &WpeEngine::onReaderableChecked, this);
    }
    static void onReaderableChecked(GObject *obj, GAsyncResult *res, gpointer data) {
        bool cancelled; JSCValue *v = finishJsEval(obj, res, &cancelled);
        if (cancelled) return;
        auto *self = static_cast<WpeEngine*>(data);
        bool can = false;
        if (v) { if (jsc_value_is_boolean(v)) can = jsc_value_to_boolean(v); g_object_unref(v); }
        if (!self) return;
        qInfo("[reader] readerable=%d", can);
        Q_EMIT self->readerableChanged(can);
    }

    // After a load finishes, wait a grace period (let a slow SPA populate) then check whether the page rendered
    // any visible content — a heavy client-side app finishes loading but builds ~nothing on the CPU interpreter.
    // m_loadGen invalidates the check if the user navigated away during the grace period.
    static constexpr int kRenderTimeoutMs = 13000;  // after load-start: time to give a heavy page to paint
    static constexpr int kBlankSamples = 8;         // fewer than this many non-white frame samples = ~blank
    struct RenderCheckMsg { WpeEngine *self; guint gen; };
    void scheduleRenderCheck() {
        auto *m = new RenderCheckMsg{ this, m_loadGen };
        GSource *s = g_timeout_source_new(kRenderTimeoutMs);
        g_source_set_callback(s, [](gpointer d) -> gboolean {
            auto *msg = static_cast<RenderCheckMsg*>(d);
            WpeEngine *self = msg->self;
            // Same load, not in reader: if the latest web frame is essentially WHITE, the page rendered no
            // visible content (a heavy SPA whose JS the CPU can't run). Flag it so the shell shows a notice.
            // Pixel-based (not DOM): an SPA shell has DOM nodes but paints nothing, so DOM heuristics lie.
            // A later non-white frame auto-clears the flag in onBuffer (a slow-but-rendering site recovers).
            if (self->m_loadGen == msg->gen && !self->m_readerMode) {
                const bool blank = self->m_lastNonWhite < kBlankSamples;
                qInfo("[render] nonWhite=%d blank=%d", self->m_lastNonWhite, blank);
                self->m_renderFailedState = blank;
                Q_EMIT self->renderFailed(blank);
            }
            return G_SOURCE_REMOVE; }, m, [](gpointer d) { delete static_cast<RenderCheckMsg*>(d); });
        g_source_attach(s, m_ctx);
        g_source_unref(s);
    }

    void loadInitial() {
        if (m_url.isEmpty()) {
            // No URL given: show the start page (bookmarks + recent history). goHome() is already
            // marshalled to the worker context via marshalToCtx, which is safe to call from here
            // (we ARE on the worker context, so the inner marshalToCtx re-posts to the same context —
            // harmless, and keeps the identical dispatch path as a tap-router-initiated goHome()).
            const std::string html = rmweb::buildStartPage(m_bookmarks, firstN(m_history, 15));
            const std::string path = m_profileDir + "/home.html";
            rmweb::detail::atomicWrite(path, html);
            webkit_web_view_load_uri(m_view, ("file://" + path).c_str());
        } else {
            webkit_web_view_load_uri(m_view, m_url.toUtf8().constData());
        }
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
            self->m_loadGen++;                          // invalidate any pending render-check from a prior load
            self->m_renderFailedState = false;
            self->m_lastNonWhite = 0;                   // no frames yet => blank until onBuffer proves otherwise
            qInfo("[t] load started @%.0fms", msSince(self->m_startUs));
            Q_EMIT self->loadingChanged(true);
            Q_EMIT self->renderFailed(false);           // new load -> clear any "couldn't render" notice
            Q_EMIT self->tlsChanged(true, QString());   // optimistic; onTlsError flips it on a cert failure
            self->scheduleRenderCheck();                // LOAD_STARTED always fires -> robust blank-check trigger
        }
        if (ev == WEBKIT_LOAD_FINISHED) {
            self->m_reloadAttempts = 0;                  // a good load refills the crash auto-reload budget
            Q_EMIT self->loadingChanged(false);
            self->checkReaderable();                     // article? -> enable/disable the Reader button
            qInfo("[t] load finished @%.0fms", msSince(self->m_startUs));
            // Record history for real web pages (not file:// start page, not reader-injected DOM).
            {
                const char* u = webkit_web_view_get_uri(view);
                const char* t = webkit_web_view_get_title(view);
                std::string url = u ? u : "";
                if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
                    self->m_curUrl = url; self->m_curTitle = t ? t : "";
                    rmweb::addHistory(self->m_history, url, self->m_curTitle, (long)time(nullptr));
                    rmweb::saveHistory(self->m_profileDir, self->m_history);
                    Q_EMIT self->bookmarkedChanged(rmweb::isBookmarked(self->m_bookmarks, url));
                } else {
                    self->m_curUrl.clear(); self->m_curTitle.clear();
                    Q_EMIT self->bookmarkedChanged(false);
                }
            }
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
        int nonWhite = 0;
        for (int yy = 0; yy < h; yy += 40) {
            const uchar *row = pix + static_cast<gsize>(yy) * stride;
            for (int xx = 0; xx < w; xx += 40) {
                const uchar *px = row + xx * 4;        // BGRA: [0]=B [1]=G [2]=R — fingerprint on luminance,
                const uchar lum = static_cast<uchar>((px[2] * 299u + px[1] * 587u + px[0] * 114u) / 1000u);
                sig = (sig ^ lum) * 16777619u;         // not one channel (else red<->green frames look identical)
                if (lum < 245) ++nonWhite;             // darker-than-white sample => the page painted content
            }
        }
        self->m_lastNonWhite = nonWhite;              // latest frame's content density (low => ~blank render)
        if (nonWhite >= kBlankSamples && self->m_renderFailedState) {   // content finally painted -> clear notice
            self->m_renderFailedState = false; Q_EMIT self->renderFailed(false);
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
    int m_lastNonWhite = 9999; // non-white grid samples in the latest frame (low => ~blank => render failed)
    bool m_renderFailedState = false; // currently flagged blank (so a later content frame can auto-clear it)
    int m_reloadAttempts = 0; // WebProcess-crash auto-reload budget (reset on a successful load)
    guint m_loadGen = 0;           // bumped on each load start -> a stale render-check (grace timer) is skipped
    bool m_readerMode = false;     // reader view currently applied (vs the original page)
    bool m_readerApplying = false; // an applyReader() JS eval is in flight (gates re-entrant Reader taps)
    std::string m_readabilityJs;   // vendored Readability.js, lazily slurped + cached
    std::string m_readerableJs;    // vendored isProbablyReaderable, lazily slurped + cached
    double m_dpr = 2.0;            // panel-px -> CSS-px divisor (for elementFromPoint link hit-testing)
    double m_zoom = 1.0;           // page zoom level (A-/A+ in normal mode; webkit_web_view_set_zoom_level)
    int m_readerFont = 38;         // reader column font px (A-/A+ in reader mode; RMWEB_READER_FONT default)
    std::string m_profileDir;                       // /home/root/.rmweb (or $RMWEB_PROFILE)
    std::vector<rmweb::Bookmark> m_bookmarks;
    std::vector<rmweb::HistoryEntry> m_history;
    rmweb::Settings m_settings;
    std::string m_curUrl, m_curTitle;               // current committed page (for history + bookmark)
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
        m_keys = rmweb::buildKeyboard(kPanelW, kPanelH, kKbTopY);   // URL keyboard, drawn into the frame (B2)
    }
    void paint(QPainter *p) override {
        const qreal w = width(), h = height();
        if (!m_img.isNull()) p->drawImage(QRectF(0, 0, w, h), m_img);
        else                 p->fillRect(QRectF(0, 0, w, h), Qt::white);
        if (m_loading && !m_renderFailed) drawLoadingBadge(p, w);    // a never-finishing blank SPA stays
        else if (m_renderFailed)          drawRenderNotice(p, w, h); // "loading" -> notice wins over the badge
        if (!m_chromeOn) return;                                     // reader-fullscreen: nothing below the bar
        drawChromeBar(p, w);
        if (!m_editing) return;          // the keyboard shows only during URL entry (entered via the Address
        drawKeyboard(p, w, h);           // hit, which needs chrome on -> editing always implies the bar shows)
    }
    // Hit-test a tap against the chrome bar (panel px); returns the control, or None (off / below the bar).
    enum Hit { None, Back, Fwd, Reload, Home, Address, ZoomOut, ZoomIn, Bookmark, Reader, Power };
    Hit hitChrome(int x, int y) const {
        if (!m_chromeOn || y >= kBarH) return None;
        const int powerX  = int(width()) - kPowerW;         // right: A- | A+ | ★ | Reader | Power
        const int readerX = powerX - kReaderW;
        const int starX   = readerX - kStarW;
        const int zInX = starX - kZoomW, zOutX = zInX - kZoomW;
        if (x < kBackX)         return Back;
        if (x < kFwdX)          return Fwd;
        if (x < kRelX)          return Reload;
        if (x < kRelX + kHomeW) return Home;                // Home sits just after Reload
        if (x >= powerX)        return Power;
        if (x >= readerX)       return Reader;
        if (x >= starX)         return Bookmark;
        if (x >= zInX)          return ZoomIn;
        if (x >= zOutX)         return ZoomOut;
        return Address;
    }
    bool chromeOn()  const { return m_chromeOn; }
    bool isLoading() const { return m_loading; }
    bool isEditing() const { return m_editing; }
    // URL entry: tapping the address field calls beginEdit() -> the on-screen keyboard shows; key taps feed
    // m_editBuf; Go emits urlEntered (main() -> engine.loadUrl); Cancel/Go end the edit. The tap router in
    // main() routes ALL taps here while editing.
    void beginEdit() { m_editing = true; m_editBuf.clear(); schedule(); }
    void endEdit()   { if (m_editing) { m_editing = false; schedule(); } }
    void handleEditTap(int x, int y) {
        const int i = rmweb::hitKey(m_keys, x, y);
        if (i < 0) return;                                       // tap outside the keys (page area) -> ignore
        switch (m_keys[i].kind) {
            case rmweb::KeyKind::Char:      m_editBuf += QString::fromStdString(m_keys[i].insert); break;
            case rmweb::KeyKind::Backspace: m_editBuf.chop(1); break;
            case rmweb::KeyKind::Cancel:    endEdit(); return;
            case rmweb::KeyKind::Go: { const QString u = m_editBuf; endEdit();
                                       if (!u.isEmpty()) Q_EMIT urlEntered(u); return; }
        }
        schedule();
    }
Q_SIGNALS:
    void urlEntered(const QString &url);   // Go pressed with a non-empty buffer -> load it (wired in main())
public Q_SLOTS:
    void setImage(const QImage &img) { m_pending = img; m_hasPending = true; schedule(); }
    // Chrome state (fed by engine signals on the GUI thread). Each re-presents the current frame with the
    // new chrome via the SAME serializer — never a bare update() (that would risk an overlapping present).
    void setChromeOn(bool v)       { if (v != m_chromeOn) { m_chromeOn = v; schedule(); } }
    void setCanBack(bool v)        { if (v != m_canBack)  { m_canBack  = v; schedule(); } }
    void setCanFwd(bool v)         { if (v != m_canFwd)   { m_canFwd   = v; schedule(); } }
    void setLoading(bool v)        { if (v != m_loading)  { m_loading  = v; if (v) m_loadProgress = 0.0; schedule(); } }
    void setLoadProgress(double p) {                       // throttle repaints to ~10% steps (limit e-ink flicker)
        const bool step = int(p * 10) != int(m_loadProgress * 10);
        m_loadProgress = p; if (step && m_loading) schedule();
    }
    void setRenderFailed(bool v)   { if (v != m_renderFailed) { m_renderFailed = v; schedule(); } }
    void setAddr(const QString &s) { if (s != m_addr)     { m_addr     = s; schedule(); } }
    void setReaderMode(bool v)     { if (v != m_readerMode)  { m_readerMode  = v; schedule(); } }
    void setReaderable(bool v)     { if (v != m_readerable) { m_readerable = v; schedule(); } }
    void setBookmarked(bool v) { if (v != m_bookmarked) { m_bookmarked = v; schedule(); } }
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
    // --- B2 frame painters (called by paint(); kept here so paint() stays a short orchestrator) -------------
    // "Working hard" indicator while a page loads: an hourglass + "Загрузка NN%" from the real load progress.
    void drawLoadingBadge(QPainter *p, qreal w) const {
        const QString lbl = QStringLiteral("Загрузка %1%").arg(int(m_loadProgress * 100));
        QFont lf = p->font(); lf.setPixelSize(40); p->setFont(lf);
        const qreal tw = p->fontMetrics().horizontalAdvance(lbl);
        const qreal iconW = 34, pad = 30, gap = 18, bh = 96, bw = pad + iconW + gap + tw + pad;
        const qreal bx = (w - bw) / 2, by = kBarH + 50, ix = bx + pad, iy = by + (bh - 48) / 2;
        p->setPen(Qt::black); p->setBrush(Qt::white);
        p->drawRoundedRect(QRectF(bx, by, bw, bh), 18, 18);
        QPolygonF top, bot;                                          // hourglass = two triangles
        top << QPointF(ix, iy) << QPointF(ix + iconW, iy) << QPointF(ix + iconW / 2, iy + 24);
        bot << QPointF(ix + iconW / 2, iy + 24) << QPointF(ix, iy + 48) << QPointF(ix + iconW, iy + 48);
        p->setBrush(Qt::black); p->drawPolygon(top); p->drawPolygon(bot);
        p->setBrush(Qt::NoBrush); p->setPen(Qt::black);
        p->drawText(QRectF(ix + iconW + gap, by, tw + 6, bh), Qt::AlignVCenter | Qt::AlignLeft, lbl);
    }
    // Load finished but the page rendered ~nothing (a heavy JS app the CPU can't run). "(!)" + two lines.
    void drawRenderNotice(QPainter *p, qreal w, qreal h) const {
        const QString t1 = QStringLiteral("Не удалось отобразить страницу");
        const QString t2 = QStringLiteral("тяжёлый сайт или веб-приложение");
        QFont f1 = p->font(); f1.setPixelSize(46);
        QFont f2 = p->font(); f2.setPixelSize(34);
        p->setFont(f1); const qreal w1 = p->fontMetrics().horizontalAdvance(t1);
        p->setFont(f2); const qreal w2 = p->fontMetrics().horizontalAdvance(t2);
        const qreal icon = 64, padX = 44, gap = 30, textW = qMax(w1, w2), bh = 210;
        const qreal bw = padX + icon + gap + textW + padX, bx = (w - bw) / 2, by = h * 0.30;
        p->setPen(Qt::black); p->setBrush(Qt::white);
        p->drawRoundedRect(QRectF(bx, by, bw, bh), 20, 20);
        const qreal cx = bx + padX + icon / 2, cy = by + bh / 2;       // warning icon: a circle with "!"
        QPen wp(Qt::black); wp.setWidth(4); p->setPen(wp); p->setBrush(Qt::NoBrush);
        p->drawEllipse(QPointF(cx, cy), icon / 2, icon / 2);
        QFont fi = f1; fi.setBold(true); p->setFont(fi); p->setPen(Qt::black);
        p->drawText(QRectF(cx - icon / 2, cy - icon / 2, icon, icon), Qt::AlignCenter, "!");
        const qreal tx = bx + padX + icon + gap;
        p->setFont(f1); p->setPen(Qt::black);
        p->drawText(QRectF(tx, by + 44, textW, 60), Qt::AlignLeft | Qt::AlignVCenter, t1);
        p->setFont(f2); p->setPen(QColor(90, 90, 90));
        p->drawText(QRectF(tx, by + 116, textW, 50), Qt::AlignLeft | Qt::AlignVCenter, t2);
        p->setPen(Qt::black);
    }
    // B2 browser chrome painted straight into the frame (QtQuick chrome does NOT composite over it; see
    // docs/research/epaper-chrome-compositing.md): Back/Fwd/Reload icons + address(+caret) + A-/A+ + Reader.
    void drawChromeBar(QPainter *p, qreal w) const {
        p->fillRect(QRectF(0, 0, w, kBarH), Qt::white);
        p->fillRect(QRectF(0, kBarH - 3, w, 3), Qt::black);
        const qreal cy = kBarH / 2.0;
        auto pen = [&](bool on) { p->setPen(on ? Qt::black : QColor(170, 170, 170)); p->setBrush(Qt::NoBrush); };
        pen(m_canBack); iconBack(p, kBackX / 2.0, cy);
        pen(m_canFwd);  iconFwd (p, (kBackX + kFwdX) / 2.0, cy);
        pen(true);      m_loading ? iconStop(p, (kFwdX + kRelX) / 2.0, cy) : iconReload(p, (kFwdX + kRelX) / 2.0, cy);
        pen(true);      iconHome(p, kRelX + kHomeW / 2.0, cy);
        const qreal powerX = w - kPowerW, readerX = powerX - kReaderW, starX = readerX - kStarW;
        const qreal zInX = starX - kZoomW, zOutX = zInX - kZoomW;
        QFont af = p->font(); af.setPixelSize(34); p->setFont(af); p->setPen(Qt::black);
        const QString addrText = m_editing ? (m_editBuf + "|") : m_addr;   // editing -> typed buffer + caret
        const auto elide = m_editing ? Qt::ElideLeft : Qt::ElideRight;     // keep the caret end visible while typing
        const int addrX = kRelX + kHomeW;
        const QString a = p->fontMetrics().elidedText(addrText, elide, int(zOutX - addrX - 40));
        p->drawText(QRectF(addrX + 20, 0, zOutX - addrX - 40, kBarH), Qt::AlignVCenter, a);
        // Text size -/+ : page zoom normally, reader font in reader mode ("A-"/"A+" is the conventional size icon).
        QFont zf = p->font(); zf.setPixelSize(40); zf.setBold(true); p->setFont(zf); p->setPen(Qt::black);
        p->drawText(QRectF(zOutX, 0, kZoomW, kBarH), Qt::AlignCenter, "A-");
        p->drawText(QRectF(zInX,  0, kZoomW, kBarH), Qt::AlignCenter, "A+");
        // Reader (document icon): inverted on a black pill when active; greyed when the page isn't an article.
        const qreal rcx = readerX + kReaderW / 2.0;
        if (m_readerMode) {
            p->setBrush(Qt::black); p->setPen(Qt::NoPen);
            p->drawRoundedRect(QRectF(readerX + 20, 12, kReaderW - 40, kBarH - 24), 12, 12);
            p->setPen(Qt::white); p->setBrush(Qt::NoBrush); iconReader(p, rcx, cy);
        } else { pen(m_readerable); iconReader(p, rcx, cy); }
        pen(true); iconStar(p, starX + kStarW / 2.0, cy, m_bookmarked);
        pen(true); iconPower(p, powerX + kPowerW / 2.0, cy);   // exit to the reMarkable menu
    }
    void iconHome(QPainter *p, qreal cx, qreal cy) const {                 // a simple house
        QPen pn = p->pen(); pn.setWidthF(4); pn.setJoinStyle(Qt::RoundJoin); p->setPen(pn); p->setBrush(Qt::NoBrush);
        const qreal w = 20, r = 16;
        QPolygonF roof; roof << QPointF(cx - w, cy) << QPointF(cx, cy - r) << QPointF(cx + w, cy);
        p->drawPolyline(roof);
        p->drawRect(QRectF(cx - w + 4, cy, 2 * (w - 4), r));
    }
    void iconStar(QPainter *p, qreal cx, qreal cy, bool filled) const {    // 5-point star, filled if bookmarked
        QPen pn = p->pen(); pn.setWidthF(4); pn.setJoinStyle(Qt::RoundJoin); p->setPen(pn);
        const qreal R = 18, r = 7.2; QPolygonF star;
        for (int i = 0; i < 10; ++i) {
            const double ang = -3.14159265 / 2 + i * 3.14159265 / 5;
            const double rad = (i % 2 == 0) ? R : r;
            star << QPointF(cx + rad * std::cos(ang), cy + rad * std::sin(ang));
        }
        if (filled) { p->setBrush(p->pen().color()); p->drawPolygon(star); p->setBrush(Qt::NoBrush); }
        else          p->drawPolygon(star);
    }
    void iconPower(QPainter *p, qreal cx, qreal cy) const {                // power symbol: ring (gap at top) + bar
        QPen pn = p->pen(); pn.setWidthF(5); pn.setCapStyle(Qt::RoundCap); p->setPen(pn); p->setBrush(Qt::NoBrush);
        const qreal r = 17;
        p->drawArc(QRectF(cx - r, cy - r, 2 * r, 2 * r), 120 * 16, 300 * 16);   // open at the top
        p->drawLine(QPointF(cx, cy - r - 5), QPointF(cx, cy - 1));              // the "I" through the gap
    }
    // --- Vector chrome icons (drawn, not font glyphs -> crisp + font-independent on e-ink). Caller sets pen colour.
    void iconBack(QPainter *p, qreal cx, qreal cy) const { drawArrow(p, cx, cy, -1); }
    void iconFwd (QPainter *p, qreal cx, qreal cy) const { drawArrow(p, cx, cy, +1); }
    void drawArrow(QPainter *p, qreal cx, qreal cy, int dir) const {       // dir: -1 left (Back), +1 right (Fwd)
        QPen pn = p->pen(); pn.setWidthF(5); pn.setCapStyle(Qt::RoundCap); pn.setJoinStyle(Qt::RoundJoin); p->setPen(pn);
        const qreal hw = 21, hd = 12, tip = cx + dir * hw;
        p->drawLine(QPointF(cx - dir * hw, cy), QPointF(tip, cy));
        p->drawLine(QPointF(tip, cy), QPointF(tip - dir * hd, cy - hd));
        p->drawLine(QPointF(tip, cy), QPointF(tip - dir * hd, cy + hd));
    }
    void iconStop(QPainter *p, qreal cx, qreal cy) const {                 // filled rounded square
        p->setBrush(p->pen().color()); p->setPen(Qt::NoPen);
        p->drawRoundedRect(QRectF(cx - 15, cy - 15, 30, 30), 5, 5);
        p->setBrush(Qt::NoBrush);
    }
    void iconReload(QPainter *p, qreal cx, qreal cy) const {               // ~circular arrow with an arrowhead
        QPen pn = p->pen(); pn.setWidthF(5); pn.setCapStyle(Qt::RoundCap); p->setPen(pn); p->setBrush(Qt::NoBrush);
        const qreal r = 16, deg = 55, pi = 3.14159265358979;
        p->drawArc(QRectF(cx - r, cy - r, 2 * r, 2 * r), int(deg * 16), 280 * 16);   // gap at the lower-right
        const qreal a = deg * pi / 180.0;                                 // arrowhead at the upper-right arc end
        const QPointF t(cx + r * std::cos(a), cy - r * std::sin(a));
        QPolygonF head; head << t << QPointF(t.x() - 15, t.y() - 4) << QPointF(t.x() - 2, t.y() - 17);
        p->setBrush(p->pen().color()); p->setPen(Qt::NoPen); p->drawPolygon(head); p->setBrush(Qt::NoBrush);
    }
    void iconReader(QPainter *p, qreal cx, qreal cy) const {               // a document with text lines
        QPen pn = p->pen(); pn.setWidthF(4); pn.setJoinStyle(Qt::RoundJoin); p->setPen(pn); p->setBrush(Qt::NoBrush);
        const qreal hw = 15, ht = 20;
        p->drawRoundedRect(QRectF(cx - hw, cy - ht, 2 * hw, 2 * ht), 4, 4);
        QPen lp = p->pen(); lp.setWidthF(3); p->setPen(lp);
        for (int i = -1; i <= 1; ++i) p->drawLine(QPointF(cx - hw + 7, cy + i * 9), QPointF(cx + hw - 7, cy + i * 9));
    }
    // On-screen URL keyboard, drawn into the frame (B2). Taps -> handleEditTap() (keyboard.h hitKey) via main().
    void drawKeyboard(QPainter *p, qreal w, qreal h) const {
        p->fillRect(QRectF(0, kKbTopY, w, h - kKbTopY), Qt::white);
        p->fillRect(QRectF(0, kKbTopY, w, 2), Qt::black);
        QFont kf = p->font(); kf.setPixelSize(44); p->setFont(kf);
        for (const rmweb::Key &k : m_keys) {
            const QRectF r(k.x, k.y, k.w, k.h);
            if (k.kind == rmweb::KeyKind::Go) { p->fillRect(r.adjusted(3, 3, -3, -3), Qt::black); p->setPen(Qt::white); }
            else { p->setPen(Qt::black); p->drawRect(r.adjusted(2, 2, -2, -2)); }
            p->drawText(r, Qt::AlignCenter, QString::fromStdString(k.label));
        }
    }
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
    static const int kBarH = 104, kBackX = 170, kFwdX = 340, kRelX = 560, kReaderW = 190, kZoomW = 120, kPowerW = 130;
    static const int kHomeW = 140, kStarW = 120;   // Home (left, after Reload) + bookmark star (right, before Reader)
    bool m_chromeOn = true, m_canBack = false, m_canFwd = false, m_loading = false;
    qreal m_loadProgress = 0.0;          // 0..1 estimated load progress (drives the loading badge)
    bool m_renderFailed = false;         // load finished but the page is ~blank (heavy SPA) -> show a notice
    bool m_readerMode = false, m_readerable = false;
    bool m_bookmarked = false;   // current page is bookmarked -> filled star
    QString m_addr;
    bool m_editing = false;             // URL-entry mode: the on-screen keyboard is shown over the page
    QString m_editBuf;                  // the URL currently being typed
    std::vector<rmweb::Key> m_keys;     // keyboard layout, built once in the ctor
    static const int kKbTopY = 1340;    // keyboard occupies [kKbTopY, kPanelH) in panel px
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

// Reading-shell host: a bare full-screen Window holding the WpeView. The browser chrome is hand-painted
// INTO the WpeView frame (the "B2" approach) — a QtQuick toolbar does NOT composite under the epaper QPA, so
// there is no QML chrome here; taps are hit-tested in C++ (the tap router in main()). Size to Screen.* (the
// official recipe — don't force geometry from C++); objectName "view" is how main() finds the item.
static const char *kQml = R"QML(
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
        // No "engine" context property: the chrome is C++ (B2), and kQml doesn't reference engine.
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
        QObject::connect(&engine, &WpeEngine::loadProgressChanged, view, &WpeView::setLoadProgress);
        QObject::connect(&engine, &WpeEngine::urlChanged,     view, &WpeView::setAddr);
        QObject::connect(&engine, &WpeEngine::readerModeChanged, view, &WpeView::setReaderMode);
        QObject::connect(&engine, &WpeEngine::readerableChanged, view, &WpeView::setReaderable);
        QObject::connect(&engine, &WpeEngine::bookmarkedChanged, view,
                         [view](bool on){ view->setBookmarked(on); }, Qt::QueuedConnection);
        QObject::connect(&engine, &WpeEngine::renderFailed,      view, &WpeView::setRenderFailed);
        // URL entry: the on-screen keyboard's Go (WpeView::urlEntered) -> load it (engine.loadUrl normalizes).
        QObject::connect(view, &WpeView::urlEntered, &app, [&engine](const QString &u){ engine.loadUrl(u); });

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
                if (view->isEditing()) { view->handleEditTap(x, y); return; }   // keyboard captures all taps
                switch (view->hitChrome(x, y)) {
                    case WpeView::Back:    engine.goBack();    return;
                    case WpeView::Fwd:     engine.goForward(); return;
                    case WpeView::Reload:  view->isLoading() ? engine.stopLoading() : engine.reload(); return;
                    case WpeView::Home:    engine.goHome();     return;
                    case WpeView::Reader:  engine.toggleReader(); return;
                    case WpeView::ZoomOut: engine.zoomBy(-1);   return;
                    case WpeView::ZoomIn:  engine.zoomBy(+1);   return;
                    case WpeView::Address: view->beginEdit();  return;   // open the on-screen URL keyboard
                    case WpeView::Bookmark: engine.toggleBookmark(); return;
                    case WpeView::Power:   std::_Exit(0);      return;   // quit to menu; launcher restores xochitl
                    case WpeView::None:    break;             // tap not on the bar
                }
                // Reading (chrome hidden): edge/top zones are fast gestures; the centre falls through to a link probe.
                if (!view->chromeOn()) {
                    switch (rmweb::classifyTap(x, y, kPanelW, kPanelH)) {
                        case rmweb::TapAction::Next:         engine.pageNext();       return;
                        case rmweb::TapAction::Prev:         engine.pagePrev();       return;
                        case rmweb::TapAction::SummonChrome: view->setChromeOn(true); return;
                        case rmweb::TapAction::Content:      break;   // centre -> probe for a link below
                    }
                }
                engine.tapLink(x, y);   // follow a link at (x,y); on a miss engine.linkMissed -> toggle chrome
            }, Qt::QueuedConnection);
        // A content tap with no link underneath -> the old behaviour: toggle the chrome (show <-> hide).
        QObject::connect(&engine, &WpeEngine::linkMissed, win ? win : qobject_cast<QObject*>(&app),
            [view]{ view->setChromeOn(!view->chromeOn()); }, Qt::QueuedConnection);
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

        // DIAG (RMWEB_DEBUG_KB): open the URL keyboard after N ms so its rendering can be grabbed (RMWEB_GRAB_MS).
        if (const int kbMs = qEnvironmentVariableIntValue("RMWEB_DEBUG_KB"); kbMs > 0) {
            QTimer::singleShot(kbMs, &app, [view]{ qInfo("[kb][dbg] beginEdit"); view->beginEdit(); });
        }

        // DIAG (RMWEB_DEBUG_ZOOM): bump page zoom +2 steps after N ms (verify the scaling with RMWEB_GRAB_MS).
        if (const int zMs = qEnvironmentVariableIntValue("RMWEB_DEBUG_ZOOM"); zMs > 0) {
            QTimer::singleShot(zMs, &app, [&engine]{ qInfo("[zoom][dbg] +2"); engine.zoomBy(1); engine.zoomBy(1); });
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
