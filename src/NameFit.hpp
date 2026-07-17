#pragma once

#include <algorithm>
#include <cmath>

/**
 * @namespace Renderer::NameFit
 * @brief Width fitting in font-relative units.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Keep short names at their authored size. For longer names, share width reduction between
 * font size and horizontal compression. Both reductions stop at fixed legibility floors.
 */
namespace Renderer::NameFit
{
/// Width budget in ems; narrower segments keep their authored typography.
inline constexpr float MAX_WIDTH_EM = 5.75f;

/// Font-size floor; horizontal compression supplies any remaining reduction.
inline constexpr float MIN_FONT_SCALE = .78f;

/// Horizontal floor; callers must handle overflow once both floors apply.
inline constexpr float MIN_HORIZONTAL_SCALE = .84f;

/**
 * @brief Exponent share assigned to font-size reduction.
 *
 * For required scale $r$ and share $s$:
 *
 * $$f = r^{\,s} \qquad h = r^{\,1-s} \qquad f \cdot h = r$$
 *
 * The identity holds before legibility floors. $r$ is MAX_WIDTH_EM / measured ems.
 * The font floor applies when $r < f_{min}^{1/s}$; both apply when $r < f_{min} h_{min}$.
 * $f_{min}$ is MIN_FONT_SCALE; $h_{min}$ is MIN_HORIZONTAL_SCALE.
 * Current floors begin near 8.6 and 8.8 em, respectively.
 */
inline constexpr float FONT_REDUCTION_SHARE = .62f;

/**
 * @struct Result
 * @brief Multipliers that fit one name segment into the width budget.
 * @author Alex (<https://github.com/lextpf>)
 */
struct Result
{
    /**
     * Font-size multiplier from MIN_FONT_SCALE to 1; remeasure before horizontal scaling.
     */
    float fontScale = 1.0f;

    /**
     * X-extent multiplier from MIN_HORIZONTAL_SCALE to 1. It compresses the x axis only and applies
     * to the already-measured extents, so it needs no re-measurement.
     */
    float horizontalScale = 1.0f;
};

/**
 * @fn Result Compute(float measuredWidth, float fontSize)
 * @brief Fit the whole name-bearing segment to the width budget.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Apply font scale, remeasure, then apply horizontal scale.
 * The floors can leave very long names wider than the budget; this helper does not clip
 * or truncate text. At both floors, the measured width is reduced by about 34 percent.
 *
 * @param measuredWidth Segment width at fontSize, in pixels.
 * @param fontSize Font size, in pixels.
 * @return {1,1} when already fitted or either input is non-finite/non-positive.
 */
inline Result Compute(float measuredWidth, float fontSize)
{
    if (!std::isfinite(measuredWidth) || !std::isfinite(fontSize) || measuredWidth <= .0f ||
        fontSize <= .0f)
    {
        return {};
    }

    const float maximumWidth = fontSize * MAX_WIDTH_EM;
    if (measuredWidth <= maximumWidth)
    {
        return {};
    }

    const float requestedScale = maximumWidth / measuredWidth;
    const float fontScale =
        std::clamp(std::pow(requestedScale, FONT_REDUCTION_SHARE), MIN_FONT_SCALE, 1.0f);
    const float horizontalScale =
        std::clamp(requestedScale / fontScale, MIN_HORIZONTAL_SCALE, 1.0f);
    return {fontScale, horizontalScale};
}
}  // namespace Renderer::NameFit
