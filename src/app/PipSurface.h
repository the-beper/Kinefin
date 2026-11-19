// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

namespace Kinefin {
// KWin's picture-in-picture surface (`xx_pip_v1`) for one window: the compositor keeps it above the other windows and moves and resizes it on request. Built only where QtWaylandClient's private headers are found (the Flatpak SDK); elsewhere, and where the compositor does not offer the protocol, `available` is false and the window stays an ordinary one
class PipSurface : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available CONSTANT)

  public:
    explicit PipSurface(QObject *parent = nullptr);

    bool available() const;
    // Gives the window the picture-in-picture role; only before the window is shown the first time. False when the surface is not available or the window is already visible
    Q_INVOKABLE bool assign(QWindow *window);
};
} // namespace Kinefin
