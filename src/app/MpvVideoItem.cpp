// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MpvVideoItem.h"
#include "player/PlayerController.h"

#include <utility>

MpvVideoItem::MpvVideoItem(QQuickItem *parent) : MpvAbstractItem(parent)
{
    connect(this, &MpvAbstractItem::ready, this, [this] {
        m_ready = true;
        // A later ready is a new render context (the item moved to another window, or the HDR window gave the video back)
        if (m_attached && m_player)
            m_player->videoRendererChanged(this);
        else
            attachWhenReady();
    });
}

MpvVideoItem::~MpvVideoItem()
{
    if (m_releaseElsewhere)
        std::exchange(m_releaseElsewhere, {})();
    if (m_player)
        m_player->detachVideo(this);
}

QObject *MpvVideoItem::player() const
{
    return m_player.data();
}

void MpvVideoItem::setPlayer(QObject *object)
{
    auto *candidate = qobject_cast<Kinefin::PlayerController *>(object);
    if (candidate == m_player)
        return;
    if (m_player)
        m_player->detachVideo(this);
    m_attached = false;
    m_player = candidate;
    attachWhenReady();
    Q_EMIT playerChanged();
}

void MpvVideoItem::setRenderElsewhere(bool elsewhere)
{
    if (elsewhere == renderElsewhere())
        return;
    m_elsewhereRequest = elsewhere ? ++m_elsewhereRequests : 0;
    Q_EMIT renderElsewhereChanged();
    // The renderer acts on it in its next frame
    update();
}

mpv_handle *MpvVideoItem::mpvHandle() const
{
    // mpvController() is protected and not const
    MpvController *controller = const_cast<MpvVideoItem *>(this)->mpvController();
    return controller ? controller->mpv() : nullptr;
}

void MpvVideoItem::attachWhenReady()
{
    if (m_ready && m_player)
        m_attached = m_player->attachVideo(this, mpvController());
}
