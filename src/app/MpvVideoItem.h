// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QtQml/qqmlregistration.h>
#include <MpvAbstractItem>

namespace Kinefin {
class PlayerController;
}

// MpvQt creates this renderer's mpv handle on a worker thread. The coordinator attaches to exactly that handle, not to a second audio-only instance
class MpvVideoItem : public MpvAbstractItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(MpvVideoItem)
    Q_PROPERTY(QObject *player READ player WRITE setPlayer NOTIFY playerChanged)
  public:
    explicit MpvVideoItem(QQuickItem *parent = nullptr);
    ~MpvVideoItem() override;
    QObject *player() const;
    void setPlayer(QObject *player);
  Q_SIGNALS:
    void playerChanged();
    // The window the item moved to could not create an mpv render context (called by the vendored mpvqt renderer by name): the picture stays black there
    void rendererFailed();

  private:
    void attachWhenReady();
    QPointer<Kinefin::PlayerController> m_player;
    bool m_ready = false;
    bool m_attached = false;
};
