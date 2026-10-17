// An ephemeral authentication window; the calling application owns the OAuth callback.
#include "auth-policy.h"
#include "auth-surface.h"
#include "auth-passkey.h"
#include "qtfbclient.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QPoint>
#include <QTimer>
#include <QJsonDocument>
#include <QMetaObject>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/resource.h>
#include <syslog.h>
#include <unistd.h>

namespace {
constexpr double Scale = 2.0;
// Only this snapshot crosses into the UI: never a complete URI, form value,
// title, script result, error description, cookie, or authentication result.
struct Snapshot {
    QImage frame;
    QString host;
    bool loading = true;
    bool failed = false;
    bool callback = false;
    quint64 generation = 0;
};
class AuthEngine {
    friend struct AuthEngineTestAccess; // Actual-WPE regression fixture; no production instance.
public:
    AuthEngine(rmweb::AuthLaunch launch, QSize size, rmweb::AuthPasskey *passkeys = nullptr)
        : m_launch(std::move(launch)), m_size(size), m_context(g_main_context_new()), m_passkeys(passkeys) {}
    void start() { std::thread([this] { run(); }).detach(); }
    Snapshot takeSnapshot() {
        std::lock_guard<std::mutex> guard(m_mutex);
        Snapshot result = m_snapshot;
        m_snapshot.frame = {};
        return result;
    }
    bool touch(WPEEventType type, int id, int x, int y, quint64 generation) {
        if (id < -1) return false;
        return post([=] {
            if (!m_inputAllowed || m_passkeyRequest || generation != m_generation) return;
            if (type == WPE_EVENT_TOUCH_DOWN) {
                if (m_contacts.contains(id) || m_contacts.size() >= 16) return;
                m_contacts.insert(id, QPoint(x, y));
                wpe_view_focus_in(view());
            } else if (!m_contacts.contains(id)) return;
            m_contacts[id] = QPoint(x, y);
            deliver(wpe_event_touch_new(type, view(), WPE_INPUT_SOURCE_TOUCHSCREEN,
                time(), WPEModifiers(0), nativeContactId(id), x / Scale, y / Scale));
            if (type == WPE_EVENT_TOUCH_UP) m_contacts.remove(id);
        });
    }
    bool key(int raw, bool pressed, quint64 generation) {
        if (!pressed && !m_ready.load()) return true;
        return post([=] {
            if (pressed && (!m_inputAllowed || m_passkeyRequest || generation != m_generation)) return;
            wpe_view_focus_in(view());
            deliverKeys(m_keyboard.event(raw, pressed));
        });
    }
    bool cancel() { return !m_ready.load() || post([this] { cancelInput(); }); }
    bool cancelPasskey() {
        return !m_ready.load() || post([this] {
            if (m_passkeyRequest) webkit_web_authentication_request_cancel(m_passkeyRequest);
        });
    }
    bool completePasskey(quint64 requestId, bool success, rmweb::AuthPasskeyAssertion assertion) {
        return post([this, requestId, success, assertion = std::move(assertion)] {
            if (!m_passkeyRequest || requestId != m_activePasskeyId) return;
            auto *request = m_passkeyRequest;
            g_object_ref(request);
            clearPasskey();
            if (success) {
                syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: assertion_submitted");
                GBytes *id = g_bytes_new(assertion.credentialId.constData(), assertion.credentialId.size());
                GBytes *data = g_bytes_new(assertion.authenticatorData.constData(), assertion.authenticatorData.size());
                GBytes *signature = g_bytes_new(assertion.signature.constData(), assertion.signature.size());
                GBytes *user = assertion.userHandle.isEmpty() ? nullptr
                    : g_bytes_new(assertion.userHandle.constData(), assertion.userHandle.size());
                webkit_web_authentication_request_complete_assertion(request, id, data, signature, user);
                g_bytes_unref(id); g_bytes_unref(data); g_bytes_unref(signature);
                if (user) g_bytes_unref(user);
            } else {
                syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: assertion_cancelled");
                webkit_web_authentication_request_cancel(request);
            }
            g_object_unref(request);
        });
    }
private:
    void clearPasskey() {
        if (!m_passkeyRequest) return;
        if (m_passkeyCancelledSignal)
            g_signal_handler_disconnect(webkit_web_authentication_request_get_cancellable(m_passkeyRequest), m_passkeyCancelledSignal);
        m_passkeyCancelledSignal = 0;
        const quint64 id = m_activePasskeyId;
        m_activePasskeyId = 0;
        g_object_unref(m_passkeyRequest); m_passkeyRequest = nullptr;
        if (m_passkeys) QMetaObject::invokeMethod(m_passkeys, [this, id] { m_passkeys->cancel(id); }, Qt::QueuedConnection);
    }
    static gboolean webAuthentication(WebKitWebView *, WebKitWebAuthenticationRequest *request, gpointer opaque) {
        auto *self = static_cast<AuthEngine *>(opaque);
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: native_request_received");
        if (!self->m_passkeys || self->m_passkeyRequest) {
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: native_request_unavailable");
            return FALSE;
        }
        auto *cancellable = webkit_web_authentication_request_get_cancellable(request);
        if (g_cancellable_is_cancelled(cancellable)) {
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: native_request_cancelled");
            return FALSE;
        }
        gsize hashLength = 0;
        const auto *hashData = static_cast<const char *>(g_bytes_get_data(
            webkit_web_authentication_request_get_client_data_hash(request), &hashLength));
        const auto optionsBytes = QByteArray(webkit_web_authentication_request_get_options_json(request));
        QJsonParseError parseError;
        const auto options = QJsonDocument::fromJson(optionsBytes, &parseError);
        if (hashLength != 32 || optionsBytes.size() > 128 * 1024 || parseError.error != QJsonParseError::NoError || !options.isObject()) {
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: native_request_malformed");
            webkit_web_authentication_request_cancel(request);
            return TRUE;
        }
        const QString origin = QString::fromUtf8(webkit_web_authentication_request_get_origin(request));
        const QString relyingParty = QString::fromUtf8(webkit_web_authentication_request_get_rp_id(request));
        const QJsonObject input {{"version", 1}, {"origin", origin}, {"options", options.object()},
            {"clientDataHash", QString::fromLatin1(QByteArray(hashData, 32).toBase64(
                 QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))}};
        self->cancelInput();
        self->m_passkeyRequest = WEBKIT_WEB_AUTHENTICATION_REQUEST(g_object_ref(request));
        self->m_activePasskeyId = ++self->m_passkeySerial;
        self->m_passkeyCancelledSignal = g_signal_connect(cancellable, "cancelled", G_CALLBACK(+[](GCancellable *, gpointer data) {
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth passkey: native_request_cancelled");
            static_cast<AuthEngine *>(data)->clearPasskey();
        }), self);
        const auto id = self->m_activePasskeyId;
        QMetaObject::invokeMethod(self->m_passkeys, [self, id, relyingParty, input] {
            self->m_passkeys->start(id, relyingParty, input);
        }, Qt::QueuedConnection);
        return TRUE;
    }
    void reportHttpFailure(guint status) {
        // Enough to distinguish a server rejection from missing native
        // dispatch. Never retain or log a resource URL, header or body, and
        // bound diagnostics even if a page keeps retrying a failed resource.
        if (status >= 400 && status <= 599 && m_httpFailureReports < 16) {
            ++m_httpFailureReports;
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "rmweb-auth http: failure_status=%u", status);
        }
    }
    static void resourceResponse(WebKitWebResource *resource, GParamSpec *, gpointer data) {
        if (auto *response = webkit_web_resource_get_response(resource))
            static_cast<AuthEngine *>(data)->reportHttpFailure(webkit_uri_response_get_status_code(response));
    }
    // Pen -1 maps to zero; fingers map to 1..0x80000000. WPE reserves
    // UINT32_MAX and UINT32_MAX-1 internally, so never cast a negative ID alone.
    static guint32 nativeContactId(int id) { return guint32(id) + 1U; }
    WPEView *view() const { return webkit_web_view_get_wpe_view(m_webView); }
    static guint32 time() { return guint32(g_get_monotonic_time() / 1000); }
    void deliver(WPEEvent *event) {
        if (!event) return;
        wpe_view_event(view(), event);
        wpe_event_unref(event);
    }
    void deliverKeys(const QVector<rmweb::AuthKeyEvent> &events) {
        for (const auto &event : events)
            deliver(wpe_event_keyboard_new(event.pressed ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
                view(), WPE_INPUT_SOURCE_KEYBOARD, time(), WPEModifiers(event.key.modifiers),
                event.key.code, event.key.value));
    }
    void cancelInput() {
        deliverKeys(m_keyboard.cancel());
        const auto contacts = m_contacts;
        m_contacts.clear();
        for (auto it = contacts.cbegin(); it != contacts.cend(); ++it)
            deliver(wpe_event_touch_new(WPE_EVENT_TOUCH_CANCEL, view(), WPE_INPUT_SOURCE_TOUCHSCREEN,
                time(), WPEModifiers(0), nativeContactId(it.key()), it.value().x() / Scale, it.value().y() / Scale));
    }
    void fail() {
        m_inputAllowed = false;
        if (m_passkeyRequest) webkit_web_authentication_request_cancel(m_passkeyRequest);
        if (m_webView) cancelInput();
        std::lock_guard<std::mutex> guard(m_mutex);
        m_snapshot.failed = true;
        m_snapshot.loading = false;
        m_snapshot.frame = {};
    }
    bool post(std::function<void()> fn) {
        if (!m_ready.load()) return false;
        if (m_pending.fetch_add(1) >= 64) {
            --m_pending;
            return false; // Caller closes the app; never silently loses a held key.
        }
        auto *work = new std::function<void()>([this, fn = std::move(fn)] {
            fn();
            --m_pending;
        });
        GSource *source = g_idle_source_new();
        g_source_set_callback(source, [](gpointer data) -> gboolean {
            (*static_cast<std::function<void()> *>(data))();
            return G_SOURCE_REMOVE;
        }, work, [](gpointer data) { delete static_cast<std::function<void()> *>(data); });
        // Attaching a source never executes a callback on the submitting Qt
        // thread, even during startup (unlike g_main_context_invoke).
        g_source_attach(source, m_context);
        g_source_unref(source);
        return true;
    }
    static gboolean policy(WebKitWebView *, WebKitPolicyDecision *decision,
                           WebKitPolicyDecisionType type, gpointer data) {
        auto *self = static_cast<AuthEngine *>(data);
        if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
            auto *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
            if (webkit_response_policy_decision_is_mime_type_supported(response)) return FALSE;
        } else if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
            auto *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
            const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
            if (uri && rmweb::authNavigation(self->m_launch, uri) != rmweb::AuthNavigation::Blocked) return FALSE;
        }
        // No downloads, external schemes, pop-up windows, or TLS exceptions.
        webkit_policy_decision_ignore(decision);
        self->fail();
        return TRUE;
    }
    static void loadChanged(WebKitWebView *webView, WebKitLoadEvent event, gpointer data) {
        auto *self = static_cast<AuthEngine *>(data);
        if (event == WEBKIT_LOAD_STARTED) {
            self->cancelInput();
            self->m_inputAllowed = false;
            ++self->m_generation;
        }
        const char *uri = webkit_web_view_get_uri(webView);
        const auto navigation = uri ? rmweb::authNavigation(self->m_launch, uri) : rmweb::AuthNavigation::Blocked;
        // WPE load-changed is for the main frame. Navigation actions also
        // include child frames but expose no main-frame flag, so allow exact
        // internal blanks there and reject a top-level blank here. Stop outside
        // the snapshot mutex: WebKit may synchronously emit another callback.
        if (event == WEBKIT_LOAD_COMMITTED && navigation == rmweb::AuthNavigation::LocalBlank) {
            webkit_web_view_stop_loading(webView);
            self->fail();
            return;
        }
        std::lock_guard<std::mutex> guard(self->m_mutex);
        if (event == WEBKIT_LOAD_STARTED) {
            self->m_snapshot.generation = self->m_generation;
            self->m_snapshot.frame = {};
            self->m_snapshot.loading = true;
            self->m_snapshot.failed = false;
            self->m_snapshot.callback = false;
        }
        if (uri && (navigation == rmweb::AuthNavigation::Web || navigation == rmweb::AuthNavigation::Callback))
            self->m_snapshot.host = QUrl::fromEncoded(uri).host();
        if (event == WEBKIT_LOAD_COMMITTED && navigation == rmweb::AuthNavigation::Callback)
            self->m_snapshot.callback = true;
        if (event == WEBKIT_LOAD_FINISHED) {
            self->m_snapshot.loading = false;
            self->m_inputAllowed = !self->m_snapshot.failed && !self->m_snapshot.callback;
        }
    }
    static void buffer(WPEView *, WPEBuffer *buffer, gpointer data) {
        auto *self = static_cast<AuthEngine *>(data);
        if (!WPE_IS_BUFFER_SHM(buffer) || wpe_buffer_get_width(buffer) != self->m_size.width()
            || wpe_buffer_get_height(buffer) != self->m_size.height()) return;
        auto *shm = WPE_BUFFER_SHM(buffer);
        const guint stride = wpe_buffer_shm_get_stride(shm);
        GBytes *bytes = wpe_buffer_shm_get_data(shm); // Borrowed; never unref or release the WPE buffer.
        gsize length = 0;
        const auto *pixels = bytes ? static_cast<const uchar *>(g_bytes_get_data(bytes, &length)) : nullptr;
        if (!pixels || stride < guint(self->m_size.width() * 4) || stride > 32768
            || length < gsize(stride) * gsize(self->m_size.height())) return;
        QImage frame = QImage(pixels, self->m_size.width(), self->m_size.height(), stride, QImage::Format_ARGB32).copy();
        std::lock_guard<std::mutex> guard(self->m_mutex);
        if (!self->m_snapshot.failed) self->m_snapshot.frame = std::move(frame);
    }
    void run() {
        g_main_context_push_thread_default(m_context);
        GError *error = nullptr;
        WPEDisplay *display = wpe_display_headless_new();
        if (!display || !wpe_display_connect(display, &error)) {
            g_clear_error(&error);
            fail();
            return;
        }
        WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
        if (!session || !webkit_network_session_is_ephemeral(session)) { fail(); return; }
        webkit_network_session_set_persistent_credential_storage_enabled(session, FALSE);
        webkit_network_session_set_tls_errors_policy(session, WEBKIT_TLS_ERRORS_POLICY_FAIL);
        g_signal_connect(session, "download-started", G_CALLBACK(+[](WebKitNetworkSession *, WebKitDownload *download, gpointer) {
            webkit_download_cancel(download);
        }), nullptr);
        m_webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
            "network-session", session, nullptr));
        g_object_unref(display);
        g_object_unref(session);
        WebKitSettings *settings = webkit_web_view_get_settings(m_webView);
        webkit_settings_set_enable_developer_extras(settings, FALSE);
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, FALSE);
        webkit_settings_set_javascript_can_access_clipboard(settings, FALSE);
        webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
        webkit_settings_set_enable_media_stream(settings, FALSE);
        webkit_settings_set_enable_webrtc(settings, FALSE);
        webkit_settings_set_enable_page_cache(settings, FALSE);
        g_signal_connect(m_webView, "decide-policy", G_CALLBACK(policy), this);
        g_signal_connect(m_webView, "webauthn-request", G_CALLBACK(webAuthentication), this);
        g_signal_connect(m_webView, "resource-load-started", G_CALLBACK(+[](WebKitWebView *, WebKitWebResource *resource,
            WebKitURIRequest *, gpointer data) {
            g_signal_connect(resource, "notify::response", G_CALLBACK(resourceResponse), data);
        }), this);
        g_signal_connect(m_webView, "load-changed", G_CALLBACK(loadChanged), this);
        g_signal_connect(m_webView, "load-failed", G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, const char *, GError *error, gpointer data) -> gboolean {
            // A superseded/stopped navigation is ordinary browser behavior.
            // Do not clear a failure already set by a rejected navigation.
            if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) return TRUE;
            static_cast<AuthEngine *>(data)->fail();
            return TRUE; // Never render an error page containing a callback URL.
        }), this);
        g_signal_connect(m_webView, "load-failed-with-tls-errors", G_CALLBACK(+[](WebKitWebView *, const char *, GTlsCertificate *, GTlsCertificateFlags, gpointer data) -> gboolean {
            static_cast<AuthEngine *>(data)->fail();
            return TRUE;
        }), this);
        g_signal_connect(m_webView, "web-process-terminated", G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer data) {
            static_cast<AuthEngine *>(data)->fail();
        }), this);
        g_signal_connect(m_webView, "permission-request", G_CALLBACK(+[](WebKitWebView *, WebKitPermissionRequest *request, gpointer) -> gboolean {
            webkit_permission_request_deny(request);
            return TRUE;
        }), nullptr);
        g_signal_connect(m_webView, "run-file-chooser", G_CALLBACK(+[](WebKitWebView *, WebKitFileChooserRequest *request, gpointer) -> gboolean {
            webkit_file_chooser_request_cancel(request);
            return TRUE;
        }), nullptr);
        WPEView *wpeView = view();
        if (WPEToplevel *top = wpe_view_get_toplevel(wpeView)) {
            wpe_toplevel_scale_changed(top, Scale);
            wpe_toplevel_resize(top, m_size.width() / Scale, m_size.height() / Scale);
        }
        wpe_view_resized(wpeView, m_size.width() / Scale, m_size.height() / Scale);
        g_signal_connect(wpeView, "buffer-rendered", G_CALLBACK(buffer), this);
        wpe_view_set_visible(wpeView, FALSE);
        wpe_view_set_visible(wpeView, TRUE);
        wpe_view_focus_in(wpeView);
        m_ready = true;
        webkit_web_view_load_uri(m_webView, m_launch.initialUrl.toEncoded().constData());
        // No persistent profile/history/HTML or form-field JavaScript. The
        // process owns this temporary session until AppLoad/Return closes it.
        g_main_loop_run(g_main_loop_new(m_context, FALSE));
    }
    rmweb::AuthLaunch m_launch;
    const QSize m_size;
    GMainContext *m_context;
    WebKitWebView *m_webView = nullptr; // GLib worker only.
    std::atomic<bool> m_ready{false};
    std::atomic<int> m_pending{0};
    std::mutex m_mutex;
    Snapshot m_snapshot;
    QHash<int, QPoint> m_contacts;
    rmweb::AuthKeyboard m_keyboard;
    quint64 m_generation = 0; // GLib worker only.
    bool m_inputAllowed = false;
    rmweb::AuthPasskey *m_passkeys = nullptr; // Qt main thread owns the helper session.
    WebKitWebAuthenticationRequest *m_passkeyRequest = nullptr; // GLib thread only.
    gulong m_passkeyCancelledSignal = 0;
    quint64 m_passkeySerial = 0;
    quint64 m_activePasskeyId = 0;
    unsigned m_httpFailureReports = 0;
};

class SnapshotPump : public QObject {
public:
    SnapshotPump(QObject *parent, std::function<void()> consume) : QObject(parent) {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(750);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this, consume = std::move(consume)] {
            consume();
            if (m_interaction.isValid() && m_interaction.elapsed() >= 1000)
                m_timer.setInterval(750);
        });
        m_timer.start();
    }
    void noteInteraction() {
        m_interaction.start();
        // Restart only when entering the burst. Continuous input must not
        // postpone the next pickup, and passive pages retain e-ink batching.
        if (m_timer.interval() != 125) m_timer.start(125);
    }
private:
    QTimer m_timer;
    QElapsedTimer m_interaction;
};

SnapshotPump *startSnapshotPump(QObject *parent, std::function<void()> consume) {
    return new SnapshotPump(parent, std::move(consume));
}

bool privateProcess() {
    const rlimit core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &core) != 0) return false;
    const int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nullFd < 0) return false;
    bool ok = true;
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) if (dup2(nullFd, fd) < 0) ok = false;
    if (nullFd > STDERR_FILENO) close(nullFd);
    return ok; // WebKit/GLib child diagnostics cannot disclose URLs or page text.
}
}

int main(int argc, char **argv) {
    if (!privateProcess()) return 2;
    if (argc != 2 && argc != 4) return 2;
    if (argc == 4 && QByteArray(argv[2]) != "--device-code") return 2;
    const auto launch = rmweb::parseAuthLaunch(argv[1], argc == 4 ? QString::fromLatin1(argv[3]) : QString{});
    if (!launch || (argc == 4 && launch->deviceCode.isEmpty()) || qEnvironmentVariableIsEmpty("QTFB_KEY")) return 2;
    std::signal(SIGTERM, [](int) { _exit(0); });
    std::signal(SIGINT, [](int) { _exit(0); });
    QCoreApplication app(argc, argv);
    rmweb::QtfbClient client;
    rmweb::AuthSurface surface;
    rmweb::AuthPasskey passkeys;
    AuthEngine engine(*launch, surface.contentSize(), &passkeys);
    surface.setOrigin(launch->initialUrl.host());
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
    quint64 displayedGeneration = 0;
    auto *paintPump = startSnapshotPump(&app, [&] {
        const auto snapshot = engine.takeSnapshot();
        displayedGeneration = snapshot.generation;
        surface.beginNavigation(displayedGeneration);
        surface.setOrigin(snapshot.host.isEmpty() ? launch->initialUrl.host() : snapshot.host);
        surface.setLoading(snapshot.loading);
        surface.setFailed(snapshot.failed);
        surface.setCallbackReached(snapshot.callback);
        if (!snapshot.frame.isNull()) surface.setFrame(snapshot.frame);
    });
    QObject::connect(&client, &rmweb::QtfbClient::initialized, &app, [&](QSize) {
        client.submitImage(surface.image());
        engine.start();
        paintPump->noteInteraction();
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
    QObject::connect(&client, &rmweb::QtfbClient::touchesCancelled, &surface, &rmweb::AuthSurface::cancelTouches);
    QObject::connect(&client, &rmweb::QtfbClient::touchMoved, &surface, &rmweb::AuthSurface::move);
    QObject::connect(&client, &rmweb::QtfbClient::touchReleased, &surface, &rmweb::AuthSurface::release);
    QObject::connect(&client, &rmweb::QtfbClient::penPressed, &surface, &rmweb::AuthSurface::penPress);
    QObject::connect(&client, &rmweb::QtfbClient::penMoved, &surface, &rmweb::AuthSurface::penMove);
    QObject::connect(&client, &rmweb::QtfbClient::penReleased, &surface, &rmweb::AuthSurface::penRelease);
    QObject::connect(&surface, &rmweb::AuthSurface::touchPressed, &app, [&](int id, int x, int y) {
        paintPump->noteInteraction();
        if (!engine.touch(WPE_EVENT_TOUCH_DOWN, id, x, y, displayedGeneration)) close(2);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchMoved, &app, [&](int id, int x, int y) {
        paintPump->noteInteraction();
        if (!engine.touch(WPE_EVENT_TOUCH_MOVE, id, x, y, displayedGeneration)) close(2);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchReleased, &app, [&](int id, int x, int y) {
        paintPump->noteInteraction();
        if (!engine.touch(WPE_EVENT_TOUCH_UP, id, x, y, displayedGeneration)) close(2);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchesCancelled, &app, [&] { if (!engine.cancel()) close(2); });
    QObject::connect(&client, &rmweb::QtfbClient::keyEvent, &app, [&](int raw, bool pressed) {
        if (!pressed || surface.acceptsKeys()) {
            paintPump->noteInteraction();
            if (!engine.key(raw, pressed, displayedGeneration)) close(2);
        }
    });
    if (!client.startFromEnvironment()) return 2;
    const int result = app.exec();
    close(result);
    return result;
}
