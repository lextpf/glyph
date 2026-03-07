#pragma once

/**
 * Check if the floating names overlay should be rendered.
 *
 * Determines whether game state allows overlay rendering. The overlay is hidden
 * during loading, menus, combat, and other states where it would be intrusive
 * or cause visual issues.
 *
 * @return `true` if overlay can be drawn, `false` if it should be hidden.
 *
 * @ingroup Utilities
 *
 * @see Renderer::IsOverlayAllowedRT
 */
bool CanDrawOverlay();
