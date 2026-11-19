/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-FileCopyrightText: 2026 Kinefin contributors
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

// Kinefin changes to mpvqt 1.2.1: the render context has one owning renderer, a renderer created while another one still owns it waits for it to be freed (the item moves between windows that render on different threads), `ready` is emitted only when a renderer becomes ready, and a failed creation calls the item's `rendererFailed` if it has one

#ifndef MPVRENDERER_H
#define MPVRENDERER_H

#include <QtCore/QElapsedTimer>
#include <QtQuick/QQuickFramebufferObject>

#include <mpv/render_gl.h>

#include "mpvabstractitem.h"

class MpvAbstractItem;

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer();
    ~MpvRenderer();

    // This function is called when a new FBO is needed.
    // This happens on the initial frame.
    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override;

    void render() override;
    void synchronize(QQuickFramebufferObject *item) override;
    void requestUpdate();

private:
    // A frame a little later, for a renderer that waits for the previous owner: not on every vsync
    void requestUpdateLater();
    // GL context current: creates the render context when none exists and this renderer has not failed before, and returns whether this renderer owns the context
    bool ensureRenderContext();
    mpv_render_context *createMpvRenderContext();
    QPointer<MpvAbstractItem> m_mpvAItem{nullptr};
    bool m_isFramebufferReady{false};
    bool m_contextFailed{false};
    QElapsedTimer m_waitClock;
    bool m_waitReported{false};
    std::shared_ptr<MpvResourceManager> m_mpvResourceManager;
};

#endif // MPVRENDERER_H
