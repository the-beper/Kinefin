// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDataStream>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <cmath>
#include <numbers>

namespace TestWav {
// The fixture is one fixed WAV: PCM data for a sine is what the demuxer reports as the duration, position and seekability; the container's mpv build has no lavfi stream protocol, so generated media stands in for the synthetic clip cases
constexpr int kWavSampleRate = 8000;

inline QString writeWavClip(const QTemporaryDir &dir, int seconds)
{
    QFile file(dir.filePath(QStringLiteral("clip%1.wav").arg(seconds)));
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    const int frames = seconds * kWavSampleRate;
    QByteArray pcm(int(frames * sizeof(qint16)), Qt::Uninitialized);
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const double phase = 2 * std::numbers::pi * 440.0 * double(i) / double(kWavSampleRate);
        samples[i] = qint16(std::sin(phase) * 8000.0);
    }
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    auto ascii = [&stream](const char *text) { stream.writeRawData(text, 4); };
    // RIFF size = whole rest of the file = data + 36
    const quint32 riffSize = quint32(frames * sizeof(qint16)) + 36;
    ascii("RIFF");
    stream << riffSize;
    ascii("WAVE");
    ascii("fmt ");
    stream << quint32(16)                              // fmt chunk size
           << quint16(1)                               // PCM
           << quint16(1)                               // mono
           << quint32(kWavSampleRate)                  // rate
           << quint32(kWavSampleRate * sizeof(qint16)) // byte rate
           << quint16(sizeof(qint16))                  // block align
           << quint16(16);                             // bits
    ascii("data");
    stream << quint32(frames * sizeof(qint16));
    if (file.write(header) == header.size() && file.write(pcm) == pcm.size()) {
        return QUrl::fromLocalFile(file.fileName()).toString(QUrl::FullyEncoded);
    }
    return {};
}
} // namespace TestWav
