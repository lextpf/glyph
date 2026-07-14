#include "RendererInternal.hpp"

#include "GameState.hpp"
#include "GraffitoMath.hpp"
#include "HudCompat.hpp"
#include "Occlusion.hpp"

#include <SKSE/SKSE.h>

#include <chrono>

namespace Renderer
{

SnapshotState& GetSnapshotState()
{
    static SnapshotState instance;
    return instance;
}

/**
 * @fn static RE::Actor* GetPlayer()
 * @brief Read the player singleton on the game thread.
 * @author Alex (<https://github.com/lextpf>)
 */
static RE::Actor* GetPlayer()
{
    return RE::PlayerCharacter::GetSingleton();
}

std::string Capitalize(const char* text)
{
    if (!text || !*text)
    {
        return "";
    }
    std::string s = text;

    size_t first = s.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
    {
        return "";
    }
    size_t last = s.find_last_not_of(" \t\r\n");
    s = s.substr(first, (last - first + 1));

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
            result.append(p, len);
            newWord = false;
        }
        p += len;
    }
    return result;
}

/**
 * @fn static bool IsHumanoidNPC(RE::Actor* actor)
 * @brief Classify humanoid actors through keywords with a missing-keyword fallback.
 * @author Alex (<https://github.com/lextpf>)
 *
 * ActorTypeNPC (Skyrim.esm 0x13794) resolves lazily after data loading.
 * Probe instance, base, then race keywords. If the keyword is unavailable, warn
 * once and use teammate/talkable status; otherwise unmatched actors are creatures.
 */
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

    return actor->IsPlayerTeammate() || actor->CanTalkToPlayer();
}

/**
 * @fn static LevelDelta ClassifyDelta( int actorLv, int playerLv, int weakAtOrBelow, int
 *     strongAtOrAbove, int deadlyAtOrAbove)
 * @brief Resolve level difference against the configured threat thresholds.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static CreatureKind ClassifyCreature(RE::Actor* actor)
 * @brief Select the first matching creature keyword or the NPC fallback.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Probe specific creature keywords before generic ActorTypeCreature.
 */
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

/**
 * @fn static ProtectionKind ClassifyProtection(RE::Actor* actor)
 * @brief Resolve essential and protected flags with essential precedence.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static RoleKind ClassifyRole(RE::Actor* actor)
 * @brief Resolve guard or merchant status for the NPC role slot.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Guard wins over merchant; vendor membership requires rank >= 0.
 */
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
                return true;  // Stop visiting
            }
            return false;
        });
    return vendor ? RoleKind::Merchant : RoleKind::Commoner;
}

/**
 * @fn static EngagementKind ClassifyEngagement(RE::Actor* actor, RE::Actor* player)
 * @brief Resolve combat, alert, or idle state for a retained actor.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Alert approximates awareness as weapon drawn while detecting the player.
 */
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
    // IsWeaponDrawn is exposed through ActorState in NG.
    if (player && actor->AsActorState()->IsWeaponDrawn() &&
        actor->RequestDetectionLevel(player) > 0)
    {
        return EngagementKind::Alert;
    }
    return EngagementKind::Idle;
}

/**
 * @fn static bool PlayerHasBounty()
 * @brief Check whether any crime faction records an unpaid player bounty.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Current bounty excludes infamy.
 */
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

/**
 * @struct HonorificRuntime
 * @brief Game-thread faction pointers and periodically refreshed actor matches.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Factions parallel Settings::Honorifics and rebuild on generation changes.
 * Actor matches refresh periodically because faction ranks can change.
 */
struct HonorificRuntime
{
    uint32_t settingsGen = 0;               // Settings generation of `factions`
    std::vector<RE::TESFaction*> factions;  // Parallel to Settings::Honorifics()

    struct CacheEntry
    {
        uint32_t lastCheckFrame = 0;  // Snapshot frame of the last match
        int index = -1;               // Matched honorific index (-1 = none)
    };
    std::unordered_map<uint32_t, CacheEntry> perActor;  // Keyed by formID
};

/**
 * @fn static HonorificRuntime& GetHonorificRuntime()
 * @brief Access game-thread faction resolutions and per-actor honorific matches.
 * @author Alex (<https://github.com/lextpf>)
 */
static HonorificRuntime& GetHonorificRuntime()
{
    static HonorificRuntime instance;
    return instance;
}

/**
 * @fn static RE::TESFaction* ResolveFactionSpec(const std::string& spec)
 * @brief Resolve a hexadecimal faction identifier against the plugin load order.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Parse "0xFORMID" or "0xFORMID@plugin.esp" (plugin defaults to Skyrim.esm)
 * and resolve the faction against the load order.
 */
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

/**
 * @fn static void RefreshHonorificRuntime()
 * @brief Rebuild faction pointers and discard matches after a settings change.
 * @author Alex (<https://github.com/lextpf>)
 */
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

// Snapshot updates between refreshes.
static constexpr uint32_t HONORIFIC_REFRESH_FRAMES = 90;

/**
 * @fn static std::string ResolveHonorific(RE::Actor* actor, bool isPlayer, uint32_t snapshotFrame)
 * @brief Resolve and cache the highest-priority matching faction title.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Highest priority wins; ties follow VisitFactions order, then the lowest
 * Honorifics index within that faction. lastCheckFrame == 0 means unchecked.
 */
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
                return false;  // Keep visiting
            });
    }

    return entry.index >= 0 ? defs[static_cast<size_t>(entry.index)].title : std::string{};
}

/**
 * @fn static uint32_t ComputeContextMask(RE::Actor* player, int visiblePlateCount)
 * @brief Derive register conditions from the player and visible plate count.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Game-thread predicates: Night before 6/after 20, City from LocTypeCity/LocTypeTown,
 * Dialogue from MenuTopicManager. Only CrowdedThreshold is configurable.
 */
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

/**
 * @fn static int PickRegister(uint32_t ctxMask)
 * @brief Select the highest-priority register whose required conditions match.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Highest priority wins, then lowest index. Empty when matches every scene;
 * unconfigured gap entries must not shadow configured registers.
 */
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

/**
 * @fn static void UpdateOcclusionForActor(ActorDrawData& d, RE::Actor* a, RE::Actor* player,
 *     uint32_t snapshotFrame, uint32_t checkInterval)
 * @brief Refresh an actor visibility result only when its check interval expires.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Interval counts snapshot updates, not render frames. Frame zero means unchecked;
 * clearing the cache resets its counter. Prune unseen actors after the pass.
 */
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

    d.isOccluded = Occlusion::IsActorOccluded(a, player, d.worldPos, Settings::Occlusion().Enabled);
    entry.lastCheckFrame = snapshotFrame;
    entry.cachedOccluded = d.isOccluded;
}

/**
 * @fn static double PoseClockSeconds()
 * @brief Read steady-clock seconds shared by pose capture and prediction.
 * @author Alex (<https://github.com/lextpf>)
 */
static double PoseClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/**
 * @fn static RE::NiPoint3 ComputeAnchorPosition(RE::Actor* a, const RE::NiPoint3& feet, bool
 *     useRenderedHeadXY)
 * @brief Resolve a plausible head anchor or fall back to actor height.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Player anchors use all rendered-head axes to avoid simulation-root stepping;
 * NPCs retain root X/Y. Reject non-finite heads, heads at/below feet, >= 512 world
 * units above feet, or >= 256 units laterally. Invalid 3D uses the height fallback.
 *
 *   Head node found and plausible        no 3D, or head rejected
 *   ------------------------------       -----------------------
 *   z = head.z + 12 + VerticalOffset     z = feet.z + height + VerticalOffset
 *   x/y = head x/y  (player)             x/y = feet x/y
 *   x/y = feet x/y  (NPC)
 *
 * Invalid height (non-finite or <= 1) becomes 128 world units. 12 units clear the
 * crown. VerticalOffset is read from live Settings.
 */
static RE::NiPoint3 ComputeAnchorPosition(RE::Actor* a,
                                          const RE::NiPoint3& feet,
                                          bool useRenderedHeadXY)
{
    if (auto* root = a->Get3D())
    {
        if (auto* head = root->GetObjectByName("NPC Head [Head]"))
        {
            RE::NiPoint3 anchor = head->world.translate;
            const float dx = anchor.x - feet.x;
            const float dy = anchor.y - feet.y;
            const float dz = anchor.z - feet.z;
            constexpr float kMaxHeadOffset = 256.0f;
            if (std::isfinite(anchor.x) && std::isfinite(anchor.y) && std::isfinite(anchor.z) &&
                dz > .0f && dz < 512.0f && dx * dx + dy * dy < kMaxHeadOffset * kMaxHeadOffset)
            {
                if (!useRenderedHeadXY)
                {
                    anchor.x = feet.x;
                    anchor.y = feet.y;
                }
                constexpr float kHeadHeadroom = 12.0f;  // Node sits at head base; clear the crown
                anchor.z += kHeadHeadroom + Settings::Display().VerticalOffset;
                return anchor;
            }
        }
    }

    RE::NiPoint3 anchor = feet;
    const float rawHeight = a->GetHeight();
    const float height = std::isfinite(rawHeight) && rawHeight > 1.0f ? rawHeight : 128.0f;
    anchor.z += height + Settings::Display().VerticalOffset;
    return anchor;
}

/**
 * @fn static void CaptureActorPose(RE::Actor* actor, ActorDrawData& out, bool useRenderedHeadXY =
 *     false)
 * @brief Capture feet and head anchors with a shared steady-clock timestamp.
 * @author Alex (<https://github.com/lextpf>)
 */
static void CaptureActorPose(RE::Actor* actor, ActorDrawData& out, bool useRenderedHeadXY = false)
{
    out.feetPos = actor->GetPosition();
    out.worldPos = ComputeAnchorPosition(actor, out.feetPos, useRenderedHeadXY);
    out.poseSampleTime = PoseClockSeconds();
}

/**
 * @fn static void CaptureActorRaycastBounds(RE::Actor* actor, RE::NiPoint3& outCenter, float&
 *     outRadius)
 * @brief Capture a targeting sphere with a height fallback for unavailable 3D.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Capture plain targeting bounds while game-thread 3D access is safe.
 * Height-derived bounds keep unloaded/invalid roots targetable.
 */
static void CaptureActorRaycastBounds(RE::Actor* actor, RE::NiPoint3& outCenter, float& outRadius)
{
    constexpr float MIN_BOUND_RADIUS = 4.0f;
    constexpr float MAX_BOUND_RADIUS = 4096.0f;
    if (actor)
    {
        if (auto* root = actor->Get3D())
        {
            const auto& bound = root->worldBound;
            if (std::isfinite(bound.center.x) && std::isfinite(bound.center.y) &&
                std::isfinite(bound.center.z) && std::isfinite(bound.radius) &&
                bound.radius >= MIN_BOUND_RADIUS && bound.radius <= MAX_BOUND_RADIUS)
            {
                outCenter = bound.center;
                outRadius = bound.radius;
                return;
            }
        }

        const float rawHeight = actor->GetHeight();
        const float height = std::isfinite(rawHeight) && rawHeight > 1.0f ? rawHeight : 128.0f;
        outCenter = actor->GetPosition();
        outCenter.z += height * .5f;
        outRadius = std::clamp(height * .5f, 18.0f, MAX_BOUND_RADIUS);
        return;
    }

    outCenter = {};
    outRadius = .0f;
}

// Expensive actor derivation follows retention:
//
//   gates      pauseSnapshotUpdates -> clear both gates, snapshot, target, return
//              neither overlay nor Deck allowed -> clear snapshot and target, return
//              no player or no ProcessLists -> clear allowDeck, snapshot, target, return
//                |
//   player     self-plate DTO: name, pose, sneak/combat/encumbered/bounty, honorific
//                |
//   cheap pass  every highActorHandles entry, capped by MaxScanActors. Applies the
//              corpse, HideCreatures and MaxScanDistance gates and captures the
//              targeting sphere. No name, relationship, badge or occlusion work.
//                |
//   retain     inject the Deck crosshair target if the plate filters dropped it,
//              pick the Graffito camera-ray target, then partial_sort the keep set:
//              camera-ray target first, Deck target second, nearest first after that
//                |
//   full pass  per kept actor: name, relationship, level delta, creature kind, role,
//              protection, engagement, honorific, HUD-compat yield, occlusion. A
//              corpse stops after the bare marker fields.
//                |
//   publish    prune the occlusion and honorific caches to the actors just seen,
//              pick the active register, re-sample the player pose last, then swap
//              snapshot and crosshairTarget under snapshotLock
void UpdateSnapshot_GameThread()
{
    // Clear in-flight flags on every exit.
    struct UpdateScope
    {
        /**
         * @fn UpdateScope()
         * @brief Mark the snapshot task active before checking its gates.
         * @author Alex (<https://github.com/lextpf>)
         */
        UpdateScope() { GetState().snapshotUpdateRunning.store(true, std::memory_order_release); }

        /**
         * @fn ~UpdateScope()
         * @brief Clear running and queued markers on every task exit.
         * @author Alex (<https://github.com/lextpf>)
         */
        ~UpdateScope()
        {
            GetState().snapshotUpdateRunning.store(false, std::memory_order_release);
            GetState().updateQueued.store(false, std::memory_order_release);
        }
    } _;

    if (GetState().pauseSnapshotUpdates.load(std::memory_order_acquire))
    {
        GetState().allowOverlay.store(false, std::memory_order_release);
        GetState().allowDeck.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        GetState().crosshairTarget = 0;
        return;
    }

    if (GetState().clearOcclusionCacheRequested.exchange(false, std::memory_order_acq_rel))
    {
        GetOcclusionCache().clear();
        GetSnapshotState().frame = 0;
    }

    // Deck can capture during combat even when ambient plates are hidden.
    bool deckEnabled = false;
    {
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        deckEnabled = Settings::Deck().Enabled;
    }
    const bool allow = GameState::CanDrawOverlay();
    const bool allowDeck = deckEnabled && GameState::CanCaptureDeck();
    GetState().allowOverlay.store(allow, std::memory_order_release);
    GetState().allowDeck.store(allowDeck, std::memory_order_release);

    if (!allow && !allowDeck)
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        GetState().crosshairTarget = 0;
        return;
    }

    auto* player = GetPlayer();
    auto* pl = RE::ProcessLists::GetSingleton();
    if (!player || !pl)
    {
        GetState().allowDeck.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot.clear();
        GetState().crosshairTarget = 0;
        return;
    }

    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
    RefreshHonorificRuntime();
    const auto actorLimits = RenderConstants::ClampActorLimits(Settings::Display().MaxPlates,
                                                               Settings::Display().MaxScanActors);
    const int maxPlates = actorLimits.maxPlates;
    const int maxScanActors = actorLimits.maxScanActors;
    const float kMaxDistSq =
        Settings::Distance().MaxScanDistance * Settings::Distance().MaxScanDistance;
    const uint32_t checkInterval =
        static_cast<uint32_t>(std::max(1, Settings::Occlusion().CheckInterval));
    const uint32_t snapshotFrame = ++GetSnapshotState().frame;

    const auto& labelSettings = Settings::Labels();
    const int playerLevel = static_cast<int>(player->GetLevel());
    const int weakBelow = labelSettings.WeakAtOrBelow;
    const int strongAbove = labelSettings.StrongAtOrAbove;
    const int deadlyAbove = labelSettings.DeadlyAtOrAbove;

    std::vector<ActorDrawData> tempBuf;
    tempBuf.reserve(static_cast<size_t>(maxPlates) + 2);
    std::unordered_set<uint32_t> seenFormIDs;
    seenFormIDs.reserve(static_cast<size_t>(maxPlates) + 2);
    struct NearActor
    {
        RE::Actor* actor{nullptr};
        float distSq{.0f};
        bool dead{false};
        bool plateEligible{true};
        RE::NiPoint3 raycastCenter{};
        float raycastRadius{.0f};
    };
    std::vector<NearActor> nearActors;
    nearActors.reserve(static_cast<size_t>(maxScanActors) + 1);

    const auto playerPos = player->GetPosition();

    const bool yieldToTrueHUD = Settings::Compat().YieldToTrueHUD && HudCompat::HasTrueHUD();
    const bool yieldToMoreHUD = Settings::Compat().YieldLevelToMoreHUD && HudCompat::HasMoreHUD();
    const bool graffitoEnabled = Settings::Graffito().Enabled;
    const bool playerPlateVisible = !Settings::Display().HidePlayer || graffitoEnabled;
    const uint32_t crosshairID =
        (yieldToMoreHUD || deckEnabled) ? HudCompat::CrosshairTargetFormID() : 0;

    // Graffito retains its self-plate; Deck may retain a private player record.
    if (playerPlateVisible || deckEnabled)
    {
        ActorDrawData d;
        d.formID = player->GetFormID();
        d.level = player->GetLevel();
        // RaceMenu updates the base name immediately; ExtraTextDisplayData may stay stale.
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
        CaptureActorPose(player, d, true);
        CaptureActorRaycastBounds(player, d.raycastCenter, d.raycastRadius);
        d.headingRadians = player->GetAngleZ();
        d.distToPlayer = .0f;
        d.isPlayer = true;
        d.isUnique = true;
        // Self-relationship defaults feed text tokens; player badges use separate facts.
        d.relationship = RelationshipKind::Follower;
        d.levelDelta = LevelDelta::Even;
        d.creatureKind = CreatureKind::NPC;

        // Scan detection only while sneaking.
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
        d.overrides = ActorOverrides::Get(d.formID);

        tempBuf.push_back(std::move(d));
        seenFormIDs.insert(player->GetFormID());
    }

    const int added = playerPlateVisible ? static_cast<int>(tempBuf.size()) : 0;

    // Scan proximity before expensive facts; list order must not choose visible
    // actors. MaxScanActors is only a runaway guard above normal high-process counts.
    int scanned = 0;
    for (auto& h : pl->highActorHandles)
    {
        if (++scanned > maxScanActors)
        {
            break;
        }
        auto aSP = h.get();
        auto* a = aSP.get();
        if (!a || a == player)
        {
            continue;
        }

        // Publish corpses as markers; the render thread replays cached live facts.
        const bool dead = a->IsDead();
        if (dead && !Settings::DeathRite().Enabled)
        {
            continue;
        }

        if (Settings::Display().HideCreatures && !IsHumanoidNPC(a))
        {
            continue;
        }

        const float distSq = playerPos.GetSquaredDistance(a->GetPosition());
        if (distSq > kMaxDistSq)
        {
            continue;
        }

        NearActor candidate{a, distSq, dead};
        CaptureActorRaycastBounds(a, candidate.raycastCenter, candidate.raycastRadius);
        nearActors.push_back(candidate);
    }

    // Retain excluded Deck targets privately without changing ambient plate selection.
    bool targetInjectedForDeck = false;
    if (deckEnabled && crosshairID != 0 && crosshairID != player->GetFormID())
    {
        const bool targetAlreadyPresent =
            std::any_of(nearActors.begin(),
                        nearActors.end(),
                        [crosshairID](const NearActor& candidate)
                        {
                            return !candidate.dead && candidate.actor &&
                                   candidate.actor->GetFormID() == crosshairID;
                        });
        if (!targetAlreadyPresent)
        {
            if (auto* target = RE::TESForm::LookupByID<RE::Actor>(crosshairID);
                target && !target->IsDead())
            {
                const float distSq = playerPos.GetSquaredDistance(target->GetPosition());
                NearActor candidate{target, distSq, false, false};
                CaptureActorRaycastBounds(target, candidate.raycastCenter, candidate.raycastRadius);
                nearActors.push_back(candidate);
                targetInjectedForDeck = true;
            }
        }
    }

    // Retain the camera-ray target within the ordinary plate budget, independent
    // of CrosshairPickData.
    uint32_t cameraRayID = 0;
    if (graffitoEnabled)
    {
        RE::NiPoint3 cameraPosition{};
        RE::NiPoint3 cameraForward{};
        if (Occlusion::GetCameraInfo(cameraPosition, cameraForward))
        {
            const auto toMath = [](const RE::NiPoint3& point)
            {
                return Graffito::Math::Vec3{static_cast<double>(point.x),
                                            static_cast<double>(point.y),
                                            static_cast<double>(point.z)};
            };
            const float maxDistance = Settings::Graffito().MaxDistance;
            const float maxDistanceSq = maxDistance * maxDistance;
            double bestHit = std::numeric_limits<double>::infinity();
            for (const auto& candidate : nearActors)
            {
                if (!candidate.actor || candidate.dead || !candidate.plateEligible ||
                    candidate.raycastRadius <= .0f ||
                    (maxDistance > .0f && candidate.distSq > maxDistanceSq))
                {
                    continue;
                }

                const double hit = Graffito::Math::RaySphereHitDistance(
                    toMath(cameraPosition),
                    toMath(cameraForward),
                    toMath(candidate.raycastCenter),
                    static_cast<double>(candidate.raycastRadius));
                const uint32_t formID = candidate.actor->GetFormID();
                if (hit < bestHit || (hit == bestHit && formID < cameraRayID))
                {
                    bestHit = hit;
                    cameraRayID = formID;
                }
            }
        }
    }

    // A Deck crosshair target outside the retained set gets an extra private slot;
    // it must not displace an ambient plate.
    const int remainingSlots = std::max(0, maxPlates - added);
    bool targetNeedsExtraSlot = targetInjectedForDeck;
    if (deckEnabled && crosshairID != 0 && !targetInjectedForDeck)
    {
        const auto targetIt = std::find_if(nearActors.begin(),
                                           nearActors.end(),
                                           [crosshairID](const NearActor& candidate)
                                           {
                                               return !candidate.dead && candidate.actor &&
                                                      candidate.actor->GetFormID() == crosshairID;
                                           });
        if (targetIt != nearActors.end())
        {
            const auto closerCount = std::count_if(nearActors.begin(),
                                                   nearActors.end(),
                                                   [targetIt](const NearActor& candidate)
                                                   { return candidate.distSq < targetIt->distSq; });
            bool cameraRayDisplacesTarget = false;
            if (cameraRayID != 0 && cameraRayID != crosshairID)
            {
                const auto cameraRayIt = std::find_if(
                    nearActors.begin(),
                    nearActors.end(),
                    [cameraRayID](const NearActor& candidate)
                    { return candidate.actor && candidate.actor->GetFormID() == cameraRayID; });
                cameraRayDisplacesTarget =
                    cameraRayIt != nearActors.end() && cameraRayIt->distSq >= targetIt->distSq;
            }
            targetNeedsExtraSlot =
                closerCount + (cameraRayDisplacesTarget ? 1 : 0) >= remainingSlots;
        }
    }
    const int slotsToKeep = remainingSlots + (targetNeedsExtraSlot ? 1 : 0);
    const size_t keep = std::min(static_cast<size_t>(slotsToKeep), nearActors.size());
    std::partial_sort(
        nearActors.begin(),
        nearActors.begin() + static_cast<std::ptrdiff_t>(keep),
        nearActors.end(),
        [cameraRayID, crosshairID, targetNeedsExtraSlot](const NearActor& lhs, const NearActor& rhs)
        {
            const bool lhsCameraRay =
                cameraRayID != 0 && lhs.actor && lhs.actor->GetFormID() == cameraRayID;
            const bool rhsCameraRay =
                cameraRayID != 0 && rhs.actor && rhs.actor->GetFormID() == cameraRayID;
            if (lhsCameraRay != rhsCameraRay)
            {
                return lhsCameraRay;
            }
            if (targetNeedsExtraSlot)
            {
                const bool lhsTarget =
                    !lhs.dead && lhs.actor && lhs.actor->GetFormID() == crosshairID;
                const bool rhsTarget =
                    !rhs.dead && rhs.actor && rhs.actor->GetFormID() == crosshairID;
                if (lhsTarget != rhsTarget)
                {
                    return lhsTarget;
                }
            }
            return lhs.distSq < rhs.distSq;
        });

    for (size_t i = 0; i < keep; ++i)
    {
        RE::Actor* a = nearActors[i].actor;
        const bool dead = nearActors[i].dead;

        ActorDrawData d;
        d.formID = a->GetFormID();
        d.level = a->GetLevel();
        const char* rawName = a->GetDisplayFullName();
        d.name = rawName ? Capitalize(rawName) : "";
        CaptureActorPose(a, d);
        d.raycastCenter = nearActors[i].raycastCenter;
        d.raycastRadius = nearActors[i].raycastRadius;
        d.headingRadians = a->GetAngleZ();
        d.distToPlayer = std::sqrt(nearActors[i].distSq);
        d.isPlayer = false;
        d.deckOnly = targetNeedsExtraSlot && d.formID == crosshairID && d.formID != cameraRayID;
        if (const auto* actorBase = a->GetActorBase())
        {
            d.isUnique = actorBase->IsUnique();
        }
        d.isDead = dead;
        if (dead)
        {
            // Corpse context stays at defaults; death rendering uses the last live record.
            seenFormIDs.insert(d.formID);
            tempBuf.push_back(std::move(d));
            continue;
        }

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

        d.protection = ClassifyProtection(a);
        d.role = ClassifyRole(a);
        d.engagement = ClassifyEngagement(a, player);

        d.honorific = ResolveHonorific(a, false, snapshotFrame);
        d.overrides = ActorOverrides::Get(d.formID);

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

    // Crowded counts visible plates only; subtract Deck-only records and a private player.
    int activeRegister = -1;
    if (Settings::RegisterConfig().Enabled && !Settings::Registers().empty())
    {
        const int privateDeckActors = static_cast<int>(std::count_if(
            tempBuf.begin(), tempBuf.end(), [](const ActorDrawData& d) { return d.deckOnly; }));
        const int visiblePlateCount =
            static_cast<int>(tempBuf.size()) - privateDeckActors -
            ((deckEnabled && !playerPlateVisible && !tempBuf.empty()) ? 1 : 0);
        activeRegister = PickRegister(ComputeContextMask(player, visiblePlateCount));
    }
    GetState().activeRegister.store(activeRegister, std::memory_order_release);

    // Resample the player last so NPC queries do not add self-plate motion latency.
    if (auto playerData = std::find_if(tempBuf.begin(),
                                       tempBuf.end(),
                                       [](const ActorDrawData& actor) { return actor.isPlayer; });
        playerData != tempBuf.end())
    {
        CaptureActorPose(player, *playerData, true);
    }

    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        GetState().snapshot = std::move(tempBuf);
        GetState().crosshairTarget = crosshairID;
    }
}

void QueueSnapshotUpdate_RenderThread()
{
    if (GetState().pauseSnapshotUpdates.load(std::memory_order_acquire))
    {
        return;
    }

    if (GetState().updateQueued.exchange(true))
    {
        return;
    }

    if (auto* task = SKSE::GetTaskInterface())
    {
        task->AddTask([]() { UpdateSnapshot_GameThread(); });
    }
    else
    {
        GetState().updateQueued.store(false);
    }
}

}  // namespace Renderer
