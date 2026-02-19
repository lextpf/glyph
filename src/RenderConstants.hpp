#pragma once

#include <cstdint>

/**
 * @namespace RenderConstants
 * @brief Compile-time constants for the rendering pipeline.
 * @author Alex (https://github.com/lextpf)
 * @ingroup RenderConstants
 *
 * Contains `constexpr` constants for actor processing limits, cache management,
 * and position smoothing.
 *
 * ## :material-ruler: Distance Units
 *
 * Skyrim uses game units where $\approx 70$ units $= 1$ meter:
 *
 * $$d_{meters} = \frac{d_{units}}{70}$$
 *
 * ## :material-account-group-outline: Actor Processing
 *
 * | Constant      | Value | Description                              |
 * |---------------|:-----:|------------------------------------------|
 * | `MAX_ACTORS`  | 16    | Max nameplates rendered simultaneously   |
 * | `MAX_SCAN`    | 32    | Max actors iterated per scan pass        |
 *
 * ## :material-chart-bell-curve-cumulative: Position Smoothing
 *
 * Nameplate positions are smoothed using exponential decay to prevent jitter.
 * Given a settle time $T$ and frame delta $\Delta t$:
 *
 * $$\alpha = 1 - \epsilon^{\,\Delta t \,/\, T}$$
 *
 * where $\epsilon = 0.01$ (1% residual). The smoothed position updates as:
 *
 * $$p_{smooth} = p_{old} + \alpha \cdot (p_{new} - p_{old})$$
 *
 * For large movements ($\|p_{new} - p_{old}\| > threshold$), a separate blend
 * factor $\beta$ is used instead to avoid excessive lag:
 *
 * $$p_{smooth} = p_{old} + \beta \cdot (p_{new} - p_{old})$$
 *
 * The threshold and blend factor are runtime-configurable via
 * `Settings::Visual().LargeMovementThreshold` (default 50px) and
 * `Settings::Visual().LargeMovementBlend` (default 0.5).
 */
namespace RenderConstants
{
// Actor Processing Limits
inline constexpr int MAX_ACTORS = 16;  ///< Maximum actors to display nameplates for at once
inline constexpr int MAX_SCAN = 32;    ///< Maximum actors to iterate when scanning

// Cache Management
inline constexpr uint32_t CACHE_GRACE_FRAMES =
    60;  ///< Frames to keep cache entries after actor leaves view (~1s at 60fps)
inline constexpr int POSITION_HISTORY_SIZE =
    8;  ///< Position history buffer size for moving average smoothing

// Debug Overlay
inline constexpr float RELOAD_NOTIFICATION_DURATION =
    2.f;  ///< Duration to show "Reloaded!" notification (seconds)
inline constexpr int FRAME_TIME_SAMPLES = 60;  ///< Number of frame time samples for averaging

// Font Indices
inline constexpr int FONT_INDEX_NAME = 0;      ///< Name font (loaded first)
inline constexpr int FONT_INDEX_LEVEL = 1;     ///< Level font
inline constexpr int FONT_INDEX_TITLE = 2;     ///< Title font
inline constexpr int FONT_INDEX_ORNAMENT = 3;  ///< Ornament/flourish font

// INI Parsing Limits
inline constexpr int MAX_TIER_INDEX =
    100;  ///< Maximum tier index in INI (prevents unbounded allocation)
inline constexpr int MAX_SPECIAL_TITLE_INDEX = 50;  ///< Maximum special title index in INI

}  // namespace RenderConstants
