// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/InputFilter.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QWindow>

namespace Kinefin {
InputFilter::InputFilter(QObject *parent) : QObject(parent) {}

bool InputFilter::eventFilter(QObject *object, QEvent *event)
{
    if (m_mouseBlocked && blocksMouse(object)) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
            return true;
        default:
            break;
        }
    }
    if (m_repeatEnabled || event->type() != QEvent::KeyPress)
        return false;
    auto *key = static_cast<QKeyEvent *>(event);
    if (!key->isAutoRepeat())
        return false;
    // A text field keeps its repeats: its cursor and its keys are the field's, not the shell's
    QWindow *window = QGuiApplication::focusWindow();
    QObject *focus = window ? window->focusObject() : nullptr;
    if (focus) {
        QInputMethodQueryEvent query(Qt::ImEnabled);
        QCoreApplication::sendEvent(focus, &query);
        if (query.value(Qt::ImEnabled).toBool())
            return false;
    }
    return true;
}

bool InputFilter::blocksMouse(QObject *object) const
{
    // The receiving item's window: its QObject parent chain ends at a QWindow
    QWindow *window = nullptr;
    for (QObject *current = object; current; current = current->parent()) {
        if (auto *candidate = qobject_cast<QWindow *>(current)) {
            window = candidate;
            break;
        }
    }
    if (!window)
        window = QGuiApplication::focusWindow();
    if (!window)
        return false;
    if (window->objectName() == QLatin1String("pictureInPictureWindow"))
        return true;
    return window->property("playbackMode").toString() == QLatin1String("full");
}
} // namespace Kinefin
