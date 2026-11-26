// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "HdrOutput.h"

#include "MpvVideoItem.h"
#include "player/PlayerController.h"

#ifdef KINEFIN_HAVE_HDR_OUTPUT
#include "HdrColorManagement.h"
#include "HdrVideoWindow.h"
#endif

#include <KLocalizedString>

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QVersionNumber>

#include <atomic>
#include <cmath>
#include <optional>
#include <utility>

namespace {
Q_LOGGING_CATEGORY(hdrLog, "kinefin.app.hdr")
}

namespace Kinefin {
namespace {
// Kirigami.Units.humanMoment: how long the HDR window may take for its first frame once one is due, and the item's renderer to be ready again after a leave
constexpr int HandoverMs = 2000;
// However long the first frame takes while paused or loading, the black screen ends after this
constexpr int EnterLimitMs = 10000;
// A window that draws no frame (hidden, minimized) still gives the video back after this
constexpr int OpaqueFrameMs = 250;

bool onWayland()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

// An HDR build, run on the Qt minor version it was built against: the private QtWaylandClient API has no binary compatibility promise. Patch releases come with the runtime's updates and are accepted
bool buildSupported()
{
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    static const bool sameMinor = [] {
        const QVersionNumber running = QVersionNumber::fromString(QString::fromLatin1(qVersion()));
        return running.majorVersion() == QT_VERSION_MAJOR
               && running.minorVersion() == QT_VERSION_MINOR;
    }();
    return sameMinor;
#else
    return false;
#endif
}

#ifdef KINEFIN_HAVE_HDR_OUTPUT
bool sameDescription(const HdrSurfaceDescription &a, const HdrSurfaceDescription &b)
{
    return a.minLum == b.minLum && a.maxLum == b.maxLum && a.refLum == b.refLum
           && a.masteringMin == b.masteringMin && a.masteringMax == b.masteringMax
           && a.maxCll == b.maxCll && a.maxFall == b.maxFall;
}
#endif

// The peak as HdrPolicy reads it: the target luminance when the compositor sends one, else the maximum of an output that is not PQ
int outputPeak(const HdrOutputInfo &output)
{
    const double peak = output.targetMaxLum ? *output.targetMaxLum
                        : output.pqTransfer ? 0
                                            : output.maxLum;
    return std::isfinite(peak) && peak > 0 ? static_cast<int>(std::lround(peak)) : 0;
}

QString notBuiltText()
{
    return i18nc("@info why the HDR Output setting cannot be used",
                 "This build of Kinefin has no HDR output");
}

QString noCompositorText()
{
    return i18nc("@info why the HDR Output setting cannot be used",
                 "The desktop does not offer HDR output to applications");
}

QString statusLine(HdrReason reason, int peak)
{
    switch (reason) {
    case HdrReason::On:
        return i18nc("@info:status HDR output in Playback Info, %1 is the screen's peak",
                     "On · peak %1 cd/m²", peak);
    case HdrReason::NotBuilt:
        return i18nc("@info:status HDR output in Playback Info",
                     "Off: This build of Kinefin has no HDR output");
    case HdrReason::NoCompositorSupport:
        return i18nc("@info:status HDR output in Playback Info",
                     "Off: The desktop does not offer HDR output to applications");
    case HdrReason::SettingOff:
        return i18nc("@info:status HDR output in Playback Info", "Off: HDR Output is set to Off");
    case HdrReason::Casting:
        return i18nc("@info:status HDR output in Playback Info",
                     "Off: The video was sent to another device");
    case HdrReason::NotMainView:
        return i18nc("@info:status HDR output in Playback Info",
                     "Off: Only the full player shows HDR video");
    case HdrReason::NotHdrVideo:
        return i18nc("@info:status HDR output in Playback Info", "Off: The video is not HDR");
    case HdrReason::NoOutputInfo:
        return i18nc("@info:status HDR output in Playback Info",
                     "Off: The brightness range of the screen is not known");
    case HdrReason::LowHeadroom:
        return i18nc("@info:status HDR output in Playback Info",
                     "Off: HDR is off for the screen, or its SDR brightness is too high");
    case HdrReason::Failed:
        return i18nc("@info:status HDR output in Playback Info",
                     "Off: The HDR video could not be shown");
    }
    return {};
}
} // namespace

struct HdrOutput::Session {
    uint64_t id = 0;
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    QPointer<HdrVideoWindow> video;
    std::unique_ptr<HdrSurfaceTag> tag;
#endif
    // Cleared when the item is in its destructor
    QPointer<MpvVideoItem> item;
    // The item's request this session made: a release that answers another one is stale
    int request = 0;
    // The item's renderer freed its render context; the description is on the surface; the render thread was started
    bool released = false;
    bool described = false;
    bool started = false;
    std::optional<int> appliedPeak;
    HdrSurfaceDescription appliedDescription;
};

HdrOutput::HdrOutput(QObject *parent) : QObject(parent)
{
    m_firstFrameTimer.setSingleShot(true);
    m_firstFrameTimer.setInterval(HandoverMs);
    connect(&m_firstFrameTimer, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("no HDR frame within %1 ms").arg(HandoverMs)); });
    m_enterLimitTimer.setSingleShot(true);
    m_enterLimitTimer.setInterval(EnterLimitMs);
    connect(&m_enterLimitTimer, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("no HDR frame within %1 ms").arg(EnterLimitMs)); });
    m_leaveTimer.setSingleShot(true);
    m_leaveTimer.setInterval(OpaqueFrameMs);
    connect(&m_leaveTimer, &QTimer::timeout, this, &HdrOutput::finishLeave);
    m_readyTimer.setSingleShot(true);
    m_readyTimer.setInterval(HandoverMs);
    connect(&m_readyTimer, &QTimer::timeout, this, &HdrOutput::switchingDone);
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    if (buildSupported() && onWayland()) {
        m_colorManagement = new HdrColorManagement(this);
        connect(m_colorManagement, &HdrColorManagement::supportChanged, this, &HdrOutput::evaluate);
        connect(m_colorManagement, &HdrColorManagement::outputChanged, this, &HdrOutput::evaluate);
    }
#endif
    decide();
}

HdrOutput::~HdrOutput()
{
    disconnect(m_syncConnection);
    disconnect(m_swapConnection);
    endSession();
}

void HdrOutput::prepareApplication()
{
    if (buildSupported() && onWayland())
        QQuickWindow::setDefaultAlphaBuffer(true);
}

void HdrOutput::setWindow(QQuickWindow *window)
{
    if (window == m_window)
        return;
    dropSession();
    disconnect(m_screenConnection);
    m_window = window;
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    if (m_window && m_colorManagement) {
        m_screenConnection =
            connect(m_window, &QWindow::screenChanged, this, [this](QScreen *screen) {
                m_colorManagement->watchOutput(screen);
                evaluate();
            });
        m_colorManagement->watchOutput(m_window->screen());
    }
#endif
    Q_EMIT windowChanged();
    evaluate();
}

void HdrOutput::setVideoItem(QQuickItem *item)
{
    if (item == m_videoItem)
        return;
    // Another item has a renderer of its own, which need not wait for this one's
    cancelReadyWait();
    if (m_session) {
        detachFromItem(true);
        setSwitching(true);
        closeHole();
    } else {
        setSwitching(false);
    }
    disconnect(m_releasedConnection);
    disconnect(m_itemWindowConnection);
    m_videoItem = item;
    // A new item is a new playback
    m_failed = false;
    if (MpvVideoItem *video = mpvItem()) {
        m_releasedConnection =
            connect(video, &MpvVideoItem::renderContextReleased, this,
                    [this, video](int request) { renderContextReleased(video, request); });
        m_itemWindowConnection =
            connect(video, &QQuickItem::windowChanged, this, &HdrOutput::evaluate);
    }
    Q_EMIT videoItemChanged();
    evaluate();
}

QObject *HdrOutput::player() const
{
    return m_player.data();
}

void HdrOutput::setPlayer(QObject *object)
{
    auto *player = qobject_cast<PlayerController *>(object);
    if (player == m_player)
        return;
    // The old player's mpv target goes back first
    dropSession();
    if (m_player)
        disconnect(m_player.data(), nullptr, this, nullptr);
    m_player = player;
    m_failed = false;
    if (m_player) {
        connect(m_player, &PlayerController::videoIsHdrChanged, this, &HdrOutput::evaluate);
        connect(m_player, &PlayerController::hdrVideoFactsChanged, this, &HdrOutput::evaluate);
        // Turning the setting Off and back to Auto is the way to try again after a failure
        connect(m_player, &PlayerController::hdrOutputSettingChanged, this, [this] {
            m_failed = false;
            evaluate();
        });
        // Casting, the item, loading, buffering, seeking and pause
        connect(m_player, &PlayerController::changed, this, &HdrOutput::evaluate);
    }
    Q_EMIT playerChanged();
    evaluate();
}

void HdrOutput::setMainView(bool mainView)
{
    if (mainView == m_mainView)
        return;
    m_mainView = mainView;
    Q_EMIT mainViewChanged();
    evaluate();
}

void HdrOutput::setActiveForTesting(bool active)
{
    setActive(active);
}

MpvVideoItem *HdrOutput::mpvItem() const
{
    return qobject_cast<MpvVideoItem *>(m_videoItem.data());
}

bool HdrOutput::isCurrent(uint64_t session) const
{
    return m_session && m_session->id == session;
}

void HdrOutput::evaluate()
{
    decide();
    MpvVideoItem *item = mpvItem();
    const bool wanted = m_decision.on && m_player && item && m_window;
    switch (m_phase) {
    case Phase::Off:
        // Not while the item's renderer is still making its context again after a leave: its release would answer the old request
        if (wanted && !m_switching)
            enter();
        break;
    case Phase::Entering:
    case Phase::On:
        if (!wanted)
            leave();
        else
            updateInPlace();
        break;
    case Phase::Leaving:
        // The end of the switch evaluates again
        break;
    }
    updateFirstFrameWait();
}

void HdrOutput::decide()
{
    // The playback ended: the player keeps the item id between the items of a queue, and clears it only then
    if (m_failed && (!m_player || (!m_player->active() && m_player->itemId().isEmpty())))
        m_failed = false;
    HdrInputs inputs;
    inputs.buildSupported = buildSupported();
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    if (m_colorManagement) {
        inputs.compositor = m_colorManagement->support();
        inputs.output = m_colorManagement->output();
    }
#endif
    const MpvVideoItem *item = mpvItem();
    inputs.mainView = m_mainView && item && m_window && item->window() == m_window;
    if (m_player) {
        inputs.settingAuto = m_player->hdrOutputSettingAuto();
        // The states in which the full player gives way to the cast's own view, which mainView leaves out too
        inputs.casting = m_player->casting() || !m_player->castTargetId().isEmpty()
                         || m_player->canContinueHere();
        inputs.videoIsHdr = m_player->videoIsHdr();
        inputs.video = m_player->hdrVideoFacts();
    }
    inputs.failed = m_failed;
    m_decision = decideHdr(inputs);
    m_outputPeak = outputPeak(inputs.output);
    updateStatus();
}

void HdrOutput::updateStatus()
{
    const int peak = m_decision.on ? m_outputPeak : 0;
    if (m_decision.reason == m_textReason && peak == m_textPeak && !m_statusText.isEmpty())
        return;
    m_textReason = m_decision.reason;
    m_textPeak = peak;

    const QString unavailable = m_decision.reason == HdrReason::NotBuilt ? notBuiltText()
                                : m_decision.reason == HdrReason::NoCompositorSupport
                                    ? noCompositorText()
                                    : QString();
    const bool available = unavailable.isEmpty();
    if (available != m_available || unavailable != m_unavailableReason) {
        m_available = available;
        m_unavailableReason = unavailable;
        Q_EMIT availableChanged();
    }

    const QString status = statusLine(m_decision.reason, peak);
    if (status != m_statusText) {
        m_statusText = status;
        Q_EMIT statusTextChanged();
    }
}

void HdrOutput::enter()
{
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    MpvVideoItem *item = mpvItem();
    mpv_handle *mpv = item ? item->mpvHandle() : nullptr;
    if (!item || !mpv || !m_window || !m_player || !m_colorManagement)
        return;
    cancelReadyWait();
    m_session = std::make_unique<Session>();
    const uint64_t id = m_session->id = ++m_sessions;
    m_session->item = item;
    m_phase = Phase::Entering;
    m_enterLimitTimer.start();
    setSwitching(true);
    qCDebug(hdrLog) << "entering, target peak" << m_decision.targetPeak;

    auto *video = new HdrVideoWindow(m_window, mpv);
    m_session->video = video;
    connect(video, &HdrVideoWindow::renderContextCreated, this,
            [this, id] { renderContextCreated(id); });
    connect(video, &HdrVideoWindow::firstFrameSwapped, this, [this, id] { firstFrameSwapped(id); });
    connect(video, &HdrVideoWindow::failed, this, [this, id](const QString &reason) {
        if (isCurrent(id))
            fail(reason);
    });

    m_session->tag = m_colorManagement->tagSurface(video);
    if (!m_session->tag)
        return fail(QStringLiteral("the HDR window has no surface to describe"));
    connect(m_session->tag.get(), &HdrSurfaceTag::described, this, [this, id] { described(id); });
    // Queued: the tag may say it while Qt destroys the surface
    connect(
        m_session->tag.get(), &HdrSurfaceTag::failed, this,
        [this, id](const QString &reason) {
            if (isCurrent(id))
                fail(QStringLiteral("image description: %1").arg(reason));
        },
        Qt::QueuedConnection);
    m_session->appliedDescription = m_decision.description;
    m_session->tag->setDescription(m_decision.description);

    if (!video->showBelowParent())
        return fail(QStringLiteral("the HDR window got no subsurface"));

    item->setReleaseElsewhere([self = QPointer<HdrOutput>(this)] {
        if (self)
            self->releaseFromItem();
    });
    item->setRenderElsewhere(true);
    m_session->request = item->renderElsewhereRequest();
#endif
}

void HdrOutput::renderContextReleased(MpvVideoItem *item, int request)
{
    if (m_phase != Phase::Entering || !m_session || m_session->released || m_session->item != item
        || m_session->request != request)
        return;
    m_session->released = true;
    // Only now: the item's renderer draws no frame with the HDR target
    m_player->setHdrTarget(m_decision.targetPeak);
    m_session->appliedPeak = m_decision.targetPeak;
    continueEnter();
}

void HdrOutput::described(uint64_t session)
{
    if (!isCurrent(session))
        return;
    m_session->described = true;
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    // A description that changed while on: the next swap commits it
    if (m_phase == Phase::On && m_session->video)
        m_session->video->redraw();
#endif
    continueEnter();
}

void HdrOutput::continueEnter()
{
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    if (m_phase != Phase::Entering || !m_session || m_session->started || !m_session->released
        || !m_session->described || !m_session->video)
        return;
    m_session->started = true;
    m_session->video->start();
#endif
}

void HdrOutput::renderContextCreated(uint64_t session)
{
    if (!isCurrent(session) || m_phase != Phase::Entering)
        return;
    qCDebug(hdrLog) << "render context created";
    // libmpv turned the video off when the item's context went
    if (m_session->item && m_player)
        m_player->videoRendererChanged(m_session->item);
}

void HdrOutput::firstFrameSwapped(uint64_t session)
{
    if (!isCurrent(session) || m_phase != Phase::Entering)
        return;
    qCDebug(hdrLog) << "first HDR frame";
    m_firstFrameTimer.stop();
    m_enterLimitTimer.stop();
    m_phase = Phase::On;
    setActive(true);
    setSwitching(false);
}

void HdrOutput::updateInPlace()
{
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    if (!m_session)
        return;
    if (m_session->released && m_session->appliedPeak != m_decision.targetPeak) {
        m_player->setHdrTarget(m_decision.targetPeak);
        m_session->appliedPeak = m_decision.targetPeak;
    }
    if (m_session->tag && !sameDescription(m_session->appliedDescription, m_decision.description)) {
        m_session->appliedDescription = m_decision.description;
        m_session->tag->setDescription(m_decision.description);
    }
#endif
}

void HdrOutput::updateFirstFrameWait()
{
    // The wait counts only while a frame is due: not while the file opens, buffers or seeks, nor paused before its first frame
    const bool waiting = m_phase == Phase::Entering && m_player && !m_player->loading()
                         && !m_player->buffering() && !m_player->seeking() && !m_player->paused();
    if (!waiting)
        m_firstFrameTimer.stop();
    else if (!m_firstFrameTimer.isActive())
        m_firstFrameTimer.start();
}

void HdrOutput::fail(const QString &reason)
{
    if (m_phase != Phase::Entering && m_phase != Phase::On)
        return;
    qCWarning(hdrLog) << "the HDR output failed, the video goes back to SDR:" << reason;
    m_failed = true;
    decide();
    leave();
}

void HdrOutput::leave()
{
    if (m_phase != Phase::Entering && m_phase != Phase::On)
        return;
    m_firstFrameTimer.stop();
    m_enterLimitTimer.stop();
    m_phase = Phase::Leaving;
    qCDebug(hdrLog) << "leaving";
    setSwitching(true);
    closeHole();
}

void HdrOutput::closeHole()
{
    if (m_awaitingFrame)
        return;
    const bool holeOpen = m_active;
    setActive(false);
    if (holeOpen && m_window)
        awaitOpaqueFrame();
    else
        finishLeave();
}

void HdrOutput::awaitOpaqueFrame()
{
    m_awaitingFrame = true;
    // The window must show its opaque video area before the surface below it goes, or the desktop shows through for a frame: a frame synchronized after now, once it is swapped. Both run on the scene graph's render thread and touch only what they hold, since this object may go on its own thread meanwhile
    auto synced = std::make_shared<std::atomic<bool>>(false);
    QQuickWindow *window = m_window;
    const QPointer<HdrOutput> self(this);
    const uint64_t session = m_session ? m_session->id : 0;
    m_syncConnection = connect(
        window, &QQuickWindow::afterSynchronizing, window, [synced] { *synced = true; },
        Qt::DirectConnection);
    m_swapConnection = connect(
        window, &QQuickWindow::frameSwapped, window,
        [synced, window, self, session] {
            if (!synced->exchange(false))
                return;
            QMetaObject::invokeMethod(
                window,
                [self, session] {
                    if (self && self->isCurrent(session))
                        self->finishLeave();
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);
    window->update();
    m_leaveTimer.start();
}

void HdrOutput::finishLeave()
{
    if (m_phase != Phase::Leaving)
        return;
    MpvVideoItem *item = m_session ? m_session->item.data() : nullptr;
    const bool released = m_session && m_session->released;
    endSession();
    endSwitchingWhenReady(item, released);
}

void HdrOutput::detachFromItem(bool itemUsable)
{
    if (!m_session)
        return;
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    // The surface keeps the last frame
    if (m_session->video)
        m_session->video->stop();
#endif
    if (itemUsable && m_session->item) {
        m_session->item->setReleaseElsewhere({});
        m_session->item->setRenderElsewhere(false);
    }
    m_session->item = nullptr;
    if (m_player)
        m_player->setHdrTarget(std::nullopt);
    m_firstFrameTimer.stop();
    if (m_phase == Phase::Entering || m_phase == Phase::On)
        m_phase = Phase::Leaving;
}

void HdrOutput::releaseFromItem()
{
    detachFromItem(false);
    cancelReadyWait();
    // The item detaches from the player next, whose signals must not start a session on it
    disconnect(m_releasedConnection);
    disconnect(m_itemWindowConnection);
    m_videoItem = nullptr;
    // The playback ends with its item
    m_failed = false;
    // Told later: the item is in its destructor, and the QML bindings on these may touch it
    QMetaObject::invokeMethod(
        this,
        [this] {
            Q_EMIT videoItemChanged();
            // A session started meanwhile on another item keeps its own state
            if (m_phase == Phase::Leaving) {
                closeHole();
            } else if (m_phase == Phase::Off) {
                setActive(false);
                setSwitching(false);
            }
            evaluate();
        },
        Qt::QueuedConnection);
}

void HdrOutput::dropSession()
{
    if (m_phase == Phase::Off && !m_session)
        return;
    MpvVideoItem *item = m_session ? m_session->item.data() : nullptr;
    const bool released = m_session && m_session->released;
    endSession();
    setActive(false);
    endSwitchingWhenReady(item, released);
}

void HdrOutput::endSession()
{
    m_firstFrameTimer.stop();
    m_leaveTimer.stop();
    disconnect(m_syncConnection);
    disconnect(m_swapConnection);
    m_awaitingFrame = false;
    m_phase = Phase::Off;
    if (!m_session)
        return;
    const std::unique_ptr<Session> session = std::move(m_session);
#ifdef KINEFIN_HAVE_HDR_OUTPUT
    // The render context goes first, while the surface it draws into is there; then the description, then the window
    if (session->video)
        session->video->stop();
    session->tag.reset();
    delete session->video.data();
#endif
    if (m_player)
        m_player->setHdrTarget(std::nullopt);
    if (session->item) {
        session->item->setReleaseElsewhere({});
        session->item->setRenderElsewhere(false);
    }
}

void HdrOutput::endSwitchingWhenReady(MpvVideoItem *item, bool released)
{
    cancelReadyWait();
    // A renderer that never freed its context has nothing to make again
    if (!item || !released) {
        switchingDone();
        return;
    }
    m_readyConnection = connect(item, &MpvAbstractItem::ready, this, &HdrOutput::switchingDone);
    m_readyTimer.start();
}

void HdrOutput::cancelReadyWait()
{
    disconnect(m_readyConnection);
    m_readyTimer.stop();
}

void HdrOutput::switchingDone()
{
    cancelReadyWait();
    setSwitching(false);
    // What changed meanwhile, and a new session that waited for the switch to end
    QMetaObject::invokeMethod(this, &HdrOutput::evaluate, Qt::QueuedConnection);
}

void HdrOutput::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
}

void HdrOutput::setSwitching(bool switching)
{
    if (switching == m_switching)
        return;
    m_switching = switching;
    Q_EMIT switchingChanged();
}
} // namespace Kinefin
