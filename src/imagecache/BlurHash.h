// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QStringView>

namespace Kinefin {
// A BlurHash (https://blurha.sh) is a few dozen characters that describe a blurred picture: Jellyfin sends one for each image of an item, so the page can show a placeholder before the image arrives. It comes from the server and is never trusted: it is checked, and decoded within fixed limits, before anything is drawn

// Whether `hash` is well formed: base 83 characters only, a grid of 1 to 9 by 1 to 9 components and exactly the length that grid needs
bool isValidBlurHash(QStringView hash);

// The picture of `hash` at `width` by `height` pixels (each 1 to 64), a null image when the hash is not valid or the size is out of range
QImage decodeBlurHash(QStringView hash, int width, int height);
} // namespace Kinefin
