// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <cstdint>
#include <optional>

namespace Kinefin {

// What the compositor's wp_color_manager_v1 offers: the HDR path needs the first, second, fourth and fifth; the mastering primaries only decide whether the description carries the mastering display's luminance
struct HdrCompositorSupport {
    bool parametric = false;
    bool setLuminances = false;
    bool setMasteringPrimaries = false;
    bool pq = false;
    bool bt2020 = false;
};

// The output's description from wp_color_management_output_v1 (KWin sends its blending colour: gamma22; HDR shows as targetMaxLum above refLum)
struct HdrOutputInfo {
    bool known = false;
    double minLum = 0;
    double maxLum = 0;
    double refLum = 0;
    std::optional<double> targetMaxLum;
    bool pqTransfer = false;
};

// mpv's video-params (gamma "pq", "hlg" and so on, primaries, and the HDR metadata; 0 when absent)
struct HdrVideoFacts {
    QString gamma;
    QString primaries;
    double maxCll = 0;
    double maxFall = 0;
    double minLuma = 0;
    double maxLuma = 0;

    bool operator==(const HdrVideoFacts &) const = default;
};

struct HdrInputs {
    bool buildSupported = false;
    HdrCompositorSupport compositor;
    bool settingAuto = true;
    bool mainView = false;
    bool casting = false;
    bool videoIsHdr = false;
    HdrVideoFacts video;
    HdrOutputInfo output;
    bool failed = false;
};

enum class HdrReason {
    On,
    NotBuilt,
    NoCompositorSupport,
    SettingOff,
    NotMainView,
    Casting,
    NotHdrVideo,
    NoOutputInfo,
    LowHeadroom,
    Failed
};

// The image description the surface declares: luminances in cd/m², the mastering and light-level values only when the file's metadata passed the checks
struct HdrSurfaceDescription {
    double minLum = 0;
    uint32_t maxLum = 10000;
    uint32_t refLum = 203;
    std::optional<double> masteringMin;
    std::optional<double> masteringMax;
    std::optional<uint32_t> maxCll;
    std::optional<uint32_t> maxFall;
};

struct HdrDecision {
    bool on = false;
    HdrReason reason = HdrReason::NotBuilt;
    // mpv's `target-peak` in cd/m², relative to the 203 cd/m² the description declares as reference white
    int targetPeak = 0;
    HdrSurfaceDescription description;
};

// The output peak over its reference white below which HDR would show no more than SDR does
inline constexpr double MinHdrHeadroom = 1.5;
// MP_REF_WHITE of mpv: what the PQ surface declares as reference white, and where mpv puts SDR white
inline constexpr double PqReferenceWhite = 203.0;

// Gates in this order: NotBuilt, NoCompositorSupport, SettingOff, Casting, NotMainView, NotHdrVideo, NoOutputInfo, LowHeadroom, Failed. The peak is `targetMaxLum` when present, else `maxLum` when the output is not PQ; the headroom is peak over `refLum` (a `refLum` of 0 or less, or no peak, is NoOutputInfo); `targetPeak` is `peak * 203 / refLum` rounded and clamped to 10..10000. The description takes the output's `minLum` (0.0001 cd/m² precision, 0 when 203 or more), 10000 and 203. The file's metadata is untrusted and the compositor ends the connection on a wrong one, so the description holds only values the protocol accepts, already rounded to its units: mastering luminance needs `compositor.setMasteringPrimaries`, 0 <= minLuma < maxLuma <= 10000 and a maximum that rounds above the minimum; `maxCll` and `maxFall` are kept in (0, 10000] and above the minimum and up to the maximum of the mastering luminance when it is described, else above the output's `minLum` (the PQ volume), `maxFall` also at most `maxCll` when that is kept; NaN, infinite and negative values are dropped
[[nodiscard]] HdrDecision decideHdr(const HdrInputs &inputs);

// The one owner of "this video is HDR": mpv's gamma ("pq" or "hlg") once it is known, else the server's range type (HDR, HDR10, HDR10Plus, HLG, DOVI*, but not DOVIWithSDR) when `serverHintUsable` (Direct and Offline routes: a transcode's picture is not the source's)
[[nodiscard]] bool videoIsHdr(const QString &mpvGamma, const QString &serverRangeType,
                              bool serverHintUsable);
} // namespace Kinefin
