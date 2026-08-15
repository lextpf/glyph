#pragma once

/**
 * @namespace GameState
 * @brief Game-thread gates for overlay visibility and card capture.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 *
 * Require an active game, no listed suppressed menu, and a player in an attached cell.
 * A missing UI singleton skips menu checks; a missing game or player closes both gates.
 * `UpdateSnapshot_GameThread` publishes results through `allowOverlay` and `allowDeck`;
 * the render thread must read those atomics instead of dereferencing game state.
 */
namespace GameState
{
/**
 * @fn bool CanDrawOverlay()
 * @brief Apply the world-readiness gates and hide plates during combat.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Only `SUPPRESSED_MENUS` entries suppress the overlay; Dialogue menu and BookMenu do not.
 * @return True when world-readiness checks pass and the player is not in combat.
 * @pre Game thread.
 * @see Renderer::IsOverlayAllowedRT
 */
bool CanDrawOverlay();

/**
 * @fn bool CanCaptureDeck()
 * @brief Apply world-readiness gates while permitting combat captures.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The caller checks the Deck enable switch separately.
 *
 * @return True when world-readiness checks pass, including during combat.
 * @pre Game thread.
 */
bool CanCaptureDeck();
}  // namespace GameState
