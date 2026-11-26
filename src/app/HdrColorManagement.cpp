// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "HdrColorManagement.h"

#include <QLoggingCategory>
#include <QScreen>
#include <QWindow>

#include <QtWaylandClient/QWaylandClientExtensionTemplate>
#include <QtWaylandClient/private/qwayland-color-management-v1.h>
#include <QtWaylandClient/private/qwaylandscreen_p.h>
#include <QtWaylandClient/private/qwaylandwindow_p.h>

#include <wayland-client-core.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unistd.h>
#include <utility>

namespace {
Q_LOGGING_CATEGORY(hdrLog, "kinefin.app.hdr")
}

namespace Kinefin {
namespace HdrWayland {
using Manager = QtWayland::wp_color_manager_v1;

// Version 1 is the one Qt's wrappers know: its image descriptions answer with `ready`, not `ready2`
class ColorManager : public QWaylandClientExtensionTemplate<ColorManager>,
                     public QtWayland::wp_color_manager_v1 {
  public:
    explicit ColorManager(std::function<void(const HdrCompositorSupport &)> done)
        : QWaylandClientExtensionTemplate<ColorManager>(1), m_done(std::move(done))
    {
        initialize();
    }
    ~ColorManager() override
    {
        if (QtWayland::wp_color_manager_v1::isInitialized())
            QtWayland::wp_color_manager_v1::destroy();
    }

  protected:
    void wp_color_manager_v1_supported_feature(uint32_t feature) override
    {
        if (feature == feature_parametric)
            m_support.parametric = true;
        else if (feature == feature_set_luminances)
            m_support.setLuminances = true;
        else if (feature == feature_set_mastering_display_primaries)
            m_support.setMasteringPrimaries = true;
    }
    void wp_color_manager_v1_supported_tf_named(uint32_t tf) override
    {
        if (tf == transfer_function_st2084_pq)
            m_support.pq = true;
    }
    void wp_color_manager_v1_supported_primaries_named(uint32_t primaries) override
    {
        if (primaries == primaries_bt2020)
            m_support.bt2020 = true;
    }
    void wp_color_manager_v1_done() override { m_done(m_support); }

  private:
    HdrCompositorSupport m_support;
    std::function<void(const HdrCompositorSupport &)> m_done;
};

class OutputWatch : public QtWayland::wp_color_management_output_v1 {
  public:
    OutputWatch(::wp_color_management_output_v1 *object, std::function<void()> changed)
        : QtWayland::wp_color_management_output_v1(object), m_changed(std::move(changed))
    {
    }
    ~OutputWatch() override { destroy(); }

  protected:
    void wp_color_management_output_v1_image_description_changed() override { m_changed(); }

  private:
    std::function<void()> m_changed;
};

// An image description until the compositor answers it, ready or failed. Retired by `retire`, which destroys the proxy at once so that no later event reaches it, and the object once the event that may be running returns
class PendingDescription : public QObject, public QtWayland::wp_image_description_v1 {
  public:
    using Answer = std::function<void(PendingDescription *, bool ready, const QString &message)>;
    PendingDescription(::wp_image_description_v1 *object, Answer answer)
        : QtWayland::wp_image_description_v1(object), m_answer(std::move(answer))
    {
    }
    ~PendingDescription() override { destroyProxy(); }
    void destroyProxy()
    {
        if (std::exchange(m_alive, false))
            destroy();
    }

  protected:
    void wp_image_description_v1_ready(uint32_t) override { m_answer(this, true, {}); }
    void wp_image_description_v1_failed(uint32_t cause, const QString &message) override
    {
        m_answer(this, false, QStringLiteral("cause %1: %2").arg(cause).arg(message));
    }

  private:
    Answer m_answer;
    bool m_alive = true;
};

class OutputInformation : public QObject, public QtWayland::wp_image_description_info_v1 {
  public:
    OutputInformation(::wp_image_description_info_v1 *object,
                      std::function<void(const HdrOutputInfo &)> done)
        : QtWayland::wp_image_description_info_v1(object), m_done(std::move(done))
    {
    }
    // The object has no destructor request: the compositor destroys it with `done`, the client frees the proxy
    ~OutputInformation() override { destroyProxy(); }
    void destroyProxy()
    {
        if (std::exchange(m_alive, false))
            ::wp_image_description_info_v1_destroy(object());
    }

  protected:
    void wp_image_description_info_v1_tf_named(uint32_t tf) override
    {
        m_info.pqTransfer = tf == Manager::transfer_function_st2084_pq;
    }
    void wp_image_description_info_v1_luminances(uint32_t minLum, uint32_t maxLum,
                                                 uint32_t referenceLum) override
    {
        m_info.minLum = minLum / 10000.0;
        m_info.maxLum = maxLum;
        m_info.refLum = referenceLum;
    }
    void wp_image_description_info_v1_target_luminance(uint32_t, uint32_t maxLum) override
    {
        m_info.targetMaxLum = maxLum;
    }
    // The descriptor is the client's to close
    void wp_image_description_info_v1_icc_file(int32_t icc, uint32_t) override { ::close(icc); }
    void wp_image_description_info_v1_done() override
    {
        destroyProxy();
        m_info.known = true;
        m_done(m_info);
    }

  private:
    HdrOutputInfo m_info;
    std::function<void(const HdrOutputInfo &)> m_done;
    bool m_alive = true;
};

class SurfaceObject : public QtWayland::wp_color_management_surface_v1 {
  public:
    using QtWayland::wp_color_management_surface_v1::wp_color_management_surface_v1;
    // An inert object (its wl_surface destroyed) is only freed on this side: the protocol names no request as allowed on it
    ~SurfaceObject() override
    {
        if (inert)
            wl_proxy_destroy(reinterpret_cast<wl_proxy *>(object()));
        else
            destroy();
    }
    bool inert = false;
};

namespace {
template <typename T> void retire(std::unique_ptr<T> &object)
{
    if (!object)
        return;
    object->destroyProxy();
    object.release()->deleteLater();
}

bool sameOutput(const HdrOutputInfo &a, const HdrOutputInfo &b)
{
    return a.known == b.known && a.minLum == b.minLum && a.maxLum == b.maxLum
           && a.refLum == b.refLum && a.targetMaxLum == b.targetMaxLum
           && a.pqTransfer == b.pqTransfer;
}

QtWaylandClient::QWaylandWindow *waylandWindow(QWindow *window)
{
    return window ? dynamic_cast<QtWaylandClient::QWaylandWindow *>(window->handle()) : nullptr;
}

// The minimum of the primary volume that PQ implies when no luminances are sent (0.005 cd/m²), in 1/10000 cd/m²
constexpr uint64_t PqDefaultMinimum = 50;

// The luminances in the protocol's units: a minimum in 1/10000 cd/m², the others in whole cd/m²
uint32_t scaledMinimum(double luminance)
{
    return static_cast<uint32_t>(std::lround(luminance * 10000));
}
} // namespace
} // namespace HdrWayland

using namespace HdrWayland;

HdrColorManagement::HdrColorManagement(QObject *parent) : QObject(parent)
{
    m_manager = std::make_unique<ColorManager>([this](const HdrCompositorSupport &support) {
        m_support = support;
        Q_EMIT supportChanged();
        refreshOutput();
    });
}

HdrColorManagement::~HdrColorManagement() = default;

void HdrColorManagement::watchOutput(QScreen *screen)
{
    m_screen = screen;
    refreshOutput();
}

void HdrColorManagement::refreshOutput()
{
    ::wl_output *output = nullptr;
    if (m_screen) {
        if (auto *screen = dynamic_cast<QtWaylandClient::QWaylandScreen *>(m_screen->handle()))
            output = screen->output();
    }
    if (output == m_watchedOutput && m_outputWatch)
        return;
    retire(m_outputInformation);
    retire(m_outputDescription);
    m_outputWatch.reset();
    m_watchedOutput = output;
    if (!output || !m_manager->isActive()) {
        setOutput({});
        return;
    }
    m_outputWatch =
        std::make_unique<OutputWatch>(m_manager->get_output(output), [this] { queryOutput(); });
    queryOutput();
}

void HdrColorManagement::queryOutput()
{
    retire(m_outputInformation);
    retire(m_outputDescription);
    if (!m_outputWatch)
        return;
    const uint64_t generation = ++m_outputGeneration;
    m_outputDescription = std::make_unique<PendingDescription>(
        m_outputWatch->get_image_description(),
        [this, generation](PendingDescription *description, bool ready, const QString &message) {
            if (generation != m_outputGeneration)
                return;
            if (!ready) {
                if (!std::exchange(m_outputFailureLogged, true))
                    qCWarning(hdrLog) << "the compositor did not describe the output:" << message;
                retire(m_outputDescription);
                setOutput({});
                return;
            }
            m_outputInformation = std::make_unique<OutputInformation>(
                description->get_information(), [this, generation](const HdrOutputInfo &info) {
                    if (generation != m_outputGeneration)
                        return;
                    retire(m_outputInformation);
                    retire(m_outputDescription);
                    setOutput(info);
                });
        });
}

void HdrColorManagement::setOutput(const HdrOutputInfo &info)
{
    if (sameOutput(info, m_output))
        return;
    m_output = info;
    qCDebug(hdrLog) << "output: known" << info.known << "min" << info.minLum << "max" << info.maxLum
                    << "reference" << info.refLum << "target max" << info.targetMaxLum.value_or(0)
                    << "pq" << info.pqTransfer;
    Q_EMIT outputChanged();
}

std::unique_ptr<HdrSurfaceTag> HdrColorManagement::tagSurface(QWindow *window)
{
    QtWaylandClient::QWaylandWindow *wayland = waylandWindow(window);
    ::wl_surface *surface = wayland ? wayland->wlSurface() : nullptr;
    if (!surface || !m_manager->isActive())
        return nullptr;
    return std::unique_ptr<HdrSurfaceTag>(
        new HdrSurfaceTag(m_manager.get(), m_support, window, surface));
}

HdrSurfaceTag::HdrSurfaceTag(ColorManager *manager, const HdrCompositorSupport &support,
                             QWindow *window, ::wl_surface *surface)
    : m_manager(manager), m_support(support), m_window(window), m_wlSurface(surface),
      m_surface(std::make_unique<SurfaceObject>(manager->get_surface(surface)))
{
    connect(waylandWindow(window), &QtWaylandClient::QWaylandWindow::wlSurfaceDestroyed, this,
            [this] {
                m_inert = true;
                retire(m_pending);
                Q_EMIT failed(QStringLiteral("the surface was destroyed"));
            });
}

HdrSurfaceTag::~HdrSurfaceTag()
{
    m_surface->inert = !surfaceAlive();
}

bool HdrSurfaceTag::surfaceAlive() const
{
    if (m_inert)
        return false;
    QtWaylandClient::QWaylandWindow *wayland = waylandWindow(m_window);
    return wayland && wayland->wlSurface() == m_wlSurface;
}

void HdrSurfaceTag::setDescription(const HdrSurfaceDescription &description)
{
    retire(m_pending);
    if (!surfaceAlive()) {
        Q_EMIT failed(QStringLiteral("the surface is gone"));
        return;
    }
    QtWayland::wp_image_description_creator_params_v1 creator(
        m_manager->create_parametric_creator());
    creator.set_tf_named(Manager::transfer_function_st2084_pq);
    creator.set_primaries_named(Manager::primaries_bt2020);
    // HdrPolicy validated the values; these checks are on what the protocol itself compares after the rounding to its units (1/10000 cd/m² here), where a failure is an invalid_luminance error that ends the connection
    uint64_t primaryMin = PqDefaultMinimum;
    if (m_support.setLuminances && std::isfinite(description.minLum) && description.minLum >= 0
        && description.minLum < description.refLum) {
        const uint32_t minimum = scaledMinimum(description.minLum);
        if (uint64_t(description.refLum) * 10000 > minimum
            && uint64_t(description.maxLum) * 10000 > minimum) {
            creator.set_luminances(minimum, description.maxLum, description.refLum);
            primaryMin = std::max<uint64_t>(minimum, PqDefaultMinimum);
        }
    }
    // max_cll and max_fall must lie above the minimum and at most at the maximum of the mastering luminance, which is the primary volume when none is sent: with PQ from the minimum sent (0.005 cd/m² without one) to 10000 cd/m² above it. The higher of the two minimums and 10000 itself hold however a compositor reads that
    uint64_t rangeMin = primaryMin;
    uint64_t rangeMax = uint64_t(10000) * 10000;
    if (m_support.setMasteringPrimaries && description.masteringMin && description.masteringMax
        && *description.masteringMin >= 0 && *description.masteringMin < *description.masteringMax
        && *description.masteringMax <= 10000) {
        const uint32_t masteringMin = scaledMinimum(*description.masteringMin);
        const auto masteringMax = static_cast<uint32_t>(std::lround(*description.masteringMax));
        if (uint64_t(masteringMax) * 10000 > masteringMin) {
            creator.set_mastering_luminance(masteringMin, masteringMax);
            rangeMin = masteringMin;
            rangeMax = uint64_t(masteringMax) * 10000;
        }
    }
    const auto inRange = [rangeMin, rangeMax](uint32_t level) {
        const uint64_t scaled = uint64_t(level) * 10000;
        return scaled > rangeMin && scaled <= rangeMax;
    };
    const bool cllSent = description.maxCll && inRange(*description.maxCll);
    if (cllSent)
        creator.set_max_cll(*description.maxCll);
    if (description.maxFall && inRange(*description.maxFall)
        && (!cllSent || *description.maxFall <= *description.maxCll))
        creator.set_max_fall(*description.maxFall);
    // create() destroys the creator
    m_pending = std::make_unique<PendingDescription>(
        creator.create(), [this](PendingDescription *pending, bool ready, const QString &message) {
            answered(pending, ready, message);
        });
}

void HdrSurfaceTag::answered(PendingDescription *description, bool ready, const QString &message)
{
    if (description != m_pending.get())
        return;
    if (ready && surfaceAlive()) {
        m_surface->set_image_description(description->object(), Manager::render_intent_perceptual);
        // The surface keeps a copy
        retire(m_pending);
        Q_EMIT described();
        return;
    }
    retire(m_pending);
    Q_EMIT failed(ready ? QStringLiteral("the surface is gone") : message);
}
} // namespace Kinefin
