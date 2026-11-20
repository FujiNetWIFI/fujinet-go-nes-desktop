/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QtMath>
#include <cstring>
#include <functional>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../help/HelpWindow.h"

namespace {

constexpr qreal kUnit = 20.0;           /* pixels per layout unit of the controllers */
constexpr qreal kMargin = 10.0;
constexpr qreal kKeyUnit = 28.0;        /* pixels per key unit of the keyboards */
constexpr qreal kDimAlpha = 0.35;       /* a part that Map is not asking about */
constexpr int kMaxPadRows = 3;

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

QString keyText(nessession *session, int target)
{
    const nes_binding bnd = nessession_binding_get(session, target);
    char key[48] = "";
    if (bnd.keysym) nessession_keysym_name(bnd.keysym, key, sizeof key);
    return local(key);
}

QString padText(nessession *session, int target)
{
    const nes_binding bnd = nessession_binding_get(session, target);
    return bnd.button == NES_PAD_BTN_NONE ? QString() : local(nes_pad_button_name(bnd.button));
}

bool isLabel(const nes_control &ctl, const char *name) { return ctl.label && std::strcmp(ctl.label, name) == 0; }

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

/* ---- ControllersStage: the pads and the console ------------------------------------- */

class ControllersStage : public QWidget {
public:
    /* onPick: a part was chosen in Map mode (its target) */
    ControllersStage(nessession *session, std::function<void(int)> onPick)
        : m_session(session), m_onPick(std::move(onPick))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        load();
    }

    /* Rebuilds the layout when a port's type changed. True if it did. */
    bool reload()
    {
        const int left = nessession_port_type(m_session, 0), right = nessession_port_type(m_session, 1);
        if (m_lay && left == m_type[0] && right == m_type[1]) return false;
        /* what the mouse held belongs to the old picture (not releaseAll: that polls, which reloads) */
        nessession_release_pointer(m_session);
        m_hold = None;
        m_holdIdx = -1;
        m_holdMask = 0;
        load();
        return true;
    }

    /* Lets go of everything the mouse holds (also on focus loss, Map entry and hide). */
    void releaseAll()
    {
        nessession_release_pointer(m_session);
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

    /* What the console sees held (keyboard, gamepads, the mouse); repaints
     * only on a change. */
    void pollHeld()
    {
        reload();
        bool changed = false;
        for (int idx = 0; idx < m_count; ++idx) {
            const nes_control &ctl = m_lay[idx];
            if (ctl.target < 0) continue;
            const char held = nessession_target_held(m_session, ctl.target) ? 1 : 0;
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
        for (int idx = 0; idx < m_count; ++idx) {
            const nes_control &ctl = m_lay[idx];
            pnt.save();
            switch (ctl.kind) {
            case NES_CTL_CAPTION: paintCaption(pnt, idx); break;
            case NES_CTL_BODY: paintBody(pnt, idx); break;
            case NES_CTL_DPAD_ARM: paintArm(pnt, idx); break;
            case NES_CTL_ROUND_BUTTON: paintRound(pnt, idx); break;
            case NES_CTL_PILL: paintPill(pnt, idx); break;
            case NES_CTL_BUTTON: paintResetKey(pnt, idx); break;
            default: break;
            }
            pnt.restore();
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        if (m_hold != None) releaseAll();     /* a stale hold: its release was lost */
        const QPointF pos = evt->position();
        const int idx = hitAt(pos);
        if (idx < 0) return;
        const nes_control &ctl = m_lay[idx];
        if (m_map != -2) {
            int target = ctl.target;
            if (ctl.kind == NES_CTL_DPAD) target = NES_TARGET_PORT(ctl.port, sideAt(idx, pos));
            if (target >= 0) m_onPick(target);
            return;
        }
        m_holdIdx = idx;
        if (ctl.kind == NES_CTL_DPAD) {
            m_hold = Dpad;
            driveDpad(pos);
        } else {
            m_hold = Part;
            m_partDown = true;
            nessession_press(m_session, ctl.target, 1);
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
        if (m_hold == Dpad) {
            driveDpad(pos);
            pollHeld();
        } else if (m_hold == Part) {
            const bool over = hitAt(pos) == m_holdIdx;
            if (over != m_partDown) {
                m_partDown = over;
                nessession_press(m_session, m_lay[m_holdIdx].target, over ? 1 : 0);
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
    enum Hold { None, Part, Dpad };

    void load()
    {
        m_type[0] = nessession_port_type(m_session, 0);
        m_type[1] = nessession_port_type(m_session, 1);
        float wide = 0, tall = 0;
        m_lay = nessession_controls_layout(m_type[0], m_type[1], &m_count, &wide, &tall);
        m_held.assign((size_t)m_count, 0);
        m_consoleIdx = -1;
        for (int idx = 0; idx < m_count; ++idx)
            if (m_lay[idx].kind == NES_CTL_BODY && isLabel(m_lay[idx], "Console")) m_consoleIdx = idx;
        setFixedSize(qCeil(wide * kUnit) + 2 * int(kMargin), qCeil(tall * kUnit) + 2 * int(kMargin));
        update();
    }

    QRectF rectOf(int idx) const
    {
        const nes_control &ctl = m_lay[idx];
        return QRectF(kMargin + ctl.x * kUnit, kMargin + ctl.y * kUnit, ctl.w * kUnit, ctl.h * kUnit);
    }
    QPointF centreOf(int idx) const { return rectOf(idx).center(); }

    int hitAt(const QPointF &pos) const
    {
        return nessession_controls_hit(m_type[0], m_type[1], float((pos.x() - kMargin) / kUnit),
                                       float((pos.y() - kMargin) / kUnit));
    }

    /* Which direction a click on the cross names (Map): the axis it is further along. */
    int sideAt(int idx, const QPointF &pos) const
    {
        const QPointF off = pos - centreOf(idx);
        if (std::abs(off.x()) > std::abs(off.y())) return off.x() < 0 ? NES_ACT_LEFT : NES_ACT_RIGHT;
        return off.y() < 0 ? NES_ACT_UP : NES_ACT_DOWN;
    }

    /* The mouse is dragging the cross of m_holdIdx: the sector it points into. */
    void driveDpad(const QPointF &pos)
    {
        const nes_control &ctl = m_lay[m_holdIdx];
        const QPointF off = (pos - centreOf(m_holdIdx)) / kUnit;
        const unsigned mask = nessession_dpad_from_vector(float(off.x()), float(off.y()));
        if (mask == m_holdMask) return;
        m_holdMask = mask;
        nessession_pointer_set(m_session, ctl.port, mask);
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
        const nes_control &ctl = m_lay[idx];
        if (ctl.kind == NES_CTL_DPAD) {
            QStringList lines;
            lines << QStringLiteral("Player %1 cross: drag to hold a direction").arg(ctl.port + 1);
            for (int act = NES_ACT_UP; act <= NES_ACT_RIGHT; ++act) {
                const int target = NES_TARGET_PORT(ctl.port, act);
                lines << QStringLiteral("%1: %2").arg(local(nes_target_short_name(target)), bound(target));
            }
            return lines.join(QLatin1Char('\n'));
        }
        return QStringLiteral("%1\nBound to: %2").arg(local(nes_target_name(ctl.target)), bound(ctl.target));
    }

    bool mapping() const { return m_map != -2; }
    bool showBind() const { return mapping() || m_showBindings; }

    /* A part's lit state: held by anyone, or the one Map picked. */
    bool lit(int idx) const
    {
        const nes_control &ctl = m_lay[idx];
        if (mapping()) return ctl.target >= 0 && ctl.target == m_map;
        return m_held[(size_t)idx] != 0;
    }

    /* While Map waits for a key, every part but the picked one steps back; and
     * while bindings show, a part with no binding of its own stays as it is. */
    bool dimmed(const nes_control &ctl) const
    {
        if (m_map < 0) return false;
        return ctl.target != m_map;
    }

    /* A key legend on a part: a dark chip, the picked one ringed in the held colour. */
    void chip(QPainter &pnt, const QRectF &box, int target)
    {
        const bool picked = mapping() && target == m_map;
        if (picked) {
            pnt.setPen(QPen(rgb(NESSESSION_HELD_RGB), 2));
            pnt.setBrush(QColor(0xFF, 0xB0, 0x4A, 90));
        } else {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(0, 0, 0, 175));
        }
        pnt.drawRoundedRect(box, 3, 3);
        pnt.setPen(Qt::white);
        const QString key = shorten(keyText(m_session, target));
        drawFitted(pnt, box.adjusted(1, 0, -1, 0), key.isEmpty() ? QStringLiteral("—") : key,
                   pixelFont(font(), 9, true));
    }

    void ring(QPainter &pnt, const QPainterPath &shape, const QColor &fill)
    {
        pnt.setPen(QPen(rgb(NESSESSION_HELD_RGB), 2));
        pnt.setBrush(fill);
        pnt.drawPath(shape);
    }

    /* ---- the art (DESIGN-nes.md) ---- */

    void paintBody(QPainter &pnt, int idx)
    {
        const nes_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        if (isLabel(ctl, "NES-001")) {
            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            grad.setColorAt(0, rgb(NESSESSION_SHELL_RGB).lighter(106));
            grad.setColorAt(1, rgb(NESSESSION_SHELL_RGB));
            pnt.setPen(QPen(rgb(NESSESSION_SHELL_SHADOW_RGB), 0.12 * kUnit));
            pnt.setBrush(grad);
            pnt.drawRoundedRect(box, 0.6 * kUnit, 0.6 * kUnit);
        } else if (isLabel(ctl, "Face")) {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(rgb(NESSESSION_FACE_RGB));
            pnt.drawRoundedRect(box, 0.3 * kUnit, 0.3 * kUnit);
        } else if (isLabel(ctl, "Stripe")) {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(rgb(NESSESSION_STRIPE_RGB));
            pnt.drawRect(box);
        } else if (isLabel(ctl, "DpadWell") || isLabel(ctl, "SelectWell") || isLabel(ctl, "ButtonWell")) {
            const qreal radius = (isLabel(ctl, "SelectWell") ? 0.4 : isLabel(ctl, "ButtonWell") ? 0.5 : 0.7) * kUnit;
            pnt.setPen(QPen(rgb(NESSESSION_SHELL_SHADOW_RGB), 0.08 * kUnit));
            pnt.setBrush(rgb(NESSESSION_SHELL_RGB));
            pnt.drawRoundedRect(box, radius, radius);
        } else if (isLabel(ctl, "Socket")) {
            pnt.setPen(QPen(rgb(NESSESSION_SOCKET_RGB), 1.5, Qt::DashLine));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawRoundedRect(box.adjusted(1, 1, -1, -1), 0.8 * kUnit, 0.8 * kUnit);
        } else if (isLabel(ctl, "Console")) {
            pnt.setPen(QPen(rgb(NESSESSION_SHELL_SHADOW_RGB), 0.1 * kUnit));
            pnt.setBrush(rgb(NESSESSION_CONSOLE_RGB));
            pnt.drawRoundedRect(box, 0.5 * kUnit, 0.5 * kUnit);
        } else if (isLabel(ctl, "Base")) {
            /* the darker lower half of the console, inside its rounded corners */
            if (m_consoleIdx >= 0) {
                QPainterPath clip;
                clip.addRoundedRect(rectOf(m_consoleIdx), 0.5 * kUnit, 0.5 * kUnit);
                pnt.setClipPath(clip);
            }
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(rgb(NESSESSION_CONSOLE_BASE_RGB));
            pnt.drawRect(box);
        } else if (isLabel(ctl, "POWER")) {
            pnt.setOpacity(0.45);
            pnt.setPen(QPen(rgb(NESSESSION_KEY_INK_RGB), 1));
            pnt.setBrush(rgb(NESSESSION_KEY_RGB));
            pnt.drawRoundedRect(box, 0.2 * kUnit, 0.2 * kUnit);
            pnt.setPen(rgb(NESSESSION_KEY_INK_RGB));
            drawFitted(pnt, box, QStringLiteral("POWER"), pixelFont(font(), 10, true));
        }
    }

    void paintCaption(QPainter &pnt, int idx)
    {
        const nes_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const QString text = local(ctl.label);
        if (isLabel(ctl, "Nintendo")) {
            pnt.setPen(rgb(NESSESSION_LEGEND_RGB));
            drawFitted(pnt, box, text, pixelFont(font(), 13, true, true));
        } else if (ctl.port >= 0 && ctl.y < 0.1f) {          /* Player 1 / Player 2 */
            pnt.setPen(palette().color(QPalette::WindowText));
            drawFitted(pnt, box, text, pixelFont(font(), 13, true));
        } else if (isLabel(ctl, "No controller")) {
            pnt.setPen(rgb(NESSESSION_SOCKET_INK_RGB));
            drawFitted(pnt, box, text, pixelFont(font(), 16, false, true));
        } else if (ctl.port < 0) {                           /* the console's name */
            pnt.setPen(rgb(NESSESSION_LEGEND_RGB));
            drawFitted(pnt, box, text, pixelFont(font(), 13, true), Qt::AlignLeft | Qt::AlignVCenter);
        } else {                                             /* SELECT, START, B, A, T */
            pnt.setPen(rgb(NESSESSION_LEGEND_RGB));
            /* SELECT and START share one size, the one SELECT fits its 1.3 u at */
            const bool legend = isLabel(ctl, "SELECT") || isLabel(ctl, "START");
            drawFitted(pnt, box, text, pixelFont(font(), legend ? 6 : 9, !legend));
        }
    }

    /* One arm of the cross; the last one also seats the centre and its dimple. */
    void paintArm(QPainter &pnt, int idx)
    {
        const nes_control &ctl = m_lay[idx];
        const QRectF box = rectOf(idx);
        const bool on = lit(idx);
        const bool over = m_hover >= 0 && m_lay[m_hover].kind == NES_CTL_DPAD && m_lay[m_hover].port == ctl.port
                          && m_hold == None && !mapping();
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(NESSESSION_DPAD_RGB).lighter(over ? 150 : 100));
        pnt.drawRoundedRect(box, 0.15 * kUnit, 0.15 * kUnit);
        if (isLabel(ctl, "Right")) {
            const QPointF mid(box.left(), box.center().y());
            pnt.drawRect(QRectF(mid.x() - 0.65 * kUnit, mid.y() - 0.65 * kUnit, 1.3 * kUnit, 1.3 * kUnit));
            pnt.setBrush(rgb(NESSESSION_DPAD_DIMPLE_RGB));
            pnt.drawEllipse(mid, 0.45 * kUnit, 0.45 * kUnit);
        }
        /* the embossed triangle near the arm's end, pointing outward */
        const QPointF tip = box.center() + QPointF(isLabel(ctl, "Right") ? 0.45 : isLabel(ctl, "Left") ? -0.45 : 0,
                                                    isLabel(ctl, "Down") ? 0.45 : isLabel(ctl, "Up") ? -0.45 : 0) * kUnit;
        const QPointF out = (isLabel(ctl, "Right") ? QPointF(1, 0) : isLabel(ctl, "Left") ? QPointF(-1, 0)
                             : isLabel(ctl, "Down") ? QPointF(0, 1) : QPointF(0, -1));
        const QPointF side(-out.y(), out.x());
        QPolygonF tri;
        tri << tip + out * 0.2 * kUnit << tip - out * 0.15 * kUnit + side * 0.25 * kUnit
            << tip - out * 0.15 * kUnit - side * 0.25 * kUnit;
        pnt.setBrush(rgb(NESSESSION_DPAD_MARK_RGB));
        pnt.drawPolygon(tri);
        if (on) {
            QColor tint = rgb(NESSESSION_DPAD_HELD_RGB);
            tint.setAlphaF(0.55);
            pnt.setBrush(tint);
            pnt.drawRoundedRect(box, 0.15 * kUnit, 0.15 * kUnit);
            pnt.setPen(QPen(rgb(NESSESSION_HELD_RGB), 1.5));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawRoundedRect(box.adjusted(1, 1, -1, -1), 0.15 * kUnit, 0.15 * kUnit);
        }
        /* the key chips of all four arms go on last, over the seat of the cross */
        if (showBind() && isLabel(ctl, "Right")) {
            for (int other = 0; other < m_count; ++other) {
                const nes_control &arm = m_lay[other];
                if (arm.kind != NES_CTL_DPAD_ARM || arm.port != ctl.port) continue;
                pnt.setOpacity(dimmed(arm) ? kDimAlpha : 1.0);
                const QRectF armBox = rectOf(other);
                chip(pnt, QRectF(armBox.center().x() - 0.6 * kUnit, armBox.center().y() - 0.35 * kUnit,
                                 1.2 * kUnit, 0.7 * kUnit), arm.target);
            }
        }
    }

    void paintRound(QPainter &pnt, int idx)
    {
        const nes_control &ctl = m_lay[idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QPointF mid = centreOf(idx);
        const qreal rad = rectOf(idx).width() / 2;
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        QRadialGradient grad(mid - QPointF(rad * 0.3, rad * 0.35), rad * 1.5);
        grad.setColorAt(0, over ? rgb(NESSESSION_BUTTON_LIGHT_RGB).lighter(112) : rgb(NESSESSION_BUTTON_LIGHT_RGB));
        grad.setColorAt(1, rgb(NESSESSION_BUTTON_RGB).darker(112));
        pnt.setPen(QPen(rgb(NESSESSION_BUTTON_RGB).darker(230), 1));
        pnt.setBrush(on ? QBrush(rgb(NESSESSION_HELD_FILL_RGB)) : QBrush(grad));
        pnt.drawEllipse(mid, rad, rad);
        if (on) {
            QPainterPath shape;
            shape.addEllipse(mid, rad, rad);
            ring(pnt, shape, QColor(0, 0, 0, 0));
        } else {
            pnt.setPen(Qt::NoPen);
            pnt.setBrush(QColor(255, 255, 255, 70));
            pnt.drawEllipse(mid + QPointF(-rad * 0.28, -rad * 0.38), rad * 0.42, rad * 0.26);
        }
        if (showBind()) {
            const QString key = shorten(keyText(m_session, ctl.target));
            pnt.setPen(Qt::white);
            drawFitted(pnt, QRectF(mid.x() - rad, mid.y() - rad * 0.45, rad * 2, rad * 0.9),
                       key.isEmpty() ? QStringLiteral("—") : key, pixelFont(font(), 9, true));
        }
    }

    /* SELECT / START (dark rubber) and the two turbo pills (light grey). */
    void paintPill(QPainter &pnt, int idx)
    {
        const nes_control &ctl = m_lay[idx];
        const bool turbo = isLabel(ctl, "TURBO B") || isLabel(ctl, "TURBO A");
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QRectF box = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        const qreal radius = box.height() / 2;
        QPainterPath shape;
        shape.addRoundedRect(box, radius, radius);
        const QColor base = turbo ? rgb(NESSESSION_SHELL_RGB) : rgb(NESSESSION_RUBBER_RGB);
        pnt.setPen(QPen(turbo ? rgb(NESSESSION_SHELL_SHADOW_RGB) : base.darker(200), 1));
        pnt.setBrush(base.lighter(over ? 140 : 100));
        pnt.drawPath(shape);
        if (on) {
            QColor fill = turbo ? rgb(NESSESSION_HELD_RGB) : rgb(NESSESSION_HELD_FILL_RGB);
            fill.setAlphaF(turbo ? 0.55 : 0.85);
            ring(pnt, shape, fill);
        }
        if (showBind()) {
            const qreal wide = turbo ? box.width() + 0.1 * kUnit : 1.7 * kUnit;   /* a turbo pill has its T legend beside it */
            chip(pnt, QRectF(box.center().x() - wide / 2, box.center().y() - 0.45 * kUnit, wide, 0.9 * kUnit),
                 ctl.target);
        }
    }

    /* The console's RESET key: a flat grey key with a dark legend. */
    void paintResetKey(QPainter &pnt, int idx)
    {
        const nes_control &ctl = m_lay[idx];
        const bool on = lit(idx);
        const bool over = !on && m_hover == idx && m_hold == None && !mapping();
        const QRectF box = rectOf(idx);
        pnt.setOpacity(dimmed(ctl) ? kDimAlpha : 1.0);
        QPainterPath shape;
        shape.addRoundedRect(box, 0.2 * kUnit, 0.2 * kUnit);
        QLinearGradient grad(box.topLeft(), box.bottomLeft());
        grad.setColorAt(0, rgb(NESSESSION_KEY_RGB).lighter(over ? 130 : 112));
        grad.setColorAt(1, rgb(NESSESSION_KEY_RGB).darker(over ? 100 : 108));
        pnt.setPen(QPen(rgb(NESSESSION_KEY_INK_RGB), 1));
        pnt.setBrush(grad);
        pnt.drawPath(shape);
        if (on) ring(pnt, shape, QColor(0xFF, 0xB0, 0x4A, 110));
        if (showBind()) {
            chip(pnt, box.adjusted(0.4 * kUnit, 0.3 * kUnit, -0.4 * kUnit, -0.3 * kUnit), ctl.target);
        } else {
            pnt.setPen(rgb(NESSESSION_KEY_INK_RGB));
            drawFitted(pnt, box, local(ctl.label), pixelFont(font(), 11, true));
        }
    }

    nessession *m_session;
    std::function<void(int)> m_onPick;
    const nes_control *m_lay = nullptr;         /* the core's static layout table */
    int m_count = 0;
    int m_type[2] = { -1, -1 };                 /* the port types drawn */
    std::vector<char> m_held;                   /* per layout entry: held by anyone */
    int m_consoleIdx = -1;
    bool m_showBindings = false;
    int m_map = -2;
    Hold m_hold = None;
    int m_holdIdx = -1;                         /* the part the mouse holds */
    bool m_partDown = false;
    unsigned m_holdMask = 0;                    /* the cross's drag, as last sent */
    int m_hover = -1;
};

/* ---- KeyboardStage: the expansion-port keyboard --------------------------------------- */

class KeyboardStage : public QWidget {
public:
    explicit KeyboardStage(nessession *session) : m_session(session)
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        reload();
    }

    /* Follows the attached keyboard; repaints (held keys change from anywhere). */
    void poll()
    {
        if (!reload() && isVisible()) update();
    }

    /* Lets go of the key the mouse holds. */
    void releaseMouse()
    {
        if (m_mouseKey < 0) return;
        nessession_keyboard_press(m_session, m_mouseKey, 0);
        m_mouseKey = -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        pnt.fillRect(rect(), palette().color(QPalette::Window));
        if (!m_skin) {
            pnt.setPen(QColor(Qt::gray));
            pnt.drawText(rect(), Qt::AlignCenter, QStringLiteral("No keyboard on the expansion port"));
            return;
        }
        for (int idx = 0; idx < m_count; ++idx) {
            const nes_skin_item &item = m_skin[idx];
            const QRectF box = rectOf(item, 0);
            if (item.kind == NES_SKIN_KEYCAP) paintKey(pnt, item);
            else paintBody(pnt, item, box);
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton || !m_skin) return;
        releaseMouse();
        const int idx = hitAt(evt->position());
        if (idx < 0) return;
        m_mouseKey = m_skin[idx].key;
        nessession_keyboard_press(m_session, m_mouseKey, 1);
        update();
    }

    void mouseMoveEvent(QMouseEvent *evt) override
    {
        const int hover = hitAt(evt->position());
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() == Qt::LeftButton) releaseMouse();
    }

    void leaveEvent(QEvent *evt) override
    {
        m_hover = -1;
        update();
        QWidget::leaveEvent(evt);
    }

private:
    /* True if the attached keyboard changed. */
    bool reload()
    {
        const int type = nessession_keyboard(m_session);
        if (m_loaded && type == m_type) return false;
        releaseMouse();
        m_loaded = true;
        m_type = type;
        m_unit = type == NES_KBD_FAMILY_BASIC ? 31.0 : 28.0;
        float wide = 0, tall = 0;
        m_skin = nessession_keyboard_skin_layout(type, &m_count, &wide, &tall);
        m_hover = -1;
        if (m_skin) setFixedSize(qCeil(wide * m_unit) + 2 * int(kMargin), qCeil(tall * m_unit) + 2 * int(kMargin));
        else setFixedSize(420, 120);
        update();
        return true;
    }

    QRectF rectOf(const nes_skin_item &item, qreal inset) const
    {
        return QRectF(kMargin + (item.x + inset) * m_unit, kMargin + (item.y + inset) * m_unit,
                      (item.w - 2 * inset) * m_unit, (item.h - 2 * inset) * m_unit);
    }

    int hitAt(const QPointF &pos) const
    {
        return nessession_keyboard_skin_hit(m_type, float((pos.x() - kMargin) / m_unit),
                                            float((pos.y() - kMargin) / m_unit));
    }

    const nes_skin_item *firstWell() const
    {
        for (int idx = 0; idx < m_count; ++idx)
            if (m_skin[idx].label && std::strcmp(m_skin[idx].label, "Well") == 0) return &m_skin[idx];
        return nullptr;
    }

    void paintBody(QPainter &pnt, const nes_skin_item &item, const QRectF &box)
    {
        const bool plate = item.text != nullptr && item.kind == NES_SKIN_BODY
                           && !(item.label && (std::strcmp(item.label, "Vent") == 0 || std::strcmp(item.label, "Well") == 0));
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgb(item.fill_rgb));
        if (item.label && std::strcmp(item.label, "Case") == 0) {
            pnt.setPen(QPen(rgb(item.fill_rgb).darker(150), 1.5));
            pnt.drawRoundedRect(box, 0.5 * m_unit, 0.5 * m_unit);
            if (nessession_keyboard_captures(m_session)) {
                /* keyboard mode: the case is edged in the accent colour */
                pnt.setPen(QPen(nesAccentColor(), 3));
                pnt.setBrush(Qt::NoBrush);
                pnt.drawRoundedRect(box.adjusted(2, 2, -2, -2), 0.5 * m_unit, 0.5 * m_unit);
            }
        } else if (item.label && std::strcmp(item.label, "Vent") == 0) {
            /* a group of thin vertical slits */
            const int slits = 14;
            const qreal pitch = box.width() / slits;
            for (int slit = 0; slit < slits; ++slit)
                pnt.drawRect(QRectF(box.left() + slit * pitch + pitch * 0.25, box.top(), pitch * 0.4, box.height()));
        } else if (item.label && std::strcmp(item.label, "Well") == 0) {
            /* the wells overlap into one stepped recess: outline them all, then
             * fill them all, so only the outer edge keeps its line */
            if (&item == firstWell()) {
                for (int pass = 0; pass < 2; ++pass) {
                    for (int other = 0; other < m_count; ++other) {
                        const nes_skin_item &well = m_skin[other];
                        if (!well.label || std::strcmp(well.label, "Well") != 0) continue;
                        const QRectF wbox = rectOf(well, 0);
                        pnt.setPen(pass == 0 ? QPen(rgb(well.ink_rgb), 2.5) : Qt::NoPen);
                        pnt.setBrush(rgb(well.fill_rgb));
                        pnt.drawRoundedRect(wbox, 0.2 * m_unit, 0.2 * m_unit);
                    }
                }
            }
        } else if (item.label && std::strcmp(item.label, "Logo") == 0) {
            pnt.drawRoundedRect(box, 0.2 * m_unit, 0.2 * m_unit);
        } else {
            pnt.drawRoundedRect(box, 0.12 * m_unit, 0.12 * m_unit);
        }
        if (plate) {
            pnt.setPen(rgb(item.ink_rgb));
            drawFitted(pnt, box.adjusted(2, 0, -2, 0), local(item.text), pixelFont(font(), 11, true));
        }
    }

    void paintKey(QPainter &pnt, const nes_skin_item &item)
    {
        const QRectF box = rectOf(item, 0.04);
        const bool held = nessession_keyboard_held(m_session, item.key) != 0;
        const bool over = !held && m_hover >= 0 && m_skin[m_hover].key == item.key;
        QColor fill = rgb(item.fill_rgb);
        if (over) fill = fill.lighter(112);
        const qreal radius = 0.12 * m_unit;
        /* the cap's sides, then its lighter top face, a little higher than it is deep */
        pnt.setPen(QPen(rgb(item.fill_rgb).darker(190), 1));
        pnt.setBrush(held ? rgb(NESSESSION_HELD_FILL_RGB).darker(130) : rgb(item.fill_rgb).darker(125));
        pnt.drawRoundedRect(box, radius, radius);
        const QRectF face(box.left() + 0.07 * m_unit, box.top() + 0.03 * m_unit,
                          box.width() - 0.14 * m_unit, box.height() - 0.13 * m_unit);
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(held ? rgb(NESSESSION_HELD_FILL_RGB) : fill.lighter(108));
        pnt.drawRoundedRect(face, radius, radius);
        if (held) {
            pnt.setPen(QPen(rgb(NESSESSION_HELD_RGB), 2));
            pnt.setBrush(Qt::NoBrush);
            pnt.drawRoundedRect(box.adjusted(1, 1, -1, -1), radius, radius);
        }
        const QColor ink = held ? QColor(Qt::white) : rgb(item.ink_rgb);
        pnt.setPen(ink);
        QString label = local(item.label);
        if (item.cap_class == NES_CAP_PALE && label.contains(QLatin1Char(' ')))
            label.replace(QLatin1Char(' '), QLatin1Char('\n'));      /* CLR / HOME on two lines */
        if (item.text && item.text[0]) {
            /* "shifted|kana": the legend large at the left, the shifted symbol small at the top right
             * and the katakana small at the bottom right, as printed */
            const QStringList parts = local(item.text).split(QLatin1Char('|'));
            const QString shifted = parts.value(0), kana = parts.value(1);
            const QRectF main(face.left() + 2, face.top() + 2, face.width() * 0.62, face.height() - 2);
            drawFitted(pnt, main, label, pixelFont(font(), 12, false), Qt::AlignLeft | Qt::AlignVCenter);
            const QFont small = pixelFont(font(), 8, false);
            const QRectF upper(face.left() + face.width() * 0.5, face.top() + 1, face.width() * 0.5 - 2, face.height() * 0.5);
            const QRectF lower(face.left() + face.width() * 0.5, face.top() + face.height() * 0.5, face.width() * 0.5 - 2, face.height() * 0.5 - 1);
            if (!shifted.isEmpty()) drawFitted(pnt, upper, shifted, small, Qt::AlignRight | Qt::AlignTop);
            if (!kana.isEmpty()) drawFitted(pnt, lower, kana, small, Qt::AlignRight | Qt::AlignBottom);
        } else {
            drawFitted(pnt, face.adjusted(1, 0, -1, 0), label, pixelFont(font(), label.contains(QLatin1Char('\n')) ? 8 : 10, true));
        }
    }

    qreal m_unit = 28.0;                        /* pixels per key unit; the compact Family BASIC case gets more */
    nessession *m_session;
    const nes_skin_item *m_skin = nullptr;
    int m_count = 0;
    int m_type = NES_KBD_NONE;
    bool m_loaded = false;
    int m_hover = -1;
    int m_mouseKey = -1;                        /* the key index the mouse holds, or -1 */
};

/* ---- ControllersWindow ------------------------------------------------------------------ */

ControllersWindow::ControllersWindow(nessession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel of
     * fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers (F9)"));
    setProperty("nesHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *outer = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    outer->setSizeConstraint(QLayout::SetFixedSize);
    m_tabs = new QTabWidget;
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    outer->addWidget(m_tabs);

    auto *page = new QWidget;
    auto *root = new QVBoxLayout(page);
    m_tabs->addTab(page, QStringLiteral("Controllers"));
    m_tabs->addTab(buildKeyboardPage(), QStringLiteral("Keyboard"));

    m_stage = new ControllersStage(session, [this](int target) { setMapState(target); });
    m_stage->setObjectName(QStringLiteral("stage"));
    root->addWidget(m_stage, 0, Qt::AlignHCenter);

    /* What is plugged into each port */
    auto *types = new QHBoxLayout;
    for (int port = 0; port < 2; ++port) {
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(16);
        for (int type = 0; nes_ctrl_type_name(type); ++type) box->addItem(local(nes_ctrl_type_name(type)));
        box->setCurrentIndex(nessession_port_type(session, port));
        box->setToolTip(QStringLiteral("What is plugged into player %1's port").arg(port + 1));
        connect(box, &QComboBox::currentIndexChanged, this, [this, port](int sel) {
            if (sel >= 0 && sel != nessession_port_type(m_session, port)) nessession_set_port_type(m_session, port, sel);
            m_stage->pollHeld();
        });
        m_type[port] = box;
        types->addWidget(new QLabel(QStringLiteral("Player %1:").arg(port + 1)));
        types->addWidget(box, 1);
        if (port == 0) types->addSpacing(12);
    }
    root->addLayout(types);

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
        nessession_binding_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_stage->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        nessession_bindings_reset(m_session);
        for (int pad = 0, count = nessession_gamepad_count(m_session); pad < count; pad++)
            nessession_gamepad_assign(m_session, pad, -1);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
        rebuildPads();
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(nessession_controls_show_bindings(session) != 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        nessession_set_controls_show_bindings(m_session, checked ? 1 : 0);
        m_stage->setShowBindings(checked);
    });
    m_stage->setShowBindings(bindings->isChecked());
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addWidget(bindings);
    mapRow->addStretch(1);
    mapRow->addWidget(nesHelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    /* The session's own actions are not on the picture: they are here, firing
     * when clicked and picked for rebinding in Map. */
    auto *sysRow = new QHBoxLayout;
    static const char *const sysNames[NES_SYSACT_COUNT] = { "Reset to CONFIG", "Stop in debugger" };
    for (int sysact = 0; sysact < NES_SYSACT_COUNT; ++sysact) {
        auto *button = new QPushButton(local(sysNames[sysact]));
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, [this, sysact] {
            if (m_mapState != -2) { setMapState(NES_TARGET_SYSACT(sysact)); return; }
            nessession_sysaction_post(m_session, sysact);
        });
        m_sysButton[sysact] = button;
        sysRow->addWidget(button);
    }
    sysRow->addStretch(1);
    root->addLayout(sysRow);

    m_hint = new ControllersHint(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] {
        m_stage->pollHeld();
        m_keyboard->poll();
    });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    syncTypes();
    setMapState(-2);
    m_stage->pollHeld();
}

QWidget *ControllersWindow::buildKeyboardPage()
{
    auto *page = new QWidget;
    auto *box = new QVBoxLayout(page);
    auto *row = new QHBoxLayout;
    m_kbdType = new QComboBox;
    m_kbdType->setFocusPolicy(Qt::NoFocus);
    for (int idx = 0; nes_keyboard_name(idx); ++idx) m_kbdType->addItem(local(nes_keyboard_name(idx)));
    m_kbdType->setCurrentIndex(nessession_keyboard(m_session));
    connect(m_kbdType, &QComboBox::currentIndexChanged, this, [this](int sel) {
        if (sel >= 0 && sel != nessession_keyboard(m_session)) nessession_set_keyboard(m_session, sel);
        m_keyboard->poll();
    });
    row->addWidget(new QLabel(QStringLiteral("Expansion port:")));
    row->addWidget(m_kbdType, 1);
    box->addLayout(row);
    m_kbdNote = new QLabel;
    m_kbdNote->setStyleSheet(QStringLiteral("color: gray;"));
    m_kbdNote->setWordWrap(true);
    m_kbdNote->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_kbdNote->setMinimumHeight(m_kbdNote->fontMetrics().lineSpacing() * 2 + 4);
    box->addWidget(m_kbdNote);
    m_keyboard = new KeyboardStage(m_session);
    box->addWidget(m_keyboard, 0, Qt::AlignHCenter);
    box->addStretch(1);
    return page;
}

void ControllersWindow::showKeyboard()
{
    m_tabs->setCurrentIndex(1);
    show();
    raise();
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::releaseEverything()
{
    m_stage->releaseAll();
    m_keyboard->releaseMouse();
    nessession_release_all(m_session);
}

void ControllersWindow::setMapState(int state)
{
    /* Entering (or changing) Map: nothing may stay held by the mouse, and the
     * keyboard's holds go too -- Map swallows key-ups, so a key held into it
     * would stick. */
    releaseEverything();
    m_mapState = state;
    m_clearButton->setEnabled(state >= 0);
    for (int sysact = 0; sysact < NES_SYSACT_COUNT; ++sysact)
        m_sysButton[sysact]->setStyleSheet(state == NES_TARGET_SYSACT(sysact)
            ? QStringLiteral("background: %1; color: black;").arg(nesAccentColor().name()) : QString());
    if (state == -2) {
        m_captureTimer.stop();
        nessession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Drag the cross, press a button or RESET with the mouse to hold it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        nessession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: black;").arg(nesAccentColor().name()));
        m_hint->setText(QStringLiteral("Click a part to remap it (a side of the cross for a direction). Escape leaves Map."));
    } else {
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                            .arg(local(nes_target_name(state))));
        nessession_gamepad_capture_begin(m_session);
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
    if (!nessession_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    nessession_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
    bound(local(nes_pad_button_name(button)), local(stolen));
}

/* ---- polling ---------------------------------------------------------------------------- */

/* Hot-plugged pads, auto-assignment changes, the type combos and the keyboard
 * (Preferences and Scroll Lock change them too). */
void ControllersWindow::pollSlow()
{
    syncTypes();
    const unsigned gen = nessession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPlayers();
    }
    const int kbd = nessession_keyboard(m_session);
    if (m_kbdType->currentIndex() != kbd) {
        QSignalBlocker block(m_kbdType);
        m_kbdType->setCurrentIndex(kbd);
    }
    QString note;
    if (kbd == NES_KBD_NONE)
        note = QStringLiteral("No keyboard on the expansion port. Choose one to type into Family BASIC, "
                              "or into any program that reads a keyboard.");
    else if (nessession_keyboard_captures(m_session))
        note = QStringLiteral("Keyboard mode: what you type goes to the keyboard (Scroll Lock gives the keys "
                              "back to the controllers). Click keys to press them.");
    else
        note = QStringLiteral("Keyboard mode is off: keys drive the controllers (Scroll Lock turns it on). "
                              "Click keys to press them.");
    if (m_kbdNote->text() != note) m_kbdNote->setText(note);
    m_keyboard->poll();
}

void ControllersWindow::syncTypes()
{
    for (int port = 0; port < 2; ++port) {
        const int type = nessession_port_type(m_session, port);
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
        const int eff = nessession_gamepad_effective_port(m_session, idx);
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
    const int count = nessession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        nessession_gamepad_name(m_session, idx, name, sizeof name);
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
        const int assign = nessession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            nessession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
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
    m_padGen = nessession_gamepad_generation(m_session);
    rebuildPads();
    pollSlow();
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
        releaseEverything();
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
        releaseEverything();
        m_swallow = 0;
    }
    return QWidget::event(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    const bool captured = nessession_keyboard_captures(m_session);
    /* F9, F11 and F12 are never bindings: in Map they only explain. In
     * keyboard mode they are keys of the emulated keyboard. */
    if (!captured && (evt->key() == Qt::Key_F9 || evt->key() == Qt::Key_F11 || evt->key() == Qt::Key_F12)) {
        if (evt->isAutoRepeat()) return;
        if (m_mapState >= 0) m_hint->setText(QStringLiteral("F9, F11 and F12 are reserved and cannot be bound."));
        else if (evt->key() == Qt::Key_F9) hide();
        return;
    }
    const uint32_t keysym = nesKeysymFromQt(evt);
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
        if (nessession_key_reserved(keysym)) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 are reserved and cannot be bound."));
            return;
        }
        char name[48] = "", stolen[128] = "";
        nessession_binding_set_key(m_session, m_mapState, keysym, stolen, sizeof stolen);
        nessession_keysym_name(keysym, name, sizeof name);
        m_swallow = keysym;
        bound(local(name), local(stolen));
        return;
    }
    if (evt->isAutoRepeat()) return;
    /* Otherwise what the main window does with it: a system action goes to the
     * main window's queue, as a gamepad's does. */
    if (keysym) {
        const int sysact = nessession_key_sysaction(m_session, keysym);
        if (sysact >= 0) { nessession_sysaction_post(m_session, sysact); return; }
        if (nessession_key(m_session, keysym, 1)) return;
    }
    QWidget::keyPressEvent(evt);
}

/* A release always goes to the session, in Map mode too: it only lets go of
 * what a press asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t keysym = nesKeysymFromQt(evt);
    if (m_swallow && keysym == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!keysym || nessession_key_sysaction(m_session, keysym) >= 0) return;
    if (!nessession_key(m_session, keysym, 0)) QWidget::keyReleaseEvent(evt);
}
