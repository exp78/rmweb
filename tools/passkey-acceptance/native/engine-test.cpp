#define ACCEPTANCE_MAIN unusedAcceptanceMain
#include "launcher.cpp"
#undef ACCEPTANCE_MAIN
#include <QElapsedTimer>

int main(int argc, char **argv) {
    const int output = dup(STDOUT_FILENO);
    if (output < 0 || !privateProcess() || argc != 2) return 2;
    const QByteArray scenario(argv[1]);
    if (scenario != "reload" && scenario != "route" && scenario != "origin" && scenario != "blank") return 2;
    QCoreApplication app(argc, argv);
    AuthEngine engine({QUrl(QStringLiteral("about:blank")), {}, {}, {}}, QSize(1620, 2000));
    engine.start();
    QElapsedTimer deadline; deadline.start();
    bool activated = false, changed = false, sawFrame = false;
    quint64 loadedGeneration = 0;
    Snapshot latest;
    QTimer timer; timer.setInterval(25);
    auto finish = [&](bool pass) {
        if (!pass) dprintf(output, "state active=%d changed=%d frame=%d generation=%llu loading=%d failed=%d\n", activated, changed, sawFrame, static_cast<unsigned long long>(latest.generation), latest.loading, latest.failed);
        const QByteArray result = pass ? "acceptance-engine PASS\n" : "acceptance-engine FAIL\n";
        const auto unused = write(output, result.constData(), result.size()); (void)unused;
        std::_Exit(pass ? 0 : 1);
    };
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (deadline.elapsed() > 12000) finish(false);
        if (!activated) { activated = AuthEngineTestAccess::activate(engine); return; }
        const auto snapshot = engine.takeSnapshot(); latest = snapshot;
        sawFrame = sawFrame || !snapshot.frame.isNull();
        if (!changed) {
            if (snapshot.generation < 2 || snapshot.loading || snapshot.failed || !sawFrame) return;
            loadedGeneration = snapshot.generation;
            const QByteArray url = scenario == "reload" ? TestOrigin + "/verify"
                : scenario == "route" ? TestOrigin + "/register"
                : scenario == "origin" ? QByteArray("https://other.example.com/verify") : QByteArray("about:blank");
            if (!AuthEngineTestAccess::navigateFixture(engine, url)) finish(false);
            changed = true;
        } else if (scenario == "reload") {
            if (snapshot.failed) finish(false);
            if (snapshot.generation > loadedGeneration && !snapshot.loading) finish(true);
        } else if (snapshot.failed) finish(true);
    });
    timer.start(); return app.exec();
}
