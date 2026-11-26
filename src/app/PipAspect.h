// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSize>
#include <Qt>
#include <QtMath>

#include <algorithm>

namespace Kinefin {
// The size of a picture-in-picture window that has the video's `aspect` (width over height; zero or less for no shape) nearest to what the compositor asked for. KWin resizes this window to any shape and the protocol has no aspect request, so the client fits every size it is configured with. `edges` are those of the resize in progress: a side leads the shape by its own dimension, a corner by the larger of the two, and with no edge the width leads. The result keeps `minimumWidth` (zero for none) and never exceeds `bounds` (an invalid size for none), which wins when they conflict, since a larger surface is a protocol error. A size without both dimensions is returned as it is
inline QSize fitToAspect(QSize asked, qreal aspect, Qt::Edges edges, int minimumWidth, QSize bounds)
{
    if (asked.width() <= 0 || asked.height() <= 0)
        return asked;
    qreal width = asked.width();
    qreal height = asked.height();
    if (aspect > 0) {
        const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
        const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
        if (horizontal && vertical)
            width = std::max(width, height * aspect);
        else if (vertical)
            width = height * aspect;
        height = width / aspect;
        if (minimumWidth > 0 && width < minimumWidth) {
            width = minimumWidth;
            height = width / aspect;
        }
    }
    if (bounds.width() > 0 && bounds.height() > 0
        && (width > bounds.width() || height > bounds.height())) {
        const qreal scale = std::min(bounds.width() / width, bounds.height() / height);
        width *= scale;
        height *= scale;
    }
    // A hair over the product, so that a scale which should land on a whole number does not fall a pixel short
    int fittedWidth = std::max(1, qFloor(width + 1e-6));
    int fittedHeight =
        aspect > 0 ? std::max(1, qRound(fittedWidth / aspect)) : std::max(1, qFloor(height + 1e-6));
    if (bounds.width() > 0 && bounds.height() > 0) {
        fittedWidth = std::min(fittedWidth, bounds.width());
        fittedHeight = std::min(fittedHeight, bounds.height());
    }
    return {fittedWidth, fittedHeight};
}
} // namespace Kinefin
