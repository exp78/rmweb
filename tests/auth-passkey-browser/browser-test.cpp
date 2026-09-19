// The real driver, Qt process session and WebKit provider joined together.
// Only this executable's explicit fixture mode produces synthetic assertions.
#define main unusedAuthBrowserMain
#include "../../engine/wpeqt/auth-main.cpp"
#undef main
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <cstdarg>
#include <cstdio>
#include <memory>

namespace {
std::mutex diagnosticMutex;
QList<QPair<int, QByteArray>> diagnostics;
bool diagnosticOverflow = false;
}
// Exercise the production syslog calls without writing fixture activity into
// the host log. This symbol exists only in the regression executable.
extern "C" void syslog(int priority, const char *format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> guard(diagnosticMutex);
    if (length < 0 || size_t(length) >= sizeof(text) || diagnostics.size() >= 128) {
        diagnosticOverflow = true;
        return;
    }
    diagnostics.append({priority, QByteArray(text, length)});
}

class AuthPasskeyTest {
public:
    static std::unique_ptr<rmweb::AuthPasskey> create(const QString &mode) {
        return std::unique_ptr<rmweb::AuthPasskey>(new rmweb::AuthPasskey(
            QCoreApplication::applicationFilePath(), {"--synthetic-helper", mode}));
    }
    static QProcess &process(rmweb::AuthPasskey &session) { return session.m_process; }
};

namespace {
struct AuthEngineTestAccess {
    static bool ready(AuthEngine &engine) { return engine.m_ready.load(); }
    static bool post(AuthEngine &engine, std::function<void()> fn) { return engine.post(std::move(fn)); }
    static WebKitWebView *page(AuthEngine &engine) { return engine.m_webView; }
    static void httpFailure(AuthEngine &engine, unsigned status) { engine.reportHttpFailure(status); }
    static bool requestCleared(AuthEngine &engine) {
        return !engine.m_passkeyRequest && !engine.m_activePasskeyId;
    }
    static void fixturePolicy(AuthEngine &engine, bool enable) {
        auto callback = reinterpret_cast<gpointer>(AuthEngine::policy);
        if (enable) g_signal_handlers_unblock_matched(engine.m_webView, G_SIGNAL_MATCH_FUNC,
            0, 0, nullptr, callback, nullptr);
        else g_signal_handlers_block_matched(engine.m_webView, G_SIGNAL_MATCH_FUNC,
            0, 0, nullptr, callback, nullptr);
    }
};

QByteArray encoded(const QByteArray &value) {
    return value.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
QByteArray authenticatorData() {
    auto data = QCryptographicHash::hash("example.com", QCryptographicHash::Sha256);
    data.append(char(0x05)); // User presence and verification; no extensions.
    data.append(QByteArray(4, '\0'));
    return data;
}
volatile sig_atomic_t terminated = 0;
int helper(const QByteArray &mode) {
    if (mode != "success" && mode != "late") return 2;
    alarm(12);
    std::signal(SIGTERM, [](int) { terminated = 1; });
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    const auto bytes = input.read(128 * 1024 + 1);
    if (bytes.size() > 128 * 1024 || !input.atEnd()) return 2;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    const auto request = document.object();
    const auto options = request.value("options").toObject();
    const QJsonArray allowed {QJsonObject {{"type", "public-key"}, {"id", "AQID"}}};
    const auto suppliedHash = request.value("clientDataHash").toString().toLatin1();
    const auto hash = QByteArray::fromBase64(suppliedHash, QByteArray::Base64UrlEncoding);
    if (error.error != QJsonParseError::NoError || request.size() != 4
        || request.value("version").toInt() != 1
        || request.value("origin").toString() != "https://login.example.com"
        || options.value("rpId").toString() != "example.com"
        || options.value("challenge").toString() != "AQIDBA"
        || options.value("userVerification").toString() != "required"
        || options.value("timeout").toInt() != 10000
        || options.value("allowCredentials").toArray() != allowed
        || hash.size() != 32 || encoded(hash) != suppliedHash) return 3;
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly)) return 2;
    const auto write = [&output](const QJsonObject &object) {
        const auto line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        return output.write(line) == line.size() && output.flush();
    };
    if (!write({{"type", "qr"}, {"size", 21}, {"modules", QString(441, '1')}})) return 4;
    if (mode == "late") {
        // Exit zero only after actual TERM and a successful late result write.
        // This distinguishes graceful cancellation from a later SIGKILL.
        while (!terminated) usleep(1000);
    }
    return write({{"type", "result"}, {"credentialId", "AQID"},
        {"authenticatorData", QString::fromLatin1(encoded(authenticatorData()))},
        {"signature", QString::fromLatin1(encoded(hash))}, {"userHandle", "BA"}}) ? 0 : 4;
}

AuthEngine *engine = nullptr;
rmweb::AuthPasskey *session = nullptr;
const char *scenario = nullptr;
int reportFd = -1;
std::atomic<bool> loaded{false}, replacementLoaded{false}, scriptPending{false}, cleared{false};
std::atomic<int> dom{-1}; // -1 pending, 0 mismatch, 1 success, 2 abort, 3 replacement.
bool sawQr = false, promptActive = false, childClean = false, cancellationSent = false;
bool observedCompletion = false, completionSucceeded = false;
unsigned completions = 0;
quint64 completedId = 0;

bool safeDiagnostics() {
    const QList<QByteArray> labels {
        "native_request_received", "native_request_cancelled", "helper_start_requested", "helper_started",
        "helper_cancelled", "qr_ready", "assertion_received", "helper_exit_success", "helper_exit_failure",
        "assertion_submitted"
    };
    const QByteArray prefix("rmweb-auth passkey: ");
    std::lock_guard<std::mutex> guard(diagnosticMutex);
    if (diagnosticOverflow) return false;
    QList<QByteArray> messages;
    QList<QByteArray> providerMessages;
    unsigned libraryWarnings = 0;
    for (const auto &event : diagnostics) {
        // The pinned engine emits this exact library warning on first use.
        // Keep the exception narrow: one message, one facility and severity.
        if (event.second == "Libgcrypt warning: missing initialization - please fix the application") {
            if (event.first != (LOG_USER | LOG_WARNING) || ++libraryWarnings > 1) return false;
            continue;
        }
        if (event.second == "rmweb-webauthn: assertion-received"
            || event.second == "rmweb-webauthn: completed" || event.second == "rmweb-webauthn: cancelled") {
            if (event.first != (LOG_AUTHPRIV | LOG_NOTICE)) return false;
            providerMessages.append(event.second);
            continue;
        }
        if (event.first != (LOG_AUTHPRIV | LOG_NOTICE) || !event.second.startsWith(prefix)
            || !labels.contains(event.second.mid(prefix.size()))) return false;
        messages.append(event.second);
    }
    const QList<QByteArray> expectedProvider {"rmweb-webauthn: assertion-received",
        std::strcmp(scenario, "success") == 0 ? "rmweb-webauthn: completed" : "rmweb-webauthn: cancelled"};
    return providerMessages == expectedProvider && messages.contains("rmweb-auth passkey: native_request_received")
        && messages.contains("rmweb-auth passkey: helper_started")
        && messages.contains("rmweb-auth passkey: qr_ready");
}
[[noreturn]] void finish(bool okay) {
    if (session) session->shutdown();
    okay = okay && safeDiagnostics();
    dprintf(reportFd, "%s: %s\n", scenario, okay ? "PASS" : "FAIL");
    // Same process-lifetime boundary as the production detached GLib worker.
    std::_Exit(okay ? 0 : 1);
}
bool named(const char *name) { return !std::strcmp(scenario, name); }

constexpr auto requestScript =
    // Capture this document's result object: an old rejection handler must not
    // write through the WindowProxy into the replacement document's sentinel.
    "window.fixtureState={outcome:'pending'};const state=window.fixtureState;window.abort=new AbortController();"
    "navigator.credentials.get({publicKey:{challenge:new Uint8Array([1,2,3,4]),"
    "rpId:'example.com',timeout:10000,userVerification:'required',"
    "allowCredentials:[{type:'public-key',id:new Uint8Array([1,2,3])}]},signal:window.abort.signal})"
    ".then(async c=>{let bytes=x=>Array.from(new Uint8Array(x)).join(',');"
    "let d=JSON.parse(new TextDecoder().decode(c.response.clientDataJSON));"
    "let h=await crypto.subtle.digest('SHA-256',c.response.clientDataJSON);"
    "let a=Array.from(new Uint8Array(c.response.authenticatorData));"
    "let r=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode('example.com'))));"
    "state.outcome=d.type==='webauthn.get'&&d.challenge==='AQIDBA'&&d.origin==='https://login.example.com'"
    "&&bytes(c.rawId)==='1,2,3'&&bytes(c.response.userHandle)==='4'"
    "&&bytes(h)===bytes(c.response.signature)&&a.length===37&&a.slice(0,32).join(',')===r.join(',')"
    "&&a[32]===5&&a.slice(33).every(v=>v===0)?'success':'mismatch';"
    "},e=>state.outcome=e.name)";

void evaluate(const char *script) {
    // Setup expressions may return a Promise, which is not a serializable
    // evaluate_javascript result even when the side effect succeeded.
    const QByteArray setup = QByteArray(script) + "; undefined";
    webkit_web_view_evaluate_javascript(AuthEngineTestAccess::page(*engine), setup.constData(), -1,
        nullptr, nullptr, nullptr, [](GObject *source, GAsyncResult *result, gpointer) {
            GError *error = nullptr;
            JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
            if (error || !value) dom = 0;
            g_clear_error(&error);
            if (value) g_object_unref(value);
        }, nullptr);
}
void fixtureLoaded(WebKitWebView *page, WebKitLoadEvent event, gpointer) {
    if (event != WEBKIT_LOAD_FINISHED) return;
    const auto *uri = webkit_web_view_get_uri(page);
    if (named("navigation") && uri && !std::strcmp(uri, "https://login.example.com/replacement")) {
        replacementLoaded = true;
        return;
    }
    if (loaded.load()) return;
    if (!uri || std::strcmp(uri, "https://login.example.com/fixture")) return;
    loaded = true;
    AuthEngineTestAccess::fixturePolicy(*engine, true);
    evaluate(requestScript);
}
void queryOutcome() {
    if (scriptPending.exchange(true)) return;
    if (!AuthEngineTestAccess::post(*engine, [] {
        webkit_web_view_evaluate_javascript(AuthEngineTestAccess::page(*engine),
            "String(window.fixtureState?.outcome || 'pending')", -1, nullptr, nullptr, nullptr,
            [](GObject *source, GAsyncResult *result, gpointer) {
                GError *error = nullptr;
                JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
                if (error || !value) dom = 0;
                else {
                    char *text = jsc_value_to_string(value);
                    const QByteArray outcome(text ? text : "");
                    g_free(text);
                    if (outcome == "success") dom = 1;
                    else if (outcome == "AbortError") dom = 2;
                    else if (outcome == "replacement") dom = 3;
                    else if (outcome != "pending") dom = 0;
                }
                g_clear_error(&error);
                if (value) g_object_unref(value);
                scriptPending = false;
            }, nullptr);
    })) finish(false);
}
}

int main(int argc, char **argv) {
    if (argc == 3 && QByteArray(argv[1]) == "--synthetic-helper") return helper(argv[2]);
    if (argc != 2) return 2;
    scenario = argv[1];
    if (!named("success") && !named("abort") && !named("navigation") && !named("http-diagnostics")) return 2;
    reportFd = dup(STDOUT_FILENO);
    if (reportFd < 0 || fcntl(reportFd, F_SETFD, FD_CLOEXEC) < 0 || !privateProcess()) return 2;
    QCoreApplication app(argc, argv);
    auto ownedSession = AuthPasskeyTest::create(named("success") ? "success" : "late");
    session = ownedSession.get();
    // No external request: the worker initially loads a local blank, replaced
    // with fixed synthetic HTTPS HTML through the existing friend seam.
    rmweb::AuthLaunch launch;
    launch.initialUrl = QUrl("about:blank");
    AuthEngine instance(launch, QSize(1620, 2000), session);
    engine = &instance;
    if (named("http-diagnostics")) {
        for (unsigned status : {0U, 200U, 399U, 600U, 0xffffffffU})
            AuthEngineTestAccess::httpFailure(instance, status);
        for (unsigned i = 0; i < 30; ++i) AuthEngineTestAccess::httpFailure(instance, 503);
        bool okay = !diagnosticOverflow && diagnostics.size() == 16;
        for (const auto &event : diagnostics)
            okay = okay && event.first == (LOG_AUTHPRIV | LOG_NOTICE)
                && event.second == "rmweb-auth http: failure_status=503";
        dprintf(reportFd, "%s: %s\n", scenario, okay ? "PASS" : "FAIL");
        return okay ? 0 : 1;
    }
    QObject::connect(session, &rmweb::AuthPasskey::promptChanged, &app,
        [&](rmweb::AuthPasskeyPrompt prompt) {
            promptActive = prompt.active;
            sawQr = sawQr || !prompt.qr.isNull();
        });
    QObject::connect(&AuthPasskeyTest::process(*session), &QProcess::finished, &app,
        [](int code, QProcess::ExitStatus status) { childClean = code == 0 && status == QProcess::NormalExit; });
    QObject::connect(session, &rmweb::AuthPasskey::completed, &app,
        [&](quint64 id, bool success, rmweb::AuthPasskeyAssertion assertion) {
            ++completions; observedCompletion = true; completionSucceeded = success; completedId = id;
            if (!engine->completePasskey(id, success, std::move(assertion))) finish(false);
            if (!named("success")) {
                // A late positive completion for the cancelled native request
                // must not settle or change the current document.
                rmweb::AuthPasskeyAssertion stale {QByteArray::fromHex("010203"), authenticatorData(), QByteArray(32, 'x'), QByteArray(1, 4)};
                if (!engine->completePasskey(id, true, std::move(stale))) finish(false);
            }
            if (!AuthEngineTestAccess::post(*engine, [] { cleared = AuthEngineTestAccess::requestCleared(*engine); })) finish(false);
        });
    engine->start();
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer timer;
    std::atomic<bool> bootstrapped{false};
    qint64 settledAt = -1;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (elapsed.elapsed() > 15000 || dom.load() == 0 || completions > 1) finish(false);
        if (!bootstrapped && AuthEngineTestAccess::ready(*engine)) {
            if (!AuthEngineTestAccess::post(*engine, [&bootstrapped] {
                auto *page = AuthEngineTestAccess::page(*engine);
                // Finish the worker's initial blank load before replacing it;
                // otherwise its queued commit can cancel our synthetic page.
                if (webkit_web_view_is_loading(page) || bootstrapped.exchange(true)) return;
                webkit_web_view_stop_loading(page);
                AuthEngineTestAccess::fixturePolicy(*engine, false);
                g_signal_connect(page, "load-changed", G_CALLBACK(fixtureLoaded), nullptr);
                webkit_web_view_load_alternate_html(page, "<!doctype html><title>Synthetic assertion</title>",
                    "https://login.example.com/fixture", "https://login.example.com/fixture");
            })) finish(false);
        }
        if (sawQr && !named("success") && !cancellationSent) {
            cancellationSent = true;
            if (!AuthEngineTestAccess::post(*engine, [] {
                if (named("abort")) evaluate("window.abort.abort()");
                else webkit_web_view_load_alternate_html(AuthEngineTestAccess::page(*engine),
                    "<!doctype html><script>window.fixtureState={outcome:'replacement'}</script>",
                    "https://login.example.com/replacement", "https://login.example.com/replacement");
            })) finish(false);
        }
        // Cancellation may finish before the replacement document loads. Its
        // retiring predecessor can legitimately report NotAllowedError then.
        if (loaded && observedCompletion && (!named("navigation") || replacementLoaded)) queryOutcome();
        const int expected = named("success") ? 1 : named("abort") ? 2 : 3;
        if (observedCompletion && dom.load() == expected && cleared.load()) {
            if (settledAt < 0) settledAt = elapsed.elapsed();
            if (elapsed.elapsed() - settledAt >= 250) {
                finish(completions == 1 && completedId == 1 && sawQr && childClean && !promptActive
                    && completionSucceeded == named("success"));
            }
        }
    });
    timer.start(20);
    app.exec();
    finish(false);
}
