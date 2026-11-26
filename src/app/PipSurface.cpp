// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PipSurface.h"

#include "PipAspect.h"
#include "PipShadow.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QVersionNumber>

#include <KWindowShadow>

#ifdef KINEFIN_HAVE_XX_PIP
#include <QtWaylandClient/QWaylandClientExtensionTemplate>
#include <QtWaylandClient/private/qwayland-xdg-shell.h>
#include <QtWaylandClient/private/qwaylandinputdevice_p.h>
#include <QtWaylandClient/private/qwaylandshellintegration_p.h>
#include <QtWaylandClient/private/qwaylandshellsurface_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>
#include <qpa/qwindowsysteminterface.h>

#include "qwayland-xx-pip-v1.h"

#include <atomic>
#include <optional>
#endif

namespace Kinefin {
#ifdef KINEFIN_HAVE_XX_PIP
namespace {
// The properties of the window that carry its aspect ratio and least width to the shell surface, which Qt creates when the window first shows
constexpr const char AspectProperty[] = "kinefinPipAspect";
constexpr const char MinimumWidthProperty[] = "kinefinPipMinimumWidth";
// The compositor sends no end of a resize: a gap this long between its configures is one
constexpr qint64 ResizeGapMs = 1000;

// A binding of its own to xdg-shell, through the wrapper QtWaylandClient exports: the picture-in-picture role is an xdg_surface role, created from this application's wl_surface
class XdgWmBase : public QWaylandClientExtensionTemplate<XdgWmBase>, public QtWayland::xdg_wm_base {
  public:
    XdgWmBase() : QWaylandClientExtensionTemplate<XdgWmBase>(1) { initialize(); }

  protected:
    // A binding that does not answer pings looks frozen to the compositor
    void xdg_wm_base_ping(uint32_t serial) override { pong(serial); }
};

class PipShell : public QWaylandClientExtensionTemplate<PipShell>,
                 public QtWayland::xx_pip_shell_v1 {
  public:
    PipShell() : QWaylandClientExtensionTemplate<PipShell>(1) { initialize(); }
};

// Opposite edges together are no edge the protocol knows (an invalid_resize_edge error ends the connection)
std::optional<uint32_t> protocolEdges(Qt::Edges edges)
{
    if (edges == Qt::Edges() || ((edges & Qt::LeftEdge) && (edges & Qt::RightEdge))
        || ((edges & Qt::TopEdge) && (edges & Qt::BottomEdge)))
        return std::nullopt;
    uint32_t result = QtWayland::xx_pip_v1::resize_edge_none;
    if (edges & Qt::TopEdge)
        result |= QtWayland::xx_pip_v1::resize_edge_top;
    if (edges & Qt::BottomEdge)
        result |= QtWayland::xx_pip_v1::resize_edge_bottom;
    if (edges & Qt::LeftEdge)
        result |= QtWayland::xx_pip_v1::resize_edge_left;
    if (edges & Qt::RightEdge)
        result |= QtWayland::xx_pip_v1::resize_edge_right;
    return result;
}

class PipShellSurface : public QtWaylandClient::QWaylandShellSurface,
                        public QtWayland::xdg_surface,
                        public QtWayland::xx_pip_v1 {
  public:
    PipShellSurface(::xdg_surface *surface, ::xx_pip_v1 *pip,
                    QtWaylandClient::QWaylandWindow *window)
        : QWaylandShellSurface(window), QtWayland::xdg_surface(surface), QtWayland::xx_pip_v1(pip)
    {
        if (const QString appId = QGuiApplication::desktopFileName(); !appId.isEmpty())
            xx_pip_v1::set_app_id(appId);
    }
    ~PipShellSurface() override
    {
        xx_pip_v1::destroy();
        xdg_surface::destroy();
    }

    // The role needs a first commit without a buffer, which the compositor answers with the first configure
    bool commitSurfaceRole() const override { return true; }
    bool isExposed() const override { return m_configured; }
    bool move(QtWaylandClient::QWaylandInputDevice *device) override
    {
        if (!m_configured || !device)
            return false;
        m_resizeEdges = {};
        m_lastConfigure.start();
        xx_pip_v1::move(device->wl_seat(), device->serial());
        return true;
    }
    bool resize(QtWaylandClient::QWaylandInputDevice *device, Qt::Edges edges) override
    {
        const std::optional<uint32_t> edge = protocolEdges(edges);
        if (!m_configured || !device || !edge)
            return false;
        m_resizeEdges = edges;
        m_lastConfigure.start();
        xx_pip_v1::resize(device->wl_seat(), device->serial(), *edge);
        return true;
    }
    // The window takes the shape of a new aspect ratio at once, keeping its width as far as the screen allows
    void refit()
    {
        QWindow *qwindow = window()->window();
        if (!m_configured || !qwindow)
            return;
        const QSize now = window()->windowContentGeometry().size();
        const QSize fitted = fitToAspect(now, aspect(), Qt::Edges(), minimumWidth(), screenLimit());
        if (fitted != now)
            qwindow->resize(fitted);
    }
    void setContentGeometry(const QRect &rect) override
    {
        if (m_configured)
            xdg_surface::set_window_geometry(rect.x(), rect.y(), rect.width(), rect.height());
    }
    void applyConfigure() override
    {
        QSize size = window()->windowContentGeometry().size();
        if (m_pendingSize.width() > 0)
            size.setWidth(m_pendingSize.width());
        if (m_pendingSize.height() > 0)
            size.setHeight(m_pendingSize.height());
        // The compositor sizes this window to any shape: it is fitted to the video's. KWin's bounds (a quarter of the work area each way) only shape the first size: like Firefox, the window then grows up to the screen, since KWin does not post the invalid_size error the protocol describes
        if (m_lastConfigure.isValid() && m_lastConfigure.elapsed() > ResizeGapMs)
            m_resizeEdges = {};
        m_lastConfigure.start();
        QWindow *qwindow = window()->window();
        const QSize limit = m_sized ? screenLimit() : m_bounds;
        size = fitToAspect(size, aspect(), m_resizeEdges, minimumWidth(), limit);
        resizeFromApplyConfigure(size);
        m_sized = true;
        // The window never grows past the screen by itself either; set after the resize, which keeps the shape, since a smaller maximum would cut the size at once
        if (const QSize screen = screenLimit();
            qwindow && screen.width() > 0 && screen.height() > 0)
            qwindow->setMaximumSize(screen);
        // Acknowledged once the size is applied, as Qt's xdg-shell does: the next commit already has it
        if (m_pendingSerial) {
            xdg_surface::ack_configure(*m_pendingSerial);
            m_pendingSerial.reset();
        }
    }

  protected:
    void xdg_surface_configure(uint32_t serial) override
    {
        m_pendingSerial = serial;
        if (!m_configured) {
            m_configured = true;
            applyConfigure();
        } else {
            applyConfigureWhenPossible();
        }
        window()->updateExposure();
    }
    void xx_pip_v1_configure_size(int32_t width, int32_t height) override
    {
        m_pendingSize = QSize(width, height);
    }
    void xx_pip_v1_configure_bounds(int32_t width, int32_t height) override
    {
        m_bounds = QSize(width, height);
    }
    // The compositor no longer shows the surface (it declined it, or the user closed it from its controls): the window closes as from its own button
    void xx_pip_v1_closed() override
    {
        QWindowSystemInterface::handleCloseEvent(window()->window());
    }

  private:
    // Not const: QWaylandShellSurface::window() is not
    qreal aspect()
    {
        const QWindow *qwindow = window()->window();
        return qwindow ? qwindow->property(AspectProperty).toReal() : 0;
    }
    int minimumWidth()
    {
        const QWindow *qwindow = window()->window();
        return qwindow ? qwindow->property(MinimumWidthProperty).toInt() : 0;
    }
    QSize screenLimit()
    {
        const QWindow *qwindow = window()->window();
        return qwindow && qwindow->screen() ? qwindow->screen()->availableGeometry().size()
                                            : QSize();
    }

    // Read by isExposed, which the render thread may call too
    std::atomic<bool> m_configured = false;
    std::optional<uint32_t> m_pendingSerial;
    QSize m_pendingSize;
    QSize m_bounds;
    // Set once the first configure has been applied inside the bounds
    bool m_sized = false;
    // The edges of the resize the user started last, which lead the shape of the sizes the compositor configures; none after a move
    Qt::Edges m_resizeEdges;
    QElapsedTimer m_lastConfigure;
};

class PipShellIntegration : public QtWaylandClient::QWaylandShellIntegration {
  public:
    bool active() const { return m_wm.isActive() && m_pip.isActive(); }
    bool initialize(QtWaylandClient::QWaylandDisplay *) override { return active(); }
    QtWaylandClient::QWaylandShellSurface *
    createShellSurface(QtWaylandClient::QWaylandWindow *window) override
    {
        ::xdg_surface *surface = m_wm.get_xdg_surface(window->wlSurface());
        ::xx_pip_v1 *pip = m_pip.get_pip(surface);
        return new PipShellSurface(surface, pip, window);
    }

  private:
    XdgWmBase m_wm;
    PipShell m_pip;
};

// One per application, made on first use on a Wayland connection. Never deleted: its protocol objects would outlive the connection at exit. The private QtWaylandClient API has no binary compatibility promise: another Qt minor version than the one it was built against leaves the window an ordinary one. Patch releases come with the runtime's updates and are accepted
PipShellIntegration *integration()
{
    static PipShellIntegration *const shared = [] {
        const QVersionNumber running = QVersionNumber::fromString(QString::fromLatin1(qVersion()));
        const bool sameMinor = running.majorVersion() == QT_VERSION_MAJOR
                               && running.minorVersion() == QT_VERSION_MINOR;
        return QGuiApplication::platformName().startsWith(QLatin1String("wayland")) && sameMinor
                   ? new PipShellIntegration
                   : nullptr;
    }();
    return shared && shared->active() ? shared : nullptr;
}
} // namespace
#endif

namespace {
// Stores a value of a shadow property; true when it changed
bool changed(qreal &member, qreal value)
{
    if (qFuzzyCompare(value + 1, member + 1))
        return false;
    member = value;
    return true;
}
} // namespace

PipSurface::PipSurface(QObject *parent) : QObject(parent) {}

bool PipSurface::available() const
{
#ifdef KINEFIN_HAVE_XX_PIP
    return integration() != nullptr;
#else
    return false;
#endif
}

void PipSurface::setAspectRatio(qreal ratio)
{
    if (qFuzzyCompare(ratio + 1, m_aspectRatio + 1))
        return;
    m_aspectRatio = ratio;
    Q_EMIT aspectRatioChanged();
#ifdef KINEFIN_HAVE_XX_PIP
    if (!m_window)
        return;
    m_window->setProperty(AspectProperty, m_aspectRatio);
    auto *waylandWindow = dynamic_cast<QtWaylandClient::QWaylandWindow *>(m_window->handle());
    if (auto *surface = waylandWindow
                            ? dynamic_cast<PipShellSurface *>(waylandWindow->shellSurface())
                            : nullptr)
        surface->refit();
#endif
}

void PipSurface::setMinimumWidth(int width)
{
    if (width == m_minimumWidth)
        return;
    m_minimumWidth = width;
    Q_EMIT minimumWidthChanged();
#ifdef KINEFIN_HAVE_XX_PIP
    if (m_window)
        m_window->setProperty(MinimumWidthProperty, m_minimumWidth);
#endif
}

void PipSurface::setShadowSize(qreal size)
{
    if (!changed(m_shadowSize, size))
        return;
    Q_EMIT shadowSizeChanged();
    applyShadow();
}

void PipSurface::setShadowYOffset(qreal offset)
{
    if (!changed(m_shadowYOffset, offset))
        return;
    Q_EMIT shadowYOffsetChanged();
    applyShadow();
}

void PipSurface::setShadowOpacity(qreal opacity)
{
    if (!changed(m_shadowOpacity, opacity))
        return;
    Q_EMIT shadowOpacityChanged();
    applyShadow();
}

void PipSurface::applyShadow()
{
    delete m_shadow;
    m_shadow = nullptr;
    m_shadowCreated = false;
    if (!m_window)
        return;
    const PipShadow shadow =
        pipShadow(m_shadowSize, m_shadowYOffset, m_shadowOpacity, m_window->devicePixelRatio());
    if (shadow.top.isNull())
        return;
    const auto tile = [](const QImage &image) {
        auto shared = KWindowShadowTile::Ptr::create();
        shared->setImage(image);
        return shared;
    };
    m_shadow = new KWindowShadow(this);
    m_shadow->setTopLeftTile(tile(shadow.topLeft));
    m_shadow->setTopTile(tile(shadow.top));
    m_shadow->setTopRightTile(tile(shadow.topRight));
    m_shadow->setRightTile(tile(shadow.right));
    m_shadow->setBottomRightTile(tile(shadow.bottomRight));
    m_shadow->setBottomTile(tile(shadow.bottom));
    m_shadow->setBottomLeftTile(tile(shadow.bottomLeft));
    m_shadow->setLeftTile(tile(shadow.left));
    m_shadow->setPadding(shadow.padding.toMargins());
    m_shadow->setWindow(m_window);
    m_shadowCreated = m_shadow->create();
    if (!m_shadowCreated)
        qWarning() << "the compositor shadow of the picture-in-picture window could not be created";
}

bool PipSurface::assign(QWindow *window)
{
#ifdef KINEFIN_HAVE_XX_PIP
    PipShellIntegration *shell = integration();
    if (!shell || !window || window->isVisible())
        return false;
    window->create();
    auto *waylandWindow = dynamic_cast<QtWaylandClient::QWaylandWindow *>(window->handle());
    if (!waylandWindow)
        return false;
    window->setProperty(AspectProperty, m_aspectRatio);
    window->setProperty(MinimumWidthProperty, m_minimumWidth);
    m_window = window;
    // The shadow needs the surface of the window: when it was not there yet it is made once the window shows
    connect(
        window, &QWindow::visibleChanged, this,
        [this](bool visible) {
            if (visible && !m_shadowCreated)
                applyShadow();
        },
        Qt::SingleShotConnection);
    waylandWindow->setShellIntegration(shell);
    applyShadow();
    return true;
#else
    Q_UNUSED(window)
    return false;
#endif
}
} // namespace Kinefin
