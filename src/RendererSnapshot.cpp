#include "RendererInternal.h"

#include "GameState.h"
#include "Occlusion.h"

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

// Get actor's fight reaction towards player
static RE::FIGHT_REACTION GetReactionToPlayer(RE::Actor* a_actor, RE::Actor* a_player)
{
    if (!a_actor || !a_player)
    {
        return RE::FIGHT_REACTION::kNeutral;
    }
    if (a_actor == a_player)
    {
        return RE::FIGHT_REACTION::kFriend;
    }

    // Hostility
    if (a_actor->IsHostileToActor(a_player) || a_player->IsHostileToActor(a_actor))
    {
        return RE::FIGHT_REACTION::kEnemy;
    }

    // Followers / teammates
    if (a_actor->IsPlayerTeammate())
    {
        return RE::FIGHT_REACTION::kAlly;
    }

    // Optional heuristic: "friendly" if they can do normal player dialogue
    if (a_actor->CanTalkToPlayer())
    {
        return RE::FIGHT_REACTION::kFriend;
    }

    return RE::FIGHT_REACTION::kNeutral;
}

// Determine actor's disposition for nameplate color
static Disposition GetDisposition(RE::Actor* a, RE::Actor* player)
{
    if (!a || !player)
    {
        return Disposition::Neutral;
    }

    // Hard override: Currently hostile (combat/crime/aggro/etc.)
    if (a->IsHostileToActor(player))
    {
        return Disposition::Enemy;
    }

    // Teammate is always "friendly enough" for UI
    if (a->IsPlayerTeammate())
    {
        return Disposition::AllyOrFriend;
    }

    // Baseline faction/relationship reaction
    switch (GetReactionToPlayer(a, player))
    {
        case RE::FIGHT_REACTION::kEnemy:
            return Disposition::Enemy;

        case RE::FIGHT_REACTION::kAlly:
        case RE::FIGHT_REACTION::kFriend:
            return Disposition::AllyOrFriend;

        case RE::FIGHT_REACTION::kNeutral:
        default:
            return Disposition::Neutral;
    }
}

// Check whether an actor is a humanoid NPC (as opposed to a creature/animal).
// Prefer ActorTypeNPC on actor, then base, then race for robustness across mods.
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

// ============================================================================
// Main snapshot functions
// ============================================================================

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
        d.dispo = GetDisposition(a, player);
        d.isPlayer = false;

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
