// Offline API plumbing with synthetic assertion bytes, never a real signature.
// Resolving the added public API dynamically also permits an unpatched-engine
// feature-gate red check. No page Javascript replaces navigator.credentials.
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <dlfcn.h>
#include <syslog.h>
#include <cstdarg>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct DiagnosticRecord { int priority; std::string text; };
std::mutex diagnosticMutex;
std::vector<DiagnosticRecord> diagnosticRecords;
bool diagnosticOverflow = false;
bool checkDiagnostics = false;
}

// Test-only ELF interposition. Capture synthetic requests in memory, never
// forward logs or change the engine's authentication decisions.
extern "C" void syslog(int priority, const char* format, ...)
{
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    std::lock_guard<std::mutex> guard(diagnosticMutex);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(text) || diagnosticRecords.size() >= 128) {
        diagnosticOverflow = true;
        return;
    }
    diagnosticRecords.push_back({ priority, text });
}

typedef struct _WebKitWebAuthenticationRequest WebKitWebAuthenticationRequest;
using Request = WebKitWebAuthenticationRequest;
namespace {
struct API {
    const gchar* (*origin)(Request*);
    const gchar* (*rp)(Request*);
    const gchar* (*options)(Request*);
    GBytes* (*hash)(Request*);
    GCancellable* (*cancellable)(Request*);
    gboolean (*complete)(Request*, GBytes*, GBytes*, GBytes*, GBytes*);
    void (*cancel)(Request*);
} api;
GMainLoop* loop;
WebKitWebView* page;
Request* retained;
std::string scenario;
std::string expectedHash;
unsigned events = 0;
unsigned cancellations = 0;
bool reentrantRejected = false;
bool started = false;
bool finished = false;
bool pendingPoll = false;
int result = 1;

bool diagnosticsAgree()
{
    std::vector<std::string> expected { "rmweb-webauthn: assertion-received" };
    if (scenario == "success" || scenario == "challenge-1025" || scenario == "challenge-16384")
        expected.push_back("rmweb-webauthn: completed");
    else if (scenario == "cancel" || scenario == "abort" || scenario == "navigation" || scenario == "timeout")
        expected.push_back("rmweb-webauthn: cancelled");
    else if (scenario == "unhandled") {
        expected.push_back("rmweb-webauthn: unhandled-ui");
        expected.push_back("rmweb-webauthn: cancelled");
    } else if (scenario == "child-blank" || scenario == "child-srcdoc")
        expected.push_back("rmweb-webauthn: reject-secure-origin");
    else if (scenario == "wrong-rp" || scenario == "public-suffix")
        expected.push_back("rmweb-webauthn: reject-rp");
    else if (scenario == "conditional" || scenario == "silent")
        expected.push_back("rmweb-webauthn: reject-mediation");
    else if (scenario == "challenge-empty") expected.push_back("rmweb-webauthn: reject-challenge-empty");
    else if (scenario == "challenge-large") expected.push_back("rmweb-webauthn: reject-challenge-too-large");
    else if (scenario == "allow-large") expected.push_back("rmweb-webauthn: reject-allowlist-too-large");
    else if (scenario == "id-empty") expected.push_back("rmweb-webauthn: reject-credential-id-empty");
    else if (scenario == "id-large") expected.push_back("rmweb-webauthn: reject-credential-id-too-large");
    else if (scenario == "option-appid") expected.push_back("rmweb-webauthn: reject-options-appid");
    else if (scenario == "option-credProps") expected.push_back("rmweb-webauthn: reject-options-credProps");
    else if (scenario == "option-largeBlob") expected.push_back("rmweb-webauthn: reject-options-largeBlob");
    else if (scenario == "option-prf") expected.push_back("rmweb-webauthn: reject-options-prf");
    else return false;
    std::lock_guard<std::mutex> guard(diagnosticMutex);
    std::vector<DiagnosticRecord> providerRecords;
    unsigned libraryWarnings = 0;
    for (const auto& record : diagnosticRecords) {
        // The pinned runtime emits this fixed library warning on first use.
        // Permit that exact record once, never arbitrary unrelated log text.
        if (record.text == "Libgcrypt warning: missing initialization - please fix the application") {
            if (++libraryWarnings > 1 || record.priority != (LOG_USER | LOG_WARNING)) return false;
        } else providerRecords.push_back(record);
    }
    if (diagnosticOverflow || providerRecords.size() != expected.size()) {
        std::fprintf(stderr, "Diagnostic record count=%zu expected=%zu overflow=%d\n",
            providerRecords.size(), expected.size(), diagnosticOverflow);
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (providerRecords[i].priority != (LOG_AUTHPRIV | LOG_NOTICE)
            || providerRecords[i].text != expected[i]) {
            // Report only a fixed mismatch category, never captured text.
            std::fprintf(stderr, "Diagnostic record %zu priority-match=%d label-match=%d\n", i,
                providerRecords[i].priority == (LOG_AUTHPRIV | LOG_NOTICE), providerRecords[i].text == expected[i]);
            return false;
        }
    }
    return true;
}

void finish(bool okay, const char* reason)
{
    if (finished) return;
    finished = true;
    if (okay && checkDiagnostics && !diagnosticsAgree()) {
        okay = false;
        reason = "fixed diagnostic labels, priority, or data-exclusion mismatch";
    }
    result = okay ? 0 : 1;
    std::fprintf(okay ? stdout : stderr, "%s %s%s: %s\n", okay ? "PASS" : "FAIL",
        checkDiagnostics ? "diagnostic-" : "", scenario.c_str(), reason);
    g_main_loop_quit(loop);
}

template<typename T> bool symbol(T& destination, const char* name)
{
    destination = reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
    return destination != nullptr;
}

std::string hex(const unsigned char* bytes, size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}

bool complete(Request* request, const std::string& corruption = {})
{
    std::vector<unsigned char> id { 1, 2, 3 };
    std::vector<unsigned char> authData(37, 0);
    std::vector<unsigned char> signature { 1, 2 };
    std::vector<unsigned char> user { 4 };
    GChecksum* digest = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(digest, reinterpret_cast<const guchar*>("example.com"), 11);
    gsize size = 32;
    g_checksum_get_digest(digest, authData.data(), &size);
    g_checksum_free(digest);
    authData[32] = 0x05;
    if (corruption == "invalid-rp-hash") authData[0] ^= 1;
    if (corruption == "invalid-up") authData[32] &= ~1U;
    if (corruption == "invalid-uv") authData[32] &= ~4U;
    if (corruption == "invalid-at") authData[32] |= 0x40;
    if (corruption == "invalid-backup") authData[32] |= 0x10;
    if (corruption == "invalid-id") id[0] = 9;
    if (corruption == "invalid-large-auth") authData.resize(16385);
    if (corruption == "invalid-large-signature") signature.resize(4097);
    if (corruption == "invalid-empty-user") user.clear();
    GBytes* idBytes = g_bytes_new(id.data(), id.size());
    GBytes* authBytes = g_bytes_new(authData.data(), authData.size());
    GBytes* signatureBytes = g_bytes_new(signature.data(), signature.size());
    GBytes* userBytes = corruption == "invalid-discoverable-user" ? nullptr : g_bytes_new(user.data(), user.size());
    const bool accepted = api.complete(request, idBytes, authBytes, signatureBytes, userBytes);
    g_bytes_unref(idBytes);
    g_bytes_unref(authBytes);
    g_bytes_unref(signatureBytes);
    if (userBytes) g_bytes_unref(userBytes);
    return accepted;
}

void evaluate(const char* script)
{
    // The fixture polls its own primitive outcome separately. Do not ask IPC
    // to serialize a Promise or a DOM node returned by the setup expression.
    const std::string command = std::string(script) + "; undefined";
    webkit_web_view_evaluate_javascript(page, command.c_str(), -1, nullptr, nullptr, nullptr,
        +[](GObject* source, GAsyncResult* async, gpointer) {
            GError* error = nullptr;
            JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), async, &error);
            if (!value || error) finish(false, "synthetic script evaluation failed");
            g_clear_error(&error);
            if (value) g_object_unref(value);
        }, nullptr);
}

std::string expectedChallenge()
{
    // Independent GLib Base64 encoding of known fixture bytes, not WebKit's
    // serializer. Check both the native options and returned clientDataJSON.
    std::vector<unsigned char> bytes { 1, 2, 3, 4 };
    if (scenario == "challenge-1025") bytes.assign(1025, 65);
    if (scenario == "challenge-16384") bytes.assign(16384, 65);
    gchar* encoded = g_base64_encode(bytes.data(), bytes.size());
    std::string value = encoded;
    g_free(encoded);
    for (auto& character : value) {
        if (character == '+') character = '-';
        else if (character == '/') character = '_';
    }
    while (!value.empty() && value.back() == '=') value.pop_back();
    return value;
}

gboolean requested(WebKitWebView*, Request* request, gpointer)
{
    ++events;
    if (events != 1 || std::strcmp(api.origin(request), "https://login.example.com")
        || std::strcmp(api.rp(request), "example.com")) {
        finish(false, "native origin, RP, or event count mismatch");
        return FALSE;
    }
    const char* options = api.options(request);
    const std::string challengeField = "\"challenge\":\"" + expectedChallenge() + "\"";
    if (!options || !std::strstr(options, challengeField.c_str())
        || !std::strstr(options, "\"rpId\":\"example.com\"")
        || !std::strstr(options, "\"userVerification\":\"required\"")) {
        finish(false, "immutable options mismatch");
        return FALSE;
    }
    gsize length = 0;
    const auto* data = static_cast<const unsigned char*>(g_bytes_get_data(api.hash(request), &length));
    if (!data || length != 32) {
        finish(false, "clientDataHash must have exactly 32 bytes");
        return FALSE;
    }
    expectedHash = hex(data, length);
    retained = static_cast<Request*>(g_object_ref(request));
    g_signal_connect(api.cancellable(request), "cancelled", G_CALLBACK(+[](GCancellable*, gpointer) {
        ++cancellations;
        // Cancellation must invalidate first, including synchronous reentry.
        reentrantRejected = !complete(retained);
    }), nullptr);
    if (scenario == "unhandled") return FALSE;
    if (scenario == "success" || scenario == "discoverable" || scenario == "child-https-success"
        || scenario == "challenge-1025" || scenario == "challenge-16384") {
        if (!complete(request) || complete(request)) finish(false, "completion must succeed exactly once");
    } else if (scenario.rfind("invalid-", 0) == 0) {
        if (complete(request, scenario)) finish(false, "invalid assertion was accepted");
    } else if (scenario == "cancel") api.cancel(request);
    else if (scenario == "abort") {
        g_idle_add(+[](gpointer) -> gboolean { evaluate("window.abort.abort()"); return G_SOURCE_REMOVE; }, nullptr);
    } else if (scenario == "navigation") {
        g_idle_add(+[](gpointer) -> gboolean {
            webkit_web_view_load_html(page, "<!doctype html><title>replacement</title>", "https://login.example.com/next");
            return G_SOURCE_REMOVE;
        }, nullptr);
    } else if (scenario == "child-https-detach") {
        g_idle_add(+[](gpointer) -> gboolean {
            evaluate("document.getElementById('child').remove()");
            return G_SOURCE_REMOVE;
        }, nullptr);
    } else if (scenario != "timeout") finish(false, "unexpected native dispatch");
    return TRUE;
}

bool expectsNoEvent()
{
    return scenario == "wrong-rp" || scenario == "public-suffix" || scenario == "http"
        || scenario == "registration" || scenario == "conditional" || scenario == "silent"
        || scenario == "challenge-empty" || scenario == "challenge-large" || scenario == "capabilities"
        || scenario == "unfocused" || scenario == "child-blank" || scenario == "child-srcdoc"
        || scenario.rfind("option-", 0) == 0 || scenario == "allow-large" || scenario == "id-empty" || scenario == "id-large";
}

std::string expectedOutcome()
{
    if (scenario == "success" || scenario == "discoverable" || scenario == "child-https-success"
        || scenario == "challenge-1025" || scenario == "challenge-16384") return "ok:" + expectedHash;
    if (scenario == "abort") return "AbortError";
    if (scenario == "wrong-rp" || scenario == "public-suffix" || scenario == "child-blank"
        || scenario == "child-srcdoc") return "SecurityError";
    if (scenario == "http") return "insecure";
    if (scenario == "registration" || scenario == "conditional" || scenario == "silent"
        || scenario == "challenge-empty" || scenario == "challenge-large" || scenario == "unhandled"
        || scenario.rfind("option-", 0) == 0 || scenario == "allow-large" || scenario == "id-empty" || scenario == "id-large") return "NotSupportedError";
    if (scenario == "capabilities") return "unsupported";
    return "NotAllowedError";
}

gboolean poll(gpointer)
{
    if (finished) return G_SOURCE_REMOVE;
    if ((scenario == "navigation" || scenario == "child-https-detach") && cancellations) {
        finish(events == 1 && cancellations == 1 && reentrantRejected && !complete(retained),
            "document invalidation cancels the request and rejects late completion");
        return G_SOURCE_REMOVE;
    }
    if (!started || pendingPoll || scenario == "navigation" || scenario == "child-https-detach") return G_SOURCE_CONTINUE;
    pendingPoll = true;
    webkit_web_view_evaluate_javascript(page, "String(window.outcome || 'pending')", -1, nullptr, nullptr, nullptr,
        +[](GObject* source, GAsyncResult* async, gpointer) {
            pendingPoll = false;
            GError* error = nullptr;
            JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), async, &error);
            if (!value || error) {
                g_clear_error(&error);
                if (value) g_object_unref(value);
                finish(false, "result evaluation failed");
                return;
            }
            gchar* raw = jsc_value_to_string(value);
            std::string outcome = raw ? raw : "";
            g_free(raw);
            g_object_unref(value);
            if (outcome == "pending") return;
            bool okay = outcome == expectedOutcome() && events == (expectsNoEvent() ? 0U : 1U);
            if (!expectsNoEvent() && scenario != "success" && scenario != "discoverable" && scenario != "child-https-success"
                && scenario != "challenge-1025" && scenario != "challenge-16384")
                okay = okay && cancellations == 1 && reentrantRejected && !complete(retained);
            if (scenario == "success" || scenario == "discoverable" || scenario == "child-https-success"
                || scenario == "challenge-1025" || scenario == "challenge-16384") okay = okay && cancellations == 0;
            finish(okay, okay ? "native API and DOM result agree" : "DOM result or cancellation mismatch");
        }, nullptr);
    return G_SOURCE_CONTINUE;
}

std::string assertionScript()
{
    if (scenario == "http") return "window.outcome = !isSecureContext && !navigator.credentials ? 'insecure' : 'unexpected'";
    if (scenario == "capabilities") return "window.outcome='pending'; Promise.all([PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable(),PublicKeyCredential.isConditionalMediationAvailable(),PublicKeyCredential.getClientCapabilities()]).then(([uv,conditional,c]) => window.outcome=(!uv&&!conditional&&c.hybridTransport===true&&c.conditionalGet===false&&c.conditionalCreate===false&&c.relatedOrigins===false&&c.userVerifyingPlatformAuthenticator===false)?'unsupported':'unexpected')";
    if (scenario == "registration") return "window.outcome='pending';navigator.credentials.create({publicKey:{challenge:new Uint8Array([1,2,3,4]),rp:{id:'example.com',name:'Synthetic'},user:{id:new Uint8Array([1]),name:'test',displayName:'Synthetic'},pubKeyCredParams:[{type:'public-key',alg:-7}]}}).then(()=>window.outcome='unexpected',e=>window.outcome=e.name)";
    std::string challenge = scenario == "challenge-empty" ? "new Uint8Array(0)"
        : scenario == "challenge-large" ? "new Uint8Array(16385)"
        : scenario == "challenge-1025" ? "new Uint8Array(1025).fill(65)"
        : scenario == "challenge-16384" ? "new Uint8Array(16384).fill(65)" : "new Uint8Array([1,2,3,4])";
    std::string rp = scenario == "wrong-rp" ? "unrelated.example" : scenario == "public-suffix" ? "com" : "example.com";
    std::string allow = scenario == "discoverable" || scenario == "invalid-discoverable-user" ? "[]" : "[{type:'public-key',id:new Uint8Array([1,2,3])}]";
    if (scenario == "allow-large") allow = "Array.from({length:65},()=>({type:'public-key',id:new Uint8Array([1,2,3])}))";
    if (scenario == "id-empty") allow = "[{type:'public-key',id:new Uint8Array(0)}]";
    if (scenario == "id-large") allow = "[{type:'public-key',id:new Uint8Array(1025)}]";
    std::string extensions;
    if (scenario == "option-appid") extensions = ",extensions:{appid:'https://login.example.com/private-provider-sentinel'}";
    if (scenario == "option-credProps") extensions = ",extensions:{credProps:true}";
    if (scenario == "option-largeBlob") extensions = ",extensions:{largeBlob:{read:true}}";
    if (scenario == "option-prf") extensions = ",extensions:{prf:{eval:{first:new TextEncoder().encode('private-provider-sentinel')}}}";
    if (!extensions.empty()) challenge = "new TextEncoder().encode('private-provider-sentinel')";
    std::string mediation = scenario == "conditional" ? "conditional" : scenario == "silent" ? "silent" : "optional";
    return "window.outcome='pending';window.abort=new AbortController();navigator.credentials.get({publicKey:{challenge:" + challenge
        + ",rpId:'" + rp + "',timeout:" + (scenario == "timeout" ? "200" : "3000")
        + ",userVerification:'required',allowCredentials:" + allow + extensions + "},mediation:'" + mediation + "',signal:window.abort.signal}).then(async c=>{"
        "let d=JSON.parse(new TextDecoder().decode(c.response.clientDataJSON));"
        "let bytes=x=>Array.from(new Uint8Array(x)).join(',');"
        "if(d.type!=='webauthn.get'||d.challenge!=='" + expectedChallenge() + "'||d.origin!=='https://login.example.com'||bytes(c.rawId)!=='1,2,3'||bytes(c.response.signature)!=='1,2'||bytes(c.response.userHandle)!=='4'){window.outcome='invalid-result';return;}"
        "let h=new Uint8Array(await crypto.subtle.digest('SHA-256',c.response.clientDataJSON));window.outcome='ok:'+Array.from(h,b=>b.toString(16).padStart(2,'0')).join('');"
        "},e=>window.outcome=e.name)";
}

std::string script()
{
    const auto request = assertionScript();
    const bool realChild = scenario == "child-https-success" || scenario == "child-https-detach";
    if (scenario != "child-blank" && scenario != "child-srcdoc" && !realChild) return request;
    gchar* quoted = g_strescape(request.c_str(), nullptr);
    std::string source = quoted;
    g_free(quoted);
    if (realChild) {
        // A disposable TLS server supplies both actual documents. This branch
        // does not change native frame URLs, WebKit policy, or certificate checks.
        return "window.outcome='pending';let f=document.getElementById('child');"
            "if(!f||f.contentWindow.location.href!=='https://login.example.com/child'){window.outcome='invalid-child';}else{"
            "f.contentWindow.focus();setTimeout(()=>{f.contentWindow.eval(\"" + source
            + "\");let p=setInterval(()=>{let result=f.contentWindow.outcome;if(result&&result!=='pending'){clearInterval(p);window.outcome=result;}},20);},50);}";
    }
    // These real same-origin child documents inherit the secure origin, but
    // still have about:blank/about:srcdoc frame URLs. No native frame is mocked.
    return "window.outcome='pending';let f=document.createElement('iframe');"
        "f.onload=()=>{f.contentWindow.focus();setTimeout(()=>{f.contentWindow.eval(\"" + source
        + "\");let p=setInterval(()=>{let result=f.contentWindow.outcome;if(result&&result!=='pending'){clearInterval(p);window.outcome=result;}},20);},50);};"
        + (scenario == "child-srcdoc" ? "f.srcdoc='<!doctype html><title>inherited</title>';" : "f.src='about:blank';")
        + "document.body.appendChild(f)";
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    scenario = argv[1];
    if (scenario.rfind("diagnostic-", 0) == 0) {
        checkDiagnostics = true;
        scenario.erase(0, std::strlen("diagnostic-"));
    }
    const char* cases[] = { "success", "discoverable", "challenge-1025", "challenge-16384", "invalid-rp-hash", "invalid-up", "invalid-uv", "invalid-at", "invalid-backup", "invalid-id", "invalid-large-auth", "invalid-large-signature", "invalid-empty-user", "invalid-discoverable-user", "cancel", "abort", "navigation", "timeout", "unhandled", "wrong-rp", "public-suffix", "http", "registration", "conditional", "silent", "challenge-empty", "challenge-large", "capabilities", "unfocused", "child-blank", "child-srcdoc", "child-https-success", "child-https-detach", "option-appid", "option-credProps", "option-largeBlob", "option-prf", "allow-large", "id-empty", "id-large" };
    bool known = false;
    for (const auto* name : cases) known = known || scenario == name;
    if (!known) return 2;
    loop = g_main_loop_new(nullptr, FALSE);
    if (!symbol(api.origin, "webkit_web_authentication_request_get_origin")
        || !symbol(api.rp, "webkit_web_authentication_request_get_rp_id")
        || !symbol(api.options, "webkit_web_authentication_request_get_options_json")
        || !symbol(api.hash, "webkit_web_authentication_request_get_client_data_hash")
        || !symbol(api.cancellable, "webkit_web_authentication_request_get_cancellable")
        || !symbol(api.complete, "webkit_web_authentication_request_complete_assertion")
        || !symbol(api.cancel, "webkit_web_authentication_request_cancel")) {
        std::fputs("FAIL native provider API is absent\n", stderr);
        return 1;
    }
    GError* error = nullptr;
    WPEDisplay* display = wpe_display_headless_new();
    if (!display || !wpe_display_connect(display, &error)) return 2;
    auto* session = webkit_network_session_new_ephemeral();
    page = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, "network-session", session, nullptr));
    g_object_unref(display);
    g_object_unref(session);
    WPEView* view = webkit_web_view_get_wpe_view(page);
    if (auto* top = wpe_view_get_toplevel(view)) wpe_toplevel_resize(top, 810, 1000);
    wpe_view_resized(view, 810, 1000);
    wpe_view_set_visible(view, TRUE);
    wpe_view_focus_in(view);
    if (!g_signal_lookup("webauthn-request", WEBKIT_TYPE_WEB_VIEW)) return 1;
    g_signal_connect(page, "webauthn-request", G_CALLBACK(requested), nullptr);
    g_signal_connect(page, "web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason, gpointer) { finish(false, "web process terminated"); }), nullptr);
    if (scenario == "child-https-success" || scenario == "child-https-detach") {
        g_signal_connect(page, "load-failed-with-tls-errors", G_CALLBACK(+[](WebKitWebView*, const gchar*, GTlsCertificate*, GTlsCertificateFlags, gpointer) -> gboolean {
            finish(false, "synthetic HTTPS certificate was rejected");
            return FALSE;
        }), nullptr);
        g_signal_connect(page, "load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, const gchar*, GError* error, gpointer) -> gboolean {
            if (error) std::fprintf(stderr, "Synthetic load error domain=%s code=%d message=%s\n", g_quark_to_string(error->domain), error->code, error->message);
            finish(false, "synthetic HTTPS load failed");
            return FALSE;
        }), nullptr);
    }
    g_signal_connect(page, "load-changed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent event, gpointer) {
        if (event != WEBKIT_LOAD_FINISHED || started) return;
        started = true;
        if (scenario == "unfocused") {
            wpe_view_focus_out(webkit_web_view_get_wpe_view(page));
            g_timeout_add(100, +[](gpointer) -> gboolean {
                evaluate(script().c_str());
                return G_SOURCE_REMOVE;
            }, nullptr);
            return;
        }
        evaluate(script().c_str());
    }), nullptr);
    if (scenario == "child-https-success" || scenario == "child-https-detach")
        webkit_web_view_load_uri(page, "https://login.example.com/parent");
    else
        webkit_web_view_load_html(page, "<!doctype html><meta charset=utf-8><title>Synthetic WebAuthn fixture</title>",
            scenario == "http" ? "http://login.example.com/" : "https://login.example.com/");
    g_timeout_add(50, poll, nullptr);
    g_timeout_add_seconds(10, +[](gpointer) -> gboolean { finish(false, started ? (events ? "bounded request/result deadline" : "bounded script/native-dispatch deadline") : "bounded page-load deadline"); return G_SOURCE_REMOVE; }, nullptr);
    g_main_loop_run(loop);
    std::fflush(nullptr);
    std::_Exit(result);
}
