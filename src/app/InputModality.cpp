// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputModality.h"

#include <QEvent>
#include <QKeyEvent>

namespace Kinefin {
InputModality::InputModality(QObject *parent) : QObject(parent) {}

QQuickWindow *InputModality::target() const
{
    return m_target;
}

void InputModality::setTarget(QQuickWindow *window)
{
    if (m_target == window)
        return;
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = window;
    if (m_target)
        m_target->installEventFilter(this);
    Q_EMIT targetChanged();
}

void InputModality::mark(bool keyboard)
{
    // The window declares the property itself: a window without it is left as it is
    if (m_target && m_target->metaObject()->indexOfProperty("keyboardDriven") >= 0)
        m_target->setProperty("keyboardDriven", keyboard);
}

bool InputModality::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_target)
        return false;
    switch (event->type()) {
    case QEvent::KeyPress:
        switch (static_cast<QKeyEvent *>(event)->key()) {
        // A modifier alone moves nothing
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_CapsLock:
        case Qt::Key_NumLock:
            break;
        default:
            mark(true);
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::TouchBegin:
        mark(false);
        break;
    default:
        break;
    }
    return false;
}
} // namespace Kinefin
