// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MouseNavigation.h"

#include <QMouseEvent>
#include <QQuickWindow>

namespace Kinefin {
MouseNavigation::MouseNavigation(QObject *parent) : QObject(parent) {}

QQuickWindow *MouseNavigation::target() const
{
    return m_target;
}

void MouseNavigation::setTarget(QQuickWindow *window)
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

bool MouseNavigation::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_target)
        return false;
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease
        && type != QEvent::MouseButtonDblClick)
        return false;
    const auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::BackButton && mouse->button() != Qt::ForwardButton)
        return false;
    // Queued: the request changes the page stack, which must not happen inside the delivery of this event
    if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::BackButton)
        QMetaObject::invokeMethod(this, &MouseNavigation::backRequested, Qt::QueuedConnection);
    return true;
}
} // namespace Kinefin
