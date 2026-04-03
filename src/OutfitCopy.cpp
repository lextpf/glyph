#include "OutfitCopy.h"

#include "PCH.h"

#include <SKSE/SKSE.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace OutfitCopy
{
RE::Actor* FindLoadedActorByBase(RE::TESNPC* npc)
{
    if (!npc)
    {
        return nullptr;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    auto* processLists = RE::ProcessLists::GetSingleton();
    if (!processLists)
    {
        return nullptr;
    }

    // Prefer loaded high-process actors to avoid scanning all forms in the game.
    for (const auto& handle : processLists->highActorHandles)
    {
        auto actorPtr = handle.get();
        auto* actor = actorPtr.get();
        if (!actor || actor == player || !actor->Is3DLoaded())
        {
            continue;
        }
        if (actor->GetActorBase() == npc)
        {
            logger::debug("OutfitCopy: Found loaded actor for NPC {:08X}", npc->GetFormID());
            return actor;
        }
    }

    return nullptr;
}

// Copy equipped outfit from source actor to target.
// Copies all equipped armor items (not weapons).
bool CopyOutfitBetweenActors(RE::Actor* sourceActor, RE::Actor* targetActor)
{
    if (!sourceActor || !targetActor)
    {
        return false;
    }

    auto* equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager)
    {
        logger::warn("OutfitCopy: ActorEquipManager unavailable, cannot copy outfit");
        return false;
    }

    auto sourceInv = sourceActor->GetInventory();
    auto playerInv = targetActor->GetInventory();
    std::unordered_set<RE::TESForm*> playerForms;
    std::unordered_set<RE::TESForm*> playerWornForms;
    std::vector<std::uint32_t> playerWornSlotMasks;
    playerForms.reserve(playerInv.size());
    playerWornForms.reserve(playerInv.size());
    playerWornSlotMasks.reserve(playerInv.size());

    for (const auto& [pForm, pData] : playerInv)
    {
        if (!pForm || !pData.second)
        {
            continue;
        }
        playerForms.insert(pForm);
        if (pData.second->IsWorn())
        {
            playerWornForms.insert(pForm);
            if (auto* wornArmor = pForm->As<RE::TESObjectARMO>(); wornArmor)
            {
                playerWornSlotMasks.push_back(static_cast<std::uint32_t>(wornArmor->GetSlotMask()));
            }
        }
    }

    int copiedCount = 0;
    int equippedCount = 0;
    int skippedConflicts = 0;

    logger::info("OutfitCopy: Copying outfit from source actor...");

    auto hasSlotConflict = [&](RE::TESObjectARMO* armor) -> bool
    {
        if (!armor)
        {
            return false;
        }
        const auto armorMask = static_cast<std::uint32_t>(armor->GetSlotMask());
        if (armorMask == 0)
        {
            return false;
        }
        for (const auto wornMask : playerWornSlotMasks)
        {
            if ((wornMask & armorMask) != 0)
            {
                return true;
            }
        }
        return false;
    };

    // Iterate through source's inventory to find equipped armor
    for (const auto& [form, data] : sourceInv)
    {
        if (!form || !data.second || data.second->IsWorn() == false)
        {
            continue;
        }

        auto armor = form->As<RE::TESObjectARMO>();
        if (!armor)
        {
            continue;
        }

        const bool hasItem = playerForms.find(form) != playerForms.end();
        const bool alreadyWorn = playerWornForms.find(form) != playerWornForms.end();

        if (!alreadyWorn && hasSlotConflict(armor))
        {
            ++skippedConflicts;
            logger::debug("OutfitCopy: Skipping equip of {} due to biped slot conflict",
                          armor->GetName());
            continue;
        }

        if (!hasItem)
        {
            // Add the item to target's inventory and equip it
            targetActor->AddObjectToContainer(armor, nullptr, 1, nullptr);
            equipManager->EquipObject(targetActor, armor);
            playerForms.insert(form);
            playerWornForms.insert(form);
            playerWornSlotMasks.push_back(static_cast<std::uint32_t>(armor->GetSlotMask()));
            copiedCount++;
            equippedCount++;
            logger::debug("OutfitCopy: Added and equipped {}", armor->GetName());
        }
        else if (playerWornForms.find(form) == playerWornForms.end())
        {
            equipManager->EquipObject(targetActor, armor);
            playerWornForms.insert(form);
            playerWornSlotMasks.push_back(static_cast<std::uint32_t>(armor->GetSlotMask()));
            equippedCount++;
        }
    }

    if (copiedCount > 0)
    {
        logger::info("OutfitCopy: Copied {} armor items from source", copiedCount);
    }
    else
    {
        logger::info(
            "OutfitCopy: No new armor items to copy (target already has them or source has none)");
    }
    if (skippedConflicts > 0)
    {
        logger::info("OutfitCopy: Skipped {} armor equips due to slot conflicts", skippedConflicts);
    }

    return equippedCount > 0;
}
}  // namespace OutfitCopy
