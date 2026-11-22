// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

namespace Kinefin {
// Watches the middle button over a window without an item in the scene: an item that takes mouse events over the whole window hides the cursor of what lies under it, and a pointer handler cannot decide whether the press goes on to the items below
class MiddleButtonWatcher : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *target READ target WRITE setTarget NOTIFY targetChanged FINAL)
    // A function called with the window point of a middle press: true takes the press and its release from the window's items, anything else leaves them to the items
    Q_PROPERTY(QJSValue takes READ takes WRITE setTakes NOTIFY takesChanged FINAL)

  public:
    explicit MiddleButtonWatcher(QObject *parent = nullptr);

    QQuickWindow *target() const;
    void setTarget(QQuickWindow *window);
    QJSValue takes() const;
    void setTakes(const QJSValue &function);

  Q_SIGNALS:
    void targetChanged();
    void takesChanged();
    // The pointer moved while the middle button was down
    void moved(qreal x, qreal y);
    // The middle button came up
    void released();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QPointer<QQuickWindow> m_target;
    QJSValue m_takes;
    bool m_held = false;
    bool m_taken = false;
};
} // namespace Kinefin
