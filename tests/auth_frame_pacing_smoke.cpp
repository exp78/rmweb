// Offline actual-engine timing fixture. No real page, credentials or captures.
#define main unusedAuthBrowserMain
#include "../engine/wpeqt/auth-main.cpp"
#undef main
#include <QElapsedTimer>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
struct AuthEngineTestAccess {
    static bool ready(AuthEngine &engine) { return engine.m_ready.load(); }
    static bool post(AuthEngine &engine, std::function<void()> fn) { return engine.post(std::move(fn)); }
    static WebKitWebView *webView(AuthEngine &engine) { return engine.m_webView; }
    static qint64 frameKey(AuthEngine &engine) {
        std::lock_guard<std::mutex> guard(engine.m_mutex);
        return engine.m_snapshot.frame.cacheKey();
    }
    static void fixturePolicy(AuthEngine &engine, bool enable) {
        const auto function = reinterpret_cast<gpointer>(AuthEngine::policy);
        if (enable) g_signal_handlers_unblock_matched(engine.m_webView, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, function, nullptr);
        else g_signal_handlers_block_matched(engine.m_webView, G_SIGNAL_MATCH_FUNC, 0, 0, nullptr, function, nullptr);
    }
};
qint64 now() { return g_get_monotonic_time() / 1000; }
struct Metrics {
    std::atomic<bool> initialFinished{false}, fixtureStarted{false};
    std::atomic<bool> loaded{false}, measuring{false}, error{false};
    std::atomic<bool> measuringBurst{false}, measuringReturn{false};
    std::atomic<int> buffers{0}, copies{0};
    std::atomic<int> burstBuffers{0}, burstCopies{0}, returnBuffers{0}, returnCopies{0};
    std::atomic<qint64> clickedAt{0}, markerRenderedAt{0}, postDelay{0};
    qint64 previousKey = 0; // GLib thread only.
} metrics;
int reportFd = -1;
bool animated = false;
bool continuous = false;
int published = 0;
int returnPublished = 0;
int nativeMoves = 0;
qint64 pickupDelay = -1;
qint64 domDelay = -1;
qint64 renderDelay = -1;
qint64 snapshotDelay = -1;
qint64 snapshotWait = -1;
qint64 compositionTime = -1;
bool lastFailed = false;
bool lastLoading = true;

[[noreturn]] void finish(bool passed) {
    dprintf(reportFd, "%s: %s buffers=%d copies=%d published=%d glib_post_ms=%lld dom_ms=%lld render_ms=%lld snapshot_ms=%lld snapshot_wait_ms=%lld compose_ms=%lld pickup_ms=%lld\n",
        continuous ? "frame-continuous" : animated ? "frame-animation" : "frame-static", passed ? "PASS" : "FAIL",
        metrics.buffers.load(), metrics.copies.load(), published,
        static_cast<long long>(metrics.postDelay.load()), static_cast<long long>(domDelay),
        static_cast<long long>(renderDelay), static_cast<long long>(snapshotDelay), static_cast<long long>(snapshotWait),
        static_cast<long long>(compositionTime), static_cast<long long>(pickupDelay));
    dprintf(reportFd, "initial_finished=%d fixture_started=%d fixture_finished=%d failed=%d loading=%d\n",
        int(metrics.initialFinished.load()), int(metrics.fixtureStarted.load()), int(metrics.loaded.load()), int(lastFailed), int(lastLoading));
    if (continuous)
        dprintf(reportFd, "native_moves=%d burst_buffers=%d burst_copies=%d return_buffers=%d return_copies=%d return_published=%d\n",
            nativeMoves, metrics.burstBuffers.load(), metrics.burstCopies.load(), metrics.returnBuffers.load(),
            metrics.returnCopies.load(), returnPublished);
    _Exit(passed ? 0 : 1);
}

void bootstrap(AuthEngine &engine) {
    AuthEngineTestAccess::fixturePolicy(engine, false);
    auto *view = AuthEngineTestAccess::webView(engine);
    auto *manager = webkit_web_view_get_user_content_manager(view);
    g_signal_connect(manager, "script-message-received::timing", G_CALLBACK(+[](WebKitUserContentManager *, JSCValue *value, gpointer) {
        // One fixed synthetic message, never arbitrary page output.
        char *message = jsc_value_to_string(value);
        if (!message || strcmp(message, "clicked")) metrics.error = true;
        else metrics.clickedAt = now();
        g_free(message);
    }), nullptr);
    if (!webkit_user_content_manager_register_script_message_handler(manager, "timing", nullptr)) {
        metrics.error = true;
        return;
    }
    g_signal_connect(view, "load-changed", G_CALLBACK(+[](WebKitWebView *view, WebKitLoadEvent event, gpointer data) {
        const char *uri = webkit_web_view_get_uri(view);
        if (!uri || strcmp(uri, "https://fixture.example.test/")) return;
        if (event == WEBKIT_LOAD_STARTED) metrics.fixtureStarted = true;
        if (event != WEBKIT_LOAD_FINISHED) return;
        if (!metrics.fixtureStarted) { metrics.error = true; return; }
        AuthEngineTestAccess::fixturePolicy(*static_cast<AuthEngine *>(data), true);
        metrics.loaded = true;
    }), &engine);
    g_signal_connect(webkit_web_view_get_wpe_view(view), "buffer-rendered", G_CALLBACK(+[](WPEView *, WPEBuffer *buffer, gpointer data) {
        const qint64 key = AuthEngineTestAccess::frameKey(*static_cast<AuthEngine *>(data));
        if (metrics.measuring) {
            ++metrics.buffers;
            if (key && key != metrics.previousKey) ++metrics.copies;
        }
        if (metrics.measuringBurst) {
            ++metrics.burstBuffers;
            if (key && key != metrics.previousKey) ++metrics.burstCopies;
        }
        if (metrics.measuringReturn) {
            ++metrics.returnBuffers;
            if (key && key != metrics.previousKey) ++metrics.returnCopies;
        }
        if (key) metrics.previousKey = key;
        // Distinguish renderer pipeline delay from Qt pickup, using only the
        // fixed synthetic marker. Do not export pixels or evaluate real pages.
        if (metrics.clickedAt && !metrics.markerRenderedAt && WPE_IS_BUFFER_SHM(buffer)
            && wpe_buffer_get_width(buffer) == 1620 && wpe_buffer_get_height(buffer) == 2000) {
            auto *shm = WPE_BUFFER_SHM(buffer);
            GBytes *bytes = wpe_buffer_shm_get_data(shm);
            gsize length = 0;
            const auto *pixels = bytes ? static_cast<const unsigned char *>(g_bytes_get_data(bytes, &length)) : nullptr;
            const gsize offset = gsize(wpe_buffer_shm_get_stride(shm)) * 1700 + 1400 * 4;
            if (pixels && offset + 4 <= length && !pixels[offset] && !pixels[offset + 1] && !pixels[offset + 2])
                metrics.markerRenderedAt = now();
        }
    }), &engine);
    QByteArray html = "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{margin:0;background:white}button{position:absolute;left:100px;top:100px;width:240px;height:80px}"
        "#motion{position:absolute;top:250px;width:600px;height:450px;background:#ddd}"
        "#marker{position:absolute;left:650px;top:800px;width:100px;height:100px;background:white}"
        "@keyframes move{from{transform:translateX(0)}to{transform:translateX(100px)}}</style>"
        "<button onclick=\"document.getElementById('marker').style.background='black';"
        "window.webkit.messageHandlers.timing.postMessage('clicked')\">Synthetic</button>"
        "<div id=motion></div><div id=marker></div>";
    if (animated) html += "<script>document.getElementById('motion').style.animation='move 1s linear infinite alternate'</script>";
    webkit_web_view_load_alternate_html(view, html.constData(), "https://fixture.example.test/", "https://fixture.example.test/");
}

int sustainedInput(QCoreApplication &app) {
    const qint64 start = now();
    qint64 previous = 0;
    qint64 maximumActiveGap = 0;
    int active = 0;
    int passive = 0;
    auto *pump = startSnapshotPump(&app, [&] {
        const qint64 elapsed = now() - start;
        if (elapsed < 3000) {
            ++active;
            if (previous) maximumActiveGap = std::max(maximumActiveGap, elapsed - previous);
            previous = elapsed;
        }
        if (elapsed >= 4500 && elapsed < 7500) ++passive;
    });
    pump->noteInteraction();
    QTimer input;
    input.setInterval(40);
    QObject::connect(&input, &QTimer::timeout, &app, [&] {
        if (now() - start >= 3000) input.stop();
        else pump->noteInteraction();
    });
    input.start();
    QTimer::singleShot(7600, &app, [&] {
        const bool passed = active >= 20 && active <= 26 && maximumActiveGap <= 250 && passive >= 3 && passive <= 5;
        dprintf(reportFd, "frame-pump: %s active_pickups=%d max_active_gap_ms=%lld passive_pickups=%d\n",
            passed ? "PASS" : "FAIL", active, static_cast<long long>(maximumActiveGap), passive);
        _Exit(passed ? 0 : 1);
    });
    return app.exec();
}
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "static") && strcmp(argv[1], "animation")
        && strcmp(argv[1], "continuous") && strcmp(argv[1], "pump"))) return 2;
    continuous = !strcmp(argv[1], "continuous");
    animated = continuous || !strcmp(argv[1], "animation");
    reportFd = dup(STDOUT_FILENO);
    if (reportFd < 0 || fcntl(reportFd, F_SETFD, FD_CLOEXEC) < 0 || !privateProcess()) return 2;
    QCoreApplication app(argc, argv);
    if (!strcmp(argv[1], "pump")) return sustainedInput(app);
    // The fixture alone constructs a local initial document. Production still
    // parses its strict launch URLs and rejects a top-level blank commit.
    // Wait for that rejection to finish before loading the synthetic document;
    // no asynchronous network failure can arrive behind the replacement.
    rmweb::AuthLaunch launch;
    launch.initialUrl = QUrl(QStringLiteral("about:blank"));
    AuthEngine engine(launch, QSize(1620, 2000));
    g_type_class_ref(WEBKIT_TYPE_WEB_VIEW);
    const guint loadSignal = g_signal_lookup("load-changed", WEBKIT_TYPE_WEB_VIEW);
    if (!loadSignal || !g_signal_add_emission_hook(loadSignal, 0,
        [](GSignalInvocationHint *, guint count, const GValue *values, gpointer data) -> gboolean {
            if (count < 2 || g_value_get_enum(&values[1]) != WEBKIT_LOAD_FINISHED) return TRUE;
            auto *view = WEBKIT_WEB_VIEW(g_value_get_object(&values[0]));
            if (view != AuthEngineTestAccess::webView(*static_cast<AuthEngine *>(data))) return TRUE;
            const char *uri = webkit_web_view_get_uri(view);
            if (uri && !strcmp(uri, "about:blank")) metrics.initialFinished = true;
            return TRUE;
        }, &engine, nullptr)) finish(false);
    rmweb::AuthSurface surface;
    quint64 generation = 0;
    SnapshotPump *paintPump = nullptr;
    QObject::connect(&surface, &rmweb::AuthSurface::touchPressed, &app, [&](int id, int x, int y) {
        paintPump->noteInteraction();
        if (!engine.touch(WPE_EVENT_TOUCH_DOWN, id, x, y, generation)) finish(false);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchMoved, &app, [&](int id, int x, int y) {
        paintPump->noteInteraction();
        if (!engine.touch(WPE_EVENT_TOUCH_MOVE, id, x, y, generation)) finish(false);
    });
    QObject::connect(&surface, &rmweb::AuthSurface::touchReleased, &app, [&](int id, int x, int y) {
        paintPump->noteInteraction();
        if (!engine.touch(WPE_EVENT_TOUCH_UP, id, x, y, generation)) finish(false);
    });
    bool bootstrapped = false;
    bool measured = false;
    bool tapScheduled = false;
    qint64 settledAt = 0;
    qint64 measurementAt = 0;
    qint64 releasedAt = 0;
    qint64 burstStarted = 0;
    qint64 burstEnded = 0;
    qint64 returnStarted = 0;
    QTimer nativeInput;
    nativeInput.setInterval(10);
    nativeInput.setTimerType(Qt::PreciseTimer);
    QObject::connect(&nativeInput, &QTimer::timeout, &app, [&] {
        if (now() - burstStarted >= 3000) {
            metrics.measuringBurst = false;
            nativeInput.stop();
            surface.release(0, 440, 1000);
            burstEnded = now();
        } else {
            ++nativeMoves;
            surface.move(0, 440 + (nativeMoves % 2), 1000);
        }
    });
    const qint64 startedAt = now();
    paintPump = startSnapshotPump(&app, [&] {
        const qint64 snapshotStartedAt = now();
        const auto snapshot = engine.takeSnapshot();
        const qint64 snapshotAcquiredAt = now();
        lastFailed = snapshot.failed;
        lastLoading = snapshot.loading;
        generation = snapshot.generation;
        surface.beginNavigation(generation);
        surface.setOrigin(snapshot.host);
        surface.setLoading(snapshot.loading);
        surface.setFailed(snapshot.failed);
        surface.setCallbackReached(snapshot.callback);
        if (!snapshot.frame.isNull()) {
            surface.setFrame(snapshot.frame);
            // Exercise the actual raster composition; inspect only one known
            // synthetic marker pixel in memory. Never export a frame.
            const qint64 compositionStartedAt = now();
            const QImage composed = surface.image();
            const qint64 compositionFinishedAt = now();
            if (metrics.measuring) ++published;
            if (metrics.measuringReturn) ++returnPublished;
            if (releasedAt && pickupDelay < 0 && qRed(composed.pixel(1400, 1860)) < 10) {
                pickupDelay = compositionFinishedAt - releasedAt;
                snapshotDelay = snapshotAcquiredAt - releasedAt;
                snapshotWait = snapshotAcquiredAt - snapshotStartedAt;
                compositionTime = compositionFinishedAt - compositionStartedAt;
                domDelay = metrics.clickedAt.load() ? metrics.clickedAt.load() - releasedAt : -1;
                renderDelay = metrics.markerRenderedAt.load() ? metrics.markerRenderedAt.load() - releasedAt : -1;
                const bool workload = animated ? metrics.buffers >= 15 && metrics.copies >= 10 && published >= 3 && published <= 5
                                               : metrics.buffers <= 3 && metrics.copies <= 3 && published <= 3;
                const bool passed = workload && metrics.buffers <= 28 && metrics.copies <= 28 && metrics.postDelay < 300
                    && domDelay >= 0 && domDelay <= 300 && pickupDelay <= 300 && !metrics.error;
                if (!continuous || !passed) finish(passed);
                burstStarted = now();
                metrics.measuringBurst = true;
                surface.press(0, 440, 1000);
                nativeInput.start();
            }
        }
        if (metrics.loaded && surface.acceptsKeys() && !settledAt) settledAt = now();
        // Align a real native tap just after the shared production pump.
        if (measured && !tapScheduled && surface.acceptsKeys()) {
            tapScheduled = true;
            QTimer::singleShot(25, &app, [&] {
                const auto postedAt = now();
                if (!AuthEngineTestAccess::post(engine, [postedAt] { metrics.postDelay = now() - postedAt; })) finish(false);
                surface.press(0, 440, 440);
                QTimer::singleShot(60, &app, [&] {
                    releasedAt = now();
                    surface.release(0, 440, 440);
                });
            });
        }
    });
    QTimer observe;
    observe.setInterval(10);
    QObject::connect(&observe, &QTimer::timeout, &app, [&] {
        if (!bootstrapped && AuthEngineTestAccess::ready(engine) && metrics.initialFinished) {
            bootstrapped = true;
            if (!AuthEngineTestAccess::post(engine, [&] { bootstrap(engine); })) finish(false);
        }
        if (settledAt && !measurementAt && now() - settledAt >= 1000) {
            measurementAt = now();
            metrics.measuring = true;
        }
        if (measurementAt && !measured && now() - measurementAt >= 3000) {
            metrics.measuring = false;
            measured = true;
        }
        if (burstEnded && !returnStarted && now() - burstEnded >= 1500) {
            returnStarted = now();
            metrics.measuringReturn = true;
        }
        if (returnStarted && now() - returnStarted >= 3000) {
            metrics.measuringReturn = false;
            finish(nativeMoves >= 200 && metrics.burstBuffers >= 40 && metrics.burstBuffers <= 94
                && metrics.burstCopies >= 40 && metrics.burstCopies <= 94
                && metrics.returnBuffers >= 15 && metrics.returnBuffers <= 28
                && metrics.returnCopies >= 10 && metrics.returnCopies <= 28
                && returnPublished >= 3 && returnPublished <= 5 && !metrics.error);
        }
        if (metrics.error || now() - startedAt > 20000) finish(false);
    });
    observe.start();
    paintPump->noteInteraction();
    engine.start();
    return app.exec();
}
