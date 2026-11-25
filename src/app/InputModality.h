// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

namespace Kinefin {
// Tells the window how it is being driven: the `keyboardDriven` property of the target window is true from a key press (a gamepad or a remote sets it itself) until the next press of a mouse button or touch. The focus ring of cards and rows shows only while it is true, so a page that opens with a current item lights nothing until the user moves
class InputModality : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *target READ target WRITE setTarget NOTIFY targetChanged FINAL)

  public:
    explicit InputModality(QObject *parent = nullptr);

    QQuickWindow *target() const;
    void setTarget(QQuickWindow *window);

  Q_SIGNALS:
    void targetChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void mark(bool keyboard);

    QPointer<QQuickWindow> m_target;
};
} // namespace Kinefin
