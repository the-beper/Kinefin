// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QOffscreenSurface>
#include <QString>
#include <QThread>
#include <QWindow>

struct mpv_handle;
struct wl_display;
struct wl_surface;

namespace Kinefin {
class HdrRenderWorker;

// The HDR video: a child window of the main window, shown as a desynchronized wl_subsurface below it, with a 10-bit OpenGL context and an mpv render context of its own on a thread of its own (mpv_render_context_render blocks until the frame is due, which on the GUI thread would stall input and QML). Only on Wayland, and only while the video item's own renderer holds no render context: libmpv allows one per core
class HdrVideoWindow : public QWindow {
    Q_OBJECT
  public:
    // Made at once, so that its surface is there to describe; shown by showBelowParent
    HdrVideoWindow(QWindow *parent, mpv_handle *mpv);
    ~HdrVideoWindow() override;

    ::wl_surface *surface() const;
    // Shows the window below its parent, desynchronized so that its frames need no commit of the parent; false when Qt made it no subsurface
    bool showBelowParent();
    // Starts the render thread: it makes its contexts once the window is exposed, then draws every frame mpv gives it. The image description must be on the surface already, since the first swap commits it
    void start();
    // Draws the current frame again, as after a new image description
    void redraw();
    // Frees the render context on the render thread and ends the thread; blocks until then. Called before the window hides, since the thread draws into its surface; a hide or a destruction of the surface that comes first does it too, and reports a failure
    void stop();

  Q_SIGNALS:
    void renderContextCreated();
    // The first frame mpv gave is on the surface
    void firstFrameSwapped();
    void failed(const QString &reason);

  protected:
    bool event(QEvent *event) override;

  private:
    void stopForLostSurface();
    void pushState();

    mpv_handle *m_mpv;
    ::wl_display *m_display;
    QOffscreenSurface m_offscreen;
    QThread m_thread;
    // Lives on m_thread until stop
    HdrRenderWorker *m_worker = nullptr;
};
} // namespace Kinefin
