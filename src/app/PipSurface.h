// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

class KWindowShadow;

namespace Kinefin {
// KWin's picture-in-picture surface (`xx_pip_v1`) for one window: the compositor keeps it above the other windows and moves and resizes it on request. Built only where QtWaylandClient's private headers are found (the Flatpak SDK); elsewhere, and where the compositor does not offer the protocol, `available` is false and the window stays an ordinary one
class PipSurface : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available CONSTANT)
    // The width over the height that the window keeps while it is resized and that it takes at once when it changes; zero or less leaves its shape to the compositor
    Q_PROPERTY(qreal aspectRatio READ aspectRatio WRITE setAspectRatio NOTIFY aspectRatioChanged)
    // The least width of the window, which the surface keeps itself: the window's own minimum size would be kept even where it exceeds the compositor's bounds, a protocol error
    Q_PROPERTY(int minimumWidth READ minimumWidth WRITE setMinimumWidth NOTIFY minimumWidthChanged)
    // The shadow the compositor draws around the window, which QML cannot draw: how far its blur reaches (logical pixels, zero for no shadow), how far it is moved down, and its alpha at the densest. Applied once the window has the role, and again when one changes
    Q_PROPERTY(qreal shadowSize READ shadowSize WRITE setShadowSize NOTIFY shadowSizeChanged)
    Q_PROPERTY(
        qreal shadowYOffset READ shadowYOffset WRITE setShadowYOffset NOTIFY shadowYOffsetChanged)
    Q_PROPERTY(
        qreal shadowOpacity READ shadowOpacity WRITE setShadowOpacity NOTIFY shadowOpacityChanged)

  public:
    explicit PipSurface(QObject *parent = nullptr);

    bool available() const;
    qreal aspectRatio() const { return m_aspectRatio; }
    void setAspectRatio(qreal ratio);
    int minimumWidth() const { return m_minimumWidth; }
    void setMinimumWidth(int width);
    qreal shadowSize() const { return m_shadowSize; }
    void setShadowSize(qreal size);
    qreal shadowYOffset() const { return m_shadowYOffset; }
    void setShadowYOffset(qreal offset);
    qreal shadowOpacity() const { return m_shadowOpacity; }
    void setShadowOpacity(qreal opacity);
    // Gives the window the picture-in-picture role; only before the window is shown the first time. False when the surface is not available or the window is already visible
    Q_INVOKABLE bool assign(QWindow *window);

  Q_SIGNALS:
    void aspectRatioChanged();
    void minimumWidthChanged();
    void shadowSizeChanged();
    void shadowYOffsetChanged();
    void shadowOpacityChanged();

  private:
    // Makes the compositor's shadow of the window given the role from the shadow properties, in place of the last one; none while there is no such window, or the properties give no shadow
    void applyShadow();

    qreal m_aspectRatio = 0;
    int m_minimumWidth = 0;
    qreal m_shadowSize = 0;
    qreal m_shadowYOffset = 0;
    qreal m_shadowOpacity = 0;
    // The window given the role, which the shell surface reads the aspect ratio from
    QPointer<QWindow> m_window;
    // A child of this object, replaced by applyShadow
    KWindowShadow *m_shadow = nullptr;
    bool m_shadowCreated = false;
};
} // namespace Kinefin
