// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

namespace Kinefin {
class MouseNavigation : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *target READ target WRITE setTarget NOTIFY targetChanged FINAL)

  public:
    explicit MouseNavigation(QObject *parent = nullptr);

    QQuickWindow *target() const;
    void setTarget(QQuickWindow *window);

  Q_SIGNALS:
    void targetChanged();
    // The back button was released over the window
    void backRequested();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QPointer<QQuickWindow> m_target;
};
} // namespace Kinefin
