#pragma once

#include "RenderConstants.hpp"

#include <cstddef>
#include <cstdint>

/**
 * @namespace DebugOverlay
 * @brief Render-thread timing and state overlay.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup DebugOverlay
 *
 * Call `UpdateFrameStats()` then `Render()` only while `enableDebugOverlay` is set in
 * the frame snapshot. Skipped updates leave stats unchanged. Frame time uses a rolling
 * average over `RenderConstants::FRAME_TIME_SAMPLES`.
 *
 * @code{.cpp}
 * // In the render loop, gate both calls on the frame's RenderSettingsSnapshot
 * // instead of reading the mutex-guarded settings directly:
 * if (snap.enableDebugOverlay) {
 *     DebugOverlay::UpdateFrameStats(stats, deltaTime, currentTime, ...);
 *     DebugOverlay::Render(context);
 * }
 * @endcode
 */
namespace DebugOverlay
{
/**
 * @struct Stats
 * @brief Caller-owned counters retained while the overlay is disabled.
 * @author Alex (<https://github.com/lextpf>)
 */
struct Stats
{
    float fps = .0f;
    float frameTimeMs = .0f;
    float avgFrameTimeMs = .0f;

    /// Snapshot plates, excluding capture-only entries and a hidden player; cacheSize counts all.
    int actorCount = 0;

    int visibleActors = 0;
    int occludedActors = 0;

    /// 1 when the snapshot includes the player, even when occluded.
    int playerVisible = 0;

    size_t cacheSize = 0;

    int updatesPerSecond = 0;  ///< Actor data updates per second

    float frameTimeHistory[RenderConstants::FRAME_TIME_SAMPLES] = {0};
    int frameTimeIndex = 0;
};

/**
 * @struct Context
 * @brief Frame values copied from RenderSettingsSnapshot for display.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Holds no lock. Only `stats` is borrowed; other members are value copies.
 */
struct Context
{
    Stats* stats = nullptr;  ///< Non-owning; must outlive Render, which only reads it
    uint32_t frameNumber = 0;
    int postLoadCooldown = 0;  ///< Frames remaining in post-load cooldown
    /// ImGui::GetTime seconds; drives the reload flash. Negative default suppresses startup flash.
    float lastReloadTime = -10.0f;
    size_t actorCacheEntrySize = 0;  ///< Sizeof(ActorCache) for memory estimate
    size_t actorDrawDataSize = 0;    ///< Sizeof(ActorDrawData) for memory estimate

    bool occlusionEnabled = false;   ///< Occlusion culling is on
    bool glowEnabled = false;        ///< Text glow is on
    bool typewriterEnabled = false;  ///< Typewriter reveal is on
    bool hidePlayer = false;         ///< Player plate is suppressed
    float verticalOffset = .0f;      ///< Plate height above the actor's head, in game units
    int maxPlates = 0;               ///< Cap on plates drawn per frame
    int maxScanActors = 0;           ///< Cap on actors scanned per snapshot
    size_t tierCount = 0;            ///< Number of configured color tiers
    int reloadKey = 0;               ///< Windows virtual-key code for hot reload; <= 0 disables it
};

/**
 * @fn void UpdateFrameStats(Stats& stats, float deltaTime, float currentTime, float&
 *     lastUpdateTime, int updateCounter, int& lastUpdateCount)
 * @brief Update timing history and the per-second snapshot rate.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param deltaTime Seconds; zero while paused yields zero FPS.
 * @param currentTime Seconds from `ImGui::GetTime()`.
 * @param lastUpdateTime Retain across frames; updated with each per-second sample.
 * @param updateCounter Total snapshot updates since startup.
 * @param lastUpdateCount Retain across frames; previous per-second counter sample.
 */
void UpdateFrameStats(Stats& stats,
                      float deltaTime,
                      float currentTime,
                      float& lastUpdateTime,
                      int updateCounter,
                      int& lastUpdateCount);

/**
 * @fn void Render(const Context& ctx)
 * @brief Draw the diagnostic window from borrowed context values.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param ctx Read only and not retained; null `stats` makes this a no-op.
 * @pre Render thread, active ImGui frame, and caller-checked enable flag.
 */
void Render(const Context& ctx);

}  // namespace DebugOverlay
