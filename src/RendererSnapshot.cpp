#include "RendererInternal.hpp"

#include "GameState.hpp"
#include "HudCompat.hpp"
#include "Occlusion.hpp"

#include <SKSE/SKSE.h>

namespace Renderer
{
// ============================================================================
// Snapshot state accessor (defined here, declared in RendererInternal.h)
// ============================================================================

SnapshotState& GetSnapshotState()
{
    static SnapshotState instance;
    return instance;
}

// ============================================================================
// Helper functions
// ============================================================================

// Get player character
static RE::Actor* GetPlayer()
{
    return RE::PlayerCharacter::GetSingleton();
}

// Capitalize text and trim whitespace (ASCII title-case only).
// Multi-byte UTF-8 codepoints are passed through unchanged because
// std::toupper is not Unicode-aware and would corrupt multi-byte sequences.
std::string Capitalize(const char* text)
{
    if (!text || !*text)
    {
        return "";
    }
    std::string s = text;

    // Trim leading/trailing whitespace
    size_t first = s.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
    {
        return "";
    }
    size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, (last - first + 1));

    // UTF-8-aware title case: only toupper single-byte ASCII characters,
    // pass multi-byte codepoints through unchanged to avoid corruption.
    std::string result;
    result.reserve(s.size());
    bool newWord = true;
    const char* p = s.c_str();
    while (*p)
    {
        size_t len = Utf8Utils::Utf8CharLen(p);
        if (len == 1)
        {
            unsigned char c = static_cast<unsigned char>(*p);
            if (isspace(c))
            {
                result += *p;
                newWord = true;
            }
            else if (newWord)
            {
                result += static_cast<char>(toupper(c));
                newWord = false;
            }
            else
            {
                result += *p;
            }
        }
        else
        {
            // Multi-byte UTF-8 character: append as-is
            result.append(p, len);
            newWord = false;
        }
        p += len;
    }
    return result;
}

// @author Claude (https://github.com/claude)
// Check whether an actor is a humanoid NPC (vs creature / animal).
//
// Authoritative signal: the ActorTypeNPC keyword (Skyrim.esm 0x13794),
// looked up lazily and cached -- the data handler isn't guaranteed to be
// ready at plugin load, but always is by the time the renderer is asking
// about live actors.
//
// Mods disagree on where they tag NPC-ness, so probe three places in order:
//   1. Actor instance keywords  - some scripts inject the tag at runtime.
//   2. Actor base (TESNPC)      - the most common author location.
//   3. Race keywords            - vanilla Bethesda content tags it here.
// If none match, treat as creature and let HideCreatures filter it out.
static bool IsHumanoidNPC(RE::Actor* actor)
{
    if (!actor)
    {
        return false;
    }

    if (!GetSnapshotState().npcKeywordLookupAttempted)
    {
        GetSnapshotState().npcKeywordLookupAttempted = true;
        if (auto* dataHandler = RE::TESDataHandler::GetSingleton(); dataHandler)
        {
            GetSnapshotState().npcKeyword =
                dataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
        }
        if (!GetSnapshotState().npcKeyword && !GetSnapshotState().npcKeywordMissingLogged)
        {
            GetSnapshotState().npcKeywordMissingLogged = true;
            logger::warn(
                "Renderer: ActorTypeNPC keyword lookup failed, using creature filter "
                "fallback heuristic");
        }
    }

    if (GetSnapshotState().npcKeyword)
    {
        if (actor->HasKeyword(GetSnapshotState().npcKeyword))
        {
            return true;
        }
        if (auto* actorBase = actor->GetActorBase(); actorBase)
        {
            if (actorBase->HasKeyword(GetSnapshotState().npcKeyword))
            {
                return true;
            }
            if (auto* race = actorBase->GetRace(); race)
            {
                if (race->HasKeyword(GetSnapshotState().npcKeyword))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Conservative fallback if ActorTypeNPC keyword is unavailable.
    return actor->IsPlayerTeammate() || actor->CanTalkToPlayer();
}

// Map an actor-vs-player level delta to a bucket using INI thresholds.
// Strictly ordered: Weak < Strong < Deadly (validated in Settings::ClampAndValidate).
static LevelDelta ClassifyDelta(
    int actorLv, int playerLv, int weakAtOrBelow, int strongAtOrAbove, int deadlyAtOrAbove)
{
    const int delta = actorLv - playerLv;
    if (delta >= deadlyAtOrAbove)
    {
        return LevelDelta::Deadly;
    }
    if (delta >= strongAtOrAbove)
    {
        return LevelDelta::Strong;
    }
    if (delta <= weakAtOrBelow)
    {
        return LevelDelta::Weak;
    }
    return LevelDelta::Even;
}

// Coarse creature classification via `ActorTypeX` keywords.
// Probe most-specific-first because Dragon/Daedra often also carry the
// generic ActorTypeCreature tag.
static CreatureKind ClassifyCreature(RE::Actor* actor)
{
    if (!actor)
    {
        return CreatureKind::NPC;
    }
    if (actor->HasKeywordString("ActorTypeDragon"))
    {
        return CreatureKind::Dragon;
    }
    if (actor->HasKeywordString("ActorTypeDaedra"))
    {
        return CreatureKind::Daedra;
    }
    if (actor->HasKeywordString("ActorTypeUndead"))
    {
        return CreatureKind::Undead;
    }
    if (actor->HasKeywordString("ActorTypeCreature") || actor->HasKeywordString("ActorTypeAnimal"))
    {
        return CreatureKind::Beast;
    }
    return CreatureKind::NPC;
}

// Invulnerability classification for the protection badge.  Essential
// overrides Protected when both flags are set.
static ProtectionKind ClassifyProtection(RE::Actor* actor)
{
    if (!actor)
    {
        return ProtectionKind::Mortal;
    }
    if (actor->IsEssential())
    {
        return ProtectionKind::Essential;
    }
    if (actor->IsProtected())
    {
        return ProtectionKind::Protected;
    }
    return ProtectionKind::Mortal;
}

// Social-role classification for the role badge.  Guard takes priority over
// merchant; "merchant" is membership in any vendor-flagged faction.
static RoleKind ClassifyRole(RE::Actor* actor)
{
    if (!actor)
    {
        return RoleKind::Commoner;
    }
    if (actor->IsGuard())
    {
        return RoleKind::Guard;
    }
    bool vendor = false;
    actor->VisitFactions(
        [&vendor](RE::TESFaction* faction, std::int8_t rank)
        {
            if (faction && rank >= 0 && faction->IsVendor())
            {
                vendor = true;
                return true;  // stop visiting
            }
            return false;
        });
    return vendor ? RoleKind::Merchant : RoleKind::Commoner;
}

// Awareness/engagement classification for the engagement badge.  Combat
// supersedes alert.  "Alert" is a proxy (weapon drawn while detecting the
// player) because no clean "aware of the player" API exists -- tune in-game.
static EngagementKind ClassifyEngagement(RE::Actor* actor, RE::Actor* player)
{
    if (!actor)
    {
        return EngagementKind::Idle;
    }
    if (actor->IsInCombat())
    {
        return EngagementKind::Combat;
    }
    // IsWeaponDrawn lives on ActorState (not re-exposed on Actor in NG) --
    // reach it through AsActorState().
    if (player && actor->AsActorState()->IsWeaponDrawn() &&
        actor->RequestDetectionLevel(player) > 0)
    {
        return EngagementKind::Alert;
    }
    return EngagementKind::Idle;
}

// True when the player owes a current bounty (crime gold) to any faction.
// Reads PlayerCharacter::crimeGoldMap; "current" excludes infamy.
static bool PlayerHasBounty()
{
    auto* pc = RE::PlayerCharacter::GetSingleton();
    if (!pc)
    {
        return false;
    }
    for (const auto& entry : pc->GetCrimeValue().crimeGoldMap)
    {
        if (entry.second.violentCur + entry.second.nonViolentCur > .0f)
        {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Deeds, Not Words -- honorific resolution (game-thread only)
// ============================================================================

/// Runtime cache for honorific matching: resolved faction pointers (parallel
/// to Settings::Honorifics(), rebuilt when the settings generation changes)
/// plus a per-actor match cache refreshed on an interval -- faction ranks
/// change rarely (quest completions), not per frame.
struct HonorificRuntime
{
    uint32_t settingsGen = 0;               ///< Settings generation of `factions`
    std::vector<RE::TESFaction*> factions;  ///< Parallel to Settings::Honorifics()

    struct CacheEntry
    {
        uint32_t lastCheckFrame = 0;  ///< Snapshot frame of the last match
        int index = -1;               ///< Matched honorific index (-1 = none)
    };
    std::unordered_map<uint32_t, CacheEntry> perActor;  ///< keyed by formID
};

static HonorificRuntime& GetHonorificRuntime()
{
    static HonorificRuntime instance;
    return instance;
}

// Parse "0xFORMID" or "0xFORMID@Plugin.esp" (plugin defaults to Skyrim.esm)
// and resolve the faction against the load order.
static RE::TESFaction* ResolveFactionSpec(const std::string& spec)
{
    if (spec.empty())
    {
        return nullptr;
    }

    std::string idPart = spec;
    std::string plugin = "Skyrim.esm";
    if (const size_t at = spec.find('@'); at != std::string::npos)
    {
        idPart = spec.substr(0, at);
        plugin = spec.substr(at + 1);
    }

    const uint32_t rawID = static_cast<uint32_t>(std::strtoul(idPart.c_str(), nullptr, 16));
    if (rawID == 0)
    {
        return nullptr;
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler)
    {
        return nullptr;
    }
    return dataHandler->LookupForm<RE::TESFaction>(rawID, plugin);
}

// Rebuild the resolved-faction table when settings change (load, F7 reload).
static void RefreshHonorificRuntime()
{
    auto& rt = GetHonorificRuntime();
    const uint32_t gen = Settings::Generation().load(std::memory_order_acquire);
    if (rt.settingsGen == gen)
    {
        return;
    }
    rt.settingsGen = gen;
    rt.perActor.clear();
    rt.factions.clear();

    const auto& defs = Settings::Honorifics();
    rt.factions.reserve(defs.size());
    for (const auto& defn : defs)
    {
        RE::TESFaction* faction = ResolveFactionSpec(defn.factionSpec);
        if (!faction && !defn.factionSpec.empty())
        {
            logger::warn("Honorific: faction '{}' did not resolve (title '{}')",
                         defn.factionSpec,
                         defn.title);
        }
        rt.factions.push_back(faction);
    }
}

/// Frames between per-actor honorific refreshes (deeds change rarely).
static constexpr uint32_t HONORIFIC_REFRESH_FRAMES = 90;

// Resolve the highest-priority honorific an actor has earned, or empty.
// One faction walk per refresh; matches are cached per formID.
static std::string ResolveHonorific(RE::Actor* actor, bool isPlayer, uint32_t snapshotFrame)
{
    const auto& defs = Settings::Honorifics();
    if (defs.empty() || !actor)
    {
        return {};
    }

    auto& rt = GetHonorificRuntime();
    auto& entry = rt.perActor[actor->GetFormID()];
    const uint32_t framesSince = snapshotFrame - entry.lastCheckFrame;
    if (entry.lastCheckFrame == 0 || framesSince >= HONORIFIC_REFRESH_FRAMES)
    {
        entry.lastCheckFrame = snapshotFrame;
        entry.index = -1;
        int bestPriority = (std::numeric_limits<int>::min)();
        actor->VisitFactions(
            [&](RE::TESFaction* faction, std::int8_t rank)
            {
                if (!faction || rank < 0)
                {
                    return false;
                }
                for (size_t i = 0; i < defs.size(); ++i)
                {
                    if (rt.factions[i] != faction)
                    {
                        continue;
                    }
                    const auto& defn = defs[i];
                    if (defn.title.empty() || rank < defn.minRank ||
                        (defn.playerOnly && !isPlayer) || (defn.npcOnly && isPlayer))
                    {
                        continue;
                    }
                    if (entry.index < 0 || defn.priority > bestPriority)
                    {
                        entry.index = static_cast<int>(i);
                        bestPriority = defn.priority;
                    }
                }
                return false;  // keep visiting
            });
    }

    return entry.index >= 0 ? defs[static_cast<size_t>(entry.index)].title : std::string{};
}

// ============================================================================
// Registers -- scene context evaluation (game-thread only)
// ============================================================================

// Compute the current scene-context predicate mask.  All predicates read
// game state, so this runs on the game thread once per snapshot.
static uint32_t ComputeContextMask(RE::Actor* player, int visiblePlateCount)
{
    uint32_t mask = 0;

    if (auto* cell = player->GetParentCell(); cell && cell->IsInteriorCell())
    {
        mask |= Settings::Context::Interior;
    }

    if (auto* calendar = RE::Calendar::GetSingleton())
    {
        const float hour = calendar->GetHour();
        if (hour < 6.0f || hour >= 20.0f)
        {
            mask |= Settings::Context::Night;
        }
    }

    if (auto* location = player->GetCurrentLocation())
    {
        if (location->HasKeywordString("LocTypeCity") || location->HasKeywordString("LocTypeTown"))
        {
            mask |= Settings::Context::City;
        }
    }

    if (player->IsSneaking())
    {
        mask |= Settings::Context::Sneaking;
    }

    if (auto* topicManager = RE::MenuTopicManager::GetSingleton();
        topicManager && topicManager->speaker.get())
    {
        mask |= Settings::Context::Dialogue;
    }

    if (visiblePlateCount >= Settings::RegisterConfig().CrowdedThreshold)
    {
        mask |= Settings::Context::Crowded;
    }

    return mask;
}

// Pick the highest-priority register matching the context mask, or -1.
// An empty When (both masks zero) matches every scene -- a base register.
// Unconfigured entries (index-gap backfill) never match; without this, a
// phantom [Register0] would shadow the user's real register on priority
// ties, since lower indices win them.
static int PickRegister(uint32_t ctxMask)
{
    const auto& regs = Settings::Registers();
    int best = -1;
    int bestPriority = (std::numeric_limits<int>::min)();
    for (size_t i = 0; i < regs.size(); ++i)
    {
        const auto& r = regs[i];
        if (!r.configured || (ctxMask & r.whenMask) != r.whenMask || (ctxMask & r.whenNotMask) != 0)
        {
            continue;
        }
        if (best < 0 || r.priority > bestPriority)
        {
            best = static_cast<int>(i);
            bestPriority = r.priority;
        }
    }
    return best;
}

// Check occlusion for an actor, using game-thread-local cached results.
static void UpdateOcclusionForActor(ActorDrawData& d,
                                    RE::Actor* a,
                                    RE::Actor* player,
                                    uint32_t snapshotFrame,
                                    uint32_t checkInterval)
{
    auto& entry = GetOcclusionCache()[d.formID];
    if (entry.lastCheckFrame != 0)
    {
        const uint32_t framesSince = snapshotFrame - entry.lastCheckFrame;
        if (framesSince < checkInterval)
        {
            d.isOccluded = entry.cachedOccluded;
            return;
        }
    }

    // Perform fresh occlusion check using nameplate world position
    d.isOccluded = Occlusion::IsActorOccluded(a, player, d.worldPos, Settings::Occlusion().Enabled);
    entry.lastCheckFrame = snapshotFrame;
    entry.cachedOccluded = d.isOccluded;
}

// World Z for the plate anchor. Reads the rendered head node so the plate
// tracks RaceMenu height/body morphs, HDT high-heels, and equipped-heel offsets
// (all of which move the skeleton, not the collision bound GetHeight() sees).
// Falls back to the bound-derived height when the 3D or node is unavailable or
// implausible (e.g. the player's first-person skeleton).
static float ComputeAnchorZ(RE::Actor* a)
{
    const float feetZ = a->GetPosition().z;
    if (auto* root = a->Get3D())
    {
        if (auto* head = root->GetObjectByName("NPC Head [Head]"))
        {
            const float headZ = head->world.translate.z;
            if (headZ > feetZ)  // plausibility guard
            {
                constexpr float kHeadHeadroom = 12.0f;  // node sits at head base; clear the crown
                return headZ + kHeadHeadroom + Settings::Display().VerticalOffset;
            }
        }
    }
    return feetZ + a->GetHeight() + Settings::Display().VerticalOffset;
}

// @author Claude (https://github.com/claude)
// Runs *only* on the game thread (scheduled via SKSE::GetTaskInterface()):
// CommonLibSSE's RE::* types (ProcessLists, Actor, TESDataHandler) aren't
// render-thread safe.
//
// The render thread consumes a plain-old-data std::vector<ActorDrawData>
// snapshot under snapshotLock and otherwise stays off RE::*. The one exception
// is a lock-free read of the world camera in WorldToScreen() (RE::NiCamera) for
// world->screen projection, where a torn read is a benign one-frame glitch.
// Everything else the renderer needs (FormID, name, world position, level,
// relationship, occlusion result) is precomputed here so the render-thread copy
// needs no further game-thread trips.
void UpdateSnapshot_GameThread()
{
    // RAII guard to ensure flags are cleared when this task exits.
    struct UpdateScope
    {
        UpdateScope() { GetState().snapshotUpdateRunning.store(true, std::memory_order_release); }

        ~UpdateScope()
        {
            GetState().snapshotUpdateRunning.store(false, std::memory_order_release);
            GetState().updateQueued.store(false, std::memory_order_release);
        }
    } _;

    if (GetState().pauseSnapshotUpdates.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        return;
    }

    if (GetState().clearOcclusionCacheRequested.exchange(false, std::memory_order_acq_rel))
    {
        GetOcclusionCache().clear();
        GetSnapshotState().frame = 0;
    }

    // Check if we're allowed to draw the overlay (not in menus, loading, etc.)
    const bool allow = GameState::CanDrawOverlay();
    GetState().allowOverlay.store(allow, std::memory_order_release);

    if (!allow)
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        return;
    }

    auto* player = GetPlayer();
    auto* pl = RE::ProcessLists::GetSingleton();
    if (!player || !pl)
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        return;
    }

    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
    RefreshHonorificRuntime();
    constexpr int MAX_ACTORS = RenderConstants::MAX_ACTORS;
    constexpr int MAX_SCAN = RenderConstants::MAX_SCAN;
    const float kMaxDistSq =
        Settings::Distance().MaxScanDistance * Settings::Distance().MaxScanDistance;
    const uint32_t checkInterval =
        static_cast<uint32_t>(std::max(1, Settings::Occlusion().CheckInterval));
    const uint32_t snapshotFrame = ++GetSnapshotState().frame;

    // Snapshot level-delta thresholds once; used for every actor classification.
    const auto& labelSettings = Settings::Labels();
    const int playerLevel = static_cast<int>(player->GetLevel());
    const int weakBelow = labelSettings.WeakAtOrBelow;
    const int strongAbove = labelSettings.StrongAtOrAbove;
    const int deadlyAbove = labelSettings.DeadlyAtOrAbove;

    std::vector<ActorDrawData> tempBuf;
    tempBuf.reserve(MAX_ACTORS);
    std::unordered_set<uint32_t> seenFormIDs;
    seenFormIDs.reserve(MAX_ACTORS);
    // Lightweight scan candidate: the cheap distance pass collects only what it
    // needs to rank actors by proximity. The expensive per-actor derivation
    // (name, relationship, badges, honorific, occlusion) is deferred to the
    // nearest MAX_ACTORS below, so it never runs for actors that won't get a plate.
    struct NearActor
    {
        RE::Actor* actor{nullptr};
        float distSq{.0f};
        bool dead{false};
    };
    std::vector<NearActor> nearActors;
    nearActors.reserve(MAX_SCAN);

    const auto playerPos = player->GetPosition();

    // One Voice Per Actor: which actors do other HUD mods already cover?
    const bool yieldToTrueHUD = Settings::Compat().YieldToTrueHUD && HudCompat::HasTrueHUD();
    const bool yieldToMoreHUD = Settings::Compat().YieldLevelToMoreHUD && HudCompat::HasMoreHUD();
    const uint32_t crosshairID = yieldToMoreHUD ? HudCompat::CrosshairTargetFormID() : 0;

    // Include the player character first
    if (!Settings::Display().HidePlayer)
    {
        ActorDrawData d;
        d.formID = player->GetFormID();
        d.level = player->GetLevel();
        // Prefer the actor-base (TESNPC) full name for the player: a RaceMenu
        // rename writes the base name immediately, while GetDisplayFullName()
        // can keep serving a stale ExtraTextDisplayData string from the player
        // reference until a save/reload rebuilds it -- the plate never picked
        // up in-session renames.
        const char* rawName = nullptr;
        if (auto* base = player->GetActorBase())
        {
            rawName = base->GetFullName();
        }
        if (!rawName || !*rawName)
        {
            rawName = player->GetDisplayFullName();
        }
        d.name = (rawName && *rawName) ? Capitalize(rawName) : "Player";
        d.worldPos = playerPos;
        d.worldPos.z = ComputeAnchorZ(player);
        d.distToPlayer = .0f;
        d.isPlayer = true;
        // Player has no meaningful "relation to self" -- pick stable defaults.
        // These still feed the %r/%d/%c text tokens; the player's BADGE set is
        // a separate branch in ComposeBadges driven by the facts below.
        d.relationship = RelationshipKind::Follower;
        d.levelDelta = LevelDelta::Even;
        d.creatureKind = CreatureKind::NPC;

        // Player status badge facts (always-on player slots).  The detection
        // scan only runs while sneaking, so its cost is near-zero otherwise.
        const bool sneaking = player->IsSneaking();
        bool detected = false;
        if (sneaking)
        {
            for (auto& handle : pl->highActorHandles)
            {
                auto otherSP = handle.get();
                auto* other = otherSP.get();
                if (!other || other == player || other->IsDead())
                {
                    continue;
                }
                if (other->IsHostileToActor(player) && other->RequestDetectionLevel(player) > 0)
                {
                    detected = true;
                    break;
                }
            }
        }
        d.sneak = sneaking ? (detected ? SneakKind::Detected : SneakKind::Hidden) : SneakKind::Off;
        d.playerInCombat = player->IsInCombat();
        d.encumbered = player->IsOverEncumbered();
        d.wanted = PlayerHasBounty();
        d.honorific = ResolveHonorific(player, true, snapshotFrame);

        tempBuf.push_back(std::move(d));
        seenFormIDs.insert(player->GetFormID());
    }

    int added = static_cast<int>(tempBuf.size());

    // Cheap pass: distance-filter EVERY high-process actor. Only proximity
    // ranking data is gathered here -- no name/relationship/badge/occlusion
    // work -- so iterating the whole list is negligible. The old loop capped
    // iteration at MAX_SCAN by LIST POSITION and folded the expensive
    // derivation inline, which meant that in a crowded area (a long
    // highActorHandles list, e.g. after passing many NPCs) a near actor sitting
    // past the cutoff was never examined and got no plate. highActorHandles is
    // bounded by the engine's high-process budget; MAX_SCAN survives only as a
    // runaway guard that never trips in normal play.
    int scanned = 0;
    for (auto& h : pl->highActorHandles)
    {
        if (++scanned > MAX_SCAN)
        {
            break;
        }
        auto aSP = h.get();
        auto* a = aSP.get();
        if (!a || a == player)
        {
            continue;
        }

        // Dead actors stay in the snapshot as bare Last Rites signals: the
        // render thread plays a one-shot valediction for corpses it saw
        // alive (replaying their last live facts) and ignores the rest.
        const bool dead = a->IsDead();
        if (dead && !Settings::DeathRite().Enabled)
        {
            continue;
        }

        // Skip creatures/animals if HideCreatures is enabled.
        if (Settings::Display().HideCreatures && !IsHumanoidNPC(a))
        {
            continue;
        }

        const float distSq = playerPos.GetSquaredDistance(a->GetPosition());
        if (distSq > kMaxDistSq)
        {
            continue;
        }

        nearActors.push_back(NearActor{a, distSq, dead});
    }

    // Keep the nearest (MAX_ACTORS - added) in-range actors. partial_sort ranks
    // only the closest handful instead of ordering the whole in-range set.
    const int remainingSlots = std::max(0, MAX_ACTORS - added);
    const size_t keep = std::min(static_cast<size_t>(remainingSlots), nearActors.size());
    std::partial_sort(nearActors.begin(),
                      nearActors.begin() + static_cast<std::ptrdiff_t>(keep),
                      nearActors.end(),
                      [](const NearActor& lhs, const NearActor& rhs)
                      { return lhs.distSq < rhs.distSq; });

    // Expensive pass: full per-actor derivation only for the plates we will show.
    for (size_t i = 0; i < keep; ++i)
    {
        RE::Actor* a = nearActors[i].actor;
        const bool dead = nearActors[i].dead;

        ActorDrawData d;
        d.formID = a->GetFormID();
        d.level = a->GetLevel();
        const char* rawName = a->GetDisplayFullName();
        d.name = rawName ? Capitalize(rawName) : "";
        d.worldPos = a->GetPosition();
        d.worldPos.z = ComputeAnchorZ(a);
        d.distToPlayer = std::sqrt(nearActors[i].distSq);
        d.isPlayer = false;
        d.isDead = dead;
        if (dead)
        {
            // The rite renders from the last live draw data, so contextual
            // facts (relationship, badges, occlusion) are not re-derived for
            // corpses -- a hostile's name must not flip neutral mid-farewell.
            seenFormIDs.insert(d.formID);
            tempBuf.push_back(std::move(d));
            continue;
        }

        // Relationship derivation -- drives the %r token, badges, and NPC text color.
        const bool hostile = a->IsHostileToActor(player);
        const bool teammate = a->IsPlayerTeammate();
        const bool canTalk = !hostile && !teammate && a->CanTalkToPlayer();
        d.relationship = hostile    ? RelationshipKind::Hostile
                         : teammate ? RelationshipKind::Follower
                         : canTalk  ? RelationshipKind::Ally
                                    : RelationshipKind::Neutral;
        d.levelDelta = ClassifyDelta(
            static_cast<int>(d.level), playerLevel, weakBelow, strongAbove, deadlyAbove);
        d.creatureKind = ClassifyCreature(a);

        // Status badge facts (always-on NPC slots).
        d.protection = ClassifyProtection(a);
        d.role = ClassifyRole(a);
        d.engagement = ClassifyEngagement(a, player);

        d.honorific = ResolveHonorific(a, false, snapshotFrame);

        // One Voice Per Actor: yield to HUD mods already covering this actor.
        d.yieldPlate = yieldToTrueHUD && HudCompat::TrueHUDShowsBarFor(a);
        d.yieldLevel = yieldToMoreHUD && d.formID == crosshairID;

        if (Settings::Occlusion().Enabled)
        {
            UpdateOcclusionForActor(d, a, player, snapshotFrame, checkInterval);
        }

        seenFormIDs.insert(d.formID);
        tempBuf.push_back(std::move(d));
    }

    for (auto it = GetOcclusionCache().begin(); it != GetOcclusionCache().end();)
    {
        if (seenFormIDs.find(it->first) == seenFormIDs.end())
        {
            it = GetOcclusionCache().erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto& honorificCache = GetHonorificRuntime().perActor;
    for (auto it = honorificCache.begin(); it != honorificCache.end();)
    {
        if (seenFormIDs.find(it->first) == seenFormIDs.end())
        {
            it = honorificCache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Registers: evaluate scene predicates against the plate count just
    // gathered and publish the active profile for the render thread to ease
    // toward.
    int activeRegister = -1;
    if (Settings::RegisterConfig().Enabled && !Settings::Registers().empty())
    {
        activeRegister = PickRegister(ComputeContextMask(player, static_cast<int>(tempBuf.size())));
    }
    GetState().activeRegister.store(activeRegister, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot = std::move(tempBuf);
    }
}

void QueueSnapshotUpdate_RenderThread()
{
    if (GetState().pauseSnapshotUpdates.load(std::memory_order_acquire))
    {
        return;
    }

    // Check if an update is already queued
    // If exchange returns true, an update is already pending, so skip
    if (GetState().updateQueued.exchange(true))
    {
        return;  // Already queued, don't queue again
    }

    // Schedule the update task on the game thread
    // We can't iterate actors safely from the render thread, so we
    // use SKSE's task interface to run on the game thread instead
    if (auto* task = SKSE::GetTaskInterface())
    {
        task->AddTask([]() { UpdateSnapshot_GameThread(); });
    }
    else
    {
        // Task interface not available, clear the flag
        GetState().updateQueued.store(false);
    }
}

}  // namespace Renderer
