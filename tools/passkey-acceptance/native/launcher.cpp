// Test-only launcher. Real AuthEngine, network session, provider, helper and input.
// Production auth parsing and executables are neither changed nor replaced.
#define main unusedProductionAuthMain
#include "../../../engine/wpeqt/auth-main.cpp"
#undef main
#include "policy.h"
#include "origin.h"

namespace {
const QByteArray TestOrigin(ACCEPTANCE_ORIGIN);
std::atomic<bool> TestActivated{false};
struct AuthEngineTestAccess {
#ifdef ACCEPTANCE_ENGINE_FIXTURE
    static bool navigateFixture(AuthEngine &engine, const QByteArray &uri) {
        return engine.post([&engine, uri] { webkit_web_view_load_uri(engine.m_webView, uri.constData()); });
    }
#endif
    static gboolean policy(WebKitWebView *, WebKitPolicyDecision *decision,
                           WebKitPolicyDecisionType type, gpointer data) {
        auto *engine = static_cast<AuthEngine *>(data);
        bool allowed = false;
        if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
            auto *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
            const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
            allowed = uri && acceptance::allowed(TestOrigin, uri);
        } else if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
            auto *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
            const char *uri = webkit_uri_request_get_uri(webkit_response_policy_decision_get_request(response));
            allowed = uri && acceptance::allowed(TestOrigin, uri)
                && webkit_response_policy_decision_is_mime_type_supported(response);
        }
        if (allowed) return FALSE;
        webkit_policy_decision_ignore(decision);
        engine->fail();
        return TRUE;
    }
    static bool activate(AuthEngine &engine, const QByteArray &launchUrl = TestOrigin + "/verify") {
        if (TestActivated.load()) return true;
        if (!engine.m_ready.load()) return false;
        engine.post([&engine, launchUrl] {
            if (webkit_web_view_is_loading(engine.m_webView) || TestActivated.load()) return;
            // Complete the inert initial load before installing the stricter
            // policy; an old blank decision must not poison the HTTPS page.
            g_signal_handlers_disconnect_by_func(engine.m_webView, reinterpret_cast<gpointer>(AuthEngine::policy), &engine);
            g_signal_connect(engine.m_webView, "decide-policy", G_CALLBACK(policy), &engine);
            TestActivated.store(true);
            webkit_web_view_load_uri(engine.m_webView, launchUrl.constData());
        });
        return false;
    }
};
}

#ifndef ACCEPTANCE_MAIN
#define ACCEPTANCE_MAIN main
#endif
int ACCEPTANCE_MAIN(int argc, char **argv) {
    if (!privateProcess()) return 2;
    if (argc != 2 || !acceptance::allowed(TestOrigin, argv[1]) || qEnvironmentVariableIsEmpty("QTFB_KEY")) return 2;
    const QByteArray launchUrl(argv[1]);
    const std::optional<rmweb::AuthLaunch> launch = rmweb::AuthLaunch{QUrl(QStringLiteral("about:blank")), {}, {}, {}};
    std::signal(SIGTERM, [](int) { _exit(0); });
    std::signal(SIGINT, [](int) { _exit(0); });
    QCoreApplication app(argc, argv);
    rmweb::QtfbClient client;
    rmweb::AuthSurface surface;
    rmweb::AuthPasskey passkeys;
    AuthEngine engine(*launch, surface.contentSize(), &passkeys);
    surface.setOrigin(QUrl::fromEncoded(TestOrigin).host());
    surface.setDeviceCode(launch->deviceCode);
    bool closing = false;
    auto close = [&client, &passkeys, &closing](int code) {
        if (closing) return;
        closing = true;
        passkeys.shutdown();
        client.close();
        // Avoid running WebKit teardown on Qt's thread. The private network
        // session and its helpers terminate with this process/IPC connection.
        std::_Exit(code);
    };
    QObject::connect(&client, &rmweb::QtfbClient::initialized, &app, [&](QSize) {
        client.submitImage(surface.image());
        engine.start();
    });
    bool paintPending = false;
    QObject::connect(&surface, &rmweb::AuthSurface::repaintRequested, &app, [&] {
        if (paintPending) return;
        paintPending = true;
        QTimer::singleShot(0, &app, [&] {
            paintPending = false;
            if (client.isReady()) client.submitImage(surface.image());
        });
    });
    QObject::connect(&surface, &rmweb::AuthSurface::closeRequested, &app, [&] { close(0); });
    QObject::connect(&passkeys, &rmweb::AuthPasskey::promptChanged, &surface, &rmweb::AuthSurface::setPasskeyPrompt);
    QObject::connect(&passkeys, &rmweb::AuthPasskey::completed, &app,
        [&](quint64 id, bool success, rmweb::AuthPasskeyAssertion assertion) {
            if (!closing && !engine.completePasskey(id, success, std::move(assertion))) close(2);
        });
    QObject::connect(&surface, &rmweb::AuthSurface::passkeyCancelRequested, &app, [&] {
        if (!engine.cancelPasskey()) close(2);
    });
    QObject::connect(&client, &rmweb::QtfbClient::connectionClosed, &app, [&] { close(0); });
    QObject::connect(&client, &rmweb::QtfbClient::error, &app, [&](const QString &) { close(2); });
    QObject::connect(&client, &rmweb::QtfbClient::rotationChanged, &surface, [&] {
        surface.cancelTouches();
        if (!engine.cancel()) close(2);
    });
    QObject::connect(&client, &rmweb::QtfbClient::touchPressed, &surface, &rmweb::AuthSurface::press);
    QObject::connect(&client, &rmweb::QtfbClient::touchMoved, &surface, &rmweb::AuthSurface::move);
    QObject::connect(&client, &rmweb::QtfbClient::touchReleased, &surface, &rmweb::AuthSurface::release);
    QObject::connect(&client, &rmweb::QtfbClient::touchesCancelled, &surface, &rmweb::AuthSurface::cancelTouches);
    QObject::connect(&client, &rmweb::QtfbClient::penPressed, &surface, &rmweb::AuthSurface::penPress);
    QObject::connect(&client, &rmweb::QtfbClient::penMoved, &surface, &rmweb::AuthSurface::penMove);
    QObject::connect(&client, &rmweb::QtfbClient::penReleased, &surface, &rmweb::AuthSurface::penRelease);
    quint64 displayedGeneration = 0;
    QObject::connect(&surface, &rmweb::AuthSurface::touchPressed, &app, [&](int id, int x, int y) {
        if (!engine.touch(WPE_EVENT_TOUCH_DOWN, id, x, y, displayedGeneration)) close(2);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchMoved, &app, [&](int id, int x, int y) {
        if (!engine.touch(WPE_EVENT_TOUCH_MOVE, id, x, y, displayedGeneration)) close(2);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchReleased, &app, [&](int id, int x, int y) {
        if (!engine.touch(WPE_EVENT_TOUCH_UP, id, x, y, displayedGeneration)) close(2);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchesCancelled, &app, [&] { if (!engine.cancel()) close(2); });
    QObject::connect(&client, &rmweb::QtfbClient::keyEvent, &app, [&](int raw, bool pressed) {
        if ((!pressed || surface.acceptsKeys()) && !engine.key(raw, pressed, displayedGeneration)) close(2);
    });
    QTimer activation;
    activation.setInterval(25);
    QObject::connect(&activation, &QTimer::timeout, &app, [&] {
        if (AuthEngineTestAccess::activate(engine, launchUrl)) activation.stop();
    });
    activation.start();
    QTimer::singleShot(15000, &app, [&] { if (activation.isActive()) close(2); });
    QTimer::singleShot(30 * 60 * 1000, &app, [&] { close(0); });
    QTimer networkPaint;
    networkPaint.setInterval(750);
    QObject::connect(&networkPaint, &QTimer::timeout, &app, [&] {
        const auto snapshot = engine.takeSnapshot();
        displayedGeneration = snapshot.generation;
        surface.beginNavigation(displayedGeneration);
        surface.setOrigin(snapshot.host.isEmpty() ? QUrl::fromEncoded(TestOrigin).host() : snapshot.host);
        surface.setLoading(snapshot.loading);
        surface.setFailed(snapshot.failed);
        surface.setCallbackReached(snapshot.callback);
        if (!snapshot.frame.isNull()) surface.setFrame(snapshot.frame);
    });
    networkPaint.start();
    if (!client.startFromEnvironment()) return 2;
    const int result = app.exec();
    close(result);
    return result;
}
