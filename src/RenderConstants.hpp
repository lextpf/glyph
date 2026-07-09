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
 * | Constant      | Value | Description                                   |
 * |---------------|:-----:|-----------------------------------------------|
 * | `MAX_ACTORS`  | 16    | Max nameplates rendered simultaneously        |
 * | `MAX_SCAN`    | 128   | Runaway guard on the cheap distance scan pass |
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
// Runaway guard on the cheap distance pre-pass (which only distance-filters
// high-process actors; the expensive per-actor derivation is capped at
// MAX_ACTORS separately). Set well above the engine's high-process actor
// budget so it never truncates the list in normal play -- capping this by list
// POSITION rather than distance is what used to drop a near actor that sat past
// the cutoff in a crowded area, leaving a freshly-approached NPC with no plate.
inline constexpr int MAX_SCAN = 128;

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
inline constexpr int MAX_HONORIFIC_INDEX = 63;      ///< Maximum honorific index in INI
inline constexpr int MAX_REGISTER_INDEX = 31;       ///< Maximum register index in INI

// ----------------------------------------------------------------------------
// Layout / animation tuning (formerly INI-configurable).
// These values are baked into the build because real-world tuning is rare and
// the matching INI knobs were almost never adjusted; surface them again as
// INI keys if you find yourself recompiling to tweak them.
// ----------------------------------------------------------------------------

inline constexpr float TITLE_MAIN_GAP =
    8.0f;  ///< Vertical gap between title and main line (pixels)
inline constexpr float INFO_LINE_GAP =
    5.0f;  ///< Vertical gap between main line and info row (pixels)
inline constexpr float SEGMENT_PADDING =
    6.0f;  ///< Horizontal padding between main-line segments (pixels)
inline constexpr float BADGE_ICON_FACTOR =
    .45f;  ///< Indicator icon size as a fraction of the level font size
inline constexpr float BADGE_SPACING =
    10.0f;  ///< Horizontal spacing between indicator icons (pixels)
inline constexpr float BADGE_ROW_GAP =
    6.0f;  ///< Vertical gap between the indicator strip and the title row (pixels)
inline constexpr float OUTLINE_MIN_SCALE =
    .75f;  ///< Minimum outline width ratio when text is downscaled
inline constexpr bool PROPORTIONAL_SPACING =
    true;  ///< Scale pixel spacings (padding, gaps, outlines) with text size

// Effect intensity range (tier progression interpolates between min and max).
// The strength band multiplies into every animated effect's amplitude; the
// old .15-.50 band stacked with per-effect caps and INI intensities into a
// ~1-7% net modulation -- mathematically invisible. The band starts clearly
// visible: even the entry tier's effect should read at a glance, with tier
// progression adding weight on top rather than gating visibility.
inline constexpr float EFFECT_ALPHA_MIN = .32f;
inline constexpr float EFFECT_ALPHA_MAX = .78f;
inline constexpr float EFFECT_STRENGTH_MIN = .78f;
inline constexpr float EFFECT_STRENGTH_MAX = 1.0f;

// Animation speed bands by tier (smaller = slower). High tiers stay the
// slowest (weighty, deliberate) but no band drops below the threshold where
// the motion stops registering at a glance.
inline constexpr float ANIM_SPEED_LOW_TIER = .24f;   ///< Tiers 0-7
inline constexpr float ANIM_SPEED_MID_TIER = .17f;   ///< Tier 8
inline constexpr float ANIM_SPEED_HIGH_TIER = .12f;  ///< Tier 9+

// Entrance/exit motion offsets (pixels). The label rises gently into place on
// entry and sinks slightly as it leaves -- weighty and cinematic, never bouncy.
inline constexpr float ENTRANCE_RISE_PX = 10.0f;  ///< Upward settle distance on entrance
inline constexpr float EXIT_SINK_PX = 8.0f;       ///< Downward recede distance on exit

}  // namespace RenderConstants
