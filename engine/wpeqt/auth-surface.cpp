#include "auth-surface.h"
#include <QPainter>
#include <QRegularExpression>
#include <algorithm>

namespace rmweb {
namespace {
constexpr int HeaderHeight = 160;
// Hand-drawn 5x7 glyph rows: no runtime font service or text input state.
const char *glyph(char c) {
    switch (c) {
#define G(c, rows)                                                                                 \
    case c:                                                                                        \
        return rows
        G('A', "0e11111f111111");
        G('B', "1e11111e11111e");
        G('C', "0e11101010110e");
        G('D', "1e11111111111e");
        G('E', "1f10101e10101f");
        G('F', "1f10101e101010");
        G('G', "0e11101711110f");
        G('H', "1111111f111111");
        G('I', "0e04040404040e");
        G('J', "0702020202120c");
        G('K', "11121418141211");
        G('L', "1010101010101f");
        G('M', "111b1515111111");
        G('N', "11191513111111");
        G('O', "0e11111111110e");
        G('P', "1e11111e101010");
        G('Q', "0e11111115120d");
        G('R', "1e11111e141211");
        G('S', "0f10100e01011e");
        G('T', "1f040404040404");
        G('U', "1111111111110e");
        G('V', "11111111110a04");
        G('W', "11111115151b11");
        G('X', "11110a040a1111");
        G('Y', "11110a04040404");
        G('Z', "1f01020408101f");
        G('a', "00000e010f110f");
        G('b', "1010161911111e");
        G('c', "00000e1110110e");
        G('d', "01010d1311110f");
        G('e', "00000e111f100e");
        G('f', "0609081c080808");
        G('g', "00000f110f010e");
        G('h', "10101619111111");
        G('i', "04000c0404040e");
        G('j', "0200060202120c");
        G('k', "101011121c1211");
        G('l', "0c04040404040e");
        G('m', "00001a15151515");
        G('n', "00001619111111");
        G('o', "00000e1111110e");
        G('p', "00001e111e1010");
        G('q', "00000f110f0101");
        G('r', "00001619101010");
        G('s', "00000f100e011e");
        G('t', "08081c08080906");
        G('u', "0000111111130d");
        G('v', "00001111110a04");
        G('w', "0000111115150a");
        G('x', "0000110a040a11");
        G('y', "000011110f010e");
        G('z', "00001f0204081f");
        G('0', "0e11131519110e");
        G('1', "040c040404040e");
        G('2', "0e11010204081f");
        G('3', "1e01010e01011e");
        G('4', "02060a121f0202");
        G('5', "1f10101e01011e");
        G('6', "0e10101e11110e");
        G('7', "1f010204080808");
        G('8', "0e11110e11110e");
        G('9', "0e11110f01010e");
        G('.', "00000000000c0c");
        G(',', "000000000c0c08");
        G('-', "0000001f000000");
        G('_', "0000000000001f");
        G('!', "04040404040004");
        G('?', "0e110102040004");
        G(':', "000c0c000c0c00");
        G(';', "000c0c000c0c08");
        G('/', "01010204081010");
        G('\\', "10100804020101");
        G('|', "04040404040404");
        G('(', "02040808080402");
        G(')', "08040202020408");
        G('[', "0e08080808080e");
        G(']', "0e02020202020e");
        G('{', "02040408040402");
        G('}', "08040402040408");
        G('<', "01020408040201");
        G('>', "10080402040810");
        G('=', "00001f001f0000");
        G('+', "0004041f040400");
        G('*', "00150e1f0e1500");
        G('#', "0a0a1f0a1f0a0a");
        G('@', "0e11171516100f");
        G('$', "040f140e051e04");
        G('%', "18190204081303");
        G('&', "0c12140c15120d");
        G('^', "040a1100000000");
        G('~', "00000916000000");
        G('\'', "04040800000000");
        G('"', "0a0a1400000000");
        G('`', "08040200000000");
#undef G
    default:
        return "00000000000000";
    }
}
int hex(char c) {
    return c <= '9' ? c - '0' : c - 'a' + 10;
}
void label(QPainter &p, const QString &text, QRect bounds, int scale = 5) {
    const QByteArray ascii = text.toLatin1();
    const int width = int(ascii.size()) * 6 * scale - scale;
    const QPoint origin(bounds.center().x() - width / 2, bounds.center().y() - 7 * scale / 2);
    p.save();
    p.setClipRect(bounds);
    for (int i = 0; i < ascii.size(); ++i) {
        const char *rows = glyph(ascii.at(i));
        for (int y = 0; y < 7; ++y) {
            const int bits = hex(rows[y * 2]) * 16 + hex(rows[y * 2 + 1]);
            for (int x = 0; x < 5; ++x)
                if (bits & (1 << (4 - x)))
                    p.fillRect(origin.x() + (i * 6 + x) * scale, origin.y() + y * scale, scale,
                               scale, Qt::black);
        }
    }
    p.restore();
}
void button(QPainter &p, QRect rect, const QString &text, int scale = 5) {
    p.setBrush(Qt::white);
    p.setPen(QPen(Qt::black, 2));
    p.drawRoundedRect(rect.adjusted(5, 5, -5, -5), 12, 12);
    label(p, text, rect.adjusted(8, 8, -8, -8), scale);
}
} // namespace
AuthSurface::AuthSurface(QSize size, QObject *parent) : QObject(parent), m_size(size) {
    if (size != QSize(1620, 2160))
        m_size = {1620, 2160};
}
QRect AuthSurface::contentRect() const {
    return {0, HeaderHeight, m_size.width(), m_size.height() - HeaderHeight};
}
QSize AuthSurface::contentSize() const {
    return contentRect().size();
}
QRect AuthSurface::returnRect() const {
    return {18, 24, 290, 100};
}
QRect AuthSurface::passkeyCancelRect() const {
    return {m_size.width() / 2 - 260, 1680, 520, 110};
}
void AuthSurface::setPasskeyPrompt(const AuthPasskeyPrompt &prompt) {
    if (m_closing) return;
    if (prompt.active != m_passkey.active) cancelTouches();
    m_passkey = prompt;
    if (!prompt.active) m_passkeyCancelling = false;
    Q_EMIT repaintRequested();
}
bool AuthSurface::pageReady() const {
    return !m_frame.isNull() && !m_waitingForFrame && !m_loading && !m_failed && !m_callback &&
           !m_closing && !m_passkey.active;
}
bool AuthSurface::acceptsKeys() const {
    return pageReady();
}
QImage AuthSurface::image() const {
    QImage output(m_size, QImage::Format_RGBA8888);
    output.fill(Qt::white);
    QPainter p(&output);
    if (!m_frame.isNull())
        p.drawImage(contentRect().topLeft(), m_frame);
    button(p, returnRect(), m_callback ? QStringLiteral("Return") : QStringLiteral("Cancel"));
    QString host = m_origin;
    if (host.size() > 48)
        host = host.left(22) + QStringLiteral("...") + host.right(23);
    label(p, host, {330, 18, 900, 36}, 3);
    if (!m_deviceCode.isEmpty())
        label(p, QStringLiteral("Code: ") + m_deviceCode, {330, 56, 900, 30}, 2);
    const QString status = m_passkey.active ? QStringLiteral("Phone passkey")
                           : m_callback  ? QStringLiteral("Return")
                           : m_failed  ? QStringLiteral("Unable to load")
                           : m_loading ? QStringLiteral("Loading...")
                                       : QStringLiteral("Sign in securely");
    label(p, status, {330, 98, 900, 38}, 3);
    p.setPen(QPen(Qt::black, 2));
    p.drawLine(0, HeaderHeight - 1, m_size.width(), HeaderHeight - 1);
    if (m_callback || m_failed || m_frame.isNull()) {
        QRect card(m_size.width() / 2 - 580, contentRect().center().y() - 110, 1160, 220);
        p.setBrush(Qt::white);
        p.drawRoundedRect(card, 20, 20);
        const QString text = m_callback ? QStringLiteral("Callback received")
                             : m_failed ? QStringLiteral("Unable to load sign-in page")
                                        : QStringLiteral("Loading sign-in page...");
        label(p, text, card.adjusted(10, 16, -10, -100), 5);
        label(p,
              m_callback ? QStringLiteral("Return to the app to continue")
              : m_failed ? QStringLiteral("Close and try again")
                         : QStringLiteral("Cancel is always available"),
              card.adjusted(10, 110, -10, -10), 4);
    }
    if (m_passkey.active) {
        p.fillRect(contentRect(), Qt::white);
        label(p, QStringLiteral("Use a passkey from your phone"), {90, 260, 1440, 80}, 5);
        // Show the whole verified RP ID; never hide its middle with an ellipsis.
        const QString &site = m_passkey.relyingParty;
        for (int start = 0; start < site.size(); start += 72)
            label(p, site.mid(start, 72), {90, 352 + (start / 72) * 32, 1440, 32}, 3);
        if (!m_passkey.qr.isNull() && !m_passkeyCancelling) {
            const int scale = 960 / m_passkey.qr.width();
            const int side = m_passkey.qr.width() * scale;
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(QRect((m_size.width() - side) / 2, 500 + (960 - side) / 2, side, side), m_passkey.qr);
        }
        label(p, m_passkeyCancelling ? QStringLiteral("Cancelling...") : m_passkey.status,
              {90, 1490, 1440, 70}, 5);
        label(p, QStringLiteral("Keep your phone nearby with Bluetooth on"), {90, 1570, 1440, 55}, 3);
        button(p, passkeyCancelRect(), QStringLiteral("Cancel passkey"), 4);
    }
    return output;
}
void AuthSurface::beginNavigation(quint64 generation) {
    if (generation <= m_navigationGeneration || m_closing)
        return;
    cancelTouches();
    m_navigationGeneration = generation;
    m_frame = {};
    m_waitingForFrame = true;
    Q_EMIT repaintRequested();
}
void AuthSurface::setFrame(const QImage &frame) {
    if (frame.isNull() || frame.size() != contentSize() || m_closing)
        return;
    m_frame = frame;
    m_waitingForFrame = false;
    Q_EMIT repaintRequested();
}
void AuthSurface::setLoading(bool loading) {
    if (m_loading == loading)
        return;
    if (loading) {
        cancelTouches();
        m_waitingForFrame = true;
    }
    m_loading = loading;
    Q_EMIT repaintRequested();
}
void AuthSurface::setFailed(bool failed) {
    if (m_failed == failed)
        return;
    if (failed)
        cancelTouches();
    m_failed = failed;
    Q_EMIT repaintRequested();
}
void AuthSurface::setCallbackReached(bool reached) {
    if (m_callback == reached)
        return;
    cancelTouches();
    m_callback = reached;
    Q_EMIT repaintRequested();
}
void AuthSurface::setOrigin(const QString &host) {
    static const QRegularExpression domain(
        QStringLiteral("\\A[a-zA-Z0-9](?:[a-zA-Z0-9.-]{0,251}[a-zA-Z0-9])?\\z"));
    const QString value = host.size() <= 253 && domain.match(host).hasMatch() ? host : QString();
    if (value == m_origin)
        return;
    m_origin = value;
    Q_EMIT repaintRequested();
}
void AuthSurface::setDeviceCode(const QString &code) {
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Z0-9-]{1,64}\\z"));
    const QString value = pattern.match(code).hasMatch() ? code : QString();
    if (value == m_deviceCode)
        return;
    m_deviceCode = value;
    Q_EMIT repaintRequested();
}
AuthSurface::Contact AuthSurface::targetAt(QPoint point) const {
    Contact c;
    c.start = point;
    if (!QRect(QPoint(), m_size).contains(point))
        return c;
    if (returnRect().contains(point)) {
        c.target = Return;
        return c;
    }
    if (m_passkey.active && !m_passkeyCancelling && passkeyCancelRect().contains(point)) {
        c.target = PasskeyCancel;
        return c;
    }
    if (!pageReady())
        return c;
    if (contentRect().contains(point)) {
        c.target = Page;
        return c;
    }
    return c;
}
void AuthSurface::cancelTouches() {
    bool page = false;
    for (const auto &c : m_contacts)
        page |= c.target == Page && !c.cancelled;
    m_contacts.clear();
    if (page)
        Q_EMIT touchesCancelled();
}
void AuthSurface::press(int id, int x, int y) {
    if (id >= 0)
        pressContact(id, x, y);
}
// Raw finger IDs are nonnegative. The pen's internal ID cannot collide with
// them, and simultaneous pen/finger input uses the same cancellation policy.
void AuthSurface::penPress(int x, int y) { pressContact(-1, x, y); }
void AuthSurface::penMove(int x, int y) { move(-1, x, y); }
void AuthSurface::penRelease(int x, int y) { release(-1, x, y); }
void AuthSurface::pressContact(int id, int x, int y) {
    if (m_closing || m_contacts.contains(id))
        return;
    if (!m_contacts.isEmpty()) {
        const auto ids = m_contacts.keys();
        cancelTouches();
        Contact blocked;
        blocked.cancelled = true;
        for (int oldId : ids)
            m_contacts.insert(oldId, blocked);
        if (m_contacts.size() < 16)
            m_contacts.insert(id, blocked);
        return;
    }
    const Contact c = targetAt({x, y});
    m_contacts.insert(id, c);
    if (c.target == Page)
        Q_EMIT touchPressed(id, x, y - HeaderHeight);
}
void AuthSurface::move(int id, int x, int y) {
    auto it = m_contacts.find(id);
    if (it == m_contacts.end() || it->cancelled)
        return;
    if (it->target == Page) {
        if (!contentRect().contains({x, y}) || !pageReady()) {
            cancelTouches();
            return;
        }
        Q_EMIT touchMoved(id, x, y - HeaderHeight);
    } else if ((it->start - QPoint(x, y)).manhattanLength() > 40)
        it->cancelled = true;
}
void AuthSurface::release(int id, int x, int y) {
    auto it = m_contacts.find(id);
    if (it == m_contacts.end())
        return;
    const Contact c = *it;
    m_contacts.erase(it);
    if (c.cancelled || m_closing)
        return;
    const Contact end = targetAt({x, y});
    if (c.target == Page) {
        if (end.target == Page)
            Q_EMIT touchReleased(id, x, y - HeaderHeight);
        else
            Q_EMIT touchesCancelled();
        return;
    }
    if (c.target != end.target || (c.start - QPoint(x, y)).manhattanLength() > 40)
        return;
    if (c.target == Return) {
        cancelTouches();
        m_closing = true;
        Q_EMIT closeRequested();
    } else if (c.target == PasskeyCancel) {
        cancelTouches();
        m_passkeyCancelling = true;
        m_passkey.qr = {};
        Q_EMIT repaintRequested();
        Q_EMIT passkeyCancelRequested();
    }
}
} // namespace rmweb
