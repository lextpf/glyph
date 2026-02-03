#include "AppearanceTemplateInternal.h"

#include <iomanip>
#include <vector>

// ============================================================================
// Ownership contract for RE::calloc-managed memory
//
// Head part arrays and tint layers allocated via RE::calloc() are tracked in
// OwnedHeadPartArrays() and OwnedTintLayers() respectively. Only pointers
// present in these sets are freed by this code. Engine-owned pointers (the
// original arrays) are never freed to avoid corrupting game state.
//
// Invariant: Before mutating playerBase->headParts or tintLayers, the NEW
// allocation is inserted into the tracking set FIRST. Only then is the old
// pointer checked against the set and conditionally freed.
// ============================================================================

namespace AppearanceTemplate
{
namespace
{
std::mutex& GetOwnedAllocationMutex()
{
    static std::mutex m;
    return m;
}

std::unordered_set<void*>& OwnedHeadPartArrays()
{
    static std::unordered_set<void*> owned;
    return owned;
}

std::unordered_set<void*>& OwnedTintLayers()
{
    static std::unordered_set<void*> owned;
    return owned;
}

// Free a pointer only if it was previously tracked in the given ownership set.
// Must be called while GetOwnedAllocationMutex() is held.
void FreeIfTracked(void* ptr, std::unordered_set<void*>& owned)
{
    if (!ptr)
    {
        return;
    }
    auto it = owned.find(ptr);
    if (it != owned.end())
    {
        RE::free(ptr);
        owned.erase(it);
    }
}

void CleanupOwnedAllocations()
{
    std::lock_guard<std::mutex> lock(GetOwnedAllocationMutex());
    for (auto* ptr : OwnedHeadPartArrays())
    {
        RE::free(ptr);
    }
    OwnedHeadPartArrays().clear();
    for (auto* ptr : OwnedTintLayers())
    {
        RE::free(ptr);
    }
    OwnedTintLayers().clear();
}

// Copy head parts (eyes, hair, brows, scars, facial hair) from template to player.
bool CopyHeadParts(RE::TESNPC* templateNPC, RE::TESNPC* playerBase)
{
    if (!templateNPC->headParts || templateNPC->numHeadParts == 0)
    {
        return true;
    }

    // Allocate and fill replacement array first, so we can fail before mutation.
    auto* newHeadParts = RE::calloc<RE::BGSHeadPart*>(templateNPC->numHeadParts);
    if (!newHeadParts)
    {
        logger::error("AppearanceTemplate: Failed to allocate head parts array (count={})",
                      templateNPC->numHeadParts);
        return false;
    }
    for (uint8_t i = 0; i < templateNPC->numHeadParts; ++i)
    {
        newHeadParts[i] = templateNPC->headParts[i];
    }

    RE::BGSHeadPart** oldHeadParts = playerBase->headParts;

    // Track ownership and mutate under a single lock to prevent leaks if
    // anything fails between mutation and tracking.
    bool oldWasOwned = false;
    {
        std::lock_guard<std::mutex> lock(GetOwnedAllocationMutex());
        auto& ownedArrays = OwnedHeadPartArrays();

        // Track new allocation before mutation
        ownedArrays.insert(newHeadParts);

        // Now mutate
        playerBase->headParts = newHeadParts;
        playerBase->numHeadParts = templateNPC->numHeadParts;

        // Free old array only if we previously allocated it
        oldWasOwned = ownedArrays.contains(oldHeadParts);
        FreeIfTracked(oldHeadParts, ownedArrays);
    }

    if (oldHeadParts && !oldWasOwned)
    {
        logger::warn(
            "AppearanceTemplate: Replaced engine-owned head parts pointer without freeing old "
            "memory (safety over potential invalid free)");
    }

    for (uint8_t i = 0; i < templateNPC->numHeadParts; ++i)
    {
        auto* part = templateNPC->headParts[i];
        if (part)
        {
            const char* typeName = "Unknown";
            switch (part->type.get())
            {
                case RE::BGSHeadPart::HeadPartType::kMisc:
                    typeName = "Misc";
                    break;
                case RE::BGSHeadPart::HeadPartType::kFace:
                    typeName = "Face";
                    break;
                case RE::BGSHeadPart::HeadPartType::kEyes:
                    typeName = "Eyes";
                    break;
                case RE::BGSHeadPart::HeadPartType::kHair:
                    typeName = "Hair";
                    break;
                case RE::BGSHeadPart::HeadPartType::kFacialHair:
                    typeName = "FacialHair";
                    break;
                case RE::BGSHeadPart::HeadPartType::kScar:
                    typeName = "Scar";
                    break;
                case RE::BGSHeadPart::HeadPartType::kEyebrows:
                    typeName = "Eyebrows";
                    break;
                default:
                    break;
            }
            logger::info("AppearanceTemplate:   [{}] {} - {} ({:08X})",
                         i,
                         typeName,
                         part->GetFormEditorID() ? part->GetFormEditorID() : "(no editor ID)",
                         part->GetFormID());
        }
    }
    logger::info("AppearanceTemplate: Copied {} head parts", templateNPC->numHeadParts);

    return true;
}

// Copy hair color, face details, body tint, and optionally skin textures.
bool CopyHairAndSkin(RE::TESNPC* templateNPC, RE::TESNPC* playerBase, bool copySkin)
{
    if (templateNPC->headRelatedData)
    {
        if (!playerBase->headRelatedData)
        {
            playerBase->headRelatedData = RE::calloc<RE::TESNPC::HeadRelatedData>(1);
            if (!playerBase->headRelatedData)
            {
                logger::error("AppearanceTemplate: Failed to allocate headRelatedData");
                return false;
            }
        }

        // Hair color
        if (templateNPC->headRelatedData->hairColor)
        {
            playerBase->headRelatedData->hairColor = templateNPC->headRelatedData->hairColor;
            logger::info("AppearanceTemplate: Copied hair color");
        }

        // Face texture set, skin detail textures
        if (templateNPC->headRelatedData->faceDetails)
        {
            playerBase->headRelatedData->faceDetails = templateNPC->headRelatedData->faceDetails;
            logger::info("AppearanceTemplate: Copied face texture set");
        }
    }

    playerBase->bodyTintColor = templateNPC->bodyTintColor;
    logger::info("AppearanceTemplate: Copied body tint color");

    if (copySkin)
    {
        if (templateNPC->farSkin)
        {
            playerBase->farSkin = templateNPC->farSkin;
            logger::info("AppearanceTemplate: Copied far skin");
        }

        // Copy skin form if available
        if (templateNPC->skin)
        {
            playerBase->skin = templateNPC->skin;
            logger::info("AppearanceTemplate: Copied skin form");
        }
    }
    else
    {
        logger::debug("AppearanceTemplate: Skin copy disabled (TemplateCopySkin = false)");
    }

    return true;
}

// Copy tint layers (skin tone, makeup, war paint, dirt) from template to player.
bool CopyTintLayers(RE::TESNPC* templateNPC, RE::TESNPC* playerBase)
{
    auto templateTints = templateNPC->tintLayers;
    if (!templateTints)
    {
        return true;
    }

    constexpr std::size_t MAX_TINT_LAYERS_SAFE = 1024;
    if (templateTints->size() > MAX_TINT_LAYERS_SAFE)
    {
        logger::error("AppearanceTemplate: Template tint layer count {} exceeds safety limit {}",
                      templateTints->size(),
                      MAX_TINT_LAYERS_SAFE);
        return false;
    }
    std::vector<RE::TESNPC::Layer*> stagedTintLayers;
    stagedTintLayers.reserve(templateTints->size());

    for (auto* srcLayer : *templateTints)
    {
        if (!srcLayer)
        {
            continue;
        }
        auto* newLayer = RE::calloc<RE::TESNPC::Layer>(1);
        if (!newLayer)
        {
            for (auto* layer : stagedTintLayers)
            {
                if (layer)
                {
                    RE::free(layer);
                }
            }
            logger::error(
                "AppearanceTemplate: Failed to allocate tint layer; aborting copy to avoid "
                "partial state");
            return false;
        }
        newLayer->tintIndex = srcLayer->tintIndex;
        newLayer->tintColor = srcLayer->tintColor;
        newLayer->preset = srcLayer->preset;
        newLayer->interpolationValue = srcLayer->interpolationValue;
        stagedTintLayers.push_back(newLayer);
    }

    if (!playerBase->tintLayers)
    {
        playerBase->tintLayers = RE::calloc<RE::BSTArray<RE::TESNPC::Layer*>>(1);
        if (!playerBase->tintLayers)
        {
            for (auto* layer : stagedTintLayers)
            {
                if (layer)
                {
                    RE::free(layer);
                }
            }
            logger::error("AppearanceTemplate: Failed to allocate player tint layer array");
            return false;
        }
    }

    bool preservedForeignLayers = false;
    {
        std::lock_guard<std::mutex> lock(GetOwnedAllocationMutex());
        auto& ownedLayers = OwnedTintLayers();
        for (auto* layer : *playerBase->tintLayers)
        {
            if (!layer)
            {
                continue;
            }
            if (ownedLayers.contains(layer))
            {
                FreeIfTracked(layer, ownedLayers);
            }
            else
            {
                preservedForeignLayers = true;
            }
        }
        playerBase->tintLayers->clear();
        for (auto* layer : stagedTintLayers)
        {
            playerBase->tintLayers->push_back(layer);
            ownedLayers.insert(layer);
        }
    }

    if (preservedForeignLayers)
    {
        logger::warn(
            "AppearanceTemplate: Existing non-plugin tint layers were replaced without "
            "explicit free (safety mode)");
    }
    logger::info("AppearanceTemplate: Copied {} tint layers", playerBase->tintLayers->size());

    return true;
}

// Call BSFaceGenManager::RegenerateHead via REL::Relocation.
// Fully reloading baked FaceGen meshes after faceNPC swap.
// Not exposed in CommonLibSSE-NG headers, so we call it directly.
void RegenerateHead(RE::Actor* a_actor)
{
    if (!a_actor)
    {
        return;
    }

    auto faceGenManager = RE::BSFaceGenManager::GetSingleton();
    if (!faceGenManager)
    {
        logger::warn("AppearanceTemplate: BSFaceGenManager not available for RegenerateHead");
        return;
    }

    // BSFaceGenManager::RegenerateHead(Actor*)
    // SSE 1.5.97: ID 26257, AE: ID 26836
    using RegenerateHead_t = void (*)(RE::BSFaceGenManager*, RE::Actor*);
    REL::Relocation<RegenerateHead_t> RegenerateHeadFunc{RELOCATION_ID(26257, 26836)};

    RegenerateHeadFunc(faceGenManager, a_actor);
    logger::info("AppearanceTemplate: Called RegenerateHead for full FaceGen reload");
}
}  // namespace

bool IsRaceCompatible(RE::TESNPC* templateNPC)
{
    if (!templateNPC)
    {
        return false;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player)
    {
        return false;
    }

    auto playerBase = player->GetActorBase();
    if (!playerBase)
    {
        return false;
    }

    auto playerRace = playerBase->GetRace();
    auto templateRace = templateNPC->GetRace();

    if (!playerRace || !templateRace)
    {
        return false;
    }

    // Check if same race
    if (playerRace == templateRace)
    {
        return true;
    }

    // Races differ: cross-race appearance copying is not supported (pointer equality only)
    logger::warn("AppearanceTemplate: Race mismatch - Player: {}, Template: {}",
                 playerRace->GetFormEditorID() ? playerRace->GetFormEditorID() : "Unknown",
                 templateRace->GetFormEditorID() ? templateRace->GetFormEditorID() : "Unknown");

    return false;
}

bool CopyAppearanceToPlayer(RE::TESNPC* templateNPC, bool includeRace, bool includeBody)
{
    if (!templateNPC)
    {
        logger::error("AppearanceTemplate: Template NPC is null");
        return false;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player)
    {
        logger::error("AppearanceTemplate: Player not available");
        return false;
    }

    auto playerBase = player->GetActorBase();
    if (!playerBase)
    {
        logger::error("AppearanceTemplate: Player base actor not available");
        return false;
    }
    bool copySkin = false;
    {
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        copySkin = Settings::Appearance().TemplateCopySkin;
    }

    logger::info("AppearanceTemplate: Copying appearance from {} to player",
                 templateNPC->GetName() ? templateNPC->GetName() : "Unknown NPC");

    const bool raceCompatible = IsRaceCompatible(templateNPC);
    if (!includeRace && !raceCompatible)
    {
        logger::error(
            "AppearanceTemplate: Aborting copy due to race mismatch with "
            "TemplateIncludeRace=false");
        logger::error(
            "AppearanceTemplate: Enable TemplateIncludeRace to safely copy race-specific head "
            "data");
        return false;
    }
    if (templateNPC->numHeadParts > 0 && !templateNPC->headParts)
    {
        logger::error("AppearanceTemplate: Template has numHeadParts={} but null headParts array",
                      templateNPC->numHeadParts);
        return false;
    }

    // This MUST be done first, before head parts, as head parts are race-specific
    if (includeRace)
    {
        auto templateRace = templateNPC->GetRace();
        if (templateRace)
        {
            auto playerRace = playerBase->GetRace();
            if (playerRace != templateRace)
            {
                logger::info(
                    "AppearanceTemplate: Changing race from {} to {}",
                    playerRace ? (playerRace->GetFormEditorID() ? playerRace->GetFormEditorID()
                                                                : "Unknown")
                               : "None",
                    templateRace->GetFormEditorID() ? templateRace->GetFormEditorID() : "Unknown");

                // Set the race on the player's base actor
                playerBase->race = templateRace;

                // Mark race as changed
                playerBase->AddChange(RE::TESNPC::ChangeFlags::ChangeFlag::kRace);

                logger::info("AppearanceTemplate: Race changed successfully");
            }
            else
            {
                logger::info("AppearanceTemplate: Race already matches, skipping");
            }
        }

        // Must match for head parts and animations to work correctly
        bool templateIsFemale = templateNPC->IsFemale();
        bool playerIsFemale = playerBase->IsFemale();
        if (templateIsFemale != playerIsFemale)
        {
            logger::info("AppearanceTemplate: Changing sex from {} to {}",
                         playerIsFemale ? "Female" : "Male",
                         templateIsFemale ? "Female" : "Male");

            if (templateIsFemale)
            {
                playerBase->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
            }
            else
            {
                playerBase->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
            }

            // Mark gender as changed
            playerBase->AddChange(RE::TESNPC::ChangeFlags::ChangeFlag::kGender);

            logger::info("AppearanceTemplate: Sex changed successfully");
        }
    }
    else
    {
        // Already validated compatibility above.
    }

    // Head parts include: Eyes, hair, facial hair, scars, brows, etc.
    if (!CopyHeadParts(templateNPC, playerBase))
    {
        return false;
    }

    if (!CopyHairAndSkin(templateNPC, playerBase, copySkin))
    {
        return false;
    }

    // Tint layers include: Skin tone, makeup, war paint, dirt, etc.
    if (!CopyTintLayers(templateNPC, playerBase))
    {
        return false;
    }

    playerBase->weight = templateNPC->weight;
    logger::info("AppearanceTemplate: Copied weight: {}", templateNPC->weight);

    // Face morphs control the facial structure
    if (templateNPC->faceNPC)
    {
        playerBase->faceNPC = templateNPC->faceNPC;
        logger::info("AppearanceTemplate: Copied face NPC reference");
    }

    // Copy face data if present
    if (templateNPC->faceData && playerBase->faceData)
    {
        // Copy morph sliders
        for (int i = 0; i < RE::TESNPC::FaceData::Morphs::kTotal; ++i)
        {
            playerBase->faceData->morphs[i] = templateNPC->faceData->morphs[i];
        }

        // Copy face parts
        for (int i = 0; i < RE::TESNPC::FaceData::Parts::kTotal; ++i)
        {
            playerBase->faceData->parts[i] = templateNPC->faceData->parts[i];
        }
        logger::info("AppearanceTemplate: Copied face morphs and parts");
    }
    else if (templateNPC->faceData && !playerBase->faceData)
    {
        // Allocate face data for player if needed
        playerBase->faceData = RE::calloc<RE::TESNPC::FaceData>(1);
        if (!playerBase->faceData)
        {
            logger::error("AppearanceTemplate: Failed to allocate faceData");
            return false;
        }
        for (int i = 0; i < RE::TESNPC::FaceData::Morphs::kTotal; ++i)
        {
            playerBase->faceData->morphs[i] = templateNPC->faceData->morphs[i];
        }
        for (int i = 0; i < RE::TESNPC::FaceData::Parts::kTotal; ++i)
        {
            playerBase->faceData->parts[i] = templateNPC->faceData->parts[i];
        }
        logger::info("AppearanceTemplate: Created and copied face morphs");
    }

    if (includeBody)
    {
        // Copy height
        playerBase->height = templateNPC->height;
        logger::info("AppearanceTemplate: Copied height: {}", templateNPC->height);

        // Note: Skin is race-dependent and copying it can cause crashes
        // Only height is safe to copy across different setups
    }

    // Mark appearance as changed
    playerBase->AddChange(RE::TESNPC::ChangeFlags::ChangeFlag::kFace);

    return true;
}

void UpdatePlayerAppearance()
{
    auto* taskInterface = SKSE::GetTaskInterface();
    auto updateTask = []()
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (player)
        {
            auto playerBase = player->GetActorBase();

            // Update hair color first
            player->UpdateHairColor();

            // Update skin color
            player->UpdateSkinColor();

            // RegenerateHead fully reloads baked FaceGen mesh
            RegenerateHead(player);

            // Force 3D model reset to apply remaining appearance changes
            // This also handles NiNode updates internally
            player->DoReset3D(true);

            // Additional 3D model update for armor/equipment refresh
            player->Update3DModel();

            // Update neck seam after head changes
            if (playerBase)
            {
                auto faceNode = player->GetFaceNodeSkinned();
                if (faceNode)
                {
                    playerBase->UpdateNeck(faceNode);
                    logger::debug("AppearanceTemplate: Updated neck seam");
                }
            }

            logger::info("AppearanceTemplate: Player appearance update completed");
        }
    };

    // Queue update for next frame when task interface is available.
    if (taskInterface)
    {
        taskInterface->AddTask(updateTask);
    }
    else
    {
        updateTask();
    }
}

}  // namespace AppearanceTemplate
