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
 * | `kMaxActors`  | 16    | Max nameplates rendered simultaneously   |
 * | `kMaxScan`    | 32    | Max actors iterated per scan pass        |
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
 * For large movements ($\|p_{new} - p_{old}\| > 50\text{px}$), a fixed blend
 * factor $\beta = 0.5$ is used instead to avoid excessive lag:
 *
 * $$p_{smooth} = p_{old} + \beta \cdot (p_{new} - p_{old})$$
 */
namespace RenderConstants
{
    // Actor Processing Limits
    constexpr int kMaxActors = 16;  ///< Maximum actors to display nameplates for at once
    constexpr int kMaxScan = 32;    ///< Maximum actors to iterate when scanning

    // Cache Management
    constexpr uint32_t kCacheGraceFrames = 60;  ///< Frames to keep cache entries after actor leaves view (~1s at 60fps)
    constexpr int kPositionHistorySize = 8;     ///< Position history buffer size for moving average smoothing

    // Debug Overlay
    constexpr float kReloadNotificationDuration = 2.0f;  ///< Duration to show "Reloaded!" notification (seconds)
    constexpr int kFrameTimeSamples = 60;                ///< Number of frame time samples for averaging

}
