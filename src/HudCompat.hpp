#pragma once

#include <cstdint>

/**
 * @namespace HudCompat
 * @brief Game-thread TrueHUD and moreHUD queries for actor snapshots.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 *
 * TrueHUD floating bars fade plates toward `TrueHUDYieldAlpha` over `YieldSettleTime`.
 * moreHUD can suppress the crosshair target's level segment. Both behaviors require
 * their compat toggles; the shipped INI disables moreHUD level yielding.
 */
namespace HudCompat
{
/**
 * @fn void Initialize()
 * @brief Request TrueHUD and detect moreHUD at SKSE kPostPostLoad.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Call once before queries; initialization is unsynchronized and non-reentrant.
 * Presence queries return false before initialization. moreHUD detection checks its DLL,
 * then its plugin only if the data handler is available. At the current kPostPostLoad
 * call site, the plugin list is not yet populated, so detection relies on the DLL.
 * Absent plugins are normal and require no fallback initialization.
 */
void Initialize();

/**
 * @fn bool HasTrueHUD()
 * @brief True when the TrueHUD API interface is available.
 * @author Alex (<https://github.com/lextpf>)
 */
bool HasTrueHUD();

/**
 * @fn bool HasMoreHUD()
 * @brief True when moreHUD is installed.
 * @author Alex (<https://github.com/lextpf>)
 */
bool HasMoreHUD();

/**
 * @fn bool TrueHUDShowsBarFor(RE::Actor* actor)
 * @brief Check for a floating TrueHUD bar, excluding docked boss bars.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return False for a null actor or absent TrueHUD.
 * @pre Game thread; reads the actor handle.
 */
bool TrueHUDShowsBarFor(RE::Actor* actor);

/**
 * @fn std::uint32_t CrosshairTargetFormID()
 * @brief Crosshair target FormID, preferring the actor-specific pick.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Generic picks can select weapons or collision proxies in front of actors.
 * @return Actor FormID, otherwise generic target FormID, otherwise zero.
 * @pre Game thread; reads `RE::CrosshairPickData`.
 */
std::uint32_t CrosshairTargetFormID();
}  // namespace HudCompat
