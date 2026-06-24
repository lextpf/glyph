#include "RendererInternal.hpp"

#include "GameState.hpp"
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

// @author Claude (https://github.com/claude)
// Runs *only* on the game thread (scheduled via SKSE::GetTaskInterface()):
// CommonLibSSE's RE::* types (ProcessLists, Actor, TESDataHandler) aren't
// render-thread safe.
//
// The render thread never touches RE::* directly; it reads a plain-old-data
// std::vector<ActorDrawData> snapshot under snapshotLock. Everything the
// renderer needs (FormID, name, world position, level, relationship,
// occlusion result) is precomputed here so the render-thread copy needs no
// further game-thread trips.
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
    struct ScanCandidate
    {
        RE::Actor* actor{nullptr};
        ActorDrawData data{};
    };
    std::vector<ScanCandidate> candidates;
    candidates.reserve(MAX_SCAN);

    const auto playerPos = player->GetPosition();

    // Include the player character first
    if (!Settings::Display().HidePlayer)
    {
        ActorDrawData d;
        d.formID = player->GetFormID();
        d.level = player->GetLevel();
        const char* rawName = player->GetDisplayFullName();
        d.name = rawName ? Capitalize(rawName) : "Player";
        d.worldPos = playerPos;
        d.worldPos.z += player->GetHeight() + Settings::Display().VerticalOffset;
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

        tempBuf.push_back(std::move(d));
        seenFormIDs.insert(player->GetFormID());
    }

    int added = static_cast<int>(tempBuf.size());
    int scanned = 0;

    for (auto& h : pl->highActorHandles)
    {
        if (scanned >= MAX_SCAN)
        {
            break;
        }
        ++scanned;

        auto aSP = h.get();
        auto* a = aSP.get();
        if (!a || a == player || a->IsDead())
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

        ScanCandidate candidate;
        candidate.actor = a;
        ActorDrawData& d = candidate.data;
        d.formID = a->GetFormID();
        d.level = a->GetLevel();
        const char* rawName = a->GetDisplayFullName();
        d.name = rawName ? Capitalize(rawName) : "";
        d.worldPos = a->GetPosition();
        d.worldPos.z += a->GetHeight() + Settings::Display().VerticalOffset;
        d.distToPlayer = std::sqrt(distSq);
        d.isPlayer = false;

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

        candidates.push_back(std::move(candidate));
    }

    std::sort(candidates.begin(),
              candidates.end(),
              [](const ScanCandidate& lhs, const ScanCandidate& rhs)
              { return lhs.data.distToPlayer < rhs.data.distToPlayer; });

    const int remainingSlots = std::max(0, MAX_ACTORS - added);
    if (static_cast<int>(candidates.size()) > remainingSlots)
    {
        candidates.resize(static_cast<size_t>(remainingSlots));
    }

    for (auto& candidate : candidates)
    {
        auto& d = candidate.data;
        if (Settings::Occlusion().Enabled)
        {
            UpdateOcclusionForActor(d, candidate.actor, player, snapshotFrame, checkInterval);
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

    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot = tempBuf;
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
