// engine/wpeqt/main.cpp — rmweb core (WPE WebKit on reMarkable Paper Pro)
//
// Current status (post-Phase 5): full reading browser with B2 chrome, reader mode, touch gestures,
// keyboard, bookmarks, history, zoom, phantom-touch guard, llvmpipe rendering (~120-250ms page turns).
//
// Architecture: WpeEngine (worker thread + WebKit) → frameReady signal → WpeView (QQuickPaintedItem).
// Input via direct evdev (event3 = touch), not Qt (epaper QPA drops it). All under /home/root/rmweb.
//
// See: CLAUDE.md, docs/research/*.md, docs/superpowers/specs/2026-06-30-rmweb-phase5-packaging-design.md
#include <QGuiApplication>
#include <QThread>
#include <QImage>
#include <QTimer>
#include <QDebug>
#include <QLoggingCategory>
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
#include <cerrno>
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
#include "fieldprobe.h"// tap-probe result protocol + JS string escaping (tests/fieldprobe_test.cpp)
#include <ctime>
using rmweb::Gesture;
using rmweb::classifyGesture;

Q_LOGGING_CATEGORY(lcEngine, "rmweb.engine", QtWarningMsg)  // per-frame/tap traces go to qCDebug(lcEngine), off by default; enable with QT_LOGGING_RULES=rmweb.engine.debug=true

// Finger digitizer raw range (Elan, verified on device) -> 1620x2160 panel; swipe thresholds in panel px.
static const int kPanelW = 1620, kPanelH = 2160, kTouchRawW = 2064, kTouchRawH = 2832;
static const double kPageStepPx = 2000.0;   // ~one screen of scroll per page turn (with a little overlap)
// (swipe/tap thresholds live in gesture.h GestureParams — single source of truth)

// Milliseconds elapsed since a monotonic timestamp (for the "[t] ... @Xms" instrumentation).
static inline double msSince(gint64 us) { return (g_get_monotonic_time() - us) / 1000.0; }

// Touch is ignored until this monotonic time (µs). The e-ink refresh induces capacitive noise on the
// digitizer -> phantom taps; we blank touch during a present + a tail. Set by the present path, read by TouchReader.
static std::atomic<gint64> g_touchGuardUntilUs{0};
static gint64 touchGuardTailUs() {
    static const gint64 v = (getenv("RMWEB_TOUCH_GUARD_MS") ? atoi(getenv("RMWEB_TOUCH_GUARD_MS")) : 450) * 1000LL;  // hardened default (Phase 2)
    return v;
}
static void bumpTouchGuard() { g_touchGuardUntilUs.store(g_get_monotonic_time() + touchGuardTailUs(), std::memory_order_seq_cst); }
static bool touchGuarded()  { return g_get_monotonic_time() < g_touchGuardUntilUs.load(std::memory_order_seq_cst); }
// True while the on-screen URL keyboard is open — TouchReader must NOT drop taps (refresh guard /
// 250 ms debounce would eat fast typing). Set only from the GUI thread; read from the touch thread.
static std::atomic<bool> g_urlEditing{false};

// Print a native backtrace on a fatal signal (straight to fd 2 -> the persistent device log), then
// re-raise so the watchdog still sees the crash. Our binary is unstripped, so addr2line on
// build/rmweb-wpeqt resolves the rmweb frames.
// ASYNC-SIGNAL-SAFE ONLY in here: backtrace() is primed once in main() before the handler is
// installed (its first call mallocs -> a crash from inside malloc would deadlock on the heap lock);
// output is write(2) + backtrace_symbols_fd (no stdio locks); snprintf into a stack buffer is OK
// (no allocation). getpid/gettid are bare syscalls. Same model as engine/prof_preload.c.
extern "C" void crashHandler(int sig) {
    void *bt[64];
    const int n = backtrace(bt, 64);
    char buf[160];
    int len = snprintf(buf, sizeof buf, "\n[CRASH] signal %d (Phase2-hardened) — backtrace (%d frames):\n", sig, n);
    if (len > 0) { const ssize_t w = write(STDERR_FILENO, buf, (size_t)len < sizeof buf ? (size_t)len : sizeof buf - 1); (void)w; }
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    len = snprintf(buf, sizeof buf, "[CRASH] PID=%d TID=%d\n", getpid(), gettid());
    if (len > 0) { const ssize_t w = write(STDERR_FILENO, buf, (size_t)len < sizeof buf ? (size_t)len : sizeof buf - 1); (void)w; }
    signal(sig, SIG_DFL);
    raise(sig);
}

// SIGTERM (the dev runner's timed kill; `systemctl stop rmweb`): exit IMMEDIATELY via _Exit.
// The orderly Qt/WebKit teardown path intermittently SIGABRTs/SEGVs on this stack, and any
// fatal signal here costs a DEVICE REBOOT via the watchdog — the ⏻ button and save mode already
// use the same _Exit escape. Async-signal-safe: _Exit only (no flush — stderr is line-buffered,
// so at most one partial line is lost; profile writes flush on normal loop exit / debounce).
extern "C" void termHandler(int) { std::_Exit(0); }

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
// Dark variant of the reader sheet (start page → Settings → "Reader theme"; persisted readerDark).
// Same layout metrics as kReaderCss — only the palette flips. Applies on the NEXT Reader activation.
static const char *kReaderCssDark =
    "html{background:#121212;-webkit-text-size-adjust:none}body{margin:0;background:#121212}"
    "#rmweb-reader{max-width:46em;margin:0 auto;padding:1.1em 1.1em 4em;"
        "font-family:Georgia,'Times New Roman',serif;font-size:__FS__px;line-height:1.6;color:#d8d8d8;"
        "background:#121212;word-wrap:break-word;overflow-wrap:break-word}"
    "#rmweb-reader .rmweb-title{font-size:1.5em;line-height:1.2;margin:0 0 .3em;font-weight:700;color:#eee}"
    "#rmweb-reader .rmweb-byline{font-size:.7em;color:#999;font-style:italic;margin:0 0 1.4em}"
    "#rmweb-reader p{margin:0 0 .9em}#rmweb-reader li{margin:.25em 0}"
    "#rmweb-reader ul,#rmweb-reader ol{margin:0 0 .9em 1.2em;padding:0}"
    "#rmweb-reader img,#rmweb-reader figure,#rmweb-reader video{max-width:100%;height:auto}"
    "#rmweb-reader figure{margin:1em 0}#rmweb-reader figcaption{font-size:.7em;color:#999;text-align:center}"
    "#rmweb-reader h1,#rmweb-reader h2,#rmweb-reader h3{line-height:1.25;margin:1.1em 0 .4em;color:#eee}"
    "#rmweb-reader h2{font-size:1.25em}#rmweb-reader h3{font-size:1.1em}"
    "#rmweb-reader a{color:#e8e8e8;text-decoration:underline}"
    "#rmweb-reader blockquote{margin:.8em 0;padding-left:.8em;border-left:4px solid #555;color:#aaa}"
    "#rmweb-reader pre{white-space:pre-wrap;word-wrap:break-word;background:#1e1e1e;padding:.6em;font-size:.8em}"
    "#rmweb-reader code{font-family:monospace;font-size:.85em}"
    "#rmweb-reader hr{border:none;border-top:1px solid #444;margin:1.2em 0}"
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
    void loadProgressChanged(double fraction);     // 0..1 estimated load progress
    void loadingChanged(bool loading);
    void readerModeChanged(bool on);               // reader view applied/cleared -> toolbar button state
    void readerableChanged(bool can);              // current page looks like an article -> enable Reader
    void renderFailed(bool failed);                // load finished but the page rendered ~blank (heavy SPA)
    void renderingChanged(bool on);                // true at LOAD_FINISHED for http/https (compositing); false at first-content or fail
    void linkMissed();                             // a content tap hit no link -> GUI falls back to chrome toggle
    void bookmarkedChanged(bool on);               // current page bookmark state changed
    void notice(const QString &text);              // transient toast in the chrome (find results, downloads)
    void fieldFocused(const QString &value, bool masked, const QString &suggest); // a text field was tapped -> open the keyboard (suggest = autofill prefill for an empty field, may be empty)
    void tlsStateChanged(int state);                 // 0 = http/none, 1 = https ok, 2 = https with cert errors
    void readProgressChanged(double frac);           // reading position 0..1 of the scrollable page; -1 = hide (page doesn't scroll)

public Q_SLOTS:
    void start() {
        g_main_context_push_thread_default(m_ctx);
        m_startUs = g_get_monotonic_time();

        // Load persistent profile (bookmarks, history, settings) before any WebKit activity.
        if (const char* p = getenv("RMWEB_PROFILE"); p && *p) m_profileDir = p; else m_profileDir = "/home/root/.rmweb";
        // glib mkdir, no shell — an apostrophe in RMWEB_PROFILE is just a path char, not injection.
        // On failure the loads below simply find nothing and later saves fail (logged in atomicWrite).
        if (g_mkdir_with_parents(m_profileDir.c_str(), 0700) != 0)
            qWarning("[profile] mkdir %s failed: %s", m_profileDir.c_str(), g_strerror(errno));
        m_bookmarks = rmweb::loadBookmarks(m_profileDir);
        m_history   = rmweb::loadHistory(m_profileDir);
        m_settings  = rmweb::loadSettings(m_profileDir);
        m_passwords = rmweb::loadPasswords(m_profileDir);
        m_scroll    = rmweb::loadScroll(m_profileDir);
        m_tabs      = rmweb::loadTabs(m_profileDir);
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
        // Tunable via RMWEB_DPR (default 2.0: an unset/invalid value parses to 0.0 and lands outside
        // [1.0,3.0] -> forced to 2.0; 1.0 = the old cramped behaviour). See zoom-readability.md.
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
        g_signal_connect(m_view, "load-failed", G_CALLBACK(&WpeEngine::onLoadFailed), this);
        g_signal_connect(m_view, "web-process-terminated", G_CALLBACK(&WpeEngine::onWebProcessTerminated), this);
        g_signal_connect(m_view, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* dec,
                                           WebKitPolicyDecisionType type, gpointer data) -> gboolean {
            // A response whose MIME type WebKit can't display (zip, epub, binary, ...) -> download it
            // to disk instead of failing the navigation (destination handled in onDownloadStarted).
            if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
                if (!webkit_response_policy_decision_is_mime_type_supported(WEBKIT_RESPONSE_POLICY_DECISION(dec))) {
                    webkit_policy_decision_download(dec);
                    return TRUE;
                }
                return FALSE;
            }
            if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
            auto* self = static_cast<WpeEngine*>(data);
            auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(dec);
            WebKitNavigationAction* act = webkit_navigation_policy_decision_get_navigation_action(nav);
            WebKitURIRequest* req = webkit_navigation_action_get_request(act);
            const char* uri = webkit_uri_request_get_uri(req);
            if (uri && std::string(uri).rfind("rmweb:", 0) == 0) {
                // rmweb: commands mutate the profile — honour them ONLY from our own start page
                // (file://...home.html). Any other page navigating here is a confused-deputy
                // attempt (location.href='rmweb:clear-history'): log it and swallow the command.
                const char *cur = self->m_view ? webkit_web_view_get_uri(self->m_view) : nullptr;
                const std::string home = "file://" + self->m_profileDir + "/home.html";
                if (cur && std::string(cur) == home) {
                    const std::string cmd = std::string(uri).substr(6);   // after "rmweb:"
                    if (cmd == "clear-history") {
                        self->m_history.clear();
                        rmweb::saveHistory(self->m_profileDir, self->m_history);
                        self->goHome();
                    } else if (cmd.rfind("close-tab:", 0) == 0) {
                        // The tab URL rides inside the command; WebKit percent-encodes non-ASCII
                        // when resolving the link, so decode before matching the store.
                        const std::string u = rmweb::urlDecode(cmd.substr(10));
                        if (rmweb::removeTab(self->m_tabs, u))
                            rmweb::saveTabs(self->m_profileDir, self->m_tabs);
                        self->goHome();
                    } else if (cmd == "toggle-dark") {
                        self->m_settings.readerDark = !self->m_settings.readerDark;
                        rmweb::saveSettings(self->m_profileDir, self->m_settings);
                        qInfo("[reader] dark theme %s", self->m_settings.readerDark ? "on" : "off");
                        self->goHome();
                    } else if (cmd == "toggle-ua") {
                        // Mobile UA = lighter server-rendered pages (a heavy news portal becomes READABLE — SSR
                        // headlines instead of a JS-app skeleton). Applied live; persists in settings.
                        self->m_settings.ua = (self->m_settings.ua == "mobile") ? "" : "mobile";
                        rmweb::saveSettings(self->m_profileDir, self->m_settings);
                        webkit_settings_set_user_agent(webkit_web_view_get_settings(self->m_view),
                            self->m_settings.ua.empty() ? nullptr : kMobileUA);   // nullptr = WPE default
                        qInfo("[ua] %s", self->m_settings.ua.empty() ? "desktop (default)" : kMobileUA);
                        self->goHome();
                    }
                } else {
                    qWarning("[nav] rmweb: command from non-start page ignored (current: %s)", cur ? cur : "(none)");
                }
                webkit_policy_decision_ignore(dec);
                return TRUE;
            }
            // Auto-refresh guard: a navigation back to the CURRENT url that WE didn't initiate
            // (meta refresh / JS location.reload / href=self) gets throttled — on this device a
            // re-render costs 40-80 s, and a heavy news portal's refresh yanked the reader view mid-article.
            // The window is anchored at LOAD_FINISHED (render time doesn't eat the pause), default
            // 15 s (RMWEB_AUTOREFRESH_MS), and reader mode blocks it outright. User reload/Go
            // (m_expectUserNav) and link/back-forward navigations always pass.
            if (uri && self->m_view && !self->m_expectUserNav) {
                const WebKitNavigationType nt = webkit_navigation_action_get_navigation_type(act);
                if (nt == WEBKIT_NAVIGATION_TYPE_OTHER || nt == WEBKIT_NAVIGATION_TYPE_RELOAD) {
                    const char *cur = webkit_web_view_get_uri(self->m_view);
                    if (cur && std::string(cur) == uri) {
                        if (self->m_readerMode) {
                            qInfo("[guard] auto-refresh blocked (reader mode)");
                            webkit_policy_decision_ignore(dec);
                            return TRUE;
                        }
                        static const gint64 minUs = []{
                            const int v = qEnvironmentVariableIntValue("RMWEB_AUTOREFRESH_MS");
                            return (v > 0 ? v : 15000) * (gint64)1000; }();
                        const gint64 dt = g_get_monotonic_time() - self->m_lastLoadFinishedUs;
                        if (self->m_lastLoadFinishedUs > 0 && dt < minUs) {
                            qInfo("[guard] auto-refresh throttled (%.1fs < %.0fs since load finished)",
                                  dt / 1e6, minUs / 1e6);
                            webkit_policy_decision_ignore(dec);
                            return TRUE;
                        }
                    }
                }
            }
            self->m_expectUserNav = false;   // consumed by the first navigation decision it reaches
            return FALSE;
        }), this);

        // User-Agent: env overrides; else persisted setting; else WPE default.
        // RMWEB_UA=mobile opts into lighter mobile layout for heavy JS-app sites; any other non-empty value =
        // that exact string; "off" = use WPE default and clear any persisted UA (saved to disk below, so
        // the clear survives the next launch).
        {
            std::string ua = m_settings.ua;
            const char *uaEnv = getenv("RMWEB_UA");
            if (uaEnv && *uaEnv && std::string(uaEnv) != "off") ua = uaEnv;
            else if (uaEnv && std::string(uaEnv) == "off") ua = "";
            if (!ua.empty()) {
                const char* real = (ua == "mobile") ? kMobileUA : ua.c_str();
                webkit_settings_set_user_agent(webkit_web_view_get_settings(m_view), real);
                qInfo("[ua] %s", real);
            }
            m_settings.ua = ua;
            // Persist the "off" clear immediately: a direct write is fine here (startup, before the loop
            // runs) — the debounced queueSave() exists for the runtime hot paths.
            if (uaEnv && std::string(uaEnv) == "off") rmweb::saveSettings(m_profileDir, m_settings);
        }
        // DIAG (RMWEB_NOJS=1): disable JavaScript entirely — splits "slow site" into JS-engine vs
        // CSS/layout cost (our own scroll/probe/reader JS goes down too; diagnostic only).
        if (qEnvironmentVariableIntValue("RMWEB_NOJS") == 1) {
            webkit_settings_set_enable_javascript(webkit_web_view_get_settings(m_view), FALSE);
            qInfo("[diag] JavaScript DISABLED (RMWEB_NOJS=1)");
        }
        // In-page find feedback ("found N" / "no matches" toast) and downloads live on the view's
        // session/context; all signals fire on this worker thread, like every other handler above.
        {
            WebKitFindController *fc = webkit_web_view_get_find_controller(m_view);
            g_signal_connect(fc, "found-text", G_CALLBACK(+[](WebKitFindController*, guint n, gpointer data){
                auto *self = static_cast<WpeEngine*>(data);
                qInfo("[find] matches=%u", n);
                // matchCount is G_MAXUINT when WebKit didn't count (we don't ask for COUNT_MATCHES
                // — counting a long page is wasted CPU) — then show a bare confirmation instead.
                Q_EMIT self->notice(n == G_MAXUINT ? QStringLiteral("Match found")
                                                   : QStringLiteral("%1 matches").arg(n));
            }), this);
            g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(+[](WebKitFindController*, gpointer data){
                auto *self = static_cast<WpeEngine*>(data);
                qInfo("[find] no matches");
                Q_EMIT self->notice(QStringLiteral("No matches"));
            }), this);
            g_signal_connect(webkit_web_view_get_network_session(m_view), "download-started",
                             G_CALLBACK(&WpeEngine::onDownloadStarted), this);
        }
        // Apply persisted zoom (must be done after the view is fully set up).
        webkit_web_view_set_zoom_level(m_view, m_zoom);

        // Persistent cookies (default on): logins/sessions survive relaunch. Stored in the profile
        // dir as sqlite; RMWEB_COOKIES=0 opts out (session-only). Must be set before the first load.
        // Policy = no third-party cookies: the content blocker already drops third-party scripts,
        // so this just starves the trackers that remain. (2022 API: the cookie manager hangs off
        // WebKitNetworkSession, not WebKitWebContext.)
        if (qgetenv("RMWEB_COOKIES") != "0") {
            WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(
                webkit_web_view_get_network_session(m_view));
            const std::string cj = m_profileDir + "/cookies.sqlite";
            webkit_cookie_manager_set_persistent_storage(cm, cj.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
            webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
            qInfo("[cookies] persistent: %s", cj.c_str());
        }

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

        // Loop exited: flush a still-pending debounced profile write so a shutdown doesn't lose
        // the last history/settings change (runs here, on the worker thread, like every save).
        flushPendingWrites();

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
        marshalToCtx([this, s] { m_expectUserNav = true; if (m_view) webkit_web_view_load_uri(m_view, s.c_str()); }); }
    void goHome() {
        // Marshalled: reads/writes m_bookmarks and m_history, which the worker-thread LOAD_FINISHED also touches.
        marshalToCtx([this] {
            const std::string html = rmweb::buildStartPage(m_bookmarks, firstN(m_history, 15),
                                                           m_tabs, m_settings.readerDark, m_settings.ua == "mobile");
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
    void reload()    { marshalToCtx([this] { m_expectUserNav = true; if (m_view) webkit_web_view_reload(m_view); }); }
    void stopLoading() { marshalToCtx([this] { if (m_view) webkit_web_view_stop_loading(m_view); }); }
    // In-page find (address bar: "/term" + Go). A fresh term starts a new search (first match is
    // scrolled to + highlighted); repeating the SAME term steps to the next match (wraps around).
    void findText(const QString &q) {
        marshalToCtx([this, q] {
            if (!m_view || q.isEmpty()) return;
            WebKitFindController *fc = webkit_web_view_get_find_controller(m_view);
            const std::string s = q.toStdString();
            if (s == m_lastFind) {
                webkit_find_controller_search_next(fc);
            } else {
                m_lastFind = s;
                webkit_find_controller_search(fc, s.c_str(),
                    WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, 128);
            }
        });
    }
    // Follow what a panel (x,y) tap hit (panel px -> CSS px: divide by dpr * zoom). ONE probe handles
    // everything tappable, in priority order: text field (focus + open the keyboard) > select (cycle
    // options) > checkbox/radio (toggle) > link/button (follow). A <label> resolves to its control.
    // Answers the fieldprobe.h line protocol ("none"/"link"/"tick\n.."/"field\n.."); wider probe
    // offsets so small controls and fat-finger taps still land.
    void tapLink(int x, int y) { probeWith(x, y, false); }
    // Long-press peek: same hit-test, but links are NOT followed — the probe answers "peek\n<href>"
    // so the GUI can toast the target instead. Fields/selects are inert in this mode.
    void peekLink(int x, int y) { probeWith(x, y, true); }
    void probeWith(int x, int y, bool peek) {
        marshalToCtx([this, x, y, peek] {
            if (!m_view) return;
            m_lastProbePeek = peek;
            const double scale = std::max(0.5, m_dpr * m_zoom);
            const int cx = int(x / scale), cy = int(y / scale);
            gchar *js = g_strdup_printf(
                "(function(x,y){"
                "var PEEK=%d;"
                "var p=[[0,0],[0,-16],[0,16],[-16,0],[16,0],[-32,0],[32,0],[0,-32],[0,32],"
                "[-16,-16],[16,-16],[-16,16],[16,16],[-40,0],[40,0],[0,-40],[0,40]];"
                "function isTxt(f){"
                "if(f.isContentEditable)return true;"
                "if(f.tagName==='TEXTAREA')return true;"
                "if(f.tagName!=='INPUT')return false;"
                "var t=(f.type||'').toLowerCase();"
                "return t===''||t==='text'||t==='search'||t==='email'||t==='url'||t==='password'"
                "||t==='tel'||t==='number';}"
                "function field(f){"
                "if(f.disabled||f.readOnly)return null;"
                "try{f.focus();}catch(e){}"
                "window.__rmwebField=f;"
                "var v=f.isContentEditable?(f.textContent||''):(f.value||'');"
                "var m=(f.type||'').toLowerCase()==='password'?'1':'0';"
                // Identity clues for autofill classification — one line, whitespace-folded.
                // f.type included: type="email" is the most common email-field marker.
                "var h=((f.autocomplete||'')+' '+(f.name||'')+' '+(f.id||'')+' '+(f.type||'')+' '+"
                "(f.getAttribute('placeholder')||'')).replace(/\\s+/g,' ').slice(0,80);"
                "return 'field\\n'+m+'\\n'+h+'\\n'+v;}"
                "function probe(e){"
                "if(!e||!e.closest)return null;"
                "var t=e,lb=t.closest('label');"
                "if(lb&&lb.control)t=lb.control;"
                "var a=t.closest('a[href],area[href],[role=link],button');"
                "if(a&&PEEK)return a.href?('peek\\n'+a.href):null;"
                "if(!PEEK){"
                "var f=t.closest('input,textarea,[contenteditable]');"
                "if(f&&isTxt(f))return field(f);"
                "var s=t.closest('select');"
                "if(s&&!s.disabled&&s.options.length){s.selectedIndex=(s.selectedIndex+1)%%s.options.length;"
                "s.dispatchEvent(new Event('change',{bubbles:true}));"
                "return 'tick\\n'+(s.options[s.selectedIndex]?s.options[s.selectedIndex].text:'');}"
                "var c=t.closest('input[type=checkbox],input[type=radio]');"
                "if(c&&!c.disabled){c.click();return 'tick\\n'+(c.checked?'on':'off');}"
                "if(a){if(a.href){location.href=a.href;return 'link';}"
                "try{a.click();return 'link';}catch(ex){}}"
                "var b=t.closest('[onclick],input[type=submit],input[type=button],input[type=reset],input[type=image]');"
                "if(b){try{b.click();return 'link';}catch(ex){}}"
                "}return null;}"
                "for(var i=0;i<p.length;i++){"
                "var r=probe(document.elementFromPoint(x+p[i][0],y+p[i][1]));"
                "if(r)return r;}"
                "return 'none';})(%d,%d)",
                peek ? 1 : 0, cx, cy);
            qCDebug(lcEngine, "[link] probe panel=(%d,%d) css=(%d,%d) dpr=%.2f zoom=%.2f peek=%d", x, y, cx, cy, m_dpr, m_zoom, peek ? 1 : 0);
            webkit_web_view_evaluate_javascript(m_view, js, -1, nullptr, nullptr, m_cancel, &WpeEngine::onTapLink, this);
            g_free(js);
        });
    }
    // Address-bar search (typed words that aren't a URL): matching bookmarks + history rows plus a
    // web-search link, as a generated results page (load_html — no history entry semantics needed).
    void searchAndShow(const QString &q) {
        marshalToCtx([this, q] {
            if (!m_view) return;
            const std::string term = q.toStdString();
            const std::string html = rmweb::buildSearchResults(term,
                rmweb::searchStore(m_bookmarks, term), rmweb::searchStore(m_history, term));
            webkit_web_view_load_html(m_view, html.c_str(), "about:blank");
        });
    }
    // Commit the keyboard text into the last focused field (window.__rmwebField, stashed by the tap
    // probe). The NATIVE value setter + input/change events make framework-controlled components
    // (React & co.) register a real edit. Empty text clears the field.
    void setFieldText(const QString &text) {
        marshalToCtx([this, text] {
            if (!m_view) return;
            m_lastCommitText = text.toStdString();   // kept until onFieldSet (password capture)
            const std::string t = rmweb::jsStringEscape(text.toStdString());
            gchar *js = g_strdup_printf(
                "(function(txt){var f=window.__rmwebField;"
                "if(!f||!f.isConnected)return 'gone';"
                "try{f.focus();}catch(e){}"
                "var ok=false,ty=(f.type||'').toLowerCase();"
                // PASSWORD: this port never re-renders the control after a late programmatic set
                // (value updates, pixels don't — neither setter nor execCommand nor blur/display
                // nudges help). Swap in a CLONE whose value ATTRIBUTE is the new text: a fresh
                // renderer paints the bullets from the attribute. Direct on-element listeners are
                // lost (delegated/framework listeners at root survive); re-stash the clone.
                "if(ty==='password'){f.setAttribute('value',txt);"
                "var c=f.cloneNode(true);f.replaceWith(c);window.__rmwebField=c;f=c;"
                "try{f.focus();}catch(e){}"
                "f.dispatchEvent(new Event('input',{bubbles:true}));ok=true;"
                // ...and even the clone doesn't self-damage — force a FULL-viewport repaint with a
                // transient ~invisible veil (alpha 0.01 still paints; removed after one composite).
                "var o=document.createElement('div');"
                "o.style.cssText='position:fixed;top:0;left:0;width:100vw;height:100vh;background:rgba(0,0,0,0.01);z-index:2147483647;pointer-events:none';"
                "document.body.appendChild(o);setTimeout(function(){o.remove();},800);}"
                "else if(f.isContentEditable){f.textContent=txt;f.dispatchEvent(new Event('input',{bubbles:true}));ok=true;}"
                "else{try{f.select();ok=document.execCommand('insertText',false,txt);}catch(e){}"
                // Fallback: NATIVE value setter (React & co. register a real edit).
                "if(!ok){var p=f.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
                "Object.getOwnPropertyDescriptor(p,'value').set.call(f,txt);"
                "f.dispatchEvent(new Event('input',{bubbles:true}));}}"
                "f.dispatchEvent(new Event('change',{bubbles:true}));"
                "try{f.blur();}catch(e){}"   // Go = done editing; also hides the caret
                // A bare value set may commit no buffer on this backend (same as scrollBy) — bump the
                // hidden marker node to dirty the page and force exactly one composite.
                "var m=document.getElementById('__r');if(!m){m=document.createElement('span');m.id='__r';"
                "m.style.cssText='position:fixed;left:-9999px;top:0';document.body.appendChild(m);}"
                "m.textContent=((+m.textContent||0)+1);"
                // Password commit: also answer the sibling login (first text-ish input in the form)
                // so the password store can remember host -> (login, password). "pw\n" + login.
                "if(ty==='password'){var u='';try{var root=f.form||document;var ins=root.querySelectorAll('input');"
                "for(var i=0;i<ins.length;i++){var t2=(ins[i].type||'').toLowerCase();"
                "if(t2===''||t2==='text'||t2==='email'||t2==='tel'){"
                "u=(ins[i].value||'').replace(/\\s+/g,' ').slice(0,80);if(u)break;}}}catch(e){}"
                "return 'pw\\n'+u;}"
                "return ok?'ok':'fallback';})(\"%s\")", t.c_str());
            webkit_web_view_evaluate_javascript(m_view, js, -1, nullptr, nullptr, m_cancel, &WpeEngine::onFieldSet, this);
            g_free(js);
        });
    }
    static void onFieldSet(GObject *obj, GAsyncResult *res, gpointer data) {
        bool cancelled; JSCValue *v = finishJsEval(obj, res, &cancelled);
        if (cancelled) return;
        auto *self = static_cast<WpeEngine*>(data);   // nullptr from logFieldState (diagnostic eval)
        std::string out;
        if (v && jsc_value_is_string(v)) { char *c = jsc_value_to_string(v); out = c ? c : ""; g_free(c); }
        qInfo("[form] setFieldText -> %s", !out.empty() ? out.c_str() : (v ? "(non-string)" : "(eval error)"));
        if (v) g_object_unref(v);
        if (!self) return;
        // A password commit answered "pw\n<sibling-login>" — remember host -> (login, obfuscated
        // password). m_lastCommitText holds the plaintext of this commit (cleared right after).
        if (out.rfind("pw\n", 0) == 0 && !self->m_lastCommitText.empty()) {
            const std::string host = rmweb::hostFromUrl(self->m_curUrl);
            if (!host.empty()) {   // no host (file://, about:) -> upsert no-ops; don't claim a save
                rmweb::upsertPassword(self->m_passwords, host, out.substr(3), self->m_lastCommitText);
                self->queueSave(&self->m_pwSaveSrc, 4);
                qInfo("[form] password saved for %s", host.c_str());
            }
        }
        self->m_lastCommitText.clear();
    }
    // Learn-as-you-type autofill: a committed (Go) non-empty field value whose hint classified as
    // email/user/name is remembered in the settings (debounced write) and offered as a keyboard
    // prefill the next time an EMPTY field of the same kind is tapped. Passwords never classify,
    // so they are never learned.
    void learnFieldText(const QString &text) {
        marshalToCtx([this, text] {
            const rmweb::FieldKind kind = m_pendingFieldKind;
            m_pendingFieldKind = rmweb::FieldKind::None;
            const std::string t = text.toStdString();
            if (t.empty() || kind == rmweb::FieldKind::None) return;
            if (kind == rmweb::FieldKind::Email) m_settings.autofillEmail = t;
            else if (kind == rmweb::FieldKind::User) m_settings.autofillUser = t;
            else if (kind == rmweb::FieldKind::Name) m_settings.autofillName = t;
            queueSave(&m_settingsSaveSrc, 1);   // debounced — same path as zoom/font settings
            qInfo("[form] autofill learned kind=%d len=%zu", static_cast<int>(kind), t.size());
        });
    }
    // DIAG: log the stashed field's tag/type/value LENGTH (password-safe — never the value itself),
    // and force a FULL-document repaint first (decides "renderer out of sync" vs "stale damage").
    void logFieldState() {
        marshalToCtx([this] {
            if (!m_view) return;
            const char *js = "(function(){var f=window.__rmwebField;if(!f)return 'none';"
                             "return 'tag='+f.tagName+' type='+String(f.type)+' len='+String((f.value||'').length)"
                             "+' conn='+f.isConnected+' attr='+String(f.getAttribute('value'));})()";
            webkit_web_view_evaluate_javascript(m_view, js, -1, nullptr, nullptr, m_cancel, &WpeEngine::onFieldSet, nullptr);
        });
    }
    void pageNext()  { pageBy(kPageStepPx); }   // façade page-turn (wraps the scroll+repaint in pageBy)
    void pagePrev()  { pageBy(-kPageStepPx); }
    // Text size -/+ (the A-/A+ chrome buttons): page zoom in normal mode, reader font in reader mode.
    void zoomBy(int dir) {
        marshalToCtx([this, dir] {
            if (!m_view) return;
            if (m_readerMode) {
                m_readerFont = std::clamp(m_readerFont + (dir > 0 ? 4 : -4), 14, 64);   // reader column font px
                gchar *js = g_strdup_printf("var r=document.getElementById('rmweb-reader');if(r)r.style.fontSize='%dpx';", m_readerFont);
                webkit_web_view_evaluate_javascript(m_view, js, -1, nullptr, nullptr, m_cancel, nullptr, nullptr);
                g_free(js);
            } else {
                m_zoom = std::clamp(m_zoom * (dir > 0 ? 1.2 : 1.0 / 1.2), 0.5, 3.0);     // page zoom level
                webkit_web_view_set_zoom_level(m_view, m_zoom);
            }
            qCDebug(lcEngine, "[zoom] reader=%d zoom=%.2f font=%d", m_readerMode, m_zoom, m_readerFont);
            m_settings.zoom = m_zoom; m_settings.readerFont = m_readerFont;
            queueSave(&m_settingsSaveSrc, 1);   // debounced — one flash write after the tap burst ends
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
            self->m_userScrolled = true;   // suppress a pending scroll-restore for this load
            // Scroll one page and force exactly ONE repaint: a bare scrollBy moves scrollY but commits no
            // buffer, so we bump a hidden marker node to dirty the page → one composite. With llvmpipe that
            // lands in ~90 ms, so one frame per turn is enough. (The earlier requestAnimationFrame burst was a
            // workaround for softpipe's ~6 s composite; it also flooded the e-ink panel with ~20 presents/turn.)
            gchar *js = g_strdup_printf(
                "(function(dy){"
                "var step=dy>0?Math.round(innerHeight*0.92):-Math.round(innerHeight*0.92);"
                "var se=document.scrollingElement||document.documentElement||document.body;"
                "var y0=se.scrollTop;se.scrollTop+=step;var sc=null,used='doc';"
                "if(se.scrollTop===y0){sc=window.__rmwebSc;"
                "if(!sc||!sc.isConnected||sc.scrollHeight-sc.clientHeight<=40){sc=null;var bh=0,a=document.querySelectorAll('div,main,article,section,ul,ol');"
                "for(var i=0;i<a.length;i++){var n=a[i],o=getComputedStyle(n).overflowY;"
                "if((o==='auto'||o==='scroll')&&n.scrollHeight-n.clientHeight>40&&n.scrollHeight>bh){bh=n.scrollHeight;sc=n;}}"
                "window.__rmwebSc=sc;}"
                "if(sc){sc.scrollTop+=step;used='el';}}"
                "var m=document.getElementById('__r');if(!m){m=document.createElement('span');m.id='__r';"
                "m.style.cssText='position:fixed;left:-9999px;top:0';document.body.appendChild(m);}"
                "m.textContent=((+m.textContent||0)+1);"
                "return 'sy='+(sc?sc.scrollTop:se.scrollTop)+' ih='+innerHeight+' sh='+se.scrollHeight"
                "+' sm='+Math.round(sc?(sc.scrollHeight-sc.clientHeight):Math.max(0,se.scrollHeight-innerHeight))+' used='+used;"
                "})(%d)",
                static_cast<int>(m->dy));
            webkit_web_view_evaluate_javascript(self->m_view, js, -1, nullptr, nullptr, self->m_cancel,
                                                &WpeEngine::onJsDone, self);
            g_free(js);
            qCDebug(lcEngine, "[t] pageBy(%d) @%.0fms", static_cast<int>(m->dy), msSince(self->m_startUs));
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
        std::string dbg = "?";
        if (v) { if (jsc_value_is_string(v)) { char *c = jsc_value_to_string(v); dbg = c ? c : ""; g_free(c); } g_object_unref(v); }
        if (!self) return;
        qCDebug(lcEngine, "[t] page JS done %s @%.0fms", dbg.c_str(), msSince(self->m_startUs));
        // The scroll JS answers "sy=<n> ..." — remember it as the reading position for the current
        // URL (debounced write), so a later visit can resume where reading stopped.
        const size_t p = dbg.find("sy=");
        if (p != std::string::npos) {
            const int pos = atoi(dbg.c_str() + p + 3);
            // Both the scroll store and the progress bar stay gated on m_curUrl: a stale eval that
            // completes AFTER a navigation committed (m_curUrl cleared at LOAD_COMMITTED) must not
            // stamp the old page's position/progress onto the new one.
            if (!self->m_curUrl.empty()) {
                self->m_curScroll = pos;
                rmweb::upsertScroll(self->m_scroll, self->m_curUrl, pos);
                self->queueSave(&self->m_scrollSaveSrc, 2);
                // Reading-progress bar: sm = max scroll of the USED scroller (pageBy/restore answer
                // it); <=40 px of scroll range = the page doesn't really scroll -> hide the bar (-1).
                const size_t sm = dbg.find(" sm=");
                if (sm != std::string::npos) {
                    const int maxY = atoi(dbg.c_str() + sm + 4);
                    double frac = -1.0;
                    if (maxY > 40) frac = std::min(1.0, std::max(0.0, double(pos) / maxY));
                    Q_EMIT self->readProgressChanged(frac);
                }
            }
        }
    }
    static void onTapLink(GObject *obj, GAsyncResult *res, gpointer data) {
        bool cancelled; JSCValue *v = finishJsEval(obj, res, &cancelled);
        if (cancelled) return;
        auto *self = static_cast<WpeEngine*>(data);
        std::string out = "none";
        if (v) { if (jsc_value_is_string(v)) { char *c = jsc_value_to_string(v); out = c ? c : ""; g_free(c); } g_object_unref(v); }
        if (!self) return;
        const rmweb::TapProbe pr = rmweb::parseTapProbe(out);
        qCDebug(lcEngine, "[link] probe hit=%d", static_cast<int>(pr.hit));
        switch (pr.hit) {
            case rmweb::TapHit::None:                       // peek mode: long-press on empty space = no-op
                if (!self->m_lastProbePeek) Q_EMIT self->linkMissed();
                break;
            case rmweb::TapHit::Link:  break;   // the navigation proceeds on its own
            case rmweb::TapHit::Peek: {          // long-press on a link -> toast its target (truncated)
                if (pr.value.empty()) break;
                std::string u = pr.value;
                if (u.size() > 72) u = u.substr(0, 72) + "\xE2\x80\xA6";   // … (3-byte UTF-8, safe append)
                Q_EMIT self->notice(QString::fromStdString(u));
                break;
            }
            case rmweb::TapHit::Tick:            // checkbox/select changed -> toast the new state
                if (!pr.value.empty()) Q_EMIT self->notice(QString::fromStdString(pr.value));
                break;
            case rmweb::TapHit::Field: {         // text field focused -> open the keyboard on its value
                const rmweb::FieldKind kind = rmweb::classifyFieldHint(pr.hint, pr.masked);
                self->m_pendingFieldKind = kind;
                // Autofill prefill: only for an EMPTY field (never overwrite existing content).
                // Password fields prefill from the per-host password store; a username field falls
                // back to the stored login when no learned username exists.
                QString suggest;
                if (pr.value.empty()) {
                    const std::string host = rmweb::hostFromUrl(self->m_curUrl);
                    const rmweb::PasswordEntry *pw = rmweb::findPassword(self->m_passwords, host);
                    if (pr.masked) {
                        if (pw) suggest = QString::fromStdString(rmweb::deobfuscatePassword(pw->passObf));
                    } else if (kind == rmweb::FieldKind::Email) suggest = QString::fromStdString(self->m_settings.autofillEmail);
                    else if (kind == rmweb::FieldKind::User) {
                        suggest = QString::fromStdString(self->m_settings.autofillUser);
                        if (suggest.isEmpty() && pw) suggest = QString::fromStdString(pw->user);
                    } else if (kind == rmweb::FieldKind::Name) suggest = QString::fromStdString(self->m_settings.autofillName);
                }
                Q_EMIT self->fieldFocused(QString::fromStdString(pr.value), pr.masked, suggest);
                break;
            }
        }
    }
    // Build the apply script: the vendored Readability lib + our glue, with the reader CSS (font size from
    // RMWEB_READER_FONT, default 30) inlined. Injected in one shot so all symbols share the same scope.
    std::string buildReaderApplyJs() {
        std::string css = m_settings.readerDark ? kReaderCssDark : kReaderCss;   // start-page theme setting
        replaceAll(css, "__FS__", std::to_string(m_readerFont));   // A-/A+ adjustable
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
    // m_loadGen invalidates the check if the user navigated away during the grace period. The timer RE-ARMS
    // itself while the load is still in progress: on a slow site (15 s+ of network) a fixed 13 s-from-start
    // deadline would otherwise judge the page "blank" while bytes are still arriving (a heavy news portal false positive).
    static constexpr int kRenderTimeoutMs = 13000;  // after load-start (re-armed while loading): time to paint
    static constexpr int kBlankSamples = 8;         // fewer than this many non-white frame samples = ~blank
    struct RenderCheckMsg { WpeEngine *self; guint gen; };
    void scheduleRenderCheck() {
        auto *m = new RenderCheckMsg{ this, m_loadGen };
        GSource *s = g_timeout_source_new(kRenderTimeoutMs);
        g_source_set_callback(s, [](gpointer d) -> gboolean {
            auto *msg = static_cast<RenderCheckMsg*>(d);
            WpeEngine *self = msg->self;
            if (self->m_loadGen != msg->gen) return G_SOURCE_REMOVE;
            // Still loading (slow network): don't judge yet — re-check after another interval.
            if (self->m_loadInProgress) return G_SOURCE_CONTINUE;
            // Same load, not in reader: if the latest web frame is essentially WHITE, the page rendered no
            // visible content (a heavy SPA whose JS the CPU can't run). Flag it so the shell shows a notice.
            // Pixel-based (not DOM): an SPA shell has DOM nodes but paints nothing, so DOM heuristics lie.
            // A later non-white frame auto-clears the flag in onBuffer (a slow-but-rendering site recovers).
            if (!self->m_readerMode) {
                const bool blank = self->m_lastNonWhite < kBlankSamples;
                qInfo("[render] nonWhite=%d blank=%d", self->m_lastNonWhite, blank);
                self->m_renderFailedState = blank;
                Q_EMIT self->renderFailed(blank);
                // renderFailed(true) = the page stayed blank -> clear the "Rendering…" badge too.
                if (blank && self->m_renderingState) { self->m_renderingState = false; Q_EMIT self->renderingChanged(false); }
            }
            return G_SOURCE_REMOVE; }, m, [](gpointer d) { delete static_cast<RenderCheckMsg*>(d); });
        g_source_attach(s, m_ctx);
        g_source_unref(s);
    }

    // Per-URL scroll restore: after LOAD_FINISHED give the layout a moment to settle, then jump back
    // to the position remembered for this URL (same marker-nudge repaint trick as the page turn).
    // Skipped if the user already page-turned on this load, or a new navigation started meanwhile.
    static constexpr guint kScrollRestoreMs = 800;
    struct ScrollRestoreMsg { WpeEngine *self; guint gen; int pos; };
    void scheduleScrollRestore(int pos) {
        auto *m = new ScrollRestoreMsg{ this, m_loadGen, pos };
        GSource *s = g_timeout_source_new(kScrollRestoreMs);
        g_source_set_callback(s, [](gpointer d) -> gboolean {
            auto *msg = static_cast<ScrollRestoreMsg*>(d);
            WpeEngine *self = msg->self;
            if (self->m_view && self->m_loadGen == msg->gen && !self->m_userScrolled
                    && !self->m_readerMode && msg->pos > 0) {
                qInfo("[scroll] restore y=%d", msg->pos);
                // Same scroller logic as the page turn: many sites (Wikipedia incl.) scroll an
                // INNER element, not the document — setting document.scrollTop there is a no-op.
                gchar *js = g_strdup_printf(
                    "(function(y){"
                    "var se=document.scrollingElement||document.documentElement||document.body;"
                    "var y0=se.scrollTop;se.scrollTop=y;var sc=null;"
                    "if(se.scrollTop===y0&&y>0){sc=window.__rmwebSc;"
                    "if(!sc||!sc.isConnected||sc.scrollHeight-sc.clientHeight<=40){sc=null;var bh=0,a=document.querySelectorAll('div,main,article,section,ul,ol');"
                    "for(var i=0;i<a.length;i++){var n=a[i],o=getComputedStyle(n).overflowY;"
                    "if((o==='auto'||o==='scroll')&&n.scrollHeight-n.clientHeight>40&&n.scrollHeight>bh){bh=n.scrollHeight;sc=n;}}"
                    "window.__rmwebSc=sc;}"
                    "if(sc)sc.scrollTop=y;}"
                    "var m=document.getElementById('__r');if(!m){m=document.createElement('span');m.id='__r';"
                    "m.style.cssText='position:fixed;left:-9999px;top:0';document.body.appendChild(m);}"
                    "m.textContent=((+m.textContent||0)+1);"
                    "return 'sy='+(sc?sc.scrollTop:se.scrollTop)"
                    "+' sm='+Math.round(sc?(sc.scrollHeight-sc.clientHeight):Math.max(0,se.scrollHeight-innerHeight));"
                    "})(%d)", msg->pos);
                webkit_web_view_evaluate_javascript(self->m_view, js, -1, nullptr, nullptr, self->m_cancel,
                                                    &WpeEngine::onJsDone, self);
                g_free(js);
            }
            return G_SOURCE_REMOVE; }, m, [](gpointer d) { delete static_cast<ScrollRestoreMsg*>(d); });
        g_source_attach(s, m_ctx);
        g_source_unref(s);
    }

    // Debounced profile writes (blocking flash I/O must not stall the WebKit worker): LOAD_FINISHED
    // (every page) and each A-/A+ tap used to write synchronously. Coalesce onto ONE pending
    // timeout per store on this context — a repeat inside the window re-arms it. Runs only on the
    // worker thread (like every m_history/m_settings access); flushPendingWrites() covers shutdown.
    static constexpr guint kSaveDebounceMs = 1500;
    struct SaveMsg { WpeEngine *self; int what; };   // what: 0 = history, 1 = settings, 2 = scroll, 3 = tabs, 4 = passwords
    static constexpr int kNumStores = 5;
    // what -> the debounce-slot member. ONE mapping used by onSaveTimer and flushPendingWrites, so
    // adding a store touches only this and runSave.
    GSource **saveSlot(int what) {
        switch (what) {
            case 0: return &m_historySaveSrc;
            case 1: return &m_settingsSaveSrc;
            case 2: return &m_scrollSaveSrc;
            case 3: return &m_tabsSaveSrc;
            default: return &m_pwSaveSrc;
        }
    }
    void queueSave(GSource **slot, int what) {
        if (*slot) { g_source_destroy(*slot); g_source_unref(*slot); *slot = nullptr; }   // re-arm the window
        auto *m = new SaveMsg{ this, what };
        GSource *s = g_timeout_source_new(kSaveDebounceMs);
        g_source_set_callback(s, &WpeEngine::onSaveTimer, m,
                              [](gpointer d) { delete static_cast<SaveMsg*>(d); });
        g_source_attach(s, m_ctx);
        *slot = s;   // keep our ref so a repeat can destroy it; onSaveTimer drops it when it fires
    }
    static gboolean onSaveTimer(gpointer d) {
        auto *msg = static_cast<SaveMsg*>(d);
        WpeEngine *self = msg->self;
        const int what = msg->what;
        GSource **slot = self->saveSlot(what);
        g_source_unref(*slot); *slot = nullptr;   // fired: drop our ref (may free msg — locals only from here)
        self->runSave(what);
        return G_SOURCE_REMOVE;
    }
    void runSave(int what) {
        if      (what == 0) rmweb::saveHistory(m_profileDir, m_history);
        else if (what == 1) rmweb::saveSettings(m_profileDir, m_settings);
        else if (what == 2) rmweb::saveScroll(m_profileDir, m_scroll);
        else if (what == 3) rmweb::saveTabs(m_profileDir, m_tabs);
        else                rmweb::savePasswords(m_profileDir, m_passwords);
    }
    // Final flush on worker-loop exit (start()): a write still inside its debounce window would be lost.
    void flushPendingWrites() {
        for (int what = 0; what < kNumStores; ++what) {
            GSource **slot = saveSlot(what);
            if (!*slot) continue;
            g_source_destroy(*slot); g_source_unref(*slot); *slot = nullptr;
            runSave(what);
        }
    }

    void loadInitial() {
        if (m_url.isEmpty()) {
            // No URL given: show the start page (bookmarks + recent history). goHome() is already
            // marshalled to the worker context via marshalToCtx, which is safe to call from here
            // (we ARE on the worker context, so the inner marshalToCtx re-posts to the same context —
            // harmless, and keeps the identical dispatch path as a tap-router-initiated goHome()).
            const std::string html = rmweb::buildStartPage(m_bookmarks, firstN(m_history, 15),
                                                           m_tabs, m_settings.readerDark, m_settings.ua == "mobile");
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
        // Generated pages loaded with an about:blank base (address-bar search results) must not
        // clobber the address bar — the typed query stays (set when the search was kicked off).
        if (u && std::string(u) == "about:blank") return;
        Q_EMIT self->urlChanged(QString::fromUtf8(u ? u : ""));
    }

    // A download started (decide-policy said "download"). Save under /home/root/Downloads
    // (RMWEB_DOWNLOADS overrides), toast in the chrome on completion/failure. The suggested
    // filename is stripped to its basename so it can never escape the downloads dir.
    static void onDownloadStarted(WebKitNetworkSession *, WebKitDownload *dl, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        g_signal_connect(dl, "decide-destination", G_CALLBACK(+[](WebKitDownload *d, gchar *suggested, gpointer) -> gboolean {
            const char *dirEnv = getenv("RMWEB_DOWNLOADS");
            const std::string dir = (dirEnv && *dirEnv) ? dirEnv : "/home/root/Downloads";
            if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) {
                qWarning("[dl] mkdir %s failed: %s", dir.c_str(), g_strerror(errno));
                return FALSE;
            }
            std::string name = (suggested && *suggested) ? suggested : "download.bin";
            const size_t slash = name.find_last_of('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (name.empty() || name == "." || name == "..") name = "download.bin";
            // 2022 API: set_destination takes an absolute PATH (the old GTK API took a file:// URI).
            webkit_download_set_destination(d, (dir + "/" + name).c_str());
            qInfo("[dl] -> %s/%s", dir.c_str(), name.c_str());
            return TRUE;
        }), nullptr);
        g_signal_connect(dl, "finished", G_CALLBACK(+[](WebKitDownload *d, gpointer data) {
            auto *self = static_cast<WpeEngine*>(data);
            std::string shown = "Download complete";
            if (const gchar *dest = webkit_download_get_destination(d)) {
                std::string s = dest;
                const size_t p = s.find_last_of('/');
                if (p != std::string::npos) s = s.substr(p + 1);
                if (!s.empty()) shown = "Saved " + s;
            }
            qInfo("[dl] finished: %s", shown.c_str());
            Q_EMIT self->notice(QString::fromStdString(shown));
        }), self);
        g_signal_connect(dl, "failed", G_CALLBACK(+[](WebKitDownload *, GError *err, gpointer data) {
            auto *self = static_cast<WpeEngine*>(data);
            qWarning("[dl] failed: %s", err ? err->message : "?");
            Q_EMIT self->notice(QStringLiteral("Download failed"));
        }), self);
    }

    static void onLoadChanged(WebKitWebView *view, WebKitLoadEvent ev, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        if (ev == WEBKIT_LOAD_STARTED) {
            self->m_loadGen++;                          // invalidate any pending render-check from a prior load
            self->m_loadInProgress = true;              // the blank-check re-arms while this is true
            self->m_renderFailedState = false;
            self->m_lastNonWhite = 0;                   // no frames yet => blank until onBuffer proves otherwise
            self->m_userScrolled = false;               // re-arm scroll-restore suppression for the new load
            self->m_lastFind.clear();                   // find state is per page; a repeat starts fresh
            // [perf] instrumentation: record per-load origin and reset milestones/flags.
            self->m_loadStartUs = g_get_monotonic_time();
            self->m_firstContentLogged = false;
            self->m_progressMilestone = 25;
            // Cancel any active "Rendering…" state from the previous navigation.
            if (self->m_renderingState) { self->m_renderingState = false; Q_EMIT self->renderingChanged(false); }
            const char *startUri = webkit_web_view_get_uri(view);
            qInfo("[t] load started @%.0fms", msSince(self->m_startUs));
            qCDebug(lcEngine, "[perf] load-started url=%s", startUri ? startUri : "");
            Q_EMIT self->loadingChanged(true);
            Q_EMIT self->renderFailed(false);           // new load -> clear any "couldn't render" notice
            self->scheduleRenderCheck();                // LOAD_STARTED always fires -> robust blank-check trigger
        }
        if (ev == WEBKIT_LOAD_COMMITTED) {
            qCDebug(lcEngine, "[perf] load-committed @%.0fms", msSince(self->m_loadStartUs));
            // A real navigation/reload landed fresh original content -> any reader view is gone; reset its state.
            if (self->m_readerMode) { self->m_readerMode = false; Q_EMIT self->readerModeChanged(false); }
            // The old page is gone from here on: drop its identity NOW so a scroll completion landing
            // in the commit->finish window isn't recorded against the previous URL (reset at FINISHED).
            self->m_curUrl.clear(); self->m_curTitle.clear(); self->m_curScroll = 0;
            // TLS state -> the address-bar lock: get_tls_info flags https + cert errors even when
            // the page still loaded, independent of tls-errors-policy.
            GTlsCertificate *cert = nullptr; GTlsCertificateFlags errs = (GTlsCertificateFlags)0;
            const gboolean secure = webkit_web_view_get_tls_info(view, &cert, &errs);
            qInfo("[tls] secure=%d errs=0x%x", secure, (unsigned)errs);
            Q_EMIT self->tlsStateChanged(secure ? (errs ? 2 : 1) : 0);
        }
        if (ev == WEBKIT_LOAD_FINISHED) {
            self->m_loadInProgress = false;              // the blank-check may now judge
            self->m_lastLoadFinishedUs = g_get_monotonic_time();   // auto-refresh throttle anchor
            self->m_reloadAttempts = 0;                  // a good load refills the crash auto-reload budget
            Q_EMIT self->loadingChanged(false);
            self->checkReaderable();                     // article? -> enable/disable the Reader button
            qInfo("[t] load finished @%.0fms", msSince(self->m_startUs));
            qCDebug(lcEngine, "[perf] load-finished @%.0fms", msSince(self->m_loadStartUs));
            // Record history for real web pages (not file:// start page, not reader-injected DOM).
            {
                const char* u = webkit_web_view_get_uri(view);
                const char* t = webkit_web_view_get_title(view);
                std::string url = u ? u : "";
                if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
                    self->m_curUrl = url; self->m_curTitle = t ? t : "";
                    rmweb::addHistory(self->m_history, url, self->m_curTitle, (long)time(nullptr));
                    self->queueSave(&self->m_historySaveSrc, 0);   // debounced — not on the WebKit hot path
                    rmweb::upsertTab(self->m_tabs, url, self->m_curTitle);   // tabs-lite: page = open tab
                    self->queueSave(&self->m_tabsSaveSrc, 3);
                    // Resume reading where this URL was left (no-op on the first visit / back at top).
                    self->m_curScroll = rmweb::scrollPosFor(self->m_scroll, url);
                    if (self->m_curScroll > 0) self->scheduleScrollRestore(self->m_curScroll);
                    Q_EMIT self->bookmarkedChanged(rmweb::isBookmarked(self->m_bookmarks, url));
                    // "Rendering…" only if content has not painted yet. If first-content already
                    // arrived during the load (common), do not raise the badge — and clear it if it
                    // was left on from a race (early first-content + late LOAD_FINISHED stuck it).
                    if (self->m_firstContentLogged && self->m_lastNonWhite >= kBlankSamples) {
                        if (self->m_renderingState) {
                            self->m_renderingState = false;
                            Q_EMIT self->renderingChanged(false);
                        }
                    } else if (!self->m_renderingState) {
                        self->m_renderingState = true;
                        Q_EMIT self->renderingChanged(true);
                    }
                } else {
                    self->m_curUrl.clear(); self->m_curTitle.clear();
                    Q_EMIT self->bookmarkedChanged(false);
                    // file:// pages (start page) don't need the "Rendering…" badge.
                    if (self->m_renderingState) {
                        self->m_renderingState = false;
                        Q_EMIT self->renderingChanged(false);
                    }
                }
            }
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

    static void onTitle(GObject *obj, GParamSpec *, gpointer) {
        const char *t = webkit_web_view_get_title(WEBKIT_WEB_VIEW(obj));
        qInfo("[meta] title=%s", t ? t : "");
    }
    static void onProgress(GObject *obj, GParamSpec *, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        const double p = webkit_web_view_get_estimated_load_progress(WEBKIT_WEB_VIEW(obj));
        Q_EMIT self->loadProgressChanged(p);
        // [perf] milestone logs at 25/50/75% (once each per load).
        while (self->m_progressMilestone <= 75 && p >= self->m_progressMilestone / 100.0) {
            qCDebug(lcEngine, "[perf] progress %d%% @%.0fms", self->m_progressMilestone, msSince(self->m_loadStartUs));
            self->m_progressMilestone += 25;
        }
    }
    static gboolean onTlsError(WebKitWebView *, gchar *failing_uri, GTlsCertificate *,
                               GTlsCertificateFlags, gpointer) {
        qWarning("[tls] cert error: %s", failing_uri ? failing_uri : "?");
        return FALSE;   // don't proceed — WebKit fails the load
    }
    // A navigation failed (DNS, refused, timeout — common on the flaky link): replace the dead end
    // with a styled error page (load_alternate_html does NOT add a history entry; the address bar
    // keeps the failed URI). Cancelled loads (superseded navigation) are swallowed silently.
    static gboolean onLoadFailed(WebKitWebView *view, WebKitLoadEvent, gchar *failing_uri,
                                 GError *error, gpointer data) {
        if (error && g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
            return TRUE;   // superseded navigation — silent (this API has no WebKitLoadError domain)
        auto *self = static_cast<WpeEngine*>(data);
        self->m_loadInProgress = false;   // failed load never emits LOAD_FINISHED — un-block the blank-check
        const char *uri = failing_uri ? failing_uri : "";
        qWarning("[nav] load failed: %s (%s)", uri, (error && error->message) ? error->message : "?");
        if (!uri[0] || !rmweb::isSafeLinkUrl(uri))   // only http(s) gets an error page
            return FALSE;
        const std::string html = buildErrorPage(uri, (error && error->message) ? error->message : "unknown error");
        webkit_web_view_load_alternate_html(view, html.c_str(), uri, nullptr);
        return TRUE;
    }
    // Error page in the start page's design language (JS-free, e-ink-safe). Retry = the failed URL.
    static std::string buildErrorPage(const std::string &uri, const std::string &msg) {
        return
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'><title>rmweb</title><style>"
            "body{font-family:sans-serif;margin:0;padding:90px 64px;color:#000;background:#fff;}"
            ".glyph{width:120px;height:120px;line-height:116px;text-align:center;border:5px solid #000;"
            "border-radius:60px;font-size:64px;font-weight:800;margin-bottom:40px;}"
            "h1{font-size:48px;margin:0 0 16px;}"
            ".u{color:#666;font-size:28px;word-break:break-all;margin-bottom:10px;}"
            ".m{color:#888;font-size:26px;margin-bottom:48px;}"
            "a.retry{display:inline-block;border:4px solid #000;border-radius:16px;padding:22px 44px;"
            "font-size:32px;font-weight:700;color:#000;text-decoration:none;}"
            "</style></head><body>"
            "<div class='glyph'>!</div><h1>Couldn't load the page</h1><div class='u'>" + rmweb::htmlEscape(uri) +
            "</div><div class='m'>" + rmweb::htmlEscape(msg) + "</div>"
            "<a class='retry' href='" + rmweb::htmlEscape(uri) + "'>Try again</a>"
            "</body></html>";
    }
    static void onWebProcessTerminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer data) {
        auto *self = static_cast<WpeEngine*>(data);
        qWarning("[crash] WebProcess terminated (reason=%d, Phase2-hardened recovery), attempts=%d", reason, self->m_reloadAttempts);
        // hardened: exponential backoff + diagnostic log (avoids tight loops on repeated crashes)
        if (self->m_reloadAttempts < 3) {
            self->m_reloadAttempts++;
            int backoffMs = 500 * (1 << (self->m_reloadAttempts - 1));
            qInfo("[recovery] scheduling reload in %d ms (attempt %d)", backoffMs, self->m_reloadAttempts);
            // The recovery reload is ENGINE-initiated, not a site auto-refresh — exempt it from
            // the auto-refresh guard or a crash within the throttle window would never recover.
            self->m_expectUserNav = true;
            // glib timer on the worker context (same pattern as scheduleRenderCheck): the worker
            // thread runs NO Qt event loop, so QTimer::singleShot here could never fire. The view
            // is ref'd for the wait; the destroy-notify drops the ref whether the timer fires or
            // is discarded with the context at teardown.
            GSource *s = g_timeout_source_new(static_cast<guint>(backoffMs));
            g_source_set_callback(s, [](gpointer v) -> gboolean {
                webkit_web_view_reload(WEBKIT_WEB_VIEW(v));
                return G_SOURCE_REMOVE;
            }, g_object_ref(view), [](gpointer v) { g_object_unref(v); });
            g_source_attach(s, self->m_ctx);
            g_source_unref(s);
        } else {
            qWarning("[crash] giving up auto-reload after %d attempts", self->m_reloadAttempts);
        }
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
        // Visible content: clear "Rendering…" whenever it is still on (not only the first time).
        // Previously first-content could fire early (stale frame), then LOAD_FINISHED re-raised the
        // badge and subsequent frames were all dups so the badge never cleared.
        if (nonWhite >= kBlankSamples) {
            if (!self->m_firstContentLogged) {
                self->m_firstContentLogged = true;
                qCDebug(lcEngine, "[perf] first-content @%.0fms (nonWhite=%d)", msSince(self->m_loadStartUs), nonWhite);
            }
            if (self->m_renderingState) {
                self->m_renderingState = false;
                Q_EMIT self->renderingChanged(false);
            }
        }
        // Skip identical frames: WebKit re-submits the same composited buffer on its idle heartbeat, and
        // the rAF page-turn pulse yields several identical ticks. Only repaint the e-ink when pixels change
        // — this kills the wasteful periodic re-present and any flicker from the nudge.
        const bool changed = (sig != self->m_lastSig);
        self->m_lastSig = sig;

        const double dt   = self->m_lastBufUs ? (tIn - self->m_lastBufUs) / 1000.0 : 0.0;
        const double flip = self->m_pageUs    ? (tIn - self->m_pageUs)    / 1000.0 : -1.0;
        qCDebug(lcEngine, "[t] frame %d @%.0fms  build=%.1fms  dt=%.1fms  flip-latency=%.1fms  sig=%08x %s  %dx%d",
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
    bool m_loadInProgress = false;   // LOAD_STARTED..FINISHED/failed — the blank-check re-arms while true
    bool m_expectUserNav = false;    // UI-initiated navigation (reload/Go) — exempt from the auto-refresh guard
    gint64 m_lastLoadFinishedUs = 0; // auto-refresh throttle anchor (set at LOAD_FINISHED)
    guint m_loadGen = 0;           // bumped on each load start -> a stale render-check (grace timer) is skipped
    gint64 m_loadStartUs = 0;      // monotonic time of the current LOAD_STARTED (for [perf] ms offsets)
    bool m_firstContentLogged = false; // true once [perf] first-content has been emitted for this load
    int m_progressMilestone = 0;   // next progress milestone to log: 25, 50, 75 (reset per load)
    bool m_renderingState = false; // true while compositing (LOAD_FINISHED -> first-content or renderFailed)
    bool m_readerMode = false;     // reader view currently applied (vs the original page)
    bool m_readerApplying = false; // an applyReader() JS eval is in flight (gates re-entrant Reader taps)
    std::string m_readabilityJs;   // vendored Readability.js, lazily slurped + cached
    std::string m_readerableJs;    // vendored isProbablyReaderable, lazily slurped + cached
    double m_dpr = 2.0;            // panel-px -> CSS-px divisor (for elementFromPoint link hit-testing)
    double m_zoom = 1.0;           // page zoom level (A-/A+ in normal mode; webkit_web_view_set_zoom_level)
    int m_readerFont = 30;         // reader column font px (A-/A+ in reader mode; RMWEB_READER_FONT default)
    std::string m_profileDir;                       // /home/root/.rmweb (or $RMWEB_PROFILE)
    std::vector<rmweb::Bookmark> m_bookmarks;
    std::vector<rmweb::HistoryEntry> m_history;
    rmweb::Settings m_settings;
    std::vector<rmweb::ScrollEntry> m_scroll;       // per-URL reading positions (scroll.txt)
    std::vector<rmweb::Tab> m_tabs;                 // open pages, MRU first (tabs.txt — tabs-lite)
    GSource *m_historySaveSrc = nullptr;            // pending debounced history write (worker ctx)
    GSource *m_settingsSaveSrc = nullptr;           // pending debounced settings write (worker ctx)
    GSource *m_scrollSaveSrc = nullptr;             // pending debounced scroll-position write
    GSource *m_tabsSaveSrc = nullptr;               // pending debounced tabs write
    std::string m_curUrl, m_curTitle;               // current committed page (for history + bookmark)
    int m_curScroll = 0;                            // last recorded scroll offset of m_curUrl (CSS px)
    bool m_userScrolled = false;                    // a page turn happened on this load (suppress restore)
    bool m_lastProbePeek = false;                   // the in-flight tap probe is a long-press peek (no linkMissed)
    rmweb::FieldKind m_pendingFieldKind = rmweb::FieldKind::None;   // kind of the last focused field (autofill learn)
    std::vector<rmweb::PasswordEntry> m_passwords;  // per-host logins, obfuscated (passwords.txt)
    GSource *m_pwSaveSrc = nullptr;                 // pending debounced passwords write
    std::string m_lastCommitText;                   // plaintext of the in-flight field commit (pw capture; cleared in onFieldSet)
    std::string m_lastFind;                         // last in-page search term (repeat = search_next)
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
                [this]{ qCDebug(lcEngine, "[t][gui] present fallback-release (no frameSwapped)"); releaseGate(); });
        rebuildKeys();   // URL keyboard, drawn into the frame (B2)
        // Keyboard: buffer keystrokes, paint once after a short idle (e-ink can't keep up with per-key presents).
        m_kbFlush.setSingleShot(true);
        connect(&m_kbFlush, &QTimer::timeout, this, [this]{
            m_kbPressed = -1;                                // release the flashed key
            if (m_editing) schedule(/*guardTouch=*/false);   // flush address+keys without re-arming touch blank
        });
        // Content present throttle: heavy SPAs (a heavy news portal etc.) emit NEW frames every ~300ms forever
        // (ads/tickers). Presenting each to e-ink locks the UI under continuous refresh + touch guard.
        // Keep the latest pixels, paint at most every kContentMinPresentMs unless forceNextContent().
        if (const int v = qEnvironmentVariableIntValue("RMWEB_CONTENT_PRESENT_MS"); v > 0)
            m_contentMinPresentMs = v;
        m_contentFlush.setSingleShot(true);
        connect(&m_contentFlush, &QTimer::timeout, this, [this]{
            if (m_hasPending) schedule(/*guardTouch=*/true);
        });
        // Toast (find results, downloads): shown for a few seconds, then cleared + repainted away.
        m_noticeTimer.setSingleShot(true);
        connect(&m_noticeTimer, &QTimer::timeout, this, [this]{
            m_notice.clear();
            schedule();
        });
    }
    // Call before user-driven actions (page turn, reload, link) so the next WPE frame paints immediately.
    void forceNextContent() {
        m_forceContentPresent = true;
        m_contentFlush.stop();
    }
    void paint(QPainter *p) override {
        const qreal w = width(), h = height();
        if (!m_img.isNull()) p->drawImage(QRectF(0, 0, w, h), m_img);
        else                 p->fillRect(QRectF(0, 0, w, h), Qt::white);
        // A failed (blank) render is a PAGE state, not an overlay: white out the stale page or the
        // previous site bleeds through around the notice box and reads as a half-rendered mess.
        if (m_renderFailed) p->fillRect(QRectF(0, 0, w, h), Qt::white);
        // Badge precedence: Loading > RenderFailed > Rendering > nothing. While the URL keyboard is
        // open the old page's states are noise — the user came here to go ELSEWHERE: the white-out
        // still hides the stale page, but the notice itself stays out of the way (field edits keep
        // everything — the page is the context you're typing into).
        const bool urlEditing = m_editing && !m_editField;
        if (m_loading && !m_renderFailed)            drawLoadingBadge(p, w);
        else if (m_renderFailed && !urlEditing)      drawRenderNotice(p, w, h);
        else if (m_rendering && !m_renderFailed)     drawRenderingBadge(p, w);
        if (!m_notice.isEmpty())                 drawNoticeToast(p, w);   // toast overlays content, not chrome
        if (m_readProgress >= 0.0 && !m_editing) drawReadProgress(p, w, h);  // bottom edge, even in reader-fullscreen
        if (!m_chromeOn && !m_editing) return;  // reader-fullscreen: hide chrome (an open keyboard
                                                //  — e.g. a tapped form field — still needs the bar)
        drawChromeBar(p, w);
        if (m_editing) drawKeyboard(p, w, h); // URL keyboard only while editing
    }
    // Hit-test a tap against the chrome bar (panel px); returns the control, or None (off / below the bar).
    enum Hit { None, Back, Fwd, Reload, Home, Address, AddressClear, ZoomOut, ZoomIn, Bookmark, Reader, Power };
    // Right-cluster geometry (panel px) — ONE source for hit-test, pressed overlay and painting.
    struct ChromeX { int powerX, readerX, starX, zInX, zOutX; };
    ChromeX chromeLayout() const {
        ChromeX c;
        c.powerX  = int(width()) - kPowerW;         // right: A- | A+ | ★ | Reader | Power
        c.readerX = c.powerX - kReaderW;
        c.starX   = c.readerX - kStarW;
        c.zInX    = c.starX - kZoomW;
        c.zOutX   = c.zInX - kZoomW;
        return c;
    }
    Hit hitChrome(int x, int y) const {
        if (!m_chromeOn || y >= kBarH) return None;
        const ChromeX c = chromeLayout();
        if (x < kBackX)         return Back;
        if (x < kFwdX)          return Fwd;
        if (x < kRelX)          return Reload;
        if (x < kRelX + kHomeW) return Home;                // Home sits just after Reload
        if (x >= c.powerX)      return Power;
        if (x >= c.readerX)     return Reader;
        if (x >= c.starX)       return Bookmark;
        if (x >= c.zInX)        return ZoomIn;
        if (x >= c.zOutX)       return ZoomOut;
        // Inside the address box: × on the right while editing clears the typed buffer.
        if (m_editing) {
            const int clearLeft = c.zOutX - 8 - kClearW;
            if (x >= clearLeft) return AddressClear;
        }
        return Address;
    }
    bool chromeOn()  const { return m_chromeOn; }
    bool isLoading() const { return m_loading; }
    bool isEditing() const { return m_editing; }
    // Tap on the X at the right end of the "Loading NN%" pill (rect stashed by drawLoadingBadge).
    bool hitLoadingStop(int x, int y) const {
        return m_loading && !m_renderFailed && m_loadingStopRect.contains(x, y);
    }
    // Brief inverted flash on a tapped chrome button — on a ~200 ms-latency panel an instant
    // acknowledgement is what makes the UI feel responsive. One present now, one to restore.
    void pressChrome(Hit h) {
        if (h == None || h == Address || h == AddressClear) return;   // the keyboard opening is feedback enough
        m_pressed = h;
        schedule(/*guardTouch=*/false);
        QTimer::singleShot(180, this, [this]{ m_pressed = None; schedule(/*guardTouch=*/false); });
    }
    // Panel-px rect of a chrome control (same layout as hitChrome via chromeLayout), for painting.
    QRectF chromeHitRect(Hit h) const {
        const ChromeX c = chromeLayout();
        switch (h) {
            case Back:     return QRectF(0, 0, kBackX, kBarH);
            case Fwd:      return QRectF(kBackX, 0, kFwdX - kBackX, kBarH);
            case Reload:   return QRectF(kFwdX, 0, kRelX - kFwdX, kBarH);
            case Home:     return QRectF(kRelX, 0, kHomeW, kBarH);
            case ZoomOut:  return QRectF(c.zOutX, 0, kZoomW, kBarH);
            case ZoomIn:   return QRectF(c.zInX, 0, kZoomW, kBarH);
            case Bookmark: return QRectF(c.starX, 0, kStarW, kBarH);
            case Reader:   return QRectF(c.readerX, 0, kReaderW, kBarH);
            case Power:    return QRectF(c.powerX, 0, kPowerW, kBarH);
            default:       return QRectF();
        }
    }
    // One glyph per chrome control, centered on its rect (chromeHitRect). Callers set pen/brush for
    // state (enabled grey / pressed white); the Reader mode chip background stays with the caller.
    void drawChromeIcon(QPainter *p, Hit h) const {
        const QRectF r = chromeHitRect(h);
        const qreal cx = r.center().x(), cy = r.center().y();
        switch (h) {
            case Back:     iconBack(p, cx, cy); break;
            case Fwd:      iconFwd(p, cx, cy); break;
            case Reload:   m_loading ? iconStop(p, cx, cy) : iconReload(p, cx, cy); break;
            case Home:     iconHome(p, cx, cy); break;
            case Bookmark: iconStar(p, cx, cy, m_bookmarked); break;
            case Reader:   iconReader(p, cx, cy); break;
            case Power:    iconPower(p, cx, cy); break;
            case ZoomOut:
            case ZoomIn: {
                QFont zf = p->font(); zf.setPixelSize(40); zf.setBold(true); p->setFont(zf);
                p->drawText(r, Qt::AlignCenter, h == ZoomOut ? "A-" : "A+");
                break;
            }
            default: break;
        }
    }
    // URL entry: tap address -> empty field + keyboard. Cancel / empty Go keep the previous URL.
    // × in the field clears the typed buffer. Go with non-empty text navigates.
    void beginEdit() {
        m_editing = true;
        m_editField = false; m_editMasked = false;
        g_urlEditing.store(true, std::memory_order_release);
        m_editBuf.clear();   // start blank; m_addr stays as the "old" value until a successful Go
        m_kbShift = false; m_kbSym = false; rebuildKeys();   // always reopen on the plain letters page
        m_kbFlush.stop();
        schedule(/*guardTouch=*/false);
    }
    // Form-field entry (a text field on the page was tapped): the keyboard opens PRE-FILLED with the
    // field's current value; Go commits into the field (fieldTextEntered), Cancel discards.
    // masked = password input -> the echo shows '*'. Chrome is summoned so the input line is visible.
    // suggest = autofill prefill, used only when the field itself is empty (a learned value the
    // user can edit or accept with Go).
    void beginFieldEdit(const QString &value, bool masked, const QString &suggest) {
        m_editing = true;
        m_editField = true; m_editMasked = masked;
        m_chromeOn = true;                      // the keyboard needs the bar (chrome may be hidden)
        g_urlEditing.store(true, std::memory_order_release);
        const bool useSuggest = value.isEmpty() && !suggest.isEmpty();   // prefill an EMPTY field only
        m_editBuf = useSuggest ? suggest : value;
        if (useSuggest) setNotice("Autofill — edit or press Go");
        m_kbShift = false; m_kbSym = false; rebuildKeys();
        m_kbFlush.stop();
        schedule(/*guardTouch=*/false);
    }
    void endEdit() {
        if (!m_editing) return;
        m_kbFlush.stop();
        m_editing = false;
        m_editField = false; m_editMasked = false;
        m_kbPressed = -1;
        g_urlEditing.store(false, std::memory_order_release);
        m_editBuf.clear();
        schedule(/*guardTouch=*/false);
    }
    void clearEditBuf() {
        if (!m_editing || m_editBuf.isEmpty()) return;
        m_editBuf.clear();
        m_kbPressed = -1;
        m_kbFlush.stop();
        schedule(/*guardTouch=*/false);
    }
    void handleEditTap(int x, int y) {
        // × in the address bar (chrome) while the keyboard is open.
        if (y < kBarH && hitChrome(x, y) == AddressClear) { clearEditBuf(); return; }
        const int i = rmweb::hitKey(m_keys, x, y);
        if (i < 0) return;                                       // tap outside the keys (page area) -> ignore
        switch (m_keys[i].kind) {
            case rmweb::KeyKind::Char:
                m_editBuf += QString::fromStdString(m_keys[i].insert);
                if (m_kbShift) { m_kbShift = false; rebuildKeys(); }   // one-shot Shift
                // Flash the key NOW (inverted) — on a 200 ms-latency panel immediate feedback is
                // the difference between "responsive" and "did it register?"; kbFlush restores it.
                m_kbPressed = i;
                schedule(/*guardTouch=*/false);
                m_kbFlush.start(kKbFlushMs);
                return;
            case rmweb::KeyKind::Shift:
                m_kbShift = !m_kbShift;
                rebuildKeys();
                schedule(/*guardTouch=*/false);                  // case labels changed — repaint keys now
                return;
            case rmweb::KeyKind::Sym:
                m_kbSym = !m_kbSym; m_kbShift = false;           // page switch drops an armed Shift
                rebuildKeys();
                schedule(/*guardTouch=*/false);
                return;
            case rmweb::KeyKind::Backspace:
                m_editBuf.chop(1);
                m_kbPressed = i;
                schedule(/*guardTouch=*/false);
                m_kbFlush.start(kKbFlushMs);
                return;
            case rmweb::KeyKind::Cancel:
                endEdit();                                       // discard typed text; keep m_addr
                return;
            case rmweb::KeyKind::Go: {
                m_kbFlush.stop();
                // Field mode commits the RAW buffer (spaces are meaningful in text); URL mode trims.
                // Empty Go on a URL keeps the old address; empty Go on a field CLEARS it.
                const QString u = m_editField ? m_editBuf : m_editBuf.trimmed();
                const bool field = m_editField;
                endEdit();
                if (field) Q_EMIT fieldTextEntered(u);
                else if (!u.isEmpty()) Q_EMIT urlEntered(u);
                return;
            }
        }
    }
Q_SIGNALS:
    void urlEntered(const QString &url);   // Go pressed with a non-empty buffer -> load it (wired in main())
    void fieldTextEntered(const QString &text);   // Go in field mode -> commit into the focused page field
public Q_SLOTS:
    void setImage(const QImage &img) {
        m_pending = img;
        m_hasPending = true;
        // Always keep the latest frame. Only schedule an e-ink present if forced (user action) or
        // the min interval since the last *content* present has elapsed (anti-frame-storm for SPAs).
        const gint64 now = g_get_monotonic_time();
        const gint64 minUs = static_cast<gint64>(m_contentMinPresentMs) * 1000LL;
        if (m_forceContentPresent || m_lastContentPresentUs == 0 ||
            (now - m_lastContentPresentUs) >= minUs) {
            m_forceContentPresent = false;
            m_contentFlush.stop();
            schedule(/*guardTouch=*/true);
            return;
        }
        // Coalesce: present the newest pending after the remaining wait.
        const int waitMs = static_cast<int>((minUs - (now - m_lastContentPresentUs) + 999) / 1000);
        if (!m_contentFlush.isActive())
            m_contentFlush.start(std::max(50, waitMs));
    }
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
    void setTlsState(int s)        { if (s != m_tlsState) { m_tlsState = s; schedule(); } }   // 0 none, 1 https, 2 https+cert errors
    void setRendering(bool v)      { if (v != m_rendering)  { m_rendering  = v; schedule(); } }
    void setAddr(const QString &s) { if (s != m_addr)     { m_addr     = s; schedule(); } }
    void setReaderMode(bool v)     { if (v != m_readerMode)  { m_readerMode  = v; schedule(); } }
    void setReaderable(bool v)     { if (v != m_readerable) { m_readerable = v; schedule(); } }
    void setBookmarked(bool v) { if (v != m_bookmarked) { m_bookmarked = v; schedule(); } }
    void setNotice(const QString &s) {           // transient toast (find results, downloads)
        if (s.isEmpty()) return;
        m_notice = s;
        m_noticeTimer.start(kNoticeMs);          // re-arms if a second notice lands quickly
        schedule();
    }
    void setReadProgress(double f) {             // reading position 0..1; -1 hides the bar.
        m_readProgress = f;                      // NO schedule(): a content frame always follows
    }                                            // (scroll/restore force one) — a separate present
                                                 // here would double the e-ink flash per page turn
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
        qCDebug(lcEngine, "[t][gui] frameSwapped @%dms (dwell=%d)", ms, m_dwellMs);
        // Waveform tail can induce phantom taps — re-arm only when this present requested a guard
        // (content/chrome). Keyboard flushes leave it off so typing is not blanked mid-burst.
        if (m_lastPresentGuarded) bumpTouchGuard();
        const int wait = m_dwellMs - ms;         // hold the rest of the dwell so the next can't overlap
        if (wait > 0) QTimer::singleShot(wait, this, [this]{ releaseGate(); });
        else releaseGate();
    }
private:
    // --- B2 frame painters (called by paint(); kept here so paint() stays a short orchestrator) -------------
    // Shared pill: draws a centered rounded-rect badge with a text label at kBarH+50. Returns the pill rect.
    QRectF drawTextPill(QPainter *p, qreal w, const QString &lbl, qreal extraLeftW = 0, qreal extraRightW = 0) const {
        QFont lf = p->font(); lf.setPixelSize(40); p->setFont(lf);
        const qreal tw = p->fontMetrics().horizontalAdvance(lbl);
        const qreal pad = 30, bh = 96, bw = pad + extraLeftW + tw + extraRightW + pad;
        const qreal bx = (w - bw) / 2, by = kBarH + 50;
        p->setPen(Qt::black); p->setBrush(Qt::white);
        p->drawRoundedRect(QRectF(bx, by, bw, bh), 18, 18);
        p->setBrush(Qt::NoBrush); p->setPen(Qt::black);
        p->drawText(QRectF(bx + pad + extraLeftW, by, tw + 6, bh), Qt::AlignVCenter | Qt::AlignLeft, lbl);
        return QRectF(bx, by, bw, bh);
    }
    // "Working hard" indicator while a page loads: an hourglass + "Loading NN%" from the real load progress,
    // plus an X at the right end — tap it to abort a load that's going nowhere (see hitLoadingStop).
    void drawLoadingBadge(QPainter *p, qreal w) const {
        const QString lbl = QStringLiteral("Loading %1%").arg(int(m_loadProgress * 100));
        const qreal iconW = 34, gap = 18, stopW = 34, stopGap = 18;
        QRectF pill = drawTextPill(p, w, lbl, iconW + gap, stopGap + stopW);
        // Hourglass icon inside the left padding area of the pill.
        const qreal ix = pill.x() + 30, iy = pill.y() + (pill.height() - 48) / 2;
        QPolygonF top, bot;
        top << QPointF(ix, iy) << QPointF(ix + iconW, iy) << QPointF(ix + iconW / 2, iy + 24);
        bot << QPointF(ix + iconW / 2, iy + 24) << QPointF(ix, iy + 48) << QPointF(ix + iconW, iy + 48);
        p->setBrush(Qt::black); p->drawPolygon(top); p->drawPolygon(bot);
        p->setBrush(Qt::NoBrush);
        // X (abort) inside the right padding area; the whole right end of the pill is the hit zone.
        const qreal sx = pill.right() - 30 - stopW, sy = pill.y() + (pill.height() - stopW) / 2;
        QPen xp(Qt::black); xp.setWidth(6); xp.setCapStyle(Qt::RoundCap); p->setPen(xp);
        p->drawLine(QPointF(sx, sy), QPointF(sx + stopW, sy + stopW));
        p->drawLine(QPointF(sx + stopW, sy), QPointF(sx, sy + stopW));
        p->setPen(Qt::black);
        m_loadingStopRect = QRectF(sx - stopGap, pill.y(), pill.right() - sx + stopGap, pill.height());
    }
    // "Rendering…" pill: shown after load-finished while llvmpipe composites the page (no progress data).
    void drawRenderingBadge(QPainter *p, qreal w) const {
        drawTextPill(p, w, QStringLiteral("Rendering…"));
    }
    // Transient toast (find results, download notices) below the badge zone — inverted so it reads
    // as an overlay, not part of the page.
    void drawNoticeToast(QPainter *p, qreal w) const {
        QFont lf = p->font(); lf.setPixelSize(36); p->setFont(lf);
        const qreal tw = p->fontMetrics().horizontalAdvance(m_notice);
        const qreal pad = 34, bh = 88, bw = pad + tw + pad;
        const QRectF r((w - bw) / 2, kBarH + 170, bw, bh);
        p->setPen(Qt::NoPen); p->setBrush(Qt::black);
        p->drawRoundedRect(r, 16, 16);
        p->setPen(Qt::white); p->setBrush(Qt::NoBrush);

        p->drawText(r, Qt::AlignCenter, m_notice);
        p->setPen(Qt::black);
    }
    // Reading-progress bar (KOReader-style): a thin track along the very bottom edge, black fill =
    // fraction read. Painted even with the chrome hidden (reader fullscreen); skipped while the
    // keyboard is up (it covers the bottom edge anyway).
    void drawReadProgress(QPainter *p, qreal w, qreal h) const {
        const qreal th = 6;
        p->setPen(Qt::NoPen);
        p->setBrush(Qt::white);
        p->drawRect(QRectF(0, h - th, w, th));                            // track
        p->setBrush(Qt::black);
        p->drawRect(QRectF(0, h - th, w * std::min(1.0, m_readProgress), th));   // fill
        p->drawRect(QRectF(0, h - th - 1, w, 1));                         // 1 px separator from content
    }
    // Load finished but the page rendered ~nothing (a heavy JS app the CPU can't run). "(!)" + two lines.
    void drawRenderNotice(QPainter *p, qreal w, qreal h) const {
        const QString t1 = QStringLiteral("Couldn't display the page");
        const QString t2 = QStringLiteral("heavy site or web app");
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
    // B2 browser chrome painted into the frame (QtQuick does not composite over WPE; see
    // docs/research/epaper-chrome-compositing.md). e-ink rules: no gradients/shadows, bold outlines,
    // large targets, address field drawn as a real rounded input box (EinkBro/KOReader pattern).
    void drawChromeBar(QPainter *p, qreal w) const {
        p->fillRect(QRectF(0, 0, w, kBarH), Qt::white);
        p->fillRect(QRectF(0, kBarH - 3, w, 3), Qt::black);
        auto pen = [&](bool on) { p->setPen(on ? Qt::black : QColor(170, 170, 170)); p->setBrush(Qt::NoBrush); };
        pen(m_canBack); drawChromeIcon(p, Back);
        pen(m_canFwd);  drawChromeIcon(p, Fwd);
        pen(true);      drawChromeIcon(p, Reload);
        pen(true);      drawChromeIcon(p, Home);

        const ChromeX c = chromeLayout();
        const int addrX = kRelX + kHomeW;
        const int clearW = m_editing ? kClearW : 0;
        // Address field: rounded box; while editing, a × on the right clears the typed buffer.
        const QRectF addrBox(addrX + 8, 14, c.zOutX - addrX - 16, kBarH - 28);
        p->setPen(QPen(Qt::black, m_editing ? 4 : 3));
        p->setBrush(m_editing ? QColor(245, 245, 245) : Qt::white);
        p->drawRoundedRect(addrBox, 14, 14);
        QFont af = p->font(); af.setPixelSize(32); p->setFont(af);
        QString addrText;
        bool grey = false;
        if (m_editing) {
            if (m_editBuf.isEmpty()) {
                // Hint shows previous URL (not submitted) so Cancel/empty-Go is obvious.
                grey = true;
                addrText = m_editField ? QStringLiteral("type text…")
                         : m_addr.isEmpty() ? QStringLiteral("type URL…") : m_addr;
            } else {
                // Password fields echo '*' (the real text stays in m_editBuf).
                addrText = (m_editMasked ? QString(m_editBuf.size(), QLatin1Char('*')) : m_editBuf)
                         + QLatin1Char('|');
            }
        } else if (m_addr.isEmpty()) {
            grey = true;
            addrText = QStringLiteral("URL or search — /text finds in page");
        } else {
            addrText = m_addr;
        }
        p->setPen(grey ? QColor(120, 120, 120) : Qt::black);
        const auto elide = m_editing && !m_editBuf.isEmpty() ? Qt::ElideLeft : Qt::ElideRight;
        const int textRightPad = 14 + clearW;
        const int lockPad = (!m_editing && m_tlsState > 0) ? 46 : 0;   // TLS lock inside the address box
        if (lockPad) {
            p->setPen(Qt::black);
            iconLock(p, addrBox.left() + 14 + 17, addrBox.center().y(), m_tlsState == 1);
            p->setPen(grey ? QColor(120, 120, 120) : Qt::black);
        }
        const QString a = p->fontMetrics().elidedText(addrText, elide, int(addrBox.width() - 14 - lockPad - textRightPad));
        p->drawText(addrBox.adjusted(14 + lockPad, 0, -textRightPad, 0), Qt::AlignVCenter | Qt::AlignLeft, a);
        if (m_editing) {
            // Clear button: circle + × (large hit target for finger on e-ink).
            const qreal cx = addrBox.right() - kClearW / 2.0, cy = addrBox.center().y();
            const qreal r = 22;
            p->setPen(QPen(Qt::black, 3));
            p->setBrush(Qt::white);
            p->drawEllipse(QPointF(cx, cy), r, r);
            p->setPen(QPen(Qt::black, 4, Qt::SolidLine, Qt::RoundCap));
            const qreal d = 10;
            p->drawLine(QPointF(cx - d, cy - d), QPointF(cx + d, cy + d));
            p->drawLine(QPointF(cx + d, cy - d), QPointF(cx - d, cy + d));
        }

        p->setPen(Qt::black);
        drawChromeIcon(p, ZoomOut);
        drawChromeIcon(p, ZoomIn);
        if (m_readerMode) {
            p->setBrush(Qt::black); p->setPen(Qt::NoPen);
            p->drawRoundedRect(QRectF(c.readerX + 20, 12, kReaderW - 40, kBarH - 24), 12, 12);
            p->setPen(Qt::white); p->setBrush(Qt::NoBrush); drawChromeIcon(p, Reader);
        } else { pen(m_readerable); drawChromeIcon(p, Reader); }
        pen(true); drawChromeIcon(p, Bookmark);
        pen(true); drawChromeIcon(p, Power);

        // Pressed-button flash: black rounded chip + the same icon in white, over the normal bar.
        if (m_pressed != None) {
            const QRectF r = chromeHitRect(m_pressed).adjusted(6, 6, -6, -6);
            p->setPen(Qt::NoPen); p->setBrush(Qt::black);
            p->drawRoundedRect(r, 12, 12);
            p->setPen(Qt::white); p->setBrush(Qt::NoBrush);
            drawChromeIcon(p, m_pressed);   // the symmetric adjust keeps the rect center
            p->setPen(Qt::black); p->setBrush(Qt::NoBrush);
        }
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
    void iconLock(QPainter *p, qreal cx, qreal cy, bool closed) const {    // TLS padlock; open shackle = cert errors
        QPen pn = p->pen(); pn.setWidthF(3.5); p->setPen(pn);
        const qreal bw = 24, bh = 19, by = cy - 1;                          // body sits below center
        if (closed) p->setBrush(Qt::black); else p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(cx - bw / 2, by, bw, bh), 4, 4);
        p->setBrush(Qt::NoBrush);                                           // shackle arc above the body
        p->drawArc(QRectF(cx - 8, by - 15, 16, 17), closed ? 0 : 35 * 16, (closed ? 180 : 145) * 16);
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
    // Rebuild the keyboard layout for the current page (letters/symbols) and Shift state.
    void rebuildKeys() { m_keys = rmweb::buildKeyboard(kPanelW, kPanelH, kKbTopY, m_kbShift, m_kbSym); }
    // On-screen URL keyboard, drawn into the frame (B2). Taps -> handleEditTap() (keyboard.h hitKey) via main().
    void drawKeyboard(QPainter *p, qreal w, qreal h) const {
        p->fillRect(QRectF(0, kKbTopY, w, h - kKbTopY), Qt::white);
        p->fillRect(QRectF(0, kKbTopY, w, 2), Qt::black);
        QFont kf = p->font(); kf.setPixelSize(44); p->setFont(kf);
        for (size_t ki = 0; ki < m_keys.size(); ++ki) {
            const rmweb::Key &k = m_keys[ki];
            const QRectF r(k.x, k.y, k.w, k.h);
            // Inverted (black) keys: Go always; Shift while armed; ?123/ABC while the symbols page is on;
            // and the just-tapped key for its ~120 ms flash (e-ink press feedback).
            const bool armed = k.kind == rmweb::KeyKind::Go
                || (k.kind == rmweb::KeyKind::Shift && m_kbShift)
                || (k.kind == rmweb::KeyKind::Sym   && m_kbSym)
                || int(ki) == m_kbPressed;
            if (armed) { p->fillRect(r.adjusted(3, 3, -3, -3), Qt::black); p->setPen(Qt::white); }
            else { p->setPen(Qt::black); p->drawRect(r.adjusted(2, 2, -2, -2)); }
            p->drawText(r, Qt::AlignCenter, QString::fromStdString(k.label));
        }
    }
    // guardTouch: e-ink refresh induces phantom taps — blank them for content presents. Keyboard
    // chrome flushes pass false so typing is not blocked mid-burst.
    void schedule(bool guardTouch = true) {
        m_nextGuardTouch = m_nextGuardTouch || guardTouch;
        if (m_inFlight) m_dirty = true;
        else presentNext();
    }
    void presentNext() {
        // Apply newest WPE frame if any; always present so chrome-only updates (URL bar, keyboard,
        // badges) still refresh when m_img is still null (before the first buffer).
        const bool hadContent = m_hasPending;
        if (m_hasPending) { m_img = m_pending; m_hasPending = false; }
        m_dirty = false; m_inFlight = true;
        m_clock.restart();
        if (hadContent) m_lastContentPresentUs = g_get_monotonic_time();
        m_lastPresentGuarded = m_nextGuardTouch;
        if (m_nextGuardTouch) bumpTouchGuard();  // content/chrome present: blank phantom noise
        m_nextGuardTouch = false;
        update();                                // -> scene render -> EPRenderLoop present to panel
        m_fallback.start(kFallbackMs);
    }
    void releaseGate() {
        m_fallback.stop(); m_inFlight = false;
        if (m_hasPending || m_dirty) presentNext();   // newer frame or a chrome change queued -> present it
    }
    static const int kFallbackMs = 2500;         // release even if frameSwapped never fires (>= worst refresh)
    static const int kKbFlushMs = 120;           // coalesce keystrokes; one e-ink paint after typing pause
    int m_dwellMs = 200;                         // min present spacing, ms (RMWEB_PRESENT_DWELL overrides)
    int m_contentMinPresentMs = 1200;            // SPA frame-storm throttle (RMWEB_CONTENT_PRESENT_MS)
    bool m_hasPending = false, m_inFlight = false, m_dirty = false;
    bool m_nextGuardTouch = true;                // whether the next present arms the phantom-touch guard
    bool m_lastPresentGuarded = true;            // last present used the guard (for frameSwapped re-arm)
    bool m_forceContentPresent = false;          // next setImage bypasses content throttle (user action)
    gint64 m_lastContentPresentUs = 0;            // last e-ink present that carried a new WPE frame
    QImage m_img, m_pending;
    QElapsedTimer m_clock;
    QTimer m_fallback;
    QTimer m_kbFlush;                            // keyboard address-bar coalesced redraw
    QTimer m_contentFlush;                       // delayed present of throttled SPA frames
    QTimer m_noticeTimer;                        // toast auto-clear (find results, downloads)
    QString m_notice;                            // toast text ("" = hidden)
    double m_readProgress = -1.0;                // reading position 0..1; <0 = bar hidden
    mutable QRectF m_loadingStopRect;            // X zone of the "Loading NN%" pill (stashed by its painter)
    static const int kNoticeMs = 4000;           // toast on-screen time
    // chrome state, painted into the frame (reader-first: shown on launch, hidden by a content tap).
    // Chrome layout (panel 1620): left cluster | wide address box | A- A+ ★ Reader Power
    // Give the URL field ~half the bar — previous widths left only ~220px and the box looked "gone".
    static const int kBarH = 104, kBackX = 150, kFwdX = 300, kRelX = 480, kReaderW = 150, kZoomW = 100, kPowerW = 110;
    static const int kHomeW = 120, kStarW = 100;
    static const int kClearW = 72;               // × clear-button zone on the right of the address box
    bool m_chromeOn = true, m_canBack = false, m_canFwd = false, m_loading = false;
    Hit m_pressed = None;                // chrome button currently flashing its pressed state
    int m_tlsState = 0;                  // 0 = http/none, 1 = https ok, 2 = https with cert errors
    qreal m_loadProgress = 0.0;          // 0..1 estimated load progress (drives the loading badge)
    bool m_renderFailed = false;         // load finished but the page is ~blank (heavy SPA) -> show a notice
    bool m_rendering = false;            // load finished, page compositing on llvmpipe -> show "Rendering…" badge
    bool m_readerMode = false, m_readerable = false;
    bool m_bookmarked = false;   // current page is bookmarked -> filled star
    QString m_addr;
    bool m_editing = false;             // URL-entry mode: the on-screen keyboard is shown over the page
    bool m_editField = false;           // editing a PAGE form field (vs the address bar URL)
    bool m_editMasked = false;          // the field is a password input -> echo '*'
    QString m_editBuf;                  // the URL currently being typed
    std::vector<rmweb::Key> m_keys;     // keyboard layout, rebuilt on page/Shift change (rebuildKeys)
    bool m_kbShift = false;             // one-shot Shift armed (letters page)
    bool m_kbSym = false;               // symbols page ("?123") is showing
    int m_kbPressed = -1;               // key index flashing its pressed state (kbFlush releases it)
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
    void longPress(int x, int y);  // stationary hold (> tapMaxDwellMs) -> link peek
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
        struct dirent *e; char path[320], name[256]; int found = -1;  /* path: "/dev/input/" + d_name(255) + NUL */
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
        const bool editing = g_urlEditing.load(std::memory_order_acquire);
        switch (classifyGesture(dx, dy, dwellMs)) {
        case Gesture::Tap: {
            // Keyboard: short debounce. Normal UI: 250 ms anti-double-tap.
            const gint64 tapDebounceUs = editing ? 40000 : 250000;
            if (m_lastTapUs && now - m_lastTapUs < tapDebounceUs) return;
            // Phantom-touch guard during e-ink refresh — never while typing.
            if (!editing && touchGuarded()) {
                qCDebug(lcEngine, "[touch] dropped (refresh guard)");
                return;
            }
            m_lastTapUs = now;
            qCDebug(lcEngine, "[touch] tap @ %d,%d%s", x, y, editing ? " (kb)" : "");
            Q_EMIT tap(x, y);
            return;
        }
        case Gesture::SwipeUp:
        case Gesture::SwipeDown:
            if (editing) return;                                         // ignore page swipes over keyboard
            if (m_lastSwipeUs && now - m_lastSwipeUs < 800000) return;   // <=1 turn / 0.8 s
            if (touchGuarded()) { qCDebug(lcEngine, "[touch] dropped (refresh guard)"); return; }
            m_lastSwipeUs = now;
            if (dy < 0) { qCDebug(lcEngine, "[touch] swipe up -> next");   Q_EMIT swipe(+1); }
            else        { qCDebug(lcEngine, "[touch] swipe down -> prev"); Q_EMIT swipe(-1); }
            return;
        case Gesture::LongPress:
            if (editing) return;                                         // no peeking while the keyboard is up
            if (m_lastTapUs && now - m_lastTapUs < 250000) return;       // same anti-double as a tap
            m_lastTapUs = now;
            qCDebug(lcEngine, "[touch] long-press @ %d,%d", x, y);
            Q_EMIT longPress(x, y);
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
// EpaperRefresh — manual e-ink panel present, DIAGNOSTIC ONLY (enabled by RMWEB_MANUAL_PRESENT; see
// main()). Calling EPFramebuffer::swapBuffers ourselves from QQuickWindow::afterRendering re-enters the
// framebuffer mutex the epaper QPA's EPRenderLoop holds across renderSceneGraph -> self-deadlock, so by
// default EPRenderLoop drives the panel and this class stays unused. Kept for cadence diagnosis:
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
        // The project's only hand-rolled refresh policy lives right here: rate-limit presents (~150 ms)
        // + full colour flash every m_fullEvery presents. The never-wired refreshpolicy.h module was
        // deleted — there is no other implementation to look for.
        // e-ink physically can't refresh faster than ~6 Hz; with llvmpipe the engine can emit frames far
        // faster, so rate-limit panel presents to protect the controller and avoid ghosting/flicker.
        const gint64 now = g_get_monotonic_time();
        if (m_lastPresentUs && (now - m_lastPresentUs) < 150000) return;   // >= ~150 ms between presents
        m_lastPresentUs = now;
        const QRect full(0, 0, kPanelW, kPanelH);
        ++m_frames;
        const bool isFull = (m_fullEvery > 0 && (m_frames % m_fullEvery) == 0);
        qCDebug(lcEngine, "[present] #%d swap enter full=%d", m_frames, isFull);
        if (isFull) m_swap(m_fb, full, 1, 4, 1);  // colour + anti-ghost flash
        else        m_swap(m_fb, full, 0, 1, 0);  // fast grayscale, no flash
        qCDebug(lcEngine, "[present] #%d swap done", m_frames);
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
    // Prime the libgcc unwinder BEFORE the handler can fire: the first backtrace() allocates, and
    // doing that inside the handler would deadlock a crash-from-malloc (prof_preload.c pattern).
    { void *tmp[4]; backtrace(tmp, 4); }
    // sigaction, all four fatal signals: SIGBUS (SHM buffers) and SIGILL (llvmpipe JITs code on the
    // CPU) are as real as SEGV/ABRT here. The handler itself restores SIG_DFL + re-raises, so the
    // watchdog still receives the signal exactly as before.
    struct sigaction sa = {};
    sa.sa_handler = crashHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    // Timed kills (dev runner, systemd) skip the crash-prone WebKit teardown entirely (termHandler).
    struct sigaction st = {};
    st.sa_handler = termHandler;
    sigemptyset(&st.sa_mask);
    sigaction(SIGTERM, &st, nullptr);
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
        // --- save mode (headless proof): write the 2nd painted frame, then exit ---
        QObject::connect(&engine, &WpeEngine::frameReady, &app,
                         [savePath, saved = false](const QImage &img, int frame) mutable {
            qInfo() << "[qt] frameReady" << frame << img.size();
            if (frame >= 2 && !saved) {
                saved = true;
                if (img.save(savePath)) qInfo() << "[qt] saved" << savePath;
                else                    qWarning() << "[qt] QImage::save FAILED" << savePath;
                // std::_Exit skips the WebKit teardown SIGABRT (watchdog-safe) — same as the ⏻ path.
                fflush(nullptr);
                std::_Exit(0);
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
            qCDebug(lcEngine, "[t][gui] frame %d -> setImage %.1fms  %dx%d", frame,
                  (g_get_monotonic_time() - t) / 1000.0, img.width(), img.height());
        });
        // Engine state -> the C++ chrome painted into the frame (queued worker->GUI).
        QObject::connect(&engine, &WpeEngine::canGoBack,      view, &WpeView::setCanBack);
        QObject::connect(&engine, &WpeEngine::canGoForward,   view, &WpeView::setCanFwd);
        QObject::connect(&engine, &WpeEngine::loadingChanged, view,
                         [view](bool on) {
            if (on) view->forceNextContent();   // navigation start: paint first frames without throttle
            view->setLoading(on);
        }, Qt::QueuedConnection);
        QObject::connect(&engine, &WpeEngine::loadProgressChanged, view, &WpeView::setLoadProgress);
        QObject::connect(&engine, &WpeEngine::urlChanged,     view, &WpeView::setAddr);
        QObject::connect(&engine, &WpeEngine::readerModeChanged, view, &WpeView::setReaderMode);
        QObject::connect(&engine, &WpeEngine::readerableChanged, view, &WpeView::setReaderable);
        QObject::connect(&engine, &WpeEngine::bookmarkedChanged, view,
                         [view](bool on){ view->setBookmarked(on); }, Qt::QueuedConnection);
        QObject::connect(&engine, &WpeEngine::renderFailed,      view, &WpeView::setRenderFailed);
        QObject::connect(&engine, &WpeEngine::tlsStateChanged,   view, &WpeView::setTlsState);
        QObject::connect(&engine, &WpeEngine::readProgressChanged, view, &WpeView::setReadProgress);
        QObject::connect(&engine, &WpeEngine::urlChanged, view,   // a new page resets the bar until
                         [view]{ view->setReadProgress(-1); });   // the first scroll/restore answers
        QObject::connect(&engine, &WpeEngine::renderingChanged, view,
                         [view](bool on){ view->setRendering(on); }, Qt::QueuedConnection);
        // URL entry: the on-screen keyboard's Go (WpeView::urlEntered) -> load it (engine.loadUrl
        // normalizes). "/text" goes to the in-page find instead (repeat "/text" = next match).
        // Anything that isn't a URL (spaces, no dot) becomes an address-bar SEARCH (local
        // bookmarks+history results page with a web-search link on top).
        QObject::connect(view, &WpeView::urlEntered, &app, [&engine, view](const QString &u){
            if (u.startsWith(QLatin1Char('/')) && u.size() > 1) {
                view->forceNextContent();   // the find scroll/highlight must paint promptly
                engine.findText(u.mid(1));
            } else if (rmweb::looksLikeUrl(u.toStdString())) {
                engine.loadUrl(u);
            } else {
                view->forceNextContent();   // the results page must paint promptly
                engine.searchAndShow(u);
                view->setAddr(u);           // keep the typed query visible (not "about:blank")
                view->setReadProgress(-1);  // generated page: no scroll metrics -> hide the bar
            }
        });
        // Engine toasts (find results, downloads) -> the chrome overlay.
        QObject::connect(&engine, &WpeEngine::notice, view, &WpeView::setNotice, Qt::QueuedConnection);
        // Form fields: a tapped text field opens the keyboard on its current value (or an autofill
        // prefill when the field is empty); Go commits the typed text into the page field (native
        // setter + input/change events) and learns it for future prefills.
        QObject::connect(&engine, &WpeEngine::fieldFocused, view,
                         [view](const QString &v, bool masked, const QString &s){ view->beginFieldEdit(v, masked, s); },
                         Qt::QueuedConnection);
        QObject::connect(view, &WpeView::fieldTextEntered, &app, [&engine, view](const QString &t){
            view->forceNextContent();   // the DOM edit must paint promptly
            engine.setFieldText(t);
            engine.learnFieldText(t);
        });

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
        QObject::connect(&touchReader, &TouchReader::swipe, &app, [&engine, view](int dir) {
            view->forceNextContent();   // page-turn frame must paint immediately (bypass SPA throttle)
            if (dir > 0) engine.pageNext(); else engine.pagePrev();
        });
        // Reader-first tap routing (queued worker->GUI). The chrome is painted INTO the frame (B2), so we
        // hit-test it in C++: a tap on the bar runs its button; a tap on the page toggles chrome (hide when
        // shown -> read fullscreen, summon when hidden); with chrome hidden the tap-zones (tapzone.h) turn
        // pages at the edges. tap(x,y) is in panel px.
        QObject::connect(&touchReader, &TouchReader::tap, win ? win : qobject_cast<QObject*>(&app),
            [&engine, view](int x, int y) {
                if (view->isEditing()) { view->handleEditTap(x, y); return; }   // keyboard captures all taps
                const WpeView::Hit ch = view->hitChrome(x, y);
                view->pressChrome(ch);   // instant inverted flash on the tapped button (ignored for None/Address)
                switch (ch) {
                    case WpeView::Back:    view->forceNextContent(); engine.goBack();    return;
                    case WpeView::Fwd:     view->forceNextContent(); engine.goForward(); return;
                    case WpeView::Reload:  view->forceNextContent();
                        view->isLoading() ? engine.stopLoading() : engine.reload(); return;
                    case WpeView::Home:    view->forceNextContent(); engine.goHome();     return;
                    case WpeView::Reader:  view->forceNextContent(); engine.toggleReader(); return;
                    case WpeView::ZoomOut: view->forceNextContent(); engine.zoomBy(-1);   return;
                    case WpeView::ZoomIn:  view->forceNextContent(); engine.zoomBy(+1);   return;
                    case WpeView::Address: view->beginEdit();  return;   // open the on-screen URL keyboard
                    case WpeView::Bookmark: engine.toggleBookmark(); return;
                    case WpeView::Power:
                        // Single tap exits. std::_Exit skips WebKit teardown SIGABRT (watchdog-safe).
                        qInfo("[exit] power — leaving");
                        std::_Exit(0);
                    case WpeView::None:    break;             // tap not on the bar
                    default:              break;
                }
                // The "Loading NN%" pill carries its own abort X — same engine path as the toolbar Stop.
                if (view->hitLoadingStop(x, y)) {
                    qInfo("[nav] stop via badge");
                    view->forceNextContent();
                    engine.stopLoading();
                    return;
                }
                // Page-turn zones: when chrome is HIDDEN use left/right edges (reading mode).
                // When chrome is SHOWN, edges would steal link taps — only swipe pages then.
                // Swipe always pages (connected above).
                if (!view->chromeOn()) {
                    rmweb::TapZones z;
                    z.edgeFrac = 0.15;   // 15% edges (was 22% — too greedy, ate link taps)
                    const auto a = rmweb::classifyTap(x, y, kPanelW, kPanelH, z);
                    if (a == rmweb::TapAction::Next) {
                        view->forceNextContent(); engine.pageNext(); return;
                    }
                    if (a == rmweb::TapAction::Prev) {
                        view->forceNextContent(); engine.pagePrev(); return;
                    }
                    if (a == rmweb::TapAction::SummonChrome) {
                        view->setChromeOn(true);
                        return;
                    }
                }
                // Content / chrome-visible: try link (or interactive control) at the point.
                view->forceNextContent();
                engine.tapLink(x, y);
            }, Qt::QueuedConnection);
        // A content tap with no link underneath -> the old behaviour: toggle the chrome (show <-> hide).
        QObject::connect(&engine, &WpeEngine::linkMissed, win ? win : qobject_cast<QObject*>(&app),
            [view]{ view->setChromeOn(!view->chromeOn()); }, Qt::QueuedConnection);
        // Long-press on a link -> toast its target URL without navigating (peek, read-only probe).
        // Long-press on the CHROME is not a content peek — hit-test first (same as the tap path).
        QObject::connect(&touchReader, &TouchReader::longPress, win ? win : qobject_cast<QObject*>(&app),
            [&engine, view](int x, int y){
                if (view->hitChrome(x, y) != WpeView::None) return;
                engine.peekLink(x, y);
            }, Qt::QueuedConnection);
        touchThread.start();

        // DIAG: GUI event-loop heartbeat. If these "[gui] tick" lines stop, the GUI thread is blocked
        // (e.g. inside present()/swapBuffers) and queued frameReady deliveries stall -> content never paints.
        // Debug-category (off by default — 2 s writes forever would wear the flash log): enable with
        // QT_LOGGING_RULES=rmweb.engine.debug=true when chasing a stall.
        { auto *hb = new QTimer(&app);
          QObject::connect(hb, &QTimer::timeout, &app, []{ qCDebug(lcEngine, "[gui] tick"); });
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

        // DIAG (RMWEB_DEBUG_FIND=term): run an in-page find once after 6 s — verifies the
        // FindController path without typing; watch for "[find] matches=N" in the log.
        if (qEnvironmentVariableIsSet("RMWEB_DEBUG_FIND")) {
            const QString term = qEnvironmentVariable("RMWEB_DEBUG_FIND");
            if (!term.isEmpty())
                QTimer::singleShot(6000, &app, [&engine, term]{
                    qInfo("[find][dbg] term=%s", qPrintable(term));
                    engine.findText(term);
                });
        }

        // DIAG (RMWEB_DEBUG_SEARCH=words): run the address-bar search once after 4 s — shows the
        // generated results page (local matches + web-search link) without typing. Pair with RMWEB_GRAB_MS.
        if (qEnvironmentVariableIsSet("RMWEB_DEBUG_SEARCH")) {
            const QString term = qEnvironmentVariable("RMWEB_DEBUG_SEARCH");
            if (!term.isEmpty())
                QTimer::singleShot(4000, &app, [&engine, view, term]{
                    qInfo("[search][dbg] term=%s", qPrintable(term));
                    engine.searchAndShow(term);
                    view->setAddr(term);           // mirror the urlEntered path (query, not the start URL)
                    view->setReadProgress(-1);
                });
        }

        // DIAG (RMWEB_DEBUG_PROBE="x,y"): run the content tap probe at panel (x,y) once after 4 s —
        // exercises the field/select/checkbox/link classifier without a finger (a field opens the
        // keyboard, a tick shows a toast; watch "[link] probe hit=N" in the log).
        // DIAG (RMWEB_DEBUG_FORM="x,y,text"): the same probe, then commit "text" into the focused
        // field at 7 s via the exact setFieldText path the keyboard's Go uses. Pair with RMWEB_GRAB_MS.
        if (qEnvironmentVariableIsSet("RMWEB_DEBUG_PROBE") || qEnvironmentVariableIsSet("RMWEB_DEBUG_FORM")) {
            const QString spec = qEnvironmentVariable("RMWEB_DEBUG_FORM").isEmpty()
                               ? qEnvironmentVariable("RMWEB_DEBUG_PROBE")
                               : qEnvironmentVariable("RMWEB_DEBUG_FORM");
            const int c1 = spec.indexOf(QLatin1Char(',')), c2 = spec.indexOf(QLatin1Char(','), c1 + 1);
            const int px = spec.left(c1).toInt();
            const int py = (c1 > 0 && c2 > 0 ? spec.mid(c1 + 1, c2 - c1 - 1) : spec.mid(c1 + 1)).toInt();
            const QString ftext = (c2 > 0) ? spec.mid(c2 + 1) : QString();
            if (c1 > 0) {
                QTimer::singleShot(4000, &app, [&engine, px, py]{
                    qInfo("[form][dbg] probe @ %d,%d", px, py);
                    engine.tapLink(px, py);
                });
                if (!ftext.isEmpty()) {
                    QTimer::singleShot(7000, &app, [&engine, view, ftext]{
                        qInfo("[form][dbg] commit: %s", qPrintable(ftext));
                        view->forceNextContent();   // mirror the real Go path so the edit frame paints
                        engine.setFieldText(ftext);
                        engine.learnFieldText(ftext);   // mirror the fieldTextEntered wire (autofill learn)
                    });
                    QTimer::singleShot(8500, &app, [&engine]{ engine.logFieldState(); });
                }
            } else qWarning("[form][dbg] bad spec (want x,y[,text]): %s", qPrintable(spec));
        }

        // DIAG (RMWEB_DEBUG_UITAP="x,y"): emit a synthetic ROUTER tap at panel (x,y) once after 5 s —
        // exercises the full touch route (chrome hit-test, loading-badge stop, page zones), unlike
        // RMWEB_DEBUG_PROBE which goes straight to the content probe.
        if (qEnvironmentVariableIsSet("RMWEB_DEBUG_UITAP")) {
            const QString spec = qEnvironmentVariable("RMWEB_DEBUG_UITAP");
            const int c1 = spec.indexOf(QLatin1Char(','));
            const int px = spec.left(c1).toInt(), py = (c1 > 0 ? spec.mid(c1 + 1) : QString()).toInt();
            if (c1 > 0) QTimer::singleShot(5000, &app, [&touchReader, px, py]{
                qInfo("[uitap][dbg] tap @ %d,%d", px, py);
                Q_EMIT touchReader.tap(px, py);
            });
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
