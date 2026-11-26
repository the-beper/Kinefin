// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QMarginsF>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace Kinefin {
// A window's shadow as the eight tiles that KWindowShadow gives the compositor, and how far it sticks out of the window (whole logical pixels); null images and no padding when nothing would show
struct PipShadow {
    QImage topLeft;
    QImage top;
    QImage topRight;
    QImage right;
    QImage bottomRight;
    QImage bottom;
    QImage bottomLeft;
    QImage left;
    QMarginsF padding;
};

// The shadow of a window much larger than `size`: black, with a Gaussian edge profile of sigma `size` / 3 on each axis, so a corner is the product of its two edges and the compositor stretches the edge tiles, which are one pixel thick. The shadow's rectangle is the window moved down by `yOffset` (less than `size`), so it sticks out `size` - `yOffset` above, `size` + `yOffset` below and `size` at the sides; `opacity` is the alpha of the shadow at its densest. The tiles are in device pixels, the padding in logical ones
inline PipShadow pipShadow(qreal size, qreal yOffset, qreal opacity, qreal devicePixelRatio)
{
    if (!(size > 0) || !(opacity > 0) || !(std::abs(yOffset) < size))
        return {};
    const qreal ratio = devicePixelRatio > 0 ? devicePixelRatio : 1;
    const qreal sigma = size / 3;
    const qreal peak = 255 * std::min<qreal>(opacity, 1);
    const QMarginsF padding(qCeil(size), qCeil(size - yOffset), qCeil(size), qCeil(size + yOffset));
    const auto pixels = [ratio](qreal logical) { return std::max(1, qRound(logical * ratio)); };
    const int left = pixels(padding.left());
    const int top = pixels(padding.top());
    const int right = pixels(padding.right());
    const int bottom = pixels(padding.bottom());

    // How much of the shadow reaches a point `distance` outside the edge of the shadow's rectangle, negative inside it
    const auto edge = [sigma](qreal distance) {
        return 0.5 * std::erfc(distance / (sigma * M_SQRT2));
    };
    // The profile at the center of a pixel, counted from the tile's outer border for the left and top tiles and from the window's edge for the right and bottom ones
    const auto leftOf = [=](int x) { return edge((left - x - 0.5) / ratio); };
    const auto rightOf = [=](int x) { return edge((x + 0.5) / ratio); };
    const auto above = [=](int y) { return edge(yOffset + (top - y - 0.5) / ratio); };
    const auto below = [=](int y) { return edge((y + 0.5) / ratio - yOffset); };
    const auto whole = [](int) { return 1.0; };

    const auto tile = [=](int width, int height, auto horizontal, auto vertical) {
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x)
                image.setPixel(x, y, qRgba(0, 0, 0, qRound(peak * horizontal(x) * vertical(y))));
        }
        return image;
    };

    return {.topLeft = tile(left, top, leftOf, above),
            .top = tile(1, top, whole, above),
            .topRight = tile(right, top, rightOf, above),
            .right = tile(right, 1, rightOf, whole),
            .bottomRight = tile(right, bottom, rightOf, below),
            .bottom = tile(1, bottom, whole, below),
            .bottomLeft = tile(left, bottom, leftOf, below),
            .left = tile(left, 1, leftOf, whole),
            .padding = padding};
}
} // namespace Kinefin
