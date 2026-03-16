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
static constexpr int MAX_ACTORS = 16;  ///< Maximum actors to display nameplates for at once
static constexpr int MAX_SCAN = 32;    ///< Maximum actors to iterate when scanning

// Cache Management
static constexpr uint32_t CACHE_GRACE_FRAMES =
    60;  ///< Frames to keep cache entries after actor leaves view (~1s at 60fps)
static constexpr int POSITION_HISTORY_SIZE =
    8;  ///< Position history buffer size for moving average smoothing

// Debug Overlay
static constexpr float RELOAD_NOTIFICATION_DURATION =
    2.f;  ///< Duration to show "Reloaded!" notification (seconds)
static constexpr int FRAME_TIME_SAMPLES = 60;  ///< Number of frame time samples for averaging

}  // namespace RenderConstants
