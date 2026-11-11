// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

namespace Kinefin {
// Describes one Jellyfin trickplay tile grid. Validation is intentionally strict:
// metadata controls a remote JPEG crop and may originate from any Jellyfin server.
struct TrickplayLayout {
    int width = 0;
    int height = 0;
    int columns = 0;
    int rows = 0;
    int imageCount = 0;
    int intervalMs = 0;
};
struct TrickplayFrame {
    int tileIndex = 0;
    int column = 0;
    int row = 0;
};

std::optional<TrickplayLayout> parseTrickplayLayout(const QJsonObject &item,
                                                    const QString &mediaSourceId);
std::optional<TrickplayFrame> trickplayFrameAt(const TrickplayLayout &layout,
                                               qint64 positionTicks);
} // namespace Kinefin
