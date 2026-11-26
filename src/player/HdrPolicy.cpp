// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/HdrPolicy.h"

#include <algorithm>
#include <cmath>

namespace Kinefin {
namespace {
// The range of the image description's luminances and of mpv's `target-peak`
constexpr double MaxLuminance = 10000.0;
constexpr double MinTargetPeak = 10.0;
// The protocol sends the minimum luminances in 0.0001 cd/m² and the others in whole cd/m²
constexpr double MinLuminanceUnit = 10000.0;

// The minimum luminance as the protocol sees it, in 0.0001 cd/m²
uint32_t minimumUnits(double luminance)
{
    return static_cast<uint32_t>(std::lround(luminance * MinLuminanceUnit));
}

// The value as the protocol's whole cd/m², or nothing when it is not in (0, 10000] or rounds to 0
std::optional<uint32_t> wholeLuminance(double value)
{
    if (!std::isfinite(value) || value <= 0 || value > MaxLuminance)
        return std::nullopt;
    const auto rounded = static_cast<uint32_t>(std::lround(value));
    return rounded > 0 ? std::optional<uint32_t>(rounded) : std::nullopt;
}

std::optional<double> outputPeak(const HdrOutputInfo &output)
{
    std::optional<double> peak = output.targetMaxLum;
    if (!peak && !output.pqTransfer)
        peak = output.maxLum;
    if (!peak || !std::isfinite(*peak) || *peak <= 0)
        return std::nullopt;
    return peak;
}

// The values are rounded as the protocol sees them, because create() raises invalid_luminance (a protocol error that ends the connection) for a max_cll or max_fall that is not above the mastering minimum and up to the mastering maximum, or without mastering luminance not above the minimum luminance sent and up to that plus 10000 cd/m²
HdrSurfaceDescription describeSurface(const HdrInputs &inputs)
{
    HdrSurfaceDescription description;
    const double outputMin = inputs.output.minLum;
    // An output minimum at or above the reference white would make set_luminances invalid: it counts as 0
    uint32_t lowest = 0;
    if (std::isfinite(outputMin) && outputMin > 0 && outputMin < PqReferenceWhite
        && minimumUnits(outputMin) < PqReferenceWhite * MinLuminanceUnit)
        lowest = minimumUnits(outputMin);
    description.minLum = lowest / MinLuminanceUnit;
    // The range, in 0.0001 cd/m², that max_cll and max_fall must lie in: above `lowest`, up to `highest`
    uint64_t highest = uint64_t(lowest) + uint64_t(MaxLuminance * MinLuminanceUnit);
    const HdrVideoFacts &video = inputs.video;
    if (inputs.compositor.setMasteringPrimaries && std::isfinite(video.minLuma)
        && std::isfinite(video.maxLuma) && video.minLuma >= 0 && video.minLuma < video.maxLuma
        && video.maxLuma <= MaxLuminance) {
        const uint32_t masteringMin = minimumUnits(video.minLuma);
        const auto masteringMax = static_cast<uint32_t>(std::lround(video.maxLuma));
        if (uint64_t(masteringMax) * uint64_t(MinLuminanceUnit) > masteringMin) {
            description.masteringMin = masteringMin / MinLuminanceUnit;
            description.masteringMax = masteringMax;
            lowest = masteringMin;
            highest = uint64_t(masteringMax) * uint64_t(MinLuminanceUnit);
        }
    }
    const auto inRange = [lowest, highest](uint32_t luminance) {
        const uint64_t units = uint64_t(luminance) * uint64_t(MinLuminanceUnit);
        return units > lowest && units <= highest;
    };
    if (const std::optional<uint32_t> cll = wholeLuminance(video.maxCll); cll && inRange(*cll))
        description.maxCll = cll;
    if (const std::optional<uint32_t> fall = wholeLuminance(video.maxFall);
        fall && inRange(*fall) && (!description.maxCll || *fall <= *description.maxCll))
        description.maxFall = fall;
    return description;
}

bool isHdrRangeType(const QString &rangeType)
{
    if (rangeType == QLatin1String("DOVIWithSDR"))
        return false;
    return rangeType == QLatin1String("HDR") || rangeType.startsWith(QLatin1String("HDR10"))
           || rangeType == QLatin1String("HLG") || rangeType.startsWith(QLatin1String("DOVI"));
}
} // namespace

HdrDecision decideHdr(const HdrInputs &inputs)
{
    HdrDecision decision;
    const auto off = [&decision](HdrReason reason) {
        decision.reason = reason;
        return decision;
    };
    const HdrCompositorSupport &compositor = inputs.compositor;
    if (!inputs.buildSupported)
        return off(HdrReason::NotBuilt);
    if (!compositor.parametric || !compositor.setLuminances || !compositor.pq || !compositor.bt2020)
        return off(HdrReason::NoCompositorSupport);
    if (!inputs.settingAuto)
        return off(HdrReason::SettingOff);
    if (inputs.casting)
        return off(HdrReason::Casting);
    if (!inputs.mainView)
        return off(HdrReason::NotMainView);
    if (!inputs.videoIsHdr)
        return off(HdrReason::NotHdrVideo);
    const std::optional<double> peak = outputPeak(inputs.output);
    const double reference = inputs.output.refLum;
    if (!inputs.output.known || !peak || !std::isfinite(reference) || reference <= 0)
        return off(HdrReason::NoOutputInfo);
    if (*peak / reference < MinHdrHeadroom)
        return off(HdrReason::LowHeadroom);
    if (inputs.failed)
        return off(HdrReason::Failed);
    decision.on = true;
    decision.reason = HdrReason::On;
    decision.targetPeak = static_cast<int>(
        std::clamp(std::round(*peak * PqReferenceWhite / reference), MinTargetPeak, MaxLuminance));
    decision.description = describeSurface(inputs);
    return decision;
}

bool videoIsHdr(const QString &mpvGamma, const QString &serverRangeType, bool serverHintUsable)
{
    if (!mpvGamma.isEmpty() && mpvGamma != QLatin1String("auto"))
        return mpvGamma == QLatin1String("pq") || mpvGamma == QLatin1String("hlg");
    return serverHintUsable && isHdrRangeType(serverRangeType);
}
} // namespace Kinefin
