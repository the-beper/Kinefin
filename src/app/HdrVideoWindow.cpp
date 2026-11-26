// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "HdrVideoWindow.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QOpenGLContext>
#include <QPlatformSurfaceEvent>
#include <QSurfaceFormat>

#include <QtWaylandClient/private/qwaylandsubsurface_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include <mpv/render_gl.h>

#include <atomic>
#include <memory>
#include <utility>

namespace {
Q_LOGGING_CATEGORY(hdrLog, "kinefin.app.hdr")
}

namespace Kinefin {
namespace {
// GL_RGB10_A2, the default framebuffer's format with the 10-bit config
constexpr int GlRgb10A2 = 0x8059;

QSurfaceFormat videoFormat()
{
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setRedBufferSize(10);
    format.setGreenBufferSize(10);
    format.setBlueBufferSize(10);
    format.setAlphaBufferSize(2);
    // A blocking swap waits for frame callbacks, which the compositor may hold back for a surface below an opaque one
    format.setSwapInterval(0);
    return format;
}

void *procAddress(void *context, const char *name)
{
    return reinterpret_cast<void *>(static_cast<QOpenGLContext *>(context)->getProcAddress(name));
}
} // namespace

// The render thread's side: everything but the constructor runs on that thread. It tells the window through events posted to it, which go with the window if it is deleted first
class HdrRenderWorker : public QObject {
  public:
    HdrRenderWorker(HdrVideoWindow *window, QOffscreenSurface *offscreen, mpv_handle *mpv,
                    ::wl_display *display, const QSurfaceFormat &format)
        : m_window(window), m_offscreen(offscreen), m_mpv(mpv), m_display(display), m_format(format)
    {
    }

    void setState(QSize pixels, bool exposed)
    {
        m_pixels = pixels;
        m_exposed = exposed;
        if (!m_exposed)
            return;
        if (!m_initialized)
            initialize();
        else
            renderFrame();
    }

    void shutdown()
    {
        if (m_render) {
            // mpv frees its GL objects with the context current, on whichever surface still takes it
            if (!m_context->makeCurrent(m_offscreen))
                m_context->makeCurrent(m_window);
            mpv_render_context_set_update_callback(m_render, nullptr, nullptr);
            mpv_render_context_free(m_render);
            m_render = nullptr;
        }
        if (m_context)
            m_context->doneCurrent();
        m_context.reset();
    }

    void renderFrame()
    {
        if (!m_render || !m_exposed || !m_frameSeen || m_pixels.isEmpty())
            return;
        if (!m_context->makeCurrent(m_window))
            return;
        mpv_opengl_fbo fbo{static_cast<int>(m_context->defaultFramebufferObject()),
                           m_pixels.width(), m_pixels.height(), GlRgb10A2};
        int flipY = 1;
        int depth = m_depth;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_DEPTH, &depth},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        const int error = mpv_render_context_render(m_render, params);
        if (error < 0) {
            if (!std::exchange(m_renderErrorLogged, true))
                qCWarning(hdrLog) << "mpv_render_context_render failed:" << mpv_error_string(error);
            return;
        }
        m_context->swapBuffers(m_window);
        mpv_render_context_report_swap(m_render);
        if (!std::exchange(m_firstFrameSent, true))
            post([](HdrVideoWindow *window) { Q_EMIT window->firstFrameSwapped(); });
    }

  private:
    template <typename Function> void post(Function function)
    {
        QMetaObject::invokeMethod(
            m_window, [window = m_window, function] { function(window); }, Qt::QueuedConnection);
    }

    void initialize()
    {
        m_initialized = true;
        m_context = std::make_unique<QOpenGLContext>();
        m_context->setFormat(m_format);
        if (!m_context->create())
            return fail(QStringLiteral("the OpenGL context could not be created"));
        // Fewer bits would band the PQ signal: the path counts as unavailable
        m_depth = m_context->format().redBufferSize();
        if (m_depth < 10)
            return fail(QStringLiteral("the OpenGL context has %1 bits per colour").arg(m_depth));
        if (!m_context->makeCurrent(m_window))
            return fail(QStringLiteral("the OpenGL context could not be made current"));
        mpv_opengl_init_params glParams{procAddress, m_context.get()};
        // No MPV_RENDER_PARAM_ADVANCED_CONTROL, as in the item's own renderer: screenshots keep mpv's software path
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glParams},
            {MPV_RENDER_PARAM_WL_DISPLAY, m_display},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        const int error = mpv_render_context_create(&m_render, m_mpv, params);
        if (error < 0) {
            m_render = nullptr;
            return fail(QStringLiteral("mpv_render_context_create failed: %1")
                            .arg(QString::fromUtf8(mpv_error_string(error))));
        }
        mpv_render_context_set_update_callback(m_render, onMpvUpdate, this);
        post([](HdrVideoWindow *window) { Q_EMIT window->renderContextCreated(); });
    }

    void fail(const QString &reason)
    {
        if (m_context)
            m_context->doneCurrent();
        post([reason](HdrVideoWindow *window) { Q_EMIT window->failed(reason); });
    }

    // Called by mpv on one of its threads: one update in the queue is enough
    static void onMpvUpdate(void *data)
    {
        auto *worker = static_cast<HdrRenderWorker *>(data);
        if (!worker->m_updateQueued.exchange(true))
            QMetaObject::invokeMethod(
                worker, [worker] { worker->processUpdate(); }, Qt::QueuedConnection);
    }

    void processUpdate()
    {
        m_updateQueued = false;
        if (!m_render)
            return;
        if (mpv_render_context_update(m_render) & MPV_RENDER_UPDATE_FRAME) {
            m_frameSeen = true;
            renderFrame();
        }
    }

    HdrVideoWindow *m_window;
    QOffscreenSurface *m_offscreen;
    mpv_handle *m_mpv;
    ::wl_display *m_display;
    QSurfaceFormat m_format;
    std::unique_ptr<QOpenGLContext> m_context;
    mpv_render_context *m_render = nullptr;
    int m_depth = 0;
    QSize m_pixels;
    bool m_exposed = false;
    bool m_initialized = false;
    // Nothing is swapped before mpv gave a frame: until then the surface has no buffer at all
    bool m_frameSeen = false;
    bool m_firstFrameSent = false;
    bool m_renderErrorLogged = false;
    std::atomic<bool> m_updateQueued{false};
};

HdrVideoWindow::HdrVideoWindow(QWindow *parent, mpv_handle *mpv) : QWindow(parent), m_mpv(mpv)
{
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    m_display = wayland ? wayland->display() : nullptr;
    setSurfaceType(QSurface::OpenGLSurface);
    // Below the main window, which keeps the input and the focus
    setFlags(Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    const QSurfaceFormat format = videoFormat();
    setFormat(format);
    setGeometry(QRect(QPoint(0, 0), parent->size()));
    connect(parent, &QWindow::widthChanged, this, [this, parent] { resize(parent->size()); });
    connect(parent, &QWindow::heightChanged, this, [this, parent] { resize(parent->size()); });
    // The wl_surface exists after create(), the wl_subsurface after show()
    create();
    m_offscreen.setFormat(format);
    m_offscreen.setScreen(parent->screen());
    m_offscreen.create();
    m_thread.setObjectName(QStringLiteral("HdrVideo"));
}

HdrVideoWindow::~HdrVideoWindow()
{
    stop();
}

::wl_surface *HdrVideoWindow::surface() const
{
    auto *wayland = dynamic_cast<QtWaylandClient::QWaylandWindow *>(handle());
    return wayland ? wayland->wlSurface() : nullptr;
}

bool HdrVideoWindow::showBelowParent()
{
    show();
    auto *self = dynamic_cast<QtWaylandClient::QWaylandWindow *>(handle());
    QWindow *parentWindow = parent();
    auto *parentWayland =
        parentWindow ? dynamic_cast<QtWaylandClient::QWaylandWindow *>(parentWindow->handle())
                     : nullptr;
    QtWaylandClient::QWaylandSubSurface *subSurface = self ? self->subSurfaceWindow() : nullptr;
    if (!subSurface || !parentWayland || !parentWayland->wlSurface())
        return false;
    // A synchronized subsurface would show a frame only with the parent's next commit
    subSurface->setDeSync();
    subSurface->place_below(parentWayland->wlSurface());
    // The stacking is state of the parent: its commit applies it before the first frame of the video
    parentWayland->commit();
    return true;
}

void HdrVideoWindow::start()
{
    if (m_worker)
        return;
    m_worker = new HdrRenderWorker(this, &m_offscreen, m_mpv, m_display, requestedFormat());
    m_worker->moveToThread(&m_thread);
    m_thread.start();
    pushState();
}

void HdrVideoWindow::redraw()
{
    if (m_worker)
        QMetaObject::invokeMethod(
            m_worker, [worker = m_worker] { worker->renderFrame(); }, Qt::QueuedConnection);
}

void HdrVideoWindow::stop()
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_worker;
    m_worker = nullptr;
}

bool HdrVideoWindow::event(QEvent *event)
{
    // As Qt Quick's threaded render loop does: the thread is done with the EGL surface before Qt destroys it
    if ((event->type() == QEvent::PlatformSurface
         && static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
        || event->type() == QEvent::Hide)
        stopForLostSurface();
    const bool handled = QWindow::event(event);
    switch (event->type()) {
    case QEvent::Expose:
    case QEvent::Resize:
    case QEvent::DevicePixelRatioChange:
        pushState();
        break;
    default:
        break;
    }
    return handled;
}

void HdrVideoWindow::stopForLostSurface()
{
    if (!m_worker)
        return;
    stop();
    // Queued: the window may be in its own destruction
    QMetaObject::invokeMethod(
        this, [this] { Q_EMIT failed(QStringLiteral("the HDR window lost its surface")); },
        Qt::QueuedConnection);
}

void HdrVideoWindow::pushState()
{
    if (!m_worker)
        return;
    const QSize pixels = size() * devicePixelRatio();
    const bool exposed = isExposed();
    // An unexposed window may lose its surface right after this event: the thread has stopped drawing into it when the call returns
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, pixels, exposed] { worker->setState(pixels, exposed); },
        exposed ? Qt::QueuedConnection : Qt::BlockingQueuedConnection);
}
} // namespace Kinefin
