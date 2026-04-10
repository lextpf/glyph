#pragma once

#include <cstdint>

/**
 * @namespace HudCompat
 * @brief One Voice Per Actor -- TrueHUD / moreHUD deconfliction.
 * @ingroup Utilities
 *
 * The premium failure mode in a heavy modlist is three mods shouting the
 * same NPC's name.  HudCompat detects the HUD mods glyph most commonly
 * shares actors with and yields per actor instead of stacking widgets:
 *
 * - **TrueHUD**: when it floats an info/boss bar over an actor, glyph fades
 *   that actor's plate to `CompatTrueHUDYieldAlpha` (default: fully out)
 *   with the usual smoothing, and returns it when the bar goes away.
 * - **moreHUD**: when its crosshair readout already shows the target's
 *   level, glyph drops the level segment from that actor's main line.
 *
 * Detection is automatic (no user patching): the TrueHUD API is requested
 * at kPostPostLoad; moreHUD presence is probed by DLL / plugin name.  All
 * queries run on the game thread and flow into the actor snapshot.
 */
namespace HudCompat
{
/// Request the TrueHUD API and probe for moreHUD.  Call once at
/// SKSE kPostPostLoad (all plugins are loaded by then).
void Initialize();

/// True when the TrueHUD API interface was acquired.
bool HasTrueHUD();

/// True when moreHUD is installed.
bool HasMoreHUD();

/// Game-thread only: does TrueHUD currently float an info/boss bar over
/// this actor?  Always false when TrueHUD is absent.
bool TrueHUDShowsBarFor(RE::Actor* actor);

/// Game-thread only: formID of the current crosshair reference, or 0.
std::uint32_t CrosshairTargetFormID();
}  // namespace HudCompat
