// Actual AuthEngine callbacks, not a parallel policy implementation. All pages
// and form-free scripts below are synthetic except the opt-in public-device case.
#define main unusedAuthBrowserMain
#include "../engine/wpeqt/auth-main.cpp"
#undef main
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {
struct AuthEngineTestAccess {
    static bool ready(AuthEngine &engine) { return engine.m_ready.load(); }
    static bool post(AuthEngine &engine, std::function<void()> fn) { return engine.post(std::move(fn)); }
    static WebKitWebView *webView(AuthEngine &engine) { return engine.m_webView; }
    static GMainContext *context(AuthEngine &engine) { return engine.m_context; }
    static void fail(AuthEngine &engine) { engine.fail(); }
    static void pointerControl(AuthEngine &engine, bool down, int x, int y) {
        engine.post([&engine, down, x, y] {
            engine.deliver(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, engine.view(), WPE_INPUT_SOURCE_MOUSE,
                AuthEngine::time(), WPEModifiers(0), x / Scale, y / Scale, 0, 0));
            engine.deliver(wpe_event_pointer_button_new(down ? WPE_EVENT_POINTER_DOWN : WPE_EVENT_POINTER_UP,
                engine.view(), WPE_INPUT_SOURCE_MOUSE, AuthEngine::time(), down ? WPE_MODIFIER_POINTER_BUTTON1 : WPEModifiers(0),
                WPE_BUTTON_PRIMARY, x / Scale, y / Scale, down ? 1 : 0));
        });
    }
    static void fixturePolicy(AuthEngine &engine, bool enable) {
        auto function = reinterpret_cast<gpointer>(AuthEngine::policy);
        if (enable) g_signal_handlers_unblock_matched(engine.m_webView, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, function, nullptr);
        else g_signal_handlers_block_matched(engine.m_webView, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, function, nullptr);
    }
};
struct Observations {
    std::atomic<int> blank{0}, srcdoc{0}, other{0}, popup{0}, cancellation{0}, errors{0}, topBlank{0}, topSrcdoc{0}, commit{0}, tlsFlags{0}, errorCode{0};
    std::atomic<bool> armed{false}, started{false}, popupBlocked{false};
    std::atomic<int> tapResult{-1}, touchStarts{0}, pointerDowns{0}, clicks{0};
    std::atomic<bool> trustedClick{false}, fieldFocused{false}, fragmentReached{false}, scrolled{false};
};
Observations observations;
int reportFd = -1;
const char *scenario;
AuthEngine *engine;
unsigned stallPort = 0;
QString previousHost;

// Only fixed case names and booleans leave this process. Helpers cannot inherit
// the report FD; production privateProcess silences all WebKit/GLib output.
[[noreturn]] void finish(bool passed) {
    dprintf(reportFd, "%s: %s\n", scenario, passed ? "PASS" : "FAIL");
    if (!passed && engine) {
        const auto state = engine->takeSnapshot();
        dprintf(reportFd, "failed=%d loading=%d blank=%d srcdoc=%d other=%d errors=%d errorCode=%d tlsFlags=%d commit=%d\n",
            int(state.failed), int(state.loading), observations.blank.load(), observations.srcdoc.load(), observations.other.load(),
            observations.errors.load(), observations.errorCode.load(), observations.tlsFlags.load(), observations.commit.load());
    }
    _Exit(passed ? 0 : 1);
}
bool named(const char *name) { return !strcmp(scenario, name); }
bool inputCase() { return named("touch-link") || named("touch-focus") || named("pointer-link") || named("pen-link") || named("touch-drag") || named("touch-cancel"); }
void inspectSyntheticInput() {
    AuthEngineTestAccess::post(*engine, [] {
        constexpr auto script = "JSON.stringify([window.touchStarts||0,window.pointerDowns||0,window.clicks||0,"
            "window.trustedClick===true,document.activeElement===document.getElementById('field'),location.hash==='#tapped',window.scrollY>0])";
        webkit_web_view_evaluate_javascript(AuthEngineTestAccess::webView(*engine), script, -1, nullptr, nullptr, nullptr,
            [](GObject *source, GAsyncResult *result, gpointer) {
                GError *error = nullptr;
                JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
                if (error || !value) { g_clear_error(&error); if (value) g_object_unref(value); observations.tapResult = 0; return; }
                char *json = jsc_value_to_string(value);
                const QJsonArray values = QJsonDocument::fromJson(json ? QByteArray(json) : QByteArray()).array();
                g_free(json); g_object_unref(value);
                if (values.size() != 7) { observations.tapResult = 0; return; }
                observations.touchStarts = values[0].toInt(); observations.pointerDowns = values[1].toInt();
                observations.clicks = values[2].toInt(); observations.trustedClick = values[3].toBool();
                observations.fieldFocused = values[4].toBool(); observations.fragmentReached = values[5].toBool();
                observations.scrolled = values[6].toBool();
                observations.tapResult = named("touch-drag") ? values[2].toInt() == 0 && values[6].toBool()
                    : named("touch-cancel") ? values[2].toInt() == 0 && !values[5].toBool()
                    : values[2].toInt() == 1 && values[3].toBool()
                        && (named("touch-focus") ? values[4].toBool() : values[5].toBool());
            }, nullptr);
    });
}
void later(guint milliseconds, std::function<void()> fn) {
    auto *work = new std::function<void()>(std::move(fn));
    GSource *source = g_timeout_source_new(milliseconds);
    g_source_set_callback(source, [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()> *>(data))();
        return G_SOURCE_REMOVE;
    }, work, [](gpointer data) { delete static_cast<std::function<void()> *>(data); });
    g_source_attach(source, AuthEngineTestAccess::context(*engine));
    g_source_unref(source);
}
gboolean observe(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer) {
    if (!observations.armed.load()) return TRUE;
    const char *name = g_signal_name(hint->signal_id);
    if (!strcmp(name, "load-failed") && count >= 4) {
        const auto *error = static_cast<const GError *>(g_value_get_boxed(&values[3]));
        if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) ++observations.cancellation;
        else { ++observations.errors; observations.errorCode = error ? error->code : 0; }
    } else if (!strcmp(name, "load-failed-with-tls-errors") || !strcmp(name, "web-process-terminated")) {
        ++observations.errors;
        if (!strcmp(name, "load-failed-with-tls-errors") && count >= 4) observations.tlsFlags = g_value_get_flags(&values[3]);
    } else if (!strcmp(name, "decide-policy") && count >= 3) {
        const auto type = WebKitPolicyDecisionType(g_value_get_enum(&values[2]));
        if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) ++observations.popup;
        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
            auto *decision = WEBKIT_NAVIGATION_POLICY_DECISION(g_value_get_object(&values[1]));
            auto *action = webkit_navigation_policy_decision_get_navigation_action(decision);
            const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
            const QByteArray encoded(uri ? uri : "");
            if (encoded == "about:blank") ++observations.blank;
            else if (encoded == "about:srcdoc") ++observations.srcdoc;
            else if (!encoded.startsWith("https:")) ++observations.other;
        }
    } else if (!strcmp(name, "load-changed") && count >= 2 && g_value_get_enum(&values[1]) == WEBKIT_LOAD_COMMITTED) {
        ++observations.commit;
        auto *view = WEBKIT_WEB_VIEW(g_value_get_object(&values[0]));
        const char *uri = webkit_web_view_get_uri(view);
        if (uri && !strcmp(uri, "about:blank")) ++observations.topBlank;
        if (uri && !strcmp(uri, "about:srcdoc")) ++observations.topSrcdoc;
    }
    return TRUE;
}
void fixtureFinished(WebKitWebView *view, WebKitLoadEvent event, gpointer) {
    static bool started = false;
    if (event == WEBKIT_LOAD_STARTED) started = true;
    if (event != WEBKIT_LOAD_FINISHED || !started || observations.started.exchange(true)) return;
    later(200, [view] {
        if (!inputCase()) previousHost = engine->takeSnapshot().host;
        AuthEngineTestAccess::fixturePolicy(*engine, true);
        observations.armed = true;
        if (inputCase()) return; // Qt applies the real snapshot before sending native input.
        if (named("child-blank") || named("child-srcdoc")) {
            const char *script = named("child-blank") ?
                "let f=document.createElement('iframe');f.src='about:blank';document.body.append(f)" :
                "let f=document.createElement('iframe');f.srcdoc='<p>synthetic frame</p>';document.body.append(f)";
            webkit_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        } else if (named("top-blank")) webkit_web_view_load_uri(view, "about:blank");
        else if (named("top-srcdoc")) webkit_web_view_load_alternate_html(view,
            "<!doctype html><p>Synthetic top-level internal document</p>", "about:srcdoc", nullptr);
        else if (named("data")) webkit_web_view_load_uri(view, "data:text/html,synthetic");
        else if (named("file")) webkit_web_view_load_uri(view, "file:///synthetic-never-existing");
        else if (named("blob")) {
            webkit_web_view_evaluate_javascript(view,
                "location=URL.createObjectURL(new Blob(['synthetic'],{type:'text/html'}))", -1, nullptr, nullptr, nullptr, nullptr, nullptr);
        } else if (named("popup")) {
            // Inspect only the return value of our synthetic fixture expression,
            // never a real page or field. Keep production window settings intact.
            webkit_web_view_evaluate_javascript(view, "window.open('https://fixture.example.test/popup') === null", -1,
                nullptr, nullptr, nullptr, [](GObject *source, GAsyncResult *result, gpointer) {
                    GError *error = nullptr;
                    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
                    observations.popupBlocked = !error && value && jsc_value_is_boolean(value) && jsc_value_to_boolean(value);
                    if (value) g_object_unref(value);
                    g_clear_error(&error);
                }, nullptr);
        } else {
            const QByteArray url = "https://127.0.0.1:" + QByteArray::number(stallPort) + "/synthetic";
            webkit_web_view_load_uri(view, url.constData());
            later(500, [view] {
                if (named("cancel-after-failure")) AuthEngineTestAccess::fail(*engine);
                webkit_web_view_stop_loading(view);
            });
        }
    });
}
void startStalledEndpoint() {
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 4)) finish(false);
    socklen_t length = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length)) finish(false);
    stallPort = ntohs(address.sin_port);
    // Bounded local TLS handshake stall; process exit closes the accepted FDs.
    std::thread([listener] {
        for (int i = 0; i < 4; ++i) if (accept(listener, nullptr, nullptr) < 0) return;
        close(listener);
    }).detach();
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    scenario = argv[1];
    if (!named("public-device") && !named("child-blank") && !named("child-srcdoc") && !named("top-blank") && !named("top-srcdoc")
        && !named("data") && !named("blob") && !named("file") && !named("popup")
        && !named("cancel") && !named("cancel-after-failure") && !inputCase()) return 2;
    reportFd = dup(STDOUT_FILENO);
    if (reportFd < 0 || fcntl(reportFd, F_SETFD, FD_CLOEXEC) < 0 || !privateProcess()) return 2;
    QCoreApplication app(argc, argv);
    const auto launch = rmweb::parseAuthLaunch("https://auth.openai.com/codex/device");
    if (!launch) finish(false);
    AuthEngine instance(*launch, QSize(1620, 2000));
    engine = &instance;
    rmweb::AuthSurface surface;
    quint64 displayedGeneration = 0;
    QObject::connect(&surface, &rmweb::AuthSurface::touchPressed, &app, [&](int id, int x, int y) {
        if (!engine->touch(WPE_EVENT_TOUCH_DOWN, id, x, y, displayedGeneration)) finish(false);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchMoved, &app, [&](int id, int x, int y) {
        if (!engine->touch(WPE_EVENT_TOUCH_MOVE, id, x, y, displayedGeneration)) finish(false);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchReleased, &app, [&](int id, int x, int y) {
        if (!engine->touch(WPE_EVENT_TOUCH_UP, id, x, y, displayedGeneration)) finish(false);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchesCancelled, &app, [&] {
        if (!engine->cancel()) finish(false);
    });
    if (named("cancel") || named("cancel-after-failure")) startStalledEndpoint();
    g_type_class_ref(WEBKIT_TYPE_WEB_VIEW);
    for (const char *signal : {"load-failed", "load-failed-with-tls-errors", "web-process-terminated", "decide-policy", "load-changed"}) {
        const guint id = g_signal_lookup(signal, WEBKIT_TYPE_WEB_VIEW);
        if (!id || !g_signal_add_emission_hook(id, 0, observe, nullptr, nullptr)) finish(false);
    }
    const bool publicMode = named("public-device");
    observations.armed = publicMode;
    engine->start();
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    bool bootstrapped = false;
    qint64 armedAt = -1;
    bool hadFrame = false;
    bool tapSent = false;
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (!publicMode && !bootstrapped && AuthEngineTestAccess::ready(*engine)) {
            bootstrapped = true;
            if (!AuthEngineTestAccess::post(*engine, [&] {
                AuthEngineTestAccess::fixturePolicy(*engine, false);
                auto *view = AuthEngineTestAccess::webView(*engine);
                webkit_web_view_stop_loading(view);
                g_signal_connect(view, "load-changed", G_CALLBACK(fixtureFinished), nullptr);
                constexpr auto inputHtml = "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<style>body{margin:0;min-height:4000px}a,input{position:absolute;left:100px;width:240px;height:80px;}"
                    "a{display:block;top:100px;background:#ddd}input{top:260px;box-sizing:border-box}</style>"
                    "<a id='link' href='#tapped'>Synthetic link</a><input id='field'>"
                    "<script>addEventListener('touchstart',()=>window.touchStarts=(window.touchStarts||0)+1);"
                    "addEventListener('pointerdown',()=>window.pointerDowns=(window.pointerDowns||0)+1);"
                    "addEventListener('click',e=>{window.clicks=(window.clicks||0)+1;window.trustedClick=e.isTrusted})</script>";
                const char *html = inputCase() ? inputHtml : "<!doctype html><html><body><p>Synthetic page</p></body></html>";
                webkit_web_view_load_alternate_html(view, html, "https://fixture.example.test/", "https://fixture.example.test/");
            })) finish(false);
        }
        if (observations.armed.load() && armedAt < 0) armedAt = elapsed.elapsed();
        const auto snapshot = engine->takeSnapshot();
        if (inputCase()) {
            displayedGeneration = snapshot.generation;
            surface.beginNavigation(displayedGeneration);
            surface.setOrigin(snapshot.host);
            surface.setLoading(snapshot.loading);
            surface.setFailed(snapshot.failed);
            surface.setCallbackReached(snapshot.callback);
            if (!snapshot.frame.isNull()) surface.setFrame(snapshot.frame);
            if (observations.armed.load() && !tapSent && surface.acceptsKeys()) {
                tapSent = true;
                // CSS target centers (220,140)/(220,300), Scale2, header160.
                const int contentY = named("touch-focus") ? 600 : 280;
                if (named("pointer-link")) {
                    AuthEngineTestAccess::pointerControl(*engine, true, 440, contentY);
                    QTimer::singleShot(80, &app, [contentY] { AuthEngineTestAccess::pointerControl(*engine, false, 440, contentY); });
                } else if (named("pen-link")) {
                    surface.penPress(440, contentY + 160);
                    QTimer::singleShot(80, &app, [&surface, contentY] { surface.penRelease(440, contentY + 160); });
                } else {
                    surface.press(0, 440, contentY + 160);
                    if (named("touch-drag")) {
                        QTimer::singleShot(80, &app, [&surface] { surface.move(0, 440, 360); });
                        QTimer::singleShot(150, &app, [&surface] { surface.move(0, 440, 320); });
                        QTimer::singleShot(230, &app, [&surface] { surface.release(0, 440, 320); });
                    } else if (named("touch-cancel")) {
                        QTimer::singleShot(80, &app, [&surface] { surface.cancelTouches(); });
                        QTimer::singleShot(150, &app, [&surface, contentY] { surface.release(0, 440, contentY + 160); });
                    } else QTimer::singleShot(80, &app, [&surface, contentY] { surface.release(0, 440, contentY + 160); });
                }
                QTimer::singleShot(700, &app, inspectSyntheticInput);
            }
        }
        if (observations.armed.load()) hadFrame = hadFrame || !snapshot.frame.isNull();
        if (elapsed.elapsed() > 20000) finish(false);
        if (inputCase()) {
            if (observations.tapResult < 0) return;
            dprintf(reportFd, "touchStarts=%d pointerDowns=%d clicks=%d trustedClick=%d focused=%d navigated=%d scrolled=%d surfaceReady=%d\n",
                observations.touchStarts.load(), observations.pointerDowns.load(), observations.clicks.load(), int(observations.trustedClick.load()),
                int(observations.fieldFocused.load()), int(observations.fragmentReached.load()), int(observations.scrolled.load()), int(surface.acceptsKeys()));
            finish(observations.tapResult == 1);
        }
        if (armedAt < 0 || elapsed.elapsed() - armedAt < (publicMode ? 12000 : 2200)) return;
        if (publicMode) finish(!snapshot.failed && !snapshot.loading && hadFrame && observations.commit > 0 && observations.errors == 0);
        if (named("child-blank")) finish(!snapshot.failed && observations.blank > 0 && snapshot.host == previousHost);
        if (named("top-blank")) finish(snapshot.failed && observations.topBlank > 0);
        if (named("top-srcdoc")) finish(snapshot.failed && observations.topSrcdoc > 0);
        if (named("child-srcdoc")) finish(!snapshot.failed && observations.srcdoc > 0 && snapshot.host == previousHost);
        if (named("popup")) finish(observations.popupBlocked.load());
        if (named("cancel")) finish(!snapshot.failed && observations.cancellation > 0);
        if (named("cancel-after-failure")) finish(snapshot.failed && observations.cancellation > 0);
        finish(snapshot.failed && observations.other > 0);
    });
    poll.start(100);
    return app.exec();
}
