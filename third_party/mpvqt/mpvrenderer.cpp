/*
 * SPDX-FileCopyrightText: 2023 George Florea Bănuș <georgefb899@gmail.com>
 * SPDX-FileCopyrightText: 2026 Kinefin contributors
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

// Kinefin changes to mpvqt 1.2.1: the render context has one owning renderer, a renderer created while another one still owns it waits for it to be freed (the item moves between windows that render on different threads), `ready` is emitted only when a renderer becomes ready, a failed creation calls the item's `rendererFailed` if it has one, and while the item's `renderElsewhereRequest` property is not zero the renderer clears its framebuffer, frees the render context, creates none and calls the item's `renderContextReleased` with that number once no renderer owns one (another renderer of the item's mpv core can then create its own)

#include "mpvrenderer.h"

#include <QGuiApplication>
#include <QHash>
#include <QLoggingCategory>
#include <QMutex>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>

#include "mpvabstractitem.h"
#include "mpvabstractitem_p.h"
#include "mpvcontroller.h"

Q_STATIC_LOGGING_CATEGORY(MpvQt_MpvRenderer, "MpvQt.MpvRenderer")

// The render context lives in the resource manager shared by every renderer of an item, and the renderers of two windows run on different threads: the owner registry (guarded by the mutex) makes one renderer the only one that renders with it and frees it
static QMutex s_ownersMutex;
static QHash<const MpvResourceManager *, const MpvRenderer *> s_owners;

static void *get_proc_address_mpv(void *ctx, const char *name)
{
    Q_UNUSED(ctx)

    QOpenGLContext *glctx = QOpenGLContext::currentContext();
    if (glctx == nullptr) {
        qCDebug(MpvQt_MpvRenderer) << "get_proc_address_mpv: glctx is nullptr";
        return nullptr;
    }

    return reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name)));
}

void on_mpv_redraw(void *ctx)
{
    auto *r = static_cast<MpvRenderer *>(ctx);
    r->requestUpdate();
}

MpvRenderer::MpvRenderer()
{
}

MpvRenderer::~MpvRenderer()
{
    if (m_mpvResourceManager) {
        QMutexLocker locker(&s_ownersMutex);
        if (s_owners.value(m_mpvResourceManager.get()) == this) {
            m_mpvResourceManager->freeContext();
            s_owners.remove(m_mpvResourceManager.get());
        }
    }
}

void MpvRenderer::synchronize(QQuickFramebufferObject *item)
{
    MpvAbstractItem *mpvAItem = static_cast<MpvAbstractItem *>(item);
    m_mpvAItem = mpvAItem;

    if (!m_mpvResourceManager) {
        m_mpvResourceManager = mpvAItem->d_ptr->m_mpvResourceManager;
    }

    // read by name, as rendererFailed is called: MpvAbstractItem has no such property. A new request may come between two synchronizations, so the number and not only zero or not is compared
    const int request = mpvAItem->property("renderElsewhereRequest").toInt();
    if (request != m_elsewhereRequest) {
        m_elsewhereRequest = request;
        m_releaseSignalled = false;
    }

    if (mpvAItem->d_ptr->m_isRendererReady != m_isFramebufferReady) {
        mpvAItem->d_ptr->m_isRendererReady = m_isFramebufferReady;

        if (m_isFramebufferReady) {
            Q_EMIT mpvAItem->ready();
        }
    }
}

void MpvRenderer::render()
{
    if (m_elsewhereRequest != 0) {
        releaseForElsewhere();
        return;
    }

    if (!ensureRenderContext()) {
        // another renderer still owns the context: its window frees it when it next renders, so try again a little later
        if (!m_contextFailed) {
            if (!m_waitClock.isValid()) {
                m_waitClock.start();
            } else if (!m_waitReported && m_waitClock.elapsed() > 3000) {
                m_waitReported = true;
                qCWarning(MpvQt_MpvRenderer) << "still waiting for the previous window to free the render context";
            }
            requestUpdateLater();
        }
        return;
    }

    QOpenGLFramebufferObject *fbo = framebufferObject();
    mpv_opengl_fbo mpfbo;
    mpfbo.fbo = static_cast<int>(fbo->handle());
    mpfbo.w = fbo->width();
    mpfbo.h = fbo->height();
    mpfbo.internal_format = 0;

    int flip_y{0};

    mpv_render_param params[] = {// Specify the default framebuffer (0) as target. This will
                                 // render onto the entire screen. If you want to show the video
                                 // in a smaller rectangle or apply fancy transformations, you'll
                                 // need to render into a separate FBO and draw it manually.
                                 {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
                                 {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    // See render_gl.h on what OpenGL environment mpv expects, and
    // other API details.
    int result = mpv_render_context_render(m_mpvResourceManager->mpvRenderContext, params);
    if (result < 0) {
        qCWarning(MpvQt_MpvRenderer) << "mpv_render_context_render failed:" << MpvController::getError(result);
        return;
    }
}

QOpenGLFramebufferObject *MpvRenderer::createFramebufferObject(const QSize &size)
{
    if (m_elsewhereRequest == 0) {
        ensureRenderContext();
    }

    return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
}

void MpvRenderer::releaseForElsewhere()
{
    // black, never the frame from before: the item shows it again when the video comes back (a new framebuffer of a resize holds undefined pixels too)
    if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
        QOpenGLFunctions *gl = context->functions();
        gl->glClearColor(0, 0, 0, 1);
        gl->glClear(GL_COLOR_BUFFER_BIT);
    }
    if (!m_mpvResourceManager || m_releaseSignalled) {
        return;
    }
    {
        QMutexLocker locker(&s_ownersMutex);
        MpvResourceManager *manager = m_mpvResourceManager.get();
        if (manager->mpvRenderContext) {
            if (s_owners.value(manager) != this) {
                // the previous window's renderer still owns it and frees it when it goes
                locker.unlock();
                requestUpdateLater();
                return;
            }
            manager->freeContext();
            s_owners.remove(manager);
            m_isFramebufferReady = false;
        }
    }
    m_releaseSignalled = true;
    if (m_mpvAItem) {
        QMetaObject::invokeMethod(m_mpvAItem.data(), "renderContextReleased", Qt::QueuedConnection, Q_ARG(int, m_elsewhereRequest));
    }
}

bool MpvRenderer::ensureRenderContext()
{
    if (!m_mpvResourceManager) {
        return false;
    }

    QMutexLocker locker(&s_ownersMutex);
    MpvResourceManager *manager = m_mpvResourceManager.get();
    if (manager->mpvRenderContext) {
        return s_owners.value(manager) == this;
    }
    if (m_contextFailed) {
        return false;
    }

    manager->mpvRenderContext = createMpvRenderContext();
    if (!manager->mpvRenderContext) {
        m_contextFailed = true;
        // the item may tell the application, which can move it back to a window that renders
        if (m_mpvAItem) {
            QMetaObject::invokeMethod(m_mpvAItem.data(), "rendererFailed", Qt::QueuedConnection);
        }
        return false;
    }
    s_owners.insert(manager, this);
    m_isFramebufferReady = true;
    // the next synchronize emits ready, wherever the context was created
    requestUpdate();
    return true;
}

mpv_render_context *MpvRenderer::createMpvRenderContext()
{
    mpv_opengl_init_params gl_init_params{get_proc_address_mpv, nullptr};

    mpv_render_param display{MPV_RENDER_PARAM_INVALID, nullptr};
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID) && !defined(Q_OS_HAIKU)
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
        display.type = MPV_RENDER_PARAM_X11_DISPLAY;
        display.data = qGuiApp->nativeInterface<QNativeInterface::QX11Application>()->display();
    }

    if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
        display.type = MPV_RENDER_PARAM_WL_DISPLAY;
        display.data = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()->display();
    }
#endif

    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                              {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                              display,
                              {MPV_RENDER_PARAM_INVALID, nullptr}};

    mpv_render_context *renderCtx = nullptr;
    mpv_handle *handle = m_mpvResourceManager->mpvHandleManager->mpvHandle;
    int result = mpv_render_context_create(&renderCtx, handle, params);
    if (result < 0) {
        qCCritical(MpvQt_MpvRenderer) << "failed to initialize mpv GL context:" << mpv_error_string(result);
        return nullptr;
    }

    mpv_render_context_set_update_callback(renderCtx, on_mpv_redraw, this);
    return renderCtx;
}

void MpvRenderer::requestUpdateLater()
{
    if (!m_mpvAItem) {
        return;
    }
    QPointer<MpvAbstractItem> item = m_mpvAItem;
    QMetaObject::invokeMethod(
        item.data(),
        [item] {
            QTimer::singleShot(50, item.data(), [item] {
                if (item) {
                    item->update();
                }
            });
        },
        Qt::QueuedConnection);
}

void MpvRenderer::requestUpdate()
{
    if (m_mpvAItem) {
        QMetaObject::invokeMethod(m_mpvAItem.data(), "requestUpdateFromRenderer", Qt::QueuedConnection);
    }
}
