#pragma once

/**
 * @namespace GameState
 * @brief Game state queries for overlay visibility.
 *
 * @ingroup Utilities
 */
namespace GameState
{
/**
 * Check if the floating names overlay should be rendered.
 *
 * Determines whether game state allows overlay rendering. The overlay is hidden
 * during loading, menus, combat, and other states where it would be intrusive
 * or cause visual issues.
 *
 * @note Game-thread only. It dereferences RE::* game state (e.g. the player's
 *       parent cell), so it must not be called from the render thread. The game
 *       thread publishes its result into the `allowOverlay` atomic; render-thread
 *       code reads it via Renderer::IsOverlayAllowedRT().
 *
 * @return `true` if overlay can be drawn, `false` if it should be hidden.
 *
 * @see Renderer::IsOverlayAllowedRT
 */
bool CanDrawOverlay();
}  // namespace GameState
