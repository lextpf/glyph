#pragma once

#include <algorithm>
#include <cmath>

/**
 * @namespace TierEmblem
 * @brief Emblem selection for one actor tier.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The manifest lists emblem images in rank order, lowest first. A tier either names its
 * emblem with the INI Badge key or takes one from a gamma-weighted curve over the whole
 * list. The named choice always wins.
 *
 * The header uses no game, CommonLibSSE or ImGui type, so tests include it directly.
 */
namespace TierEmblem
{
/**
 * @fn int BandIndex(int tierIdx, int tierCount, int imageCount, float gamma)
 * @brief Map tier progress to a gamma-weighted emblem index.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Tier progress runs from 0 at the lowest tier to 1 at the highest. The curve raises that
 * progress to the gamma power and scales it across the emblem list, so a gamma above 1 makes
 * high emblems rarer and a gamma below 1 makes them more common.
 *
 * @param tierIdx Zero-based tier index.
 * @param tierCount Configured tier count.
 * @param imageCount Emblems available in the manifest list.
 * @param gamma Weighting exponent, clamped to [0.1, 8]; 1.0 spaces the emblems evenly.
 * @return Emblem index in [0, imageCount), or 0 when imageCount or tierCount is 1 or less.
 */
inline int BandIndex(int tierIdx, int tierCount, int imageCount, float gamma)
{
    if (imageCount <= 1 || tierCount <= 1)
    {
        return 0;
    }
    const float t =
        std::clamp(static_cast<float>(tierIdx) / static_cast<float>(tierCount - 1), .0f, 1.0f);
    const float g = std::clamp(gamma, .1f, 8.0f);
    const int band = static_cast<int>(std::floor(std::pow(t, g) * static_cast<float>(imageCount)));
    return std::clamp(band, 0, imageCount - 1);
}

/**
 * @fn int Select(int explicitBadge, int tierIdx, int tierCount, int imageCount, float gamma)
 * @brief Resolve the emblem index for one tier.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A badge number addresses the manifest list directly. Zero, a negative number, or a number
 * past the end of the list falls back to the gamma curve, so a typo still draws an emblem.
 *
 * @param explicitBadge One-based manifest entry from the tier Badge key; 0 selects the curve.
 * @param tierIdx Zero-based tier index.
 * @param tierCount Configured tier count.
 * @param imageCount Emblems available in the manifest list.
 * @param gamma Weighting exponent for the fallback curve; see BandIndex.
 * @return Emblem index in [0, imageCount), or -1 when the list holds no emblem.
 */
inline int Select(int explicitBadge, int tierIdx, int tierCount, int imageCount, float gamma)
{
    if (imageCount <= 0)
    {
        return -1;
    }
    if (explicitBadge >= 1 && explicitBadge <= imageCount)
    {
        return explicitBadge - 1;
    }
    return BandIndex(tierIdx, tierCount, imageCount, gamma);
}
}  // namespace TierEmblem
