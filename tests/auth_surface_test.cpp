#include "auth-surface.h"
#include <QSignalSpy>
#include <QtTest>
using rmweb::AuthSurface;
static void tap(AuthSurface &s, QPoint p) {
    s.press(1, p.x(), p.y());
    s.release(1, p.x(), p.y());
}
static void ready(AuthSurface &s) {
    QImage f(s.contentSize(), QImage::Format_RGB32);
    f.fill(Qt::red);
    s.setFrame(f);
    s.setLoading(false);
}
class AuthSurfaceTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void passkeyPromptBlocksPageAndKeyboardUntilDismissed() {
        AuthSurface s;
        ready(s);
        QSignalSpy page(&s, &AuthSurface::touchPressed), cancel(&s, &AuthSurface::passkeyCancelRequested);
        QImage qr(29, 29, QImage::Format_RGB32); qr.fill(Qt::black);
        s.setPasskeyPrompt({true, "example.test", "Scan with your phone", qr});
        QVERIFY(!s.acceptsKeys());
        tap(s, {800, 500});
        QCOMPARE(page.size(), 0);
        tap(s, s.passkeyCancelRect().center());
        tap(s, s.passkeyCancelRect().center());
        QCOMPARE(cancel.size(), 1);
        QVERIFY(!s.acceptsKeys());
        s.setPasskeyPrompt({});
        QVERIFY(s.acceptsKeys());
        tap(s, {800, 500});
        QCOMPARE(page.size(), 1);
    }
    void passkeyPromptCancelsHeldPageContactAndKeepsExit() {
        AuthSurface s;
        ready(s);
        QSignalSpy cancelled(&s, &AuthSurface::touchesCancelled), close(&s, &AuthSurface::closeRequested);
        s.press(4, 100, 300);
        s.setPasskeyPrompt({true, "example.test", "Preparing phone sign-in", {}});
        QCOMPARE(cancelled.size(), 1);
        const auto exit = s.returnRect().center();
        s.penPress(exit.x(), exit.y()); s.penRelease(exit.x(), exit.y());
        QCOMPARE(close.size(), 1);
    }
    void headerIsNeverPageInput() {
        AuthSurface s;
        ready(s);
        QSignalSpy touches(&s, &AuthSurface::touchPressed),
            closes(&s, &AuthSurface::closeRequested);
        QCOMPARE(s.contentSize(), QSize(1620, 2000));
        QCOMPARE(s.image().pixelColor(800, 400), QColor(Qt::red));
        QVERIFY(s.image().pixelColor(800, 80) != QColor(Qt::red));
        tap(s, s.returnRect().center());
        tap(s, s.returnRect().center());
        QCOMPARE(closes.size(), 1);
        QCOMPARE(touches.size(), 0);
    }
    void forwardsOnlyContentCoordinates() {
        AuthSurface s;
        ready(s);
        QSignalSpy p(&s, &AuthSurface::touchPressed), m(&s, &AuthSurface::touchMoved),
            r(&s, &AuthSurface::touchReleased);
        s.press(20, 100, 260);
        s.move(20, 120, 300);
        s.release(20, 120, 310);
        QCOMPARE(p.first(), QVariantList({20, 100, 100}));
        QCOMPARE(m.first(), QVariantList({20, 120, 140}));
        QCOMPARE(r.first(), QVariantList({20, 120, 150}));
    }
    void penUsesContentCoordinatesAndASeparateContact() {
        AuthSurface s;
        ready(s);
        QSignalSpy p(&s, &AuthSurface::touchPressed), m(&s, &AuthSurface::touchMoved),
            r(&s, &AuthSurface::touchReleased), cancel(&s, &AuthSurface::touchesCancelled);
        s.penPress(100, 260);
        s.penMove(120, 300);
        s.penRelease(120, 310);
        QCOMPARE(p.first(), QVariantList({-1, 100, 100}));
        QCOMPARE(m.first(), QVariantList({-1, 120, 140}));
        QCOMPARE(r.first(), QVariantList({-1, 120, 150}));
        s.penPress(100, 260);
        s.press(0, 300, 400);
        s.penRelease(100, 260);
        s.release(0, 300, 400);
        QCOMPARE(cancel.size(), 1);
        QCOMPARE(r.size(), 1);
        s.press(-1, 100, 260); // Only the explicit pen interface owns this ID.
        QCOMPARE(p.size(), 2);
    }
    void penHonorsReadinessAndReturn() {
        AuthSurface s;
        QSignalSpy p(&s, &AuthSurface::touchPressed), close(&s, &AuthSurface::closeRequested);
        s.penPress(100, 300);
        s.penRelease(100, 300);
        QCOMPARE(p.size(), 0);
        const auto button = s.returnRect().center();
        s.penPress(button.x(), button.y());
        s.penRelease(button.x(), button.y());
        QCOMPARE(close.size(), 1);
    }
    void wrongSizedFrameIsRejected() {
        AuthSurface s;
        ready(s);
        QImage wrong(1620, 1180, QImage::Format_RGB32);
        wrong.fill(Qt::blue);
        s.setFrame(wrong);
        QCOMPARE(s.contentSize(), QSize(1620, 2000));
        QCOMPARE(s.image().pixelColor(800, 1800), QColor(Qt::red));
    }
    void crossingChromeCancelsNativeContact() {
        AuthSurface s;
        ready(s);
        QSignalSpy c(&s, &AuthSurface::touchesCancelled), r(&s, &AuthSurface::touchReleased),
            close(&s, &AuthSurface::closeRequested);
        s.press(4, 100, 300);
        s.move(4, 100, 80);
        s.release(4, 100, 80);
        QCOMPARE(c.size(), 1);
        QCOMPARE(r.size(), 0);
        QCOMPARE(close.size(), 0);
    }
    void multiTouchCancelsPageAndReturn() {
        AuthSurface s;
        ready(s);
        QSignalSpy cancel(&s, &AuthSurface::touchesCancelled),
            close(&s, &AuthSurface::closeRequested);
        s.press(1, 100, 300);
        s.press(2, 200, 300);
        s.release(1, 100, 300);
        s.release(2, 200, 300);
        QCOMPARE(cancel.size(), 1);
        const auto p = s.returnRect().center();
        s.press(1, p.x(), p.y());
        s.press(2, p.x(), p.y());
        s.release(1, p.x(), p.y());
        s.release(2, p.x(), p.y());
        QCOMPARE(close.size(), 0);
    }
    void freshTouchSequenceRecoversAnOrphanedCancelledContact() {
        AuthSurface s;
        ready(s);
        QSignalSpy close(&s, &AuthSurface::closeRequested);
        s.press(7, 400, 400); // Its release was lost when Qt cancelled the sequence.
        tap(s, s.returnRect().center());
        QCOMPARE(close.size(), 0); // Reproduces the on-device lockout.
        s.cancelTouches(); // AppLoad now forwards the next native TouchBegin.
        QCOMPARE(close.size(), 0); // Cancellation must never activate a control.
        tap(s, s.returnRect().center());
        QCOMPARE(close.size(), 1);
    }
    void loadingFailureAndCallbackBlockPage() {
        AuthSurface s;
        QSignalSpy p(&s, &AuthSurface::touchPressed);
        tap(s, {800, 400});
        QCOMPARE(p.size(), 0);
        ready(s);
        s.setLoading(true);
        tap(s, {800, 400});
        s.setLoading(false);
        s.setFailed(true);
        tap(s, {800, 400});
        s.setFailed(false);
        s.setCallbackReached(true);
        tap(s, {800, 400});
        QCOMPARE(p.size(), 0);
    }
    void originAndCodeAreBoundedPublicLabels() {
        AuthSurface s;
        s.setOrigin("auth.openai.com");
        const auto valid = s.image();
        s.setOrigin("auth.openai.com/path?code=secret");
        QVERIFY(s.image() != valid);
        const auto rejected = s.image();
        s.setOrigin(QString(1000, 'a'));
        QCOMPARE(s.image(), rejected);
        s.setDeviceCode("ABCD-1234");
        QVERIFY(s.image() != rejected);
        s.setDeviceCode("password value");
        QCOMPARE(s.image(), rejected);
    }
    void fastNavigationInvalidatesInputEvenWhenLoadingTransitionWasMissed() {
        AuthSurface s;
        s.beginNavigation(1);
        ready(s);
        QVERIFY(s.acceptsKeys());
        QSignalSpy cancel(&s, &AuthSurface::touchesCancelled);
        s.press(1, 100, 300);
        // STARTED and FINISHED happened between snapshot polls. Only the
        // generation and final loading=false are observed by the UI.
        s.beginNavigation(2);
        s.setLoading(false);
        QCOMPARE(cancel.size(), 1);
        QVERIFY(!s.acceptsKeys());
        QVERIFY(s.image().pixelColor(800, 400) != QColor(Qt::red));
        ready(s);
        QVERIFY(s.acceptsKeys());
        s.beginNavigation(2);
        s.beginNavigation(1);
        QVERIFY(s.acceptsKeys());
        QCOMPARE(s.image().pixelColor(800, 400), QColor(Qt::red));
    }
    void keyGateRequiresCurrentInteractiveFrame() {
        AuthSurface s;
        QVERIFY(!s.acceptsKeys());
        ready(s);
        QVERIFY(s.acceptsKeys());
        s.setLoading(true);
        QVERIFY(!s.acceptsKeys());
        s.setLoading(false);
        QVERIFY(!s.acceptsKeys());
        ready(s);
        QVERIFY(s.acceptsKeys());
        s.setFailed(true);
        QVERIFY(!s.acceptsKeys());
        s.setFailed(false);
        s.setCallbackReached(true);
        QVERIFY(!s.acceptsKeys());
        s.setCallbackReached(false);
        QVERIFY(s.acceptsKeys());
        tap(s, s.returnRect().center());
        QVERIFY(!s.acceptsKeys());
    }
    void networkCompletionCannotActivateOldFrame() {
        AuthSurface s;
        ready(s);
        QSignalSpy p(&s, &AuthSurface::touchPressed);
        s.setLoading(true);
        s.setLoading(false);
        tap(s, {800, 400});
        QCOMPARE(p.size(), 0);
        ready(s);
        tap(s, {800, 400});
        QCOMPARE(p.size(), 1);
    }
    void renderPublicFixture() {
        const auto directory = qEnvironmentVariable("RMWEB_AUTH_SURFACE_FIXTURE_DIR");
        if (directory.isEmpty())
            return;
        AuthSurface s;
        s.setOrigin("auth.openai.com");
        s.setDeviceCode("ABCD-1234");
        QVERIFY(s.image().save(directory + "/auth-loading.png"));
        QImage frame(s.contentSize(), QImage::Format_RGB32);
        frame.fill(Qt::white);
        s.setFrame(frame);
        s.setLoading(false);
        QVERIFY(s.image().save(directory + "/auth-page.png"));
        s.setCallbackReached(true);
        QVERIFY(s.image().save(directory + "/auth-return.png"));
    }
    void explicitCancellationAllowsNewContactWithoutOldRelease() {
        AuthSurface s;
        ready(s);
        QSignalSpy p(&s, &AuthSurface::touchPressed), r(&s, &AuthSurface::touchReleased),
            c(&s, &AuthSurface::touchesCancelled);
        s.press(2, 100, 300);
        s.cancelTouches();
        s.press(2, 120, 320);
        s.release(2, 120, 320);
        QCOMPARE(p.size(), 2);
        QCOMPARE(r.size(), 1);
        QCOMPARE(c.size(), 1);
    }
    void stateChangeCancelsInFlightTouch() {
        AuthSurface s;
        ready(s);
        QSignalSpy cancel(&s, &AuthSurface::touchesCancelled),
            released(&s, &AuthSurface::touchReleased);
        s.press(42, 100, 300);
        s.setLoading(true);
        s.release(42, 100, 300);
        QCOMPARE(cancel.size(), 1);
        QCOMPARE(released.size(), 0);
    }
};
QTEST_GUILESS_MAIN(AuthSurfaceTest)
#include "auth_surface_test.moc"
