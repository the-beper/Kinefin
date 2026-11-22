// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MiddleButtonWatcher.h"

#include <QLoggingCategory>
#include <QMouseEvent>

namespace Kinefin {
namespace {
Q_LOGGING_CATEGORY(logAutoScroll, "kinefin.app.autoscroll")
} // namespace

MiddleButtonWatcher::MiddleButtonWatcher(QObject *parent) : QObject(parent) {}

QQuickWindow *MiddleButtonWatcher::target() const
{
    return m_target;
}

void MiddleButtonWatcher::setTarget(QQuickWindow *window)
{
    if (m_target == window)
        return;
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = window;
    m_held = false;
    m_taken = false;
    if (m_target)
        m_target->installEventFilter(this);
    Q_EMIT targetChanged();
}

QJSValue MiddleButtonWatcher::takes() const
{
    return m_takes;
}

void MiddleButtonWatcher::setTakes(const QJSValue &function)
{
    m_takes = function;
    Q_EMIT takesChanged();
}

bool MiddleButtonWatcher::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_target)
        return false;
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::MiddleButton)
            return false;
        m_held = true;
        m_taken = false;
        if (m_takes.isCallable()) {
            const QPointF at = mouse->position();
            const QJSValue answer = m_takes.call({QJSValue(at.x()), QJSValue(at.y())});
            // A function that threw answers an Error object, which is truthy: it takes nothing
            if (answer.isError())
                qCWarning(logAutoScroll) << "the middle press handler failed:" << answer.toString();
            else
                m_taken = answer.toBool();
        }
        return m_taken;
    }
    // The second press of a double click is the one that ends a scroll: it goes to the items like any press that is not a first one
    case QEvent::MouseButtonDblClick: {
        if (static_cast<QMouseEvent *>(event)->button() != Qt::MiddleButton)
            return false;
        m_held = true;
        m_taken = false;
        return false;
    }
    case QEvent::MouseMove: {
        if (!m_held)
            return false;
        const QPointF at = static_cast<QMouseEvent *>(event)->position();
        Q_EMIT moved(at.x(), at.y());
        return false;
    }
    case QEvent::MouseButtonRelease: {
        if (static_cast<QMouseEvent *>(event)->button() != Qt::MiddleButton || !m_held)
            return false;
        const bool taken = m_taken;
        m_held = false;
        m_taken = false;
        Q_EMIT released();
        return taken;
    }
    default:
        return false;
    }
}
} // namespace Kinefin
