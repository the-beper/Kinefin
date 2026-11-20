// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>

namespace Kinefin {
// The settings that shape the device profile (upstream's ids and defaults). `ClientSettings::deviceProfileOptions()` fills this struct; until the PlaybackInfo caller uses it, the defaults apply
struct DeviceProfileOptions {
    // "auto", "2.0", "5.1,2.0" or "7.1,5.1,2.0": only "2.0" limits the transcode to two channels
    QString channels = QStringLiteral("2.0");
    bool preferTranscodeToH265 = false;
    bool allowTranscodeToHevc = false;
    bool forceTranscodeHevc = false;
    bool forceTranscode4k = false;
    bool forceTranscodeAv1 = false;
    bool forceTranscodeHdr = false;
    bool forceTranscodeHi10p = false;
    bool forceTranscodeDovi = true;
    bool alwaysForceTranscode = false;
};

// Builds the `DeviceProfile` object of PlaybackInfo and capabilities requests, point for point as upstream does
[[nodiscard]] QJsonObject buildDeviceProfile(const DeviceProfileOptions &options);
} // namespace Kinefin
