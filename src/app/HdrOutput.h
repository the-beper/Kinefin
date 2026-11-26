// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "player/HdrPolicy.h"

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>

class MpvVideoItem;

namespace Kinefin {
class HdrColorManagement;
class PlayerController;

// The HDR path of the full player: gathers what decides it (the build, the compositor's colour management, the setting, the view, the video and the output), asks `decideHdr`, and hands the video between the item's own renderer and the HDR window below the main window. One instance, in Main.qml
class HdrOutput : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    // The MpvVideoItem the player shows
    Q_PROPERTY(QQuickItem *videoItem READ videoItem WRITE setVideoItem NOTIFY videoItemChanged)
    // The PlayerController
    Q_PROPERTY(QObject *player READ player WRITE setPlayer NOTIFY playerChanged)
    // The full player is shown: not the mini player, not picture in picture, not a cast
    Q_PROPERTY(bool mainView READ mainView WRITE setMainView NOTIFY mainViewChanged)
    // This build and the desktop can show HDR video: the setting can be used
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    // Why not, empty when available
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY availableChanged)
    // From the first HDR frame on the surface below the window until the path leaves: the video area of the window is transparent while it is set
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // A handover in either direction: from its start to the first HDR frame, or to the item's renderer being ready again; the video stays covered while it is set
    Q_PROPERTY(bool switching READ switching NOTIFY switchingChanged)
    // The Playback Info line: on with the output's peak, or off with the reason
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)

  public:
    explicit HdrOutput(QObject *parent = nullptr);
    ~HdrOutput() override;

    // Before the first QQuickWindow: the windows get an alpha channel, which the hole the HDR video shows through needs, only where the path can exist (an HDR build on Wayland)
    static void prepareApplication();

    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);
    QQuickItem *videoItem() const { return m_videoItem; }
    void setVideoItem(QQuickItem *item);
    QObject *player() const;
    void setPlayer(QObject *player);
    bool mainView() const { return m_mainView; }
    void setMainView(bool mainView);
    bool available() const { return m_available; }
    QString unavailableReason() const { return m_unavailableReason; }
    bool active() const { return m_active; }
    bool switching() const { return m_switching; }
    QString statusText() const { return m_statusText; }

    // For the UI tests, which have no HDR surface: sets `active` as the first HDR frame would
    void setActiveForTesting(bool active);

  Q_SIGNALS:
    void windowChanged();
    void videoItemChanged();
    void playerChanged();
    void mainViewChanged();
    void availableChanged();
    void activeChanged();
    void switchingChanged();
    void statusTextChanged();

  private:
    enum class Phase { Off, Entering, On, Leaving };
    // One attempt of the path, from its start to its end: the HDR window, the tag of its surface and how far the handover got
    struct Session;

    MpvVideoItem *mpvItem() const;
    bool isCurrent(uint64_t session) const;
    // Inputs, decision, availability and status, then what the phase does about the decision
    void evaluate();
    void decide();
    void updateStatus();
    void enter();
    void renderContextReleased(MpvVideoItem *item, int request);
    void described(uint64_t session);
    void renderContextCreated(uint64_t session);
    void firstFrameSwapped(uint64_t session);
    void continueEnter();
    void updateInPlace();
    void updateFirstFrameWait();
    void fail(const QString &reason);
    void leave();
    // The window shows its video area opaque again, and the HDR window goes once a frame of it is on screen
    void closeHole();
    void awaitOpaqueFrame();
    void finishLeave();
    // The session lets go of its item at once (it goes, or another one comes): the render context is freed and the mpv target given back, the HDR window stays until closeHole is done. `itemUsable` is false in the item's destructor, which is not called back
    void detachFromItem(bool itemUsable);
    // Called by the item's destructor: the render context goes before the mpv core, the item is not touched any more, and the HDR window goes once the window shows opaque again
    void releaseFromItem();
    // The whole session goes at once, without waiting for a frame (the item, the window or the player changes)
    void dropSession();
    // Stops the render thread, removes the description, deletes the window and gives the video back to the item; emits nothing
    void endSession();
    // The switch ends once the item's renderer made its render context again, when it had freed it; a new session waits for that
    void endSwitchingWhenReady(MpvVideoItem *item, bool released);
    void cancelReadyWait();
    void switchingDone();
    void setActive(bool active);
    void setSwitching(bool switching);

    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem> m_videoItem;
    QPointer<PlayerController> m_player;
    bool m_mainView = false;
    bool m_available = false;
    QString m_unavailableReason;
    bool m_active = false;
    bool m_switching = false;
    QString m_statusText;
    // What the status and the reason texts were made from, so that the frequent evaluations translate nothing again
    HdrReason m_textReason = HdrReason::On;
    int m_textPeak = -1;

    HdrColorManagement *m_colorManagement = nullptr;
    HdrDecision m_decision;
    // The output's own peak in cd/m², which the status shows
    int m_outputPeak = 0;
    Phase m_phase = Phase::Off;
    std::unique_ptr<Session> m_session;
    uint64_t m_sessions = 0;
    // A failed attempt keeps the path off until the playback ends or the setting changes
    bool m_failed = false;

    QTimer m_firstFrameTimer;
    // The first-frame wait stops while paused: this one does not, so a frame that never comes ends the black screen
    QTimer m_enterLimitTimer;
    QTimer m_leaveTimer;
    QTimer m_readyTimer;
    QMetaObject::Connection m_syncConnection;
    QMetaObject::Connection m_swapConnection;
    bool m_awaitingFrame = false;
    QMetaObject::Connection m_readyConnection;
    QMetaObject::Connection m_screenConnection;
    QMetaObject::Connection m_releasedConnection;
    QMetaObject::Connection m_itemWindowConnection;
};
} // namespace Kinefin
