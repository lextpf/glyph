#pragma once

#include <RE/Skyrim.h>

/**
 * @namespace OutfitCopy
 * @brief Outfit copying between actors.
 * @ingroup AppearanceTemplate
 *
 * Extracts the outfit-copy logic from AppearanceTemplate so that the
 * core appearance system and the equipment transfer are independently
 * readable and testable.
 */
namespace OutfitCopy
{
/**
 * Copy equipped armor from one actor to another.
 *
 * Iterates the source actor's inventory for worn armor items and
 * adds/equips them on the target.  Items already present and worn
 * are skipped.  Biped slot conflicts with gear the target is already
 * wearing are also skipped to avoid visual glitches.
 *
 * @param sourceActor Actor to copy equipment from.
 * @param targetActor Actor to receive the equipment.
 * @return `true` if at least one item was equipped.
 */
bool CopyOutfitBetweenActors(RE::Actor* sourceActor, RE::Actor* targetActor);

/**
 * Find a loaded actor whose base NPC matches the given form.
 *
 * Searches high-process actor handles for a loaded actor backed by
 * @p npc.  Returns `nullptr` if none is currently in memory.
 *
 * @param npc Base NPC form to match against.
 * @return Matching loaded actor, or `nullptr`.
 */
RE::Actor* FindLoadedActorByBase(RE::TESNPC* npc);
}  // namespace OutfitCopy
