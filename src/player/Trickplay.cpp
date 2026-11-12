// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/Trickplay.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace Kinefin {
std::optional<TrickplayLayout> parseTrickplayLayout(const QJsonObject &item,
                                                    const QString &mediaSourceId)
{
    const QJsonObject map = item.value(QStringLiteral("Trickplay")).toObject();
    // Jellyfin indexes the width maps by media source id; a few older builds
    // send a single width map instead of the nested form.
    QJsonObject widths = map.value(mediaSourceId).toObject();
    // Older servers may return the width map directly, without the media-source level.
    if (widths.isEmpty() && !map.isEmpty()) {
        bool isWidth = false;
        map.constBegin().key().toInt(&isWidth);
        if (isWidth)
            widths = map;
        else if (map.size() == 1)
            widths = map.constBegin().value().toObject();
    }
    if (widths.isEmpty())
        return std::nullopt;
    std::optional<TrickplayLayout> best;
    for (auto it = widths.constBegin(); it != widths.constEnd(); ++it) {
        bool ok = false;
        const int keyWidth = it.key().toInt(&ok);
        if (!ok || keyWidth < 16 || keyWidth > 640 || !it.value().isObject())
            continue;
        const QJsonObject value = it.value().toObject();
        TrickplayLayout layout;
        layout.width = value.value(QStringLiteral("Width")).toInt();
        layout.height = value.value(QStringLiteral("Height")).toInt();
        layout.columns = value.value(QStringLiteral("TileWidth")).toInt();
        layout.rows = value.value(QStringLiteral("TileHeight")).toInt();
        layout.imageCount = value.value(QStringLiteral("ThumbnailCount")).toInt();
        layout.intervalMs = value.value(QStringLiteral("Interval")).toInt();
        if (layout.width != keyWidth || layout.width < 16 || layout.width > 640
            || layout.height < 16 || layout.height > 360 || layout.columns < 1
            || layout.columns > 20 || layout.rows < 1 || layout.rows > 20 || layout.imageCount < 1
            || layout.imageCount > 1'000'000 || layout.intervalMs < 1000
            || layout.intervalMs > 3600'000
            || qint64(layout.width) * layout.height * layout.columns * layout.rows > 12'000'000)
            continue;
        // Prefer the closest width to the 320px thumbnail target.
        if (!best || std::abs(layout.width - 320) < std::abs(best->width - 320))
            best = layout;
    }
    return best;
}

std::optional<TrickplayFrame> trickplayFrameAt(const TrickplayLayout &layout, qint64 positionTicks)
{
    if (positionTicks < 0 || layout.intervalMs <= 0 || layout.imageCount <= 0 || layout.rows < 1
        || layout.columns < 1)
        return std::nullopt;
    const qint64 image = (positionTicks / 10'000) / layout.intervalMs;
    if (image >= layout.imageCount)
        return std::nullopt;
    const int perTile = layout.rows * layout.columns;
    const int slot = int(image % perTile);
    return TrickplayFrame{int(image / perTile), slot % layout.columns, slot / layout.columns};
}
} // namespace Kinefin
