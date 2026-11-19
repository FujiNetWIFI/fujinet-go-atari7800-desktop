/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * One painted widget over the session's layout (a7800session_controls_layout,
 * in layout units; a7800session_controls_hit says what a click landed on), so
 * every frontend shows the same parts in the same places: a view per port that
 * follows what is plugged in -- the ProLine's black slab with its grey stick
 * and two red buttons, the 2600's CX40, the XG-1 light gun's silhouette or an
 * empty socket -- over the console's front panel with its silver band, its
 * PAUSE / SELECT / RESET keys and the two difficulty slides. The colours are
 * the A7800SESSION_*_RGB defines, read off photographs of the real thing
 * (controllers-port/DESIGN-7800.md).
 *
 * Gestures act on mouse press AND release, not "clicked": the console scans its
 * inputs once a frame, so a press present only for the instant of a click would
 * fall between frames. A button or a console key is HELD while the mouse is
 * down on it (dragging off lets go, back on holds again); dragging the stick
 * holds one of eight directions (a7800session_pointer_set from the vector to
 * the stick's centre) and releasing lets go; a difficulty slide flips on the
 * click. Everything the mouse holds is let go on release, on losing the focus,
 * on hiding, on a port changing type and on entering Map. A hold whose release
 * was lost (a mouse move with no button down) is let go too.
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit with the HELD ring and a fill, so the window doubles as an input tester
 * for a new pad. Keys typed with the window active go to the console as in the
 * main window. A light gun's view only has captions: the picture is its target.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per layout unit (every
 * type has the layout's one extent), Map legends are fitted into their parts and
 * gamepad names are elided, so neither Map mode nor a long name nor a port's
 * type resizes it. The timers run only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a part (a button, a console key,
 * a side of a stick's dish or the two system buttons below), then press a host
 * key or a gamepad button. Rebinding steals the key from whatever held it
 * (named in the hint). Escape cancels; while Map is armed nothing is typed into
 * the console. F9, F11 and F12 cannot be bound.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QCheckBox>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSignalBlocker>
#include <QRadialGradient>
#include <QToolTip>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../help/HelpWindow.h"

namespace {

constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.4;        /* an unpicked part while Map waits for a key */
constexpr qreal kUnit = 17.0;           /* pixels per layout unit */
constexpr int kMargin = 12;             /* round the drawing */
constexpr unsigned kDirMask = (1u << A7800_ACT_UP) | (1u << A7800_ACT_DOWN) | (1u << A7800_ACT_LEFT) | (1u << A7800_ACT_RIGHT);

/* The XG-1's plastic, read from reference/atari/Atari_XG-1_light_gun.jpg. */
constexpr unsigned kGunBodyRgb = 0xB7B3A9;
constexpr unsigned kGunShadeRgb = 0x8E8A81;
constexpr unsigned kGunTriggerRgb = 0x5B5F64;

QColor rgb(unsigned value) { return QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF); }

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont pixelFont(QFont fnt, qreal pixels, bool bold, bool italic = false)
{
    fnt.setPixelSize(qMax(1, qRound(pixels)));
    fnt.setBold(bold);
    fnt.setItalic(italic);
    return fnt;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont fnt, int align = Qt::AlignCenter)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(fnt);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const int size = fnt.pixelSize();
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5)
            break;
        fnt.setPixelSize(size - 1);
    }
    pnt.setFont(fnt);
    pnt.drawText(rect, align, text);
}

/* Long key names shortened to fit a part (tooltips keep the full name). */
QString shorten(QString text)
{
    text.replace(QStringLiteral("Keypad "), QStringLiteral("KP "));
    text.replace(QStringLiteral("Left "), QStringLiteral("L "));
    text.replace(QStringLiteral("Right "), QStringLiteral("R "));
    text.replace(QStringLiteral("Backspace"), QStringLiteral("BkSp"));
    text.replace(QStringLiteral("Escape"), QStringLiteral("Esc"));
    text.replace(QStringLiteral("Return"), QStringLiteral("Ret"));
    text.replace(QStringLiteral("Enter"), QStringLiteral("Ent"));
    return text;
}

QString keyText(a7800session *session, int target)
{
    const a7800_binding bnd = a7800session_binding_get(session, target);
    char key[48] = "";
    if (bnd.keysym) a7800session_keysym_name(bnd.keysym, key, sizeof key);
    return local(key);
}

QString padText(a7800session *session, int target)
{
    const a7800_binding bnd = a7800session_binding_get(session, target);
    return bnd.button == A7800_PAD_BTN_NONE ? QString() : local(a7800_pad_button_name(bnd.button));
}

} // namespace

/* ---- ControllersHint --------------------------------------------------------------- */

/* A label that never widens the window: it wraps into a fixed height. */
class ControllersHint : public QWidget {
public:
    ControllersHint(bool dim, int lines) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QString text() const { return m_text; }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            pnt.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ControllersStage: the controllers and the console ------------------------------ */

class ControllersStage : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target) */
    ControllersStage(a7800session *session, std::function<void(int)> onPick)
        : m_session(session), m_onPick(std::move(onPick))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Rebuilds the layout when a port's effective type changed. True if it did. */
    bool reload()
    {
        const int left = a7800session_view_type(m_session, 0), right = a7800session_view_type(m_session, 1);
        if (m_lay && left == m_type[0] && right == m_type[1]) return false;
        releaseAll();
        load();
        return true;
    }

    /* Lets go of everything the mouse holds (also on focus loss, Map entry and hide). */
    void releaseAll()
    {
        a7800session_release_pointer(m_session);
        m_hold = None;
        m_holdIdx = -1;
        m_holdMask = 0;
        pollHeld();
        update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* What the console sees held (keyboard, gamepads, the mouse) and where the
     * slides are; repaints only on a change. */
    void pollHeld()
    {
        reload();
        bool changed = false;
        for (int port = 0; port < A7800_PORTS; ++port) {
            const unsigned dirs = a7800session_buttons_held(m_session, port) & kDirMask;
            if (dirs != m_dirs[port]) {
                m_dirs[port] = dirs;
                changed = true;
            }
        }
        for (int idx = 0; idx < m_count; ++idx) {
            const a7800_control &ctl = m_lay[idx];
            char held;
            if (ctl.kind == A7800_CTL_STICK) {
                held = m_dirs[ctl.port] ? 1 : 0;
            } else if (ctl.kind == A7800_CTL_ROUND_BUTTON || ctl.kind == A7800_CTL_BUTTON) {
                held = a7800session_target_held(m_session, ctl.target) ? 1 : 0;
            } else if (ctl.kind == A7800_CTL_SWITCH) {
                held = a7800session_switch_get(m_session, ctl.target - A7800_TARGET_SWITCH(0)) ? 1 : 0;
            } else {
                continue;
            }
            if (held != m_held[(size_t)idx]) {
                m_held[(size_t)idx] = held;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        /* opaque: Qt's Format_RGB32 grabs carry no alpha (the KDE transparency trap) */
        pnt.fillRect(rect(), palette().color(QPalette::Window));
        paintCables(pnt);
        for (int idx = 0; idx < m_count; ++idx) {
            const a7800_control &ctl = m_lay[idx];
            pnt.save();
            switch (ctl.kind) {
            case A7800_CTL_CAPTION: paintCaption(pnt, idx); break;
            case A7800_CTL_BODY: paintBody(pnt, idx); break;
            case A7800_CTL_STICK: paintStick(pnt, idx); break;
            case A7800_CTL_ROUND_BUTTON: paintRoundButton(pnt, idx); break;
            case A7800_CTL_BUTTON: paintKey(pnt, idx); break;
            case A7800_CTL_SWITCH: paintSlide(pnt, idx); break;
            default: break;
            }
            pnt.restore();
            pnt.setOpacity(1.0);
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();     /* a stale hold: its release was lost */
        const QPointF pos = evt->position();
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const a7800_control &ctl = m_lay[idx];
        if (m_map != -2) {
            int target = ctl.target;
            if (ctl.kind == A7800_CTL_STICK) target = A7800_TARGET_PORT(ctl.port, sideAt(idx, pos));
            if (target >= 0) m_onPick(target);
            return;
        }
        m_holdIdx = idx;
        if (ctl.kind == A7800_CTL_STICK) {
            m_hold = Stick;
            driveStick(pos);
        } else if (ctl.kind == A7800_CTL_SWITCH) {
            /* a latching slide: it flips on the press and the release does nothing */
            a7800session_press(m_session, ctl.target, 1);
            m_holdIdx = -1;
        } else {
            m_hold = Part;
            m_partDown = true;
            a7800session_press(m_session, ctl.target, 1);
        }
        pollHeld();
        update();
    }

    /* Dragging off a part lets it go (and back on, holds it again). */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        if (m_hold != None && !(evt->buttons() & Qt::LeftButton)) {
            releaseAll();       /* the release never came (a grab lost) */
            return;
        }
        const QPointF pos = evt->position();
        const int hover = m_hold == None ? hitAt(pos) : -1;
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        if (m_hold == Stick) {
            driveStick(pos);
            pollHeld();
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                a7800session_press(m_session, m_lay[m_holdIdx].target, over ? 1 : 0);
                pollHeld();
                update();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();
        m_hover = hitAt(evt->position());
        update();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(evt);
    }

    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(evt);
            const QString tip = tipAt(help->pos());
            if (tip.isEmpty())
                QToolTip::hideText();
            else
                QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
        return QWidget::event(evt);
    }

private:
    enum Hold { None, Part, Stick };

    void load()
    {
        m_type[0] = a7800session_view_type(m_session, 0);
        m_type[1] = a7800session_view_type(m_session, 1);
        float wide = 0, tall = 0;
        m_lay = a7800session_controls_layout(m_type[0], m_type[1], &m_count, &wide, &tall);
        m_held.assign((size_t)m_count, 0);
        m_dirs[0] = m_dirs[1] = 0;
        setFixedSize(qCeil(wide * kUnit) + 2 * kMargin, qCeil(tall * kUnit) + 2 * kMargin);
        update();
    }

    QRectF rectOf(int idx) const
    {
        const a7800_control &ctl = m_lay[idx];
        return QRectF(kMargin + ctl.x * kUnit, kMargin + ctl.y * kUnit, ctl.w * kUnit, ctl.h * kUnit);
    }
    QPointF centreOf(int idx) const { return rectOf(idx).center(); }
    qreal radiusOf(int idx) const { return rectOf(idx).width() / 2; }

    /* A point in a part's own units, from its top-left. */
    static QPointF at(const QRectF &box, qreal ux, qreal uy) { return box.topLeft() + QPointF(ux, uy) * kUnit; }
    static QRectF area(const QRectF &box, qreal ux, qreal uy, qreal uw, qreal uh)
    {
        return QRectF(at(box, ux, uy), QSizeF(uw, uh) * kUnit);
    }

    int hitAt(const QPointF &pos) const
    {
        return a7800session_controls_hit(m_type[0], m_type[1], float((pos.x() - kMargin) / kUnit),
                                         float((pos.y() - kMargin) / kUnit));
    }

    /* Which direction a click on the stick's dish names (Map): the axis it is
     * further along, so the middle still picks one. */
    int sideAt(int idx, const QPointF &pos) const
    {
        const QPointF off = pos - centreOf(idx);
        if (std::abs(off.x()) > std::abs(off.y())) return off.x() < 0 ? A7800_ACT_LEFT : A7800_ACT_RIGHT;
        return off.y() < 0 ? A7800_ACT_UP : A7800_ACT_DOWN;
    }

    /* The mouse is dragging the stick of m_holdIdx: the sector it points into. */
    void driveStick(const QPointF &pos)
    {
        const a7800_control &ctl = m_lay[m_holdIdx];
        const QPointF off = pos - centreOf(m_holdIdx);
        const unsigned mask = a7800session_stick_from_vector(float(off.x()), float(off.y()), float(radiusOf(m_holdIdx)));
        if (mask == m_holdMask) return;
        m_holdMask = mask;
        a7800session_pointer_set(m_session, ctl.port, mask);
    }

    QString bound(int target) const
    {
        const QString key = keyText(m_session, target), pad = padText(m_session, target);
        QString text = key;
        if (!pad.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral(" / ")) + pad;
        return text.isEmpty() ? QStringLiteral("nothing") : text;
    }

    QString tipAt(const QPoint &pos) const
    {
        const int idx = hitAt(pos);
        if (idx < 0) return QString();
        const a7800_control &ctl = m_lay[idx];
        if (ctl.kind == A7800_CTL_STICK) {
            QStringList lines;
            lines << QStringLiteral("Player %1 stick: drag to hold a direction").arg(ctl.port + 1);
            for (int act = A7800_ACT_UP; act <= A7800_ACT_RIGHT; ++act) {
                const int target = A7800_TARGET_PORT(ctl.port, act);
                lines << QStringLiteral("%1: %2").arg(local(a7800_target_short_name(target)), bound(target));
            }
            return lines.join(QLatin1Char('\n'));
        }
        if (ctl.kind == A7800_CTL_SWITCH)
            return QStringLiteral("%1 difficulty: click to flip it (A is the pro position)\nBound to: %2")
                .arg(ctl.target == A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF) ? QStringLiteral("Left") : QStringLiteral("Right"),
                     bound(ctl.target));
        return QStringLiteral("%1\nBound to: %2").arg(local(a7800_target_name(ctl.target)), bound(ctl.target));
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or the one Map picked. */
    bool lit(int idx) const
    {
        const a7800_control &ctl = m_lay[idx];
        if (mapping()) return ctl.target >= 0 && ctl.target == m_map;
        return m_held[(size_t)idx] != 0;
    }

    /* While Map waits for a key, every part but the picked one steps back. */
    bool dimmed(const a7800_control &ctl) const
    {
        if (m_map < 0) return false;
        if (ctl.kind == A7800_CTL_STICK)
            return m_map / A7800_ACT_PER_PORT != ctl.port || m_map % A7800_ACT_PER_PORT > A7800_ACT_RIGHT;
        return ctl.target != m_map;
    }

    /* The held look: a ring in the HELD colour and a 35% fill of it. */
    void held(QPainter &pnt, const QPainterPath &shape, const QColor &fill = QColor()) const
    {
        QColor tint = fill.isValid() ? fill : rgb(A7800SESSION_HELD_RGB);
        if (!fill.isValid()) tint.setAlphaF(0.35);
        pnt.setPen(QPen(rgb(A7800SESSION_HELD_RGB), 2));
        pnt.setBrush(tint);
        pnt.drawPath(shape);
    }

    /* The stick's tilt, -1..1 each way, from what the console sees held. */
    QPointF tiltOf(int port) const
    {
        const unsigned dirs = m_dirs[port];
        double xtilt = 0, ytilt = 0;
        if (dirs & (1u << A7800_ACT_LEFT)) xtilt -= 1;
        if (dirs & (1u << A7800_ACT_RIGHT)) xtilt += 1;
        if (dirs & (1u << A7800_ACT_UP)) ytilt -= 1;
        if (dirs & (1u << A7800_ACT_DOWN)) ytilt += 1;
        if (xtilt != 0 && ytilt != 0) { xtilt *= 0.7071; ytilt *= 0.7071; }
        return QPointF(xtilt, ytilt);
    }

    /* The body entry of a port's view. */
    int bodyIndex(int port) const
    {
        for (int idx = 0; idx < m_count; ++idx)
            if (m_lay[idx].kind == A7800_CTL_BODY && m_lay[idx].port == port) return idx;
        return -1;
    }

    /* ---- painting: shared ---- */

    /* The ProLine's cable leaves the top of its case; it is under the case. */
    void paintCables(QPainter &pnt)
    {
        for (int port = 0; port < A7800_PORTS; ++port) {
            if (m_type[port] != A7800_CTRL_PROLINE) continue;
            const int body = bodyIndex(port);
            if (body < 0) continue;
            const QRectF box = rectOf(body);
            pnt.setPen(QPen(rgb(A7800SESSION_CABLE_RGB), 0.45 * kUnit, Qt::SolidLine, Qt::RoundCap));
            pnt.drawLine(at(box, 13.5, 0.3), at(box, 13.9, -0.55));
        }
    }

    void paintCaption(QPainter &pnt, int idx)
    {
        const a7800_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QString text = local(ctl.label);
        if (ctl.device < 0 && ctl.port < 0) {            /* "7800 PROSYSTEM", on the silver band */
            pnt.setPen(rgb(A7800SESSION_BAND_TEXT_RGB));
            QFont fnt = pixelFont(font(), 12, true);
            fnt.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
            drawFitted(pnt, box, text, fnt, Qt::AlignLeft | Qt::AlignVCenter);
        } else if (ctl.device < 0) {                      /* "Player N", over its view */
            pnt.setPen(palette().color(QPalette::WindowText));
            drawFitted(pnt, box.adjusted(0, -1, 0, 3), text, pixelFont(font(), 12, true));
        } else {                                          /* a view's own words */
            pnt.setPen(QColor(Qt::gray));
            drawFitted(pnt, box.adjusted(0, 0, 0, 2), text, pixelFont(font(), 11, false));
        }
    }

    void paintBody(QPainter &pnt, int idx)
    {
        const QString label = local(m_lay[idx].label);
        if (label == QLatin1String("ProLine")) paintProlineBody(pnt, idx);
        else if (label == QLatin1String("CX40")) paintCx40Body(pnt, idx);
        else if (label == QLatin1String("XG-1")) paintGun(pnt, idx);
        else if (label == QLatin1String("Socket")) paintSocket(pnt, idx);
        else if (label == QLatin1String("Console")) paintConsole(pnt, idx);
        else if (label == QLatin1String("Band")) paintBand(pnt, idx);
        else if (label == QLatin1String("Rainbow")) paintRainbow(pnt, idx);
        else if (label == QLatin1String("POWER")) paintPower(pnt, idx);
    }

    /* ---- the ProLine (DESIGN-7800.md section 1) ---- */

    void paintProlineBody(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        QPainterPath shape;
        shape.addRoundedRect(box, 0.5 * kUnit, 0.5 * kUnit);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A7800SESSION_CASE_SHADOW_RGB));
        pnt.drawPath(shape.translated(0, 2));
        QLinearGradient grad(box.topLeft(), box.bottomRight());
        grad.setColorAt(0, rgb(A7800SESSION_CASE_RGB).lighter(115));
        grad.setColorAt(1, rgb(A7800SESSION_CASE_RGB));
        pnt.setBrush(grad);
        pnt.setPen(QPen(rgb(A7800SESSION_CASE_BEVEL_RGB), 1.5));
        pnt.drawPath(shape);
        pnt.save();
        pnt.setClipPath(shape);

        /* the raised triangle panel over the left half */
        QPolygonF panel;
        panel << at(box, 0.8, 0.2) << at(box, 7.8, 0.2) << at(box, 0.8, 7.2);
        pnt.setPen(QPen(rgb(A7800SESSION_PANEL_EDGE_RGB).lighter(190), 1.5));
        pnt.setBrush(rgb(A7800SESSION_CASE_RGB).lighter(122));
        pnt.drawPolygon(panel);

        /* the two buttons' ribbed swooshes, running down-right off the recess */
        static const double centres[2] = { 8.4, 12.4 };
        for (double centre : centres) {
            pnt.setPen(QPen(rgb(A7800SESSION_SWOOSH_RGB), 2.4 * kUnit, Qt::SolidLine, Qt::RoundCap));
            pnt.drawLine(at(box, centre + 0.4, 5.2 + 0.2), at(box, centre + 3.0, 5.2 + 2.8));
            pnt.setPen(QPen(rgb(A7800SESSION_CASE_SHADOW_RGB), 0.1 * kUnit));
            pnt.drawLine(at(box, centre + 1.3, 5.2 - 0.6), at(box, centre + 4.2, 5.2 + 2.2));
        }
        /* the recesses and the embossed 1 and 2 */
        for (int btn = 0; btn < 2; ++btn) {
            const QPointF mid = at(box, centres[btn], 5.2);
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(rgb(A7800SESSION_BUTTON_RECESS_RGB));
            pnt.drawEllipse(mid, 1.5 * kUnit, 1.5 * kUnit);
            pnt.setPen(rgb(A7800SESSION_NUMERAL_RGB));
            drawFitted(pnt, area(box, centres[btn] - 2.3, 3.0, 1.0, 0.9),
                       btn ? QStringLiteral("2") : QStringLiteral("1"), pixelFont(font(), 8, true, true));
        }

        /* the embossed fuji mark and ATARI, upper right, slanting up */
        pnt.save();
        pnt.translate(at(box, 12.0, 2.3));
        pnt.rotate(-15);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A7800SESSION_LOGO_RGB));
        for (int bar = 0; bar < 3; ++bar) {
            const qreal base = (-3.2 + bar * 0.3) * kUnit;
            QPolygonF mark;
            mark << QPointF(base, 0.5 * kUnit) << QPointF(base + 0.15 * kUnit, -0.5 * kUnit)
                 << QPointF(base + 0.3 * kUnit, -0.5 * kUnit) << QPointF(base + 0.15 * kUnit, 0.5 * kUnit);
            pnt.drawPolygon(mark);
        }
        QFont fnt = pixelFont(font(), 13, true);
        fnt.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        pnt.setFont(fnt);
        pnt.setPen(rgb(A7800SESSION_LOGO_RGB));
        pnt.drawText(QRectF(-2.0 * kUnit, -0.7 * kUnit, 4.6 * kUnit, 1.4 * kUnit), Qt::AlignCenter, QStringLiteral("ATARI"));
        pnt.restore();
        pnt.restore();
    }

    /* The grey stick in its dish, seen from above; the cap leans toward the
     * held direction and each held side tints that quadrant of the dish. */
    void paintProlineStick(QPainter &pnt, int idx)
    {
        const a7800_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QPointF mid = box.center();
        const qreal rad = box.width() / 2;
        const bool on = !mapping() && m_held[(size_t)idx] != 0;
        const bool over = m_hover == idx && m_hold == None && !mapping();
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);

        QRadialGradient dish(mid - QPointF(rad * 0.3, rad * 0.3), rad * 1.5);
        dish.setColorAt(0, rgb(A7800SESSION_DISH_RGB).lighter(over ? 130 : 112));
        dish.setColorAt(1, rgb(A7800SESSION_DISH_RGB).darker(115));
        pnt.setPen(QPen(rgb(A7800SESSION_DISH_RECESS_RGB).darker(130), 1));
        pnt.setBrush(dish);
        pnt.drawEllipse(mid, rad, rad);
        /* the cross-shaped recess: four notches */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A7800SESSION_DISH_RECESS_RGB));
        pnt.drawRect(QRectF(mid.x() - 0.45 * kUnit, mid.y() - rad * 0.88, 0.9 * kUnit, 0.7 * kUnit));
        pnt.drawRect(QRectF(mid.x() - 0.45 * kUnit, mid.y() + rad * 0.88 - 0.7 * kUnit, 0.9 * kUnit, 0.7 * kUnit));
        pnt.drawRect(QRectF(mid.x() - rad * 0.88, mid.y() - 0.45 * kUnit, 0.7 * kUnit, 0.9 * kUnit));
        pnt.drawRect(QRectF(mid.x() + rad * 0.88 - 0.7 * kUnit, mid.y() - 0.45 * kUnit, 0.7 * kUnit, 0.9 * kUnit));

        const QPointF tilt = tiltOf(ctl.port);
        if (on) {
            /* a quadrant (90 degrees) centred on each held side */
            QPainterPath tint;
            const unsigned dirs = m_dirs[ctl.port];
            const struct { unsigned bit; int centre; } sides[4] = {
                { 1u << A7800_ACT_RIGHT, 0 }, { 1u << A7800_ACT_UP, 90 },
                { 1u << A7800_ACT_LEFT, 180 }, { 1u << A7800_ACT_DOWN, 270 } };
            for (const auto &side : sides) {
                if (!(dirs & side.bit)) continue;
                QPainterPath wedge;
                wedge.moveTo(mid);
                wedge.arcTo(QRectF(mid.x() - rad, mid.y() - rad, rad * 2, rad * 2), side.centre - 45, 90);
                wedge.closeSubpath();
                tint = tint.united(wedge);
            }
            QColor fill = rgb(A7800SESSION_HELD_RGB);
            fill.setAlphaF(0.35);
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(fill);
            pnt.drawPath(tint);
        }
        /* the shaft, and the round cap on it */
        const QPointF cap = mid + tilt * 0.9 * kUnit;
        const qreal capRad = 1.0 * kUnit;
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A7800SESSION_CASE_RGB));
        pnt.drawEllipse(mid + tilt * 0.45 * kUnit + QPointF(0.15 * kUnit, 0.2 * kUnit), capRad * 0.8, capRad * 0.8);
        QRadialGradient capGrad(cap - QPointF(capRad * 0.35, capRad * 0.4), capRad * 1.5);
        capGrad.setColorAt(0, rgb(A7800SESSION_CAP_HI_RGB));
        capGrad.setColorAt(0.6, on ? rgb(A7800SESSION_HELD_RGB) : rgb(A7800SESSION_CAP_RGB));
        capGrad.setColorAt(1, (on ? rgb(A7800SESSION_HELD_RGB) : rgb(A7800SESSION_CAP_RGB)).darker(125));
        pnt.setPen(QPen(rgb(A7800SESSION_DISH_RECESS_RGB).darker(130), 1));
        pnt.setBrush(capGrad);
        pnt.drawEllipse(cap, capRad, capRad);
        if (on) {
            QPainterPath ring;
            ring.addEllipse(mid, rad, rad);
            pnt.setPen(QPen(rgb(A7800SESSION_HELD_RGB), 2));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawPath(ring);
        }
        if (showBind()) paintStickBindings(pnt, ctl.port, mid, rad);
    }

    /* ---- the CX40 (DESIGN-atari.md section 2) ---- */

    void paintCx40Body(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        QLinearGradient face(box.topLeft(), box.bottomRight());
        face.setColorAt(0, rgb(A7800SESSION_CX40_CASE_RGB).lighter(170));
        face.setColorAt(1, rgb(A7800SESSION_CX40_CASE_RGB));
        pnt.setPen(QPen(rgb(A7800SESSION_CASE_SHADOW_RGB), 0.14 * kUnit));
        pnt.setBrush(face);
        pnt.drawRoundedRect(box, 0.9 * kUnit, 0.9 * kUnit);
        pnt.setPen(QPen(rgb(A7800SESSION_CX40_BEVEL_RGB), 0.1 * kUnit));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawRoundedRect(area(box, 0.25, 0.25, 7.5, 7.5), 0.7 * kUnit, 0.7 * kUnit);

        /* the fire button's recess: a round pit joined to the ring by a neck */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A7800SESSION_CX40_RING_RGB));
        pnt.drawEllipse(at(box, 1.7, 1.7), 1.1 * kUnit, 1.1 * kUnit);
        QPolygonF neck;
        neck << at(box, 1.7 - 0.45 * 0.7071 - 0.0, 1.7 + 0.45 * 0.7071) << at(box, 1.7 + 2.2 * 0.7071 - 0.45 * 0.7071, 1.7 + 2.2 * 0.7071 + 0.45 * 0.7071)
             << at(box, 1.7 + 2.2 * 0.7071 + 0.45 * 0.7071, 1.7 + 2.2 * 0.7071 - 0.45 * 0.7071) << at(box, 1.7 + 0.45 * 0.7071, 1.7 - 0.45 * 0.7071);
        pnt.drawPolygon(neck);

        /* the ring, its held sector and its ticks */
        const QPointF centre = at(box, 4.5, 4.5);
        pnt.drawEllipse(centre, 3.0 * kUnit, 3.0 * kUnit);
        const int port = m_lay[idx].port;
        const QPointF tilt = tiltOf(port);
        if (!mapping() && m_dirs[port]) {
            const qreal angle = qRadiansToDegrees(std::atan2(-tilt.y(), tilt.x()));
            QPainterPath wedge;
            wedge.moveTo(centre);
            wedge.arcTo(QRectF(centre.x() - 3.0 * kUnit, centre.y() - 3.0 * kUnit, 6.0 * kUnit, 6.0 * kUnit), angle - 22.5, 45);
            wedge.closeSubpath();
            QColor tint = rgb(A7800SESSION_HELD_RGB);
            tint.setAlpha(110);
            pnt.setBrush(tint);
            pnt.drawPath(wedge);
        }
        pnt.setPen(QPen(rgb(A7800SESSION_CX40_TICK_RGB), 0.1 * kUnit, Qt::SolidLine, Qt::RoundCap));
        for (int tick = 0; tick < 36; ++tick) {
            const qreal angle = qDegreesToRadians(tick * 10.0);
            pnt.drawLine(at(box, 4.5 + std::cos(angle) * 2.55, 4.5 + std::sin(angle) * 2.55),
                         at(box, 4.5 + std::cos(angle) * 2.85, 4.5 + std::sin(angle) * 2.85));
        }
        pnt.setPen(rgb(A7800SESSION_CX40_BEVEL_RGB));
        drawFitted(pnt, area(box, 3.7, 1.52, 1.6, 0.5), QStringLiteral("TOP"), pixelFont(font(), 8, true));
    }

    void paintCx40Stick(QPainter &pnt, int idx)
    {
        const a7800_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QPointF mid = box.center();
        const QPointF tilt = tiltOf(ctl.port);
        const bool over = m_hover == idx && m_hold == None && !mapping();
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);

        /* the boot: ribbed rings, squashed a little toward the tilt */
        static const struct { qreal rad; unsigned shade; } rings[] = { { 2.1, 0x151515 }, { 1.7, 0x262626 }, { 1.3, 0x151515 } };
        pnt.setPen(Qt::NoPen);
        for (const auto &ring : rings) {
            pnt.setBrush(rgb(ring.shade).lighter(over ? 130 : 100));
            pnt.drawEllipse(mid + tilt * 0.3 * kUnit * (ring.rad / 2.1), ring.rad * kUnit,
                            ring.rad * kUnit * (1 - 0.08 * std::hypot(tilt.x(), tilt.y())));
        }
        /* the shaft cap: an octagon that slides up to 1.0 u toward where it is pushed */
        const QPointF cap = mid + tilt * 1.0 * kUnit;
        QPolygonF octagon;
        for (int corner = 0; corner < 8; ++corner) {
            const qreal angle = qDegreesToRadians(22.5 + corner * 45.0);
            octagon << cap + QPointF(std::cos(angle), std::sin(angle)) * 0.85 * kUnit;
        }
        pnt.setPen(QPen(rgb(A7800SESSION_CX40_BEVEL_RGB), 0.1 * kUnit));
        pnt.setBrush(QColor(0x0C, 0x0C, 0x0C));
        pnt.drawPolygon(octagon);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(QColor(255, 255, 255, 24));
        pnt.drawEllipse(cap + QPointF(-0.25, -0.28) * kUnit, 0.3 * kUnit, 0.22 * kUnit);
        if (showBind()) paintStickBindings(pnt, ctl.port, mid, 3.0 * kUnit);
    }

    /* ---- the XG-1 and the empty socket ---- */

    /* A side view of the gun, traced from the photograph (its 1270 x 1200 pixels
     * scaled into the layout box, height fitted). */
    void paintGun(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        const qreal scale = box.height() / 1120.0;
        const qreal xoff = box.left() + (box.width() - 1240.0 * scale) / 2;
        auto map = [&](qreal px, qreal py) { return QPointF(xoff + (px - 20) * scale, box.top() + (py - 30) * scale); };
        auto poly = [&](std::initializer_list<QPointF> pts) {
            QPolygonF out;
            for (const QPointF &pt : pts) out << map(pt.x(), pt.y());
            return out;
        };
        pnt.setPen(QPen(rgb(kGunShadeRgb).darker(130), 1));
        /* the barrel and the front sight */
        pnt.setBrush(rgb(kGunBodyRgb));
        pnt.drawPolygon(poly({ { 40, 100 }, { 380, 190 }, { 390, 300 }, { 40, 215 } }));
        pnt.drawRect(QRectF(map(140, 30), map(190, 105)));
        /* the receiver, with the grip under it */
        pnt.drawPolygon(poly({ { 370, 185 }, { 640, 210 }, { 1120, 330 }, { 1255, 400 }, { 1250, 520 }, { 1000, 495 },
                               { 990, 885 }, { 820, 830 }, { 860, 540 }, { 770, 470 }, { 700, 440 }, { 430, 330 }, { 380, 300 } }));
        /* the long recessed panel along its flank */
        pnt.setPen(QPen(rgb(kGunShadeRgb), 1));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawPolygon(poly({ { 540, 245 }, { 1010, 345 }, { 1190, 445 }, { 1030, 455 }, { 650, 340 } }));
        /* the grip's ribs */
        for (int rib = 0; rib < 9; ++rib) {
            const qreal ypos = 560 + rib * 36;
            pnt.drawLine(map(860 + rib * 4, ypos), map(985, ypos));
        }
        /* the trigger, dark */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(kGunTriggerRgb));
        pnt.drawPolygon(poly({ { 780, 470 }, { 860, 520 }, { 860, 600 }, { 800, 560 } }));
    }

    void paintSocket(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        QPen pen(rgb(A7800SESSION_SOCKET_RGB).lighter(180), 1.5, Qt::DashLine);
        pnt.setPen(pen);
        pnt.setBrush(rgb(A7800SESSION_SOCKET_RGB).darker(150));
        pnt.drawRoundedRect(box, 0.8 * kUnit, 0.8 * kUnit);
        /* a row of nine socket pins */
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(A7800SESSION_SOCKET_RGB).lighter(200));
        for (int pin = 0; pin < 9; ++pin)
            pnt.drawEllipse(at(box, 1.9 + pin * 1.0, 4.6), 0.22 * kUnit, 0.22 * kUnit);
    }

    /* ---- the console strip (DESIGN-7800.md section 5) ---- */

    void paintConsole(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        pnt.setPen(QPen(rgb(A7800SESSION_CASE_BEVEL_RGB), 1.5));
        pnt.setBrush(rgb(A7800SESSION_CASE_RGB));
        pnt.drawRoundedRect(box, 0.5 * kUnit, 0.5 * kUnit);
    }

    void paintBand(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, rgb(A7800SESSION_BAND_HI_RGB));
        grad.setColorAt(1, rgb(A7800SESSION_BAND_LO_RGB));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(grad);
        pnt.drawRect(box);
    }

    void paintRainbow(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        QLinearGradient grad(box.topLeft(), box.topRight());
        grad.setColorAt(0.00, rgb(A7800SESSION_RAINBOW_ORANGE_RGB));
        grad.setColorAt(0.40, rgb(A7800SESSION_RAINBOW_VIOLET_RGB));
        grad.setColorAt(0.72, rgb(A7800SESSION_RAINBOW_BLUE_RGB));
        grad.setColorAt(1.00, rgb(A7800SESSION_RAINBOW_GREEN_RGB));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(grad);
        pnt.drawRect(box);
    }

    /* POWER: a key of the console that is no target (the emulator is on). */
    void paintPower(QPainter &pnt, int idx)
    {
        const QRectF box = rectOf(idx);
        pnt.setOpacity(0.45);
        pnt.setPen(QPen(rgb(A7800SESSION_CASE_BEVEL_RGB), 1));
        pnt.setBrush(rgb(A7800SESSION_KEY_RGB));
        pnt.drawRoundedRect(box, 3, 3);
        pnt.setPen(rgb(A7800SESSION_KEY_LABEL_RGB));
        drawFitted(pnt, box, QStringLiteral("POWER"), pixelFont(font(), 9, true));
    }

    /* A key's legend: the label, or while Map / Show bindings is on the key
     * bound to it. */
    QString legend(const a7800_control &ctl) const
    {
        if (!showBind() || ctl.target < 0) return local(ctl.label);
        const QString key = shorten(keyText(m_session, ctl.target));
        return key.isEmpty() ? QStringLiteral("—") : key;
    }

    /* SELECT / RESET / PAUSE: a flat black key with a small white legend. */
    void paintKey(QPainter &pnt, int idx)
    {
        const a7800_control &ctl = m_lay[idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QRectF box = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(QPen(rgb(A7800SESSION_CASE_BEVEL_RGB), 1));
        pnt.setBrush(rgb(A7800SESSION_KEY_RGB).lighter(over ? 190 : 100));
        pnt.drawRoundedRect(box, 3, 3);
        if (on) {
            QPainterPath shape;
            shape.addRoundedRect(box, 3, 3);
            held(pnt, shape);
        }
        pnt.setPen(rgb(A7800SESSION_KEY_LABEL_RGB));
        drawFitted(pnt, box.adjusted(1, 0, -1, 0), legend(ctl), pixelFont(font(), 9, true));
    }

    /* A difficulty slide: a dark slot with a silver slider at its B (left) or A
     * (right) end, the letters either side; its position is the state. */
    void paintSlide(QPainter &pnt, int idx)
    {
        const a7800_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const bool pos = m_held[(size_t)idx] != 0;           /* 1 = A */
        const bool picked = mapping() && ctl.target == m_map;
        const bool over = m_hover == idx && m_hold == None && !mapping();
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);

        const QRectF slot = area(box, 0.8, 0.1, 3.0, 1.2);
        pnt.setPen(QPen(rgb(A7800SESSION_CASE_BEVEL_RGB), 1));
        pnt.setBrush(rgb(A7800SESSION_SLIDE_SLOT_RGB));
        pnt.drawRoundedRect(slot, 0.35 * kUnit, 0.35 * kUnit);
        const qreal knobWide = 1.4 * kUnit;
        const QRectF knob(pos ? slot.right() - knobWide - 0.1 * kUnit : slot.left() + 0.1 * kUnit, slot.top() + 0.1 * kUnit,
                          knobWide, slot.height() - 0.2 * kUnit);
        QLinearGradient grad(knob.topLeft(), knob.bottomLeft());
        grad.setColorAt(0, rgb(A7800SESSION_SLIDE_KNOB_RGB).lighter(over ? 120 : 108));
        grad.setColorAt(1, rgb(A7800SESSION_SLIDE_KNOB_RGB).darker(120));
        pnt.setPen(QPen(rgb(A7800SESSION_SLIDE_KNOB_RGB).darker(170), 1));
        pnt.setBrush(grad);
        pnt.drawRoundedRect(knob, 0.25 * kUnit, 0.25 * kUnit);
        /* the knob's ridges */
        pnt.setPen(QPen(rgb(A7800SESSION_SLIDE_KNOB_RGB).darker(160), 1));
        for (int ridge = -1; ridge <= 1; ++ridge)
            pnt.drawLine(QPointF(knob.center().x() + ridge * 0.3 * kUnit, knob.top() + 0.2 * kUnit),
                         QPointF(knob.center().x() + ridge * 0.3 * kUnit, knob.bottom() - 0.2 * kUnit));
        pnt.setPen(rgb(A7800SESSION_KEY_LABEL_RGB));
        const QFont letters = pixelFont(font(), 10, true);
        drawFitted(pnt, area(box, 0.0, 0.1, 0.8, 1.2), QStringLiteral("B"), letters);
        drawFitted(pnt, area(box, 3.8, 0.1, 0.8, 1.2), QStringLiteral("A"), letters);
        if (picked) {
            QPainterPath shape;
            shape.addRoundedRect(box.adjusted(-2, -2, 2, 2), 4, 4);
            pnt.setPen(QPen(rgb(A7800SESSION_HELD_RGB), 2));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawPath(shape);
        }
        /* its name under it, or while bindings show the key bound to it */
        const bool left = ctl.target == A7800_TARGET_SWITCH(A7800_SW_LEFT_DIFF);
        const QString name = showBind() ? shorten(keyText(m_session, ctl.target)) : (left ? QStringLiteral("LEFT DIFF") : QStringLiteral("RIGHT DIFF"));
        pnt.setPen(rgb(A7800SESSION_KEY_LABEL_RGB));
        drawFitted(pnt, area(box, -0.4, 1.4, 5.4, 0.9), showBind() && name.isEmpty() ? QStringLiteral("—") : name,
                   pixelFont(font(), 8, true));
    }

    /* ---- the stick and the round buttons ---- */

    void paintStick(QPainter &pnt, int idx)
    {
        if (m_lay[idx].device == A7800_CTRL_JOY2600) paintCx40Stick(pnt, idx);
        else paintProlineStick(pnt, idx);
    }

    void paintRoundButton(QPainter &pnt, int idx)
    {
        const a7800_control &ctl = m_lay[idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QPointF mid = centreOf(idx);
        const bool fire = ctl.device == A7800_CTRL_JOY2600;
        const qreal rad = (fire ? 0.8 : 1.25) * kUnit;
        const unsigned base = fire ? A7800SESSION_CX40_FIRE_RGB : A7800SESSION_BUTTON_RGB;
        const unsigned hi = fire ? A7800SESSION_CX40_FIRE_HI_RGB : A7800SESSION_BUTTON_HI_RGB;
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        QRadialGradient grad(mid - QPointF(rad * 0.3, rad * 0.35), rad * 1.5);
        grad.setColorAt(0, over ? rgb(hi).lighter(115) : rgb(hi));
        grad.setColorAt(1, rgb(base).darker(on ? 100 : 118));
        pnt.setPen(QPen(rgb(base).darker(230), 1));
        pnt.setBrush(on ? QBrush(rgb(A7800SESSION_HELD_BUTTON_FILL_RGB)) : QBrush(grad));
        pnt.drawEllipse(mid, rad, rad);
        if (on) {
            QPainterPath shape;
            shape.addEllipse(mid, rad, rad);
            pnt.setPen(QPen(rgb(A7800SESSION_HELD_RGB), 2));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawPath(shape);
        } else {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(255, 255, 255, 70));
            pnt.drawEllipse(mid + QPointF(-rad * 0.28, -rad * 0.38), rad * 0.42, rad * 0.26);
        }
        if (showBind()) {
            const QString key = shorten(keyText(m_session, ctl.target));
            pnt.setPen(Qt::white);
            drawFitted(pnt, QRectF(mid.x() - rad, mid.y() - rad * 0.5, rad * 2, rad), key.isEmpty() ? QStringLiteral("—") : key,
                       pixelFont(font(), 9, true));
        }
    }

    /* The four directions' keys round the dish; the picked one is lit. */
    void paintStickBindings(QPainter &pnt, int port, const QPointF &mid, qreal rad)
    {
        static const int acts[4] = { A7800_ACT_UP, A7800_ACT_RIGHT, A7800_ACT_DOWN, A7800_ACT_LEFT };
        static const QPointF dirs[4] = { QPointF(0, -1), QPointF(1, 0), QPointF(0, 1), QPointF(-1, 0) };
        for (int dir = 0; dir < 4; ++dir) {
            const int target = A7800_TARGET_PORT(port, acts[dir]);
            const QPointF spot = mid + dirs[dir] * rad * 0.62;
            const QRectF cell(spot.x() - rad * 0.34, spot.y() - rad * 0.19, rad * 0.68, rad * 0.38);
            if (mapping() && target == m_map) {
                pnt.setPen(QPen(rgb(A7800SESSION_HELD_RGB), 2));
                pnt.setBrush(QColor(0xFF, 0xB0, 0x4A, 90));
            } else {
                pnt.setPen(Qt::NoPen);
                pnt.setBrush(QColor(0, 0, 0, 160));
            }
            pnt.drawRoundedRect(cell, 3, 3);
            pnt.setPen(Qt::white);
            const QString key = shorten(keyText(m_session, target));
            drawFitted(pnt, cell, key.isEmpty() ? QStringLiteral("—") : key, pixelFont(font(), 9, true));
        }
    }

    a7800session *m_session;
    std::function<void(int)> m_onPick;
    const a7800_control *m_lay = nullptr;       /* the session's static layout table */
    int m_count = 0;
    int m_type[A7800_PORTS] = { -1, -1 };       /* the effective types drawn */
    std::vector<char> m_held;                   /* per layout entry: held, or a slide's position */
    unsigned m_dirs[A7800_PORTS] = {};          /* each port's held directions, as the console sees them */
    bool m_showBindings = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;                         /* the part the mouse holds */
    unsigned m_holdMask = 0;                    /* the stick's drag, as last sent */
    bool m_partDown = false;                    /* the pointer is still over the part it pressed */
    int m_hover = -1;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

ControllersWindow::ControllersWindow(a7800session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel of
     * fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers (F9)"));
    setProperty("a7800HelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    m_stage = new ControllersStage(session, [this](int target) { setMapState(target); });
    m_stage->setObjectName(QStringLiteral("stage"));
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    /* What is plugged into each port */
    auto *types = new QHBoxLayout;
    for (int port = 0; port < A7800_PORTS; ++port) {
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(20);
        for (int type = 0; a7800_ctrl_type_name(type); ++type) box->addItem(local(a7800_ctrl_type_name(type)));
        box->setCurrentIndex(a7800session_port_type(session, port));
        box->setToolTip(QStringLiteral("What is plugged into player %1's port. Auto follows the running game: "
                                       "the light gun for the light-gun titles, a ProLine joystick otherwise").arg(port + 1));
        connect(box, &QComboBox::currentIndexChanged, this, [this, port](int sel) {
            if (sel >= 0 && sel != a7800session_port_type(m_session, port)) a7800session_set_port_type(m_session, port, sel);
            m_stage->pollHeld();
        });
        m_type[port] = box;
        types->addWidget(new QLabel(QStringLiteral("Player %1:").arg(port + 1)));
        types->addWidget(box, 1);
        if (port == 0) types->addSpacing(12);
    }
    root->addLayout(types);
    syncTypes();

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        a7800session_binding_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_stage->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        a7800session_bindings_reset(m_session);
        for (int pad = 0, count = a7800session_gamepad_count(m_session); pad < count; pad++)
            a7800session_gamepad_assign(m_session, pad, -1);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
        rebuildPads();
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(a7800session_get_int(session, "controllers_show_bindings", 0) > 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        a7800session_set_int(m_session, "controllers_show_bindings", checked ? 1 : 0);
        m_stage->setShowBindings(checked);
    });
    m_stage->setShowBindings(bindings->isChecked());
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(bindings);
    mapRow->addStretch(1);
    mapRow->addWidget(a7800HelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    /* The session's own actions are not on the picture: they are here, firing
     * when clicked and picked for rebinding in Map. */
    auto *sysRow = new QHBoxLayout;
    static const char *const sysNames[A7800_SYSACT_COUNT] = { "Reboot to CONFIG", "Stop in debugger" };
    for (int sysact = 0; sysact < A7800_SYSACT_COUNT; ++sysact) {
        auto *button = new QPushButton(local(sysNames[sysact]));
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, [this, sysact] {
            if (m_mapState != -2) { setMapState(A7800_TARGET_SYSACT(sysact)); return; }
            a7800session_sysaction_post(m_session, sysact);
        });
        m_sysButton[sysact] = button;
        sysRow->addWidget(button);
    }
    sysRow->addStretch(1);
    root->addLayout(sysRow);

    m_hint = new ControllersHint(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { m_stage->pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    m_stage->pollHeld();
}

void ControllersWindow::present()
{
    m_padGen = a7800session_gamepad_generation(m_session);
    rebuildPads();
    syncTypes();
    show();
    raise();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    /* Entering (or changing) Map: nothing may stay held by the mouse, and the
     * keyboard's holds go too -- Map swallows key-ups, so a key held into it
     * would stick. */
    m_stage->releaseAll();
    a7800session_release_all(m_session);
    m_mapState = state;
    m_clearButton->setEnabled(state >= 0);
    for (int sysact = 0; sysact < A7800_SYSACT_COUNT; ++sysact)
        m_sysButton[sysact]->setStyleSheet(state == A7800_TARGET_SYSACT(sysact)
            ? QStringLiteral("background: %1; color: black;").arg(a7800AccentColor().name()) : QString());
    if (state == -2) {
        m_captureTimer.stop();
        a7800session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Drag a stick, press a button or a console key with the mouse to hold it; click a slide to flip it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        a7800session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: black;").arg(a7800AccentColor().name()));
        m_hint->setText(QStringLiteral("Click a part to remap it (a side of the dish for a stick direction). Escape leaves Map."));
    } else {
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(a7800_target_name(state))));
        a7800session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_stage->setMap(m_mapState);
    m_stage->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(const QString &what, const QString &stolen)
{
    setMapState(-1);
    m_hint->setText(stolen.isEmpty() ? QStringLiteral("Bound %1").arg(what)
                                     : QStringLiteral("Bound %1 (was %2)").arg(what, stolen));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!a7800session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    a7800session_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
    bound(local(a7800_pad_button_name(button)), local(stolen));
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads, auto-assignment changes and the type combos (Preferences
 * and the running game change them too). */
void ControllersWindow::pollSlow()
{
    syncTypes();
    const unsigned gen = a7800session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPlayers();
    }
}

void ControllersWindow::syncTypes()
{
    for (int port = 0; port < A7800_PORTS; ++port) {
        const int type = a7800session_port_type(m_session, port);
        const int detected = a7800session_port_detected(m_session, port);
        const QString autoText = QStringLiteral("Auto (now: %1)").arg(local(a7800_ctrl_type_name(detected)));
        if (m_type[port]->itemText(A7800_CTRL_AUTO) != autoText) m_type[port]->setItemText(A7800_CTRL_AUTO, autoText);
        if (m_type[port]->currentIndex() != type) {
            QSignalBlocker block(m_type[port]);
            m_type[port]->setCurrentIndex(type);
        }
    }
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPlayers()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int eff = a7800session_gamepad_effective_port(m_session, idx);
        const QString text = eff >= 0 ? QStringLiteral("plays Player %1").arg(eff + 1) : QStringLiteral("plays no player");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the player it plays and the one it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int count = a7800session_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        a7800session_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new ControllersHint(false, 1);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic (connection order), 0.. = that player */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
        const int assign = a7800session_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign >= A7800_PORTS ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            a7800session_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPadPlayers();
        });
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows)
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are listed in Preferences").arg(count - kMaxPadRows));
    refreshPadPlayers();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::showEvent(QShowEvent *evt)
{
    m_stage->pollHeld();
    m_heldTimer.start(40);      /* 25 Hz */
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    m_swallow = 0;
    QWidget::hideEvent(evt);
}

/* Losing the focus lets go of the mouse's holds and the keyboard's. */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        m_stage->releaseAll();
        a7800session_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

/* F9 (this window), F11 and F12 are application-wide shortcuts. While Map
 * waits for a key they are taken here instead, so the hint can say they are
 * reserved (and so nothing but Escape closes Map). */
bool ControllersWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::ShortcutOverride && m_mapState >= 0) {
        evt->accept();
        return true;
    }
    if (evt->type() == QEvent::WindowDeactivate) {
        m_stage->releaseAll();
        a7800session_release_all(m_session);
        m_swallow = 0;
    }
    return QWidget::event(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    /* F9, F11 and F12 are never bindings: in Map they only explain */
    if (evt->key() == Qt::Key_F9 || evt->key() == Qt::Key_F11 || evt->key() == Qt::Key_F12) {
        if (evt->isAutoRepeat()) return;
        if (m_mapState >= 0) m_hint->setText(QStringLiteral("F9, F11 and F12 are reserved and cannot be bound."));
        else if (evt->key() == Qt::Key_F9) hide();
        return;
    }
    const uint32_t keysym = a7800KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = keysym;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !keysym) return;      /* armed, or nothing to bind: nothing is typed */
        if (a7800session_key_reserved(keysym)) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 are reserved and cannot be bound."));
            return;
        }
        char name[48] = "", stolen[128] = "";
        a7800session_binding_set_key(m_session, m_mapState, keysym, stolen, sizeof stolen);
        a7800session_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(local(name), local(stolen));
        return;
    }
    if (evt->isAutoRepeat()) return;
    /* Otherwise what the main window does with it: a system action goes to the
     * main window's queue, as a gamepad's does. */
    if (keysym) {
        const int sysact = a7800session_key_sysaction(m_session, keysym);
        if (sysact >= 0) { a7800session_sysaction_post(m_session, sysact); return; }
        if (a7800session_key(m_session, keysym, 1)) return;
    }
    QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = a7800KeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!keysym || a7800session_key_sysaction(m_session, keysym) >= 0) return;
    if (!a7800session_key(m_session, keysym, 0)) QWidget::keyReleaseEvent(evt);
}
