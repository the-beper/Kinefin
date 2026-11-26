// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QtQml/qqmlregistration.h>
#include <MpvAbstractItem>

#include <functional>
#include <utility>

namespace Kinefin {
class PlayerController;
}

// MpvQt creates this renderer's mpv handle on a worker thread. The coordinator attaches to exactly that handle, not to a second audio-only instance
class MpvVideoItem : public MpvAbstractItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(MpvVideoItem)
    Q_PROPERTY(QObject *player READ player WRITE setPlayer NOTIFY playerChanged)
    // Another renderer draws the video (the HDR window): the item's own renderer frees its mpv render context and makes none until this is cleared
    Q_PROPERTY(bool renderElsewhere READ renderElsewhere WRITE setRenderElsewhere NOTIFY
                   renderElsewhereChanged)
    // The number of the request renderElsewhere stands for, 0 while it is false; read by the vendored mpvqt renderer by name, which hands it back with renderContextReleased
    Q_PROPERTY(int renderElsewhereRequest READ renderElsewhereRequest NOTIFY renderElsewhereChanged)
  public:
    explicit MpvVideoItem(QQuickItem *parent = nullptr);
    ~MpvVideoItem() override;
    QObject *player() const;
    void setPlayer(QObject *player);
    bool renderElsewhere() const { return m_elsewhereRequest != 0; }
    void setRenderElsewhere(bool elsewhere);
    int renderElsewhereRequest() const { return m_elsewhereRequest; }
    // The mpv core, for the renderer that draws the video elsewhere
    mpv_handle *mpvHandle() const;
    // Run first by the destructor: the other renderer frees its render context there, since that must go before the mpv core does. An empty function clears it
    void setReleaseElsewhere(std::function<void()> release)
    {
        m_releaseElsewhere = std::move(release);
    }
  Q_SIGNALS:
    void playerChanged();
    void renderElsewhereChanged();
    // No renderer of the item holds the render context any more for the request `request` (called by the vendored mpvqt renderer by name): the other renderer may create its own
    void renderContextReleased(int request);
    // The window the item moved to could not create an mpv render context (called by the vendored mpvqt renderer by name): the picture stays black there
    void rendererFailed();

  private:
    void attachWhenReady();
    QPointer<Kinefin::PlayerController> m_player;
    bool m_ready = false;
    bool m_attached = false;
    int m_elsewhereRequest = 0;
    int m_elsewhereRequests = 0;
    std::function<void()> m_releaseElsewhere;
};
