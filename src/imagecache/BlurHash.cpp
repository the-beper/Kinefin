// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagecache/BlurHash.h"

#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace Kinefin {
namespace {
constexpr std::string_view kAlphabet(
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~");
constexpr int kMaxComponents = 9;
constexpr int kMaxSide = 64;

// The value of one base 83 character, -1 for any other
int digitOf(QChar character)
{
    if (character.unicode() >= 128)
        return -1;
    const size_t at = kAlphabet.find(char(character.unicode()));
    return at == std::string_view::npos ? -1 : int(at);
}

// The value of `length` characters from `start`, -1 when one of them is no base 83 character
int decode83(QStringView hash, int start, int length)
{
    int value = 0;
    for (int i = start; i < start + length; ++i) {
        const int digit = digitOf(hash.at(i));
        if (digit < 0)
            return -1;
        value = value * 83 + digit;
    }
    return value;
}

double toLinear(int value)
{
    const double v = value / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

int toSrgb(double value)
{
    const double v = std::clamp(value, 0.0, 1.0);
    return v <= 0.0031308 ? int(v * 12.92 * 255 + 0.5)
                          : int((1.055 * std::pow(v, 1 / 2.4) - 0.055) * 255 + 0.5);
}

double signedSquare(double value)
{
    return std::copysign(value * value, value);
}

struct Components {
    int x = 0;
    int y = 0;
};

// The grid of components the first character names, none when the hash is too short for it
std::optional<Components> componentsOf(QStringView hash)
{
    if (hash.size() < 6)
        return std::nullopt;
    const int size = decode83(hash, 0, 1);
    if (size < 0)
        return std::nullopt;
    const Components components{size % kMaxComponents + 1, size / kMaxComponents + 1};
    return components.y <= kMaxComponents ? std::optional(components) : std::nullopt;
}
} // namespace

bool isValidBlurHash(QStringView hash)
{
    const auto components = componentsOf(hash);
    return components && hash.size() == 4 + 2 * components->x * components->y
           && std::all_of(hash.begin(), hash.end(), [](QChar c) { return digitOf(c) >= 0; });
}

QImage decodeBlurHash(QStringView hash, int width, int height)
{
    if (!isValidBlurHash(hash) || width < 1 || height < 1 || width > kMaxSide || height > kMaxSide)
        return {};
    const Components grid = *componentsOf(hash);
    const double maxValue = (decode83(hash, 1, 1) + 1) / 166.0;

    std::array<std::array<double, 3>, kMaxComponents * kMaxComponents> colors{};
    const int dc = decode83(hash, 2, 4);
    colors[0] = {toLinear(dc >> 16), toLinear((dc >> 8) & 255), toLinear(dc & 255)};
    for (int i = 1; i < grid.x * grid.y; ++i) {
        const int value = decode83(hash, 4 + i * 2, 2);
        const int red = value / (19 * 19);
        const int green = value / 19 % 19;
        const int blue = value % 19;
        colors[i] = {signedSquare((red - 9) / 9.0) * maxValue,
                     signedSquare((green - 9) / 9.0) * maxValue,
                     signedSquare((blue - 9) / 9.0) * maxValue};
    }

    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            double red = 0;
            double green = 0;
            double blue = 0;
            for (int j = 0; j < grid.y; ++j) {
                for (int i = 0; i < grid.x; ++i) {
                    const double basis =
                        std::cos(M_PI * x * i / width) * std::cos(M_PI * y * j / height);
                    const auto &color = colors[i + j * grid.x];
                    red += color[0] * basis;
                    green += color[1] * basis;
                    blue += color[2] * basis;
                }
            }
            line[x] = qRgb(toSrgb(red), toSrgb(green), toSrgb(blue));
        }
    }
    return image;
}
} // namespace Kinefin
