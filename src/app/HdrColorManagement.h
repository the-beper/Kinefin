// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "player/HdrPolicy.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <memory>

class QScreen;
class QWindow;
struct wl_output;
struct wl_surface;

namespace Kinefin {
class HdrSurfaceTag;

namespace HdrWayland {
class ColorManager;
class OutputWatch;
class PendingDescription;
class OutputInformation;
class SurfaceObject;
} // namespace HdrWayland

// A client of the compositor's wp_color_manager_v1 on QtWaylandClient's own connection, through Qt's exported wrappers of the protocol: what the compositor supports, the image description of the output the window is on, and the tags of surfaces. Never waits: every answer of the compositor arrives as a signal from Qt's own dispatch of the Wayland queue, on the GUI thread. Only on Wayland
class HdrColorManagement : public QObject {
    Q_OBJECT
  public:
    explicit HdrColorManagement(QObject *parent = nullptr);
    ~HdrColorManagement() override;

    // All false until the compositor has listed what it supports
    HdrCompositorSupport support() const { return m_support; }
    // Follows the output of `screen`: read now, and again whenever the compositor says its description changed
    void watchOutput(QScreen *screen);
    // Not known until the compositor answered, nor after it failed to
    HdrOutputInfo output() const { return m_output; }
    // The colour-management object of the window's wl_surface, created at once; null without a manager or a surface. Only one may exist per surface: a second is a protocol error
    std::unique_ptr<HdrSurfaceTag> tagSurface(QWindow *window);

  Q_SIGNALS:
    void supportChanged();
    void outputChanged();

  private:
    void refreshOutput();
    void queryOutput();
    void setOutput(const HdrOutputInfo &info);

    std::unique_ptr<HdrWayland::ColorManager> m_manager;
    HdrCompositorSupport m_support;
    QPointer<QScreen> m_screen;
    ::wl_output *m_watchedOutput = nullptr;
    std::unique_ptr<HdrWayland::OutputWatch> m_outputWatch;
    // The query in flight: the description stays until its information is done
    std::unique_ptr<HdrWayland::PendingDescription> m_outputDescription;
    std::unique_ptr<HdrWayland::OutputInformation> m_outputInformation;
    uint64_t m_outputGeneration = 0;
    bool m_outputFailureLogged = false;
    HdrOutputInfo m_output;
};

// The image description of one surface: PQ, BT.2020 and what an `HdrSurfaceDescription` carries. A new description replaces one still pending; `described` says that it is set on the surface (its next commit applies it), `failed` that the compositor refused it or the surface went. Deleting the tag removes the description from the surface
class HdrSurfaceTag : public QObject {
    Q_OBJECT
  public:
    ~HdrSurfaceTag() override;
    void setDescription(const HdrSurfaceDescription &description);

  Q_SIGNALS:
    void described();
    void failed(const QString &reason);

  private:
    friend class HdrColorManagement;
    HdrSurfaceTag(HdrWayland::ColorManager *manager, const HdrCompositorSupport &support,
                  QWindow *window, ::wl_surface *surface);
    void answered(HdrWayland::PendingDescription *description, bool ready, const QString &message);
    // The window still has the surface the tag was made for: requests on the object of a destroyed surface are a protocol error
    bool surfaceAlive() const;

    HdrWayland::ColorManager *m_manager;
    HdrCompositorSupport m_support;
    QPointer<QWindow> m_window;
    ::wl_surface *m_wlSurface;
    bool m_inert = false;
    std::unique_ptr<HdrWayland::SurfaceObject> m_surface;
    std::unique_ptr<HdrWayland::PendingDescription> m_pending;
};
} // namespace Kinefin
