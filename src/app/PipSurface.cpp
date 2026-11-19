// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PipSurface.h"

#include <QGuiApplication>
#include <QVersionNumber>
#include <QtMath>

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
        xx_pip_v1::move(device->wl_seat(), device->serial());
        return true;
    }
    bool resize(QtWaylandClient::QWaylandInputDevice *device, Qt::Edges edges) override
    {
        const std::optional<uint32_t> edge = protocolEdges(edges);
        if (!m_configured || !device || !edge)
            return false;
        xx_pip_v1::resize(device->wl_seat(), device->serial(), *edge);
        return true;
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
        // A surface larger than the bounds is a protocol error, which ends the connection: it shrinks to them, keeping its shape
        if (m_bounds.width() > 0 && m_bounds.height() > 0 && size.width() > 0 && size.height() > 0
            && (size.width() > m_bounds.width() || size.height() > m_bounds.height())) {
            const double scale = qMin(double(m_bounds.width()) / size.width(),
                                      double(m_bounds.height()) / size.height());
            size = QSize(qMin(m_bounds.width(), qFloor(size.width() * scale)),
                         qMin(m_bounds.height(), qFloor(size.height() * scale)));
        }
        resizeFromApplyConfigure(size);
        // The window never grows past the bounds by itself either; set after the resize, which keeps the shape, since a smaller maximum would cut the size at once
        if (QWindow *qwindow = window()->window();
            qwindow && m_bounds.width() > 0 && m_bounds.height() > 0)
            qwindow->setMaximumSize(m_bounds);
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
    // Read by isExposed, which the render thread may call too
    std::atomic<bool> m_configured = false;
    std::optional<uint32_t> m_pendingSerial;
    QSize m_pendingSize;
    QSize m_bounds;
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

// One per application, made on first use on a Wayland connection. Never deleted: its protocol objects would outlive the connection at exit. The private QtWaylandClient API has no binary compatibility promise: another Qt minor version than the one it was built against leaves the window an ordinary one. Patch releases come with the runtime's updates (built against 6.11.2, the host's runtime ran 6.11.3 in the probe) and are accepted
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

PipSurface::PipSurface(QObject *parent) : QObject(parent) {}

bool PipSurface::available() const
{
#ifdef KINEFIN_HAVE_XX_PIP
    return integration() != nullptr;
#else
    return false;
#endif
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
    waylandWindow->setShellIntegration(shell);
    return true;
#else
    Q_UNUSED(window)
    return false;
#endif
}
} // namespace Kinefin
