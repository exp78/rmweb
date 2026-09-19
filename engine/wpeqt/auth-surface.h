#pragma once
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include "auth-passkey.h"

namespace rmweb {
// Private authentication chrome. No text-field model, history, fonts or QPA.
class AuthSurface : public QObject {
    Q_OBJECT
public:
    explicit AuthSurface(QSize size = {1620, 2160}, QObject *parent = nullptr);
    QImage image() const;
    QSize contentSize() const;
    bool acceptsKeys() const;
    QRect contentRect() const;
    QRect returnRect() const;
    QRect passkeyCancelRect() const;
    void setPasskeyPrompt(const AuthPasskeyPrompt &prompt);
    void beginNavigation(quint64 generation);
    void setFrame(const QImage &frame);
    void setLoading(bool loading);
    void setFailed(bool failed);
    void setCallbackReached(bool reached);
    void setOrigin(const QString &host);
    void setDeviceCode(const QString &code);
    void press(int id, int x, int y);
    void move(int id, int x, int y);
    void release(int id, int x, int y);
    void penPress(int x, int y);
    void penMove(int x, int y);
    void penRelease(int x, int y);
    void cancelTouches();
Q_SIGNALS:
    void repaintRequested();
    void closeRequested();
    void passkeyCancelRequested();
    void touchPressed(int id, int x, int y);
    void touchMoved(int id, int x, int y);
    void touchReleased(int id, int x, int y);
    void touchesCancelled();

private:
    enum Target { None, Page, Return, PasskeyCancel };
    struct Contact {
        Target target = None;
        QPoint start;
        bool cancelled = false;
    };
    Contact targetAt(QPoint point) const;
    void pressContact(int id, int x, int y);
    bool pageReady() const;
    QSize m_size;
    QImage m_frame;
    quint64 m_navigationGeneration = 0;
    QHash<int, Contact> m_contacts;
    bool m_loading = true;
    bool m_waitingForFrame = true;
    bool m_failed = false;
    bool m_callback = false;
    bool m_closing = false;
    QString m_origin;
    QString m_deviceCode;
    AuthPasskeyPrompt m_passkey;
    bool m_passkeyCancelling = false;
};
} // namespace rmweb
