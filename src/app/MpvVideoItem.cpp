// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "MpvVideoItem.h"
#include "player/PlayerController.h"

MpvVideoItem::MpvVideoItem(QQuickItem *parent) : MpvAbstractItem(parent)
{
    connect(this, &MpvAbstractItem::ready, this, [this] {
        m_ready = true;
        // A later ready is a new render context (the item moved to another window)
        if (m_attached && m_player)
            m_player->videoRendererChanged(this);
        else
            attachWhenReady();
    });
}

MpvVideoItem::~MpvVideoItem()
{
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

void MpvVideoItem::attachWhenReady()
{
    if (m_ready && m_player)
        m_attached = m_player->attachVideo(this, mpvController());
}
