#include "Renderer.h"

#include "AppearanceTemplate.h"
#include "DebugOverlay.h"
#include "GameState.h"
#include "Occlusion.h"
#include "ParticleTextures.h"
#include "RenderConstants.h"
#include "Settings.h"
#include "TextEffects.h"
#include "Utf8Utils.h"

#include <SKSE/SKSE.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Renderer
{
using Utf8Utils::Utf8CharCount;
using Utf8Utils::Utf8Next;
using Utf8Utils::Utf8Truncate;

// Calculate tight vertical bounds of text glyphs
static void CalcTightYBoundsFromTop(
    ImFont* font, float fontSize, const char* text, float& outTop, float& outBottom)
{
    // Initialize to extremes so first glyph will replace them
    outTop = +FLT_MAX;
    outBottom = -FLT_MAX;

    if (!font || !text || !*text)
    {
        outTop = .0f;
        outBottom = .0f;
        return;
    }

    // Calculate scale factor from font's native size to desired size
    const float scale = fontSize / font->FontSize;

    // Iterate through each character in the UTF-8 string
    for (const char* p = text; *p;)
    {
        unsigned int cp;
        p = Utf8Next(p, cp);

        // Skip newlines (shouldn't be in our text, but be safe)
        if (cp == '\n' || cp == '\r')
        {
            continue;
        }

        // Get glyph data for this character
        const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
        if (!g)
        {
            continue;  // Character not in font
        }

        // Y0 and Y1 are relative to the baseline
        // Y0 is negative (above baseline), Y1 is positive (below baseline)
        // We want the tightest bounds, so min of Y0 (most negative), max of Y1 (most positive)
        outTop = std::min(outTop, g->Y0 * scale);
        outBottom = std::max(outBottom, g->Y1 * scale);
    }

    // If no valid glyphs were found, return zeros
    if (outTop == +FLT_MAX)
    {
        outTop = .0f;
        outBottom = .0f;
    }
}

// Cache entry for smooth actor nameplate animations.
// Stores smoothed values and state for position, alpha, and effects.
struct ActorCache
{
    ImVec2 smooth{};               // Smoothed screen position (result of moving average)
    float alphaSmooth = 1.0f;      // Smoothed alpha for fade transitions
    float textSizeScale = 1.0f;    // Smoothed font scale for distance-based sizing
    float occlusionSmooth = 1.0f;  // Smoothed occlusion (1.0=visible, 0.0=hidden)

    bool initialized = false;    // True after first frame of data
    uint32_t lastSeenFrame = 0;  // Frame counter when actor was last in snapshot

    uint32_t lastOcclusionCheckFrame = 0;  // Frame when LOS was last checked
    bool cachedOccluded = false;           // Cached LOS result
    bool wasOccluded = false;              // Previous frame's occlusion state

    static constexpr int HISTORY_SIZE = RenderConstants::POSITION_HISTORY_SIZE;
    ImVec2 posHistory[HISTORY_SIZE]{};
    int historyIndex = 0;
    bool historyFilled = false;

    float typewriterTime = .0f;       // Seconds since actor first appeared
    bool typewriterComplete = false;  // True when reveal animation finished

    std::string cachedName;  // Last known name (to detect changes)

    // Add position sample to history and return smoothed average.
    ImVec2 AddAndGetSmoothed(const ImVec2& pos)
    {
        posHistory[historyIndex] = pos;
        historyIndex = (historyIndex + 1) % HISTORY_SIZE;
        if (historyIndex == 0)
        {
            historyFilled = true;
        }

        int count = historyFilled ? HISTORY_SIZE : historyIndex;
        if (count == 0)
        {
            return pos;
        }

        ImVec2 sum{0, 0};
        for (int i = 0; i < count; i++)
        {
            sum.x += posHistory[i].x;
            sum.y += posHistory[i].y;
        }
        return ImVec2(sum.x / count, sum.y / count);
    }
};

// Actor disposition
enum class Disposition : std::uint8_t
{
    Neutral,      // Neutral NPCs (white/gray)
    Enemy,        // Hostile NPCs (red)
    AllyOrFriend  // Friendly/allied NPCs (blue)
};

// Data for rendering a single actor's nameplate
struct ActorDrawData
{
    uint32_t formID{0};                       // Actor's form ID (unique identifier)
    RE::NiPoint3 worldPos{};                  // World position above actor's head
    std::string name;                         // Display name (capitalized)
    uint16_t level{0};                        // Actor's level
    float distToPlayer{.0f};                  // Distance to player in units
    Disposition dispo{Disposition::Neutral};  // Disposition towards player
    bool isPlayer{false};                     // Whether this is the player character
    bool isOccluded{false};                   // Whether actor is occluded from view
};

struct OcclusionCacheEntry
{
    uint32_t lastCheckFrame{0};
    bool cachedOccluded{false};
};

// Encapsulates all mutable renderer state into a single struct.
struct RendererState
{
    // Cache
    std::unordered_map<uint32_t, ActorCache> cache;
    uint32_t frame = 0;

    // Snapshot & Thread Safety
    std::vector<ActorDrawData> snapshot;
    std::mutex snapshotLock;
    std::atomic<bool> updateQueued{false};
    std::atomic<bool> pauseSnapshotUpdates{false};
    std::atomic<bool> snapshotUpdateRunning{false};
    std::atomic<bool> clearOcclusionCacheRequested{false};

    // Frame State
    bool wasInInvalidState = true;
    int postLoadCooldown = 0;

    // Debug Stats
    DebugOverlay::Stats debugStats;
    float lastDebugUpdateTime = .0f;
    int updateCounter = 0;
    int lastUpdateCount = 0;

    // Hot Reload
    bool reloadKeyWasDown = false;
    float lastReloadTime = -10.0f;
    std::atomic<bool> reloadRequested{false};

    // Overlay Flags
    std::atomic<bool> allowOverlay{false};
    std::atomic<bool> manualEnabled{true};
};

static RendererState& GetState()
{
    static RendererState instance;
    return instance;
}

static std::unordered_map<uint32_t, OcclusionCacheEntry>& GetOcclusionCache()
{
    static std::unordered_map<uint32_t, OcclusionCacheEntry> instance;
    return instance;
}
static uint32_t s_snapshotFrame = 0;

// Cached ActorTypeNPC keyword for creature filtering (game-thread only).
static RE::BGSKeyword* s_npcKeyword = nullptr;
static bool s_npcKeywordLookupAttempted = false;
static bool s_npcKeywordMissingLogged = false;

static constexpr float RELOAD_NOTIFICATION_DURATION = RenderConstants::RELOAD_NOTIFICATION_DURATION;

// Per-frame overlap Y offsets, keyed by form ID
static std::unordered_map<uint32_t, float>& OverlapOffsets()
{
    static std::unordered_map<uint32_t, float> offsets;
    return offsets;
}

bool IsOverlayAllowedRT()
{
    return GetState().manualEnabled.load(std::memory_order_acquire) &&
           GetState().allowOverlay.load(std::memory_order_acquire);
}

bool ToggleEnabled()
{
    bool expected = GetState().manualEnabled.load(std::memory_order_relaxed);
    while (!GetState().manualEnabled.compare_exchange_weak(
        expected, !expected, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
    }
    return !expected;
}

static ImFont* GetFontAt(int index)
{
    auto& io = ImGui::GetIO();
    if (!io.Fonts || io.Fonts->Fonts.Size <= 0)
    {
        return nullptr;
    }
    if (index >= 0 && index < io.Fonts->Fonts.Size)
    {
        if (auto* font = io.Fonts->Fonts[index])
        {
            return font;
        }
    }
    return io.Fonts->Fonts[0];
}

// Get player character
static RE::Actor* GetPlayer()
{
    return RE::PlayerCharacter::GetSingleton();
}

// Capitalize text and trim whitespace (UTF-8-aware)
static std::string Capitalize(const char* text)
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

// Project world position to screen coordinates
bool WorldToScreen(const RE::NiPoint3& worldPos,
                   RE::NiPoint3& screenPos,
                   RE::NiPoint3* cameraPosOut = nullptr)
{
    // Get the main world camera
    auto* cam = RE::Main::WorldRootCamera();
    if (!cam)
    {
        return false;
    }

    // Get camera runtime data containing transformation matrices
    const auto& rt = cam->GetRuntimeData();    // Contains worldToCam matrix
    const auto& rt2 = cam->GetRuntimeData2();  // Contains viewport info

    // Optionally output camera position (used for distance calculations elsewhere)
    if (cameraPosOut)
    {
        *cameraPosOut = cam->world.translate;
    }

    // Project world coordinates to normalized screen coordinates [0,1]
    float x = .0f, y = .0f, z = .0f;

    // WorldPtToScreenPt3 returns false if point is behind camera or outside frustum
    // The last parameter (1e-5f) is the epsilon for near-plane clipping
    if (!RE::NiCamera::WorldPtToScreenPt3(rt.worldToCam, rt2.port, worldPos, x, y, z, 1e-5f))
    {
        return false;  // Point not visible (behind camera or clipped)
    }

    // Get actual screen resolution
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return false;
    }
    const auto ss = renderer->GetScreenSize();
    const float w = static_cast<float>(ss.width);
    const float h = static_cast<float>(ss.height);

    // Convert normalized coordinates [0,1] to pixel coordinates
    screenPos.x = x * w;           // Left to right: 0 to width
    screenPos.y = (1.0f - y) * h;  // Flip Y: top to bottom (screen space is inverted)
    screenPos.z = z;               // Depth value for Z-testing
    return true;
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
    d.isOccluded =
        Occlusion::IsActorOccluded(a, player, d.worldPos, Settings::EnableOcclusionCulling);
    entry.lastCheckFrame = snapshotFrame;
    entry.cachedOccluded = d.isOccluded;
}

static void UpdateSnapshot_GameThread()
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
        s_snapshotFrame = 0;
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
    const float kMaxDistSq = Settings::MaxScanDistance * Settings::MaxScanDistance;
    const uint32_t checkInterval =
        static_cast<uint32_t>(std::max(1, Settings::OcclusionCheckInterval));
    const uint32_t snapshotFrame = ++s_snapshotFrame;

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
    if (!Settings::HidePlayer)
    {
        ActorDrawData d;
        d.formID = player->GetFormID();
        d.level = player->GetLevel();
        const char* rawName = player->GetDisplayFullName();
        d.name = rawName ? Capitalize(rawName) : "Player";
        d.worldPos = playerPos;
        d.worldPos.z += player->GetHeight() + Settings::VerticalOffset;
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
        // Prefer ActorTypeNPC on actor, then base, then race for robustness across mods.
        if (Settings::HideCreatures)
        {
            if (!s_npcKeywordLookupAttempted)
            {
                s_npcKeywordLookupAttempted = true;
                if (auto* dataHandler = RE::TESDataHandler::GetSingleton(); dataHandler)
                {
                    s_npcKeyword = dataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
                }
                if (!s_npcKeyword && !s_npcKeywordMissingLogged)
                {
                    s_npcKeywordMissingLogged = true;
                    logger::warn(
                        "Renderer: ActorTypeNPC keyword lookup failed, using creature filter "
                        "fallback heuristic");
                }
            }

            bool isHumanoidNPC = false;
            if (s_npcKeyword)
            {
                isHumanoidNPC = a->HasKeyword(s_npcKeyword);
                if (!isHumanoidNPC)
                {
                    if (auto* actorBase = a->GetActorBase(); actorBase)
                    {
                        isHumanoidNPC = actorBase->HasKeyword(s_npcKeyword);
                        if (!isHumanoidNPC)
                        {
                            if (auto* race = actorBase->GetRace(); race)
                            {
                                isHumanoidNPC = race->HasKeyword(s_npcKeyword);
                            }
                        }
                    }
                }
            }
            else
            {
                // Conservative fallback if ActorTypeNPC keyword is unavailable.
                isHumanoidNPC = a->IsPlayerTeammate() || a->CanTalkToPlayer();
            }

            if (!isHumanoidNPC)
            {
                continue;
            }
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
        d.worldPos.z += a->GetHeight() + Settings::VerticalOffset;
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
        if (Settings::EnableOcclusionCulling)
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

static void QueueSnapshotUpdate_RenderThread()
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

static void PruneCacheToSnapshot(const std::vector<ActorDrawData>& snap)
{
    // Grace period prevents jitter when actors briefly leave the snapshot
    constexpr uint32_t CACHE_GRACE_FRAMES = RenderConstants::CACHE_GRACE_FRAMES;
    std::unordered_set<uint32_t> visibleFormIDs;
    visibleFormIDs.reserve(snap.size());
    for (const auto& d : snap)
    {
        visibleFormIDs.insert(d.formID);
    }

    for (auto it = GetState().cache.begin(); it != GetState().cache.end();)
    {
        const bool inSnapshot = visibleFormIDs.find(it->first) != visibleFormIDs.end();
        if (inSnapshot)
        {
            it->second.lastSeenFrame = GetState().frame;  // Update last seen
        }

        if (!inSnapshot)
        {
            // Check if grace period has expired
            uint32_t framesSinceLastSeen = GetState().frame - it->second.lastSeenFrame;
            if (framesSinceLastSeen > CACHE_GRACE_FRAMES)
            {
                it = GetState().cache.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// Compute blend factor for frame-rate independent exponential smoothing.
// Returns alpha in [0,1] for use with: current = lerp(current, target, alpha)
static float ExpApproachAlpha(float dt, float settleTime, float epsilon = .01f)
{
    dt = std::max(.0f, dt);
    settleTime = std::max(1e-5f, settleTime);
    return std::clamp(1.0f - std::pow(epsilon, dt / settleTime), .0f, 1.0f);
}

// Use effect parameter if positive, otherwise return fallback default.
static constexpr float ParamOr(float p, float fallback)
{
    return p > .0f ? p : fallback;
}

static void ApplyTextEffect(ImDrawList* drawList,
                            ImFont* font,
                            float fontSize,
                            ImVec2 pos,
                            const char* text,
                            const Settings::EffectParams& effect,
                            ImU32 colL,
                            ImU32 colR,
                            ImU32 highlight,
                            ImU32 outlineColor,
                            float outlineWidth,
                            float phase01,
                            float strength,
                            float textSizeScale,
                            float alpha)
{
    switch (effect.type)
    {
        case Settings::EffectType::None:
            TextEffects::AddTextOutline4(
                drawList, font, fontSize, pos, text, colL, outlineColor, outlineWidth);
            break;

        case Settings::EffectType::Gradient:
            TextEffects::WithOutline<TextEffects::AddTextHorizontalGradient>(
                drawList, font, fontSize, pos, text, outlineColor, outlineWidth, colL, colR);
            break;

        case Settings::EffectType::VerticalGradient:
            TextEffects::WithOutline<TextEffects::AddTextVerticalGradient>(
                drawList, font, fontSize, pos, text, outlineColor, outlineWidth, colL, colR);
            break;

        case Settings::EffectType::DiagonalGradient:
            TextEffects::WithOutline<TextEffects::AddTextDiagonalGradient>(
                drawList,
                font,
                fontSize,
                pos,
                text,
                outlineColor,
                outlineWidth,
                colL,
                colR,
                ImVec2(effect.param1, effect.param2));
            break;

        case Settings::EffectType::RadialGradient:
            TextEffects::WithOutline<TextEffects::AddTextRadialGradient>(drawList,
                                                                         font,
                                                                         fontSize,
                                                                         pos,
                                                                         text,
                                                                         outlineColor,
                                                                         outlineWidth,
                                                                         colL,
                                                                         colR,
                                                                         effect.param1,
                                                                         nullptr);
            break;

        case Settings::EffectType::Shimmer:
            TextEffects::WithOutline<TextEffects::AddTextShimmer>(
                drawList,
                font,
                fontSize,
                pos,
                text,
                outlineColor,
                outlineWidth,
                colL,
                colR,
                highlight,
                phase01,
                effect.param1,
                ParamOr(effect.param2, 1.0f) * strength);
            break;

        case Settings::EffectType::ChromaticShimmer:
            TextEffects::AddTextOutline4ChromaticShimmer(drawList,
                                                         font,
                                                         fontSize,
                                                         pos,
                                                         text,
                                                         colL,
                                                         colR,
                                                         highlight,
                                                         outlineColor,
                                                         outlineWidth,
                                                         phase01,
                                                         effect.param1,
                                                         effect.param2 * strength,
                                                         effect.param3 * textSizeScale,
                                                         effect.param4);
            break;

        case Settings::EffectType::PulseGradient:
        {
            float time = (float)ImGui::GetTime();
            TextEffects::WithOutline<TextEffects::AddTextPulseGradient>(drawList,
                                                                        font,
                                                                        fontSize,
                                                                        pos,
                                                                        text,
                                                                        outlineColor,
                                                                        outlineWidth,
                                                                        colL,
                                                                        colR,
                                                                        time,
                                                                        effect.param1,
                                                                        effect.param2 * strength);
        }
        break;

        case Settings::EffectType::RainbowWave:
            TextEffects::AddTextOutline4RainbowWave(drawList,
                                                    font,
                                                    fontSize,
                                                    pos,
                                                    text,
                                                    effect.param1,
                                                    effect.param2,
                                                    effect.param3,
                                                    effect.param4,
                                                    effect.param5,
                                                    alpha,
                                                    outlineColor,
                                                    outlineWidth,
                                                    effect.useWhiteBase);
            break;

        case Settings::EffectType::ConicRainbow:
            TextEffects::AddTextOutline4ConicRainbow(drawList,
                                                     font,
                                                     fontSize,
                                                     pos,
                                                     text,
                                                     effect.param1,
                                                     effect.param2,
                                                     effect.param3,
                                                     effect.param4,
                                                     alpha,
                                                     outlineColor,
                                                     outlineWidth,
                                                     effect.useWhiteBase);
            break;

        case Settings::EffectType::Aurora:
            TextEffects::WithOutline<TextEffects::AddTextAurora>(drawList,
                                                                 font,
                                                                 fontSize,
                                                                 pos,
                                                                 text,
                                                                 outlineColor,
                                                                 outlineWidth,
                                                                 colL,
                                                                 colR,
                                                                 ParamOr(effect.param1, .5f),
                                                                 ParamOr(effect.param2, 3.0f),
                                                                 ParamOr(effect.param3, 1.0f),
                                                                 ParamOr(effect.param4, .3f));
            break;

        case Settings::EffectType::Sparkle:
            TextEffects::WithOutline<TextEffects::AddTextSparkle>(
                drawList,
                font,
                fontSize,
                pos,
                text,
                outlineColor,
                outlineWidth,
                colL,
                colR,
                highlight,
                ParamOr(effect.param1, .3f),
                ParamOr(effect.param2, 2.0f),
                ParamOr(effect.param3, 1.0f) * strength);
            break;

        case Settings::EffectType::Plasma:
            TextEffects::WithOutline<TextEffects::AddTextPlasma>(drawList,
                                                                 font,
                                                                 fontSize,
                                                                 pos,
                                                                 text,
                                                                 outlineColor,
                                                                 outlineWidth,
                                                                 colL,
                                                                 colR,
                                                                 ParamOr(effect.param1, 2.0f),
                                                                 ParamOr(effect.param2, 3.0f),
                                                                 ParamOr(effect.param3, .5f));
            break;

        case Settings::EffectType::Scanline:
            TextEffects::WithOutline<TextEffects::AddTextScanline>(
                drawList,
                font,
                fontSize,
                pos,
                text,
                outlineColor,
                outlineWidth,
                colL,
                colR,
                highlight,
                ParamOr(effect.param1, .5f),
                ParamOr(effect.param2, .15f),
                ParamOr(effect.param3, 1.0f) * strength);
            break;
    }
}

// Describes one formatted text segment on the main nameplate line.
struct RenderSeg
{
    std::string text;         // Formatted text to display
    std::string displayText;  // Text after typewriter truncation
    bool isLevel;             // Whether to use level font
    ImFont* font;             // Font to use for rendering
    float fontSize;           // Scaled font size
    ImVec2 size;              // Measured size of this segment
    ImVec2 displaySize;       // Measured size of displayText
};

// All color, tier, and effect data computed once per label.
struct LabelStyle
{
    int tierIdx;
    const Settings::TierDefinition* tier;
    const Settings::SpecialTitleDefinition* specialTitle;

    ImU32 colL, colR;            // Name gradient (packed)
    ImU32 colLTitle, colRTitle;  // Title gradient
    ImU32 colLLevel, colRLevel;  // Level gradient
    ImU32 highlight;             // Shimmer / sparkle highlight
    ImU32 outlineColor;          // Black outline (alpha-scaled)
    ImU32 shadowColor;           // Black shadow  (alpha-scaled)

    ImVec4 Lc, Rc;            // Tier left/right (pasteled)
    ImVec4 LcName, RcName;    // Washed name colors (float, for glow)
    ImVec4 LcTitle, RcTitle;  // Washed title colors (float, for glow)
    ImVec4 LcLevel, RcLevel;  // Softened level colors (float, for glow)
    ImVec4 dispoCol;          // Disposition color
    ImVec4 specialGlowColor;  // Special title glow

    float alpha;
    float titleAlpha;
    float levelAlpha;
    float effectAlpha;
    float strength;
    float phase01;

    bool tierAllowsGlow;
    bool tierAllowsParticles;
    bool tierAllowsOrnaments;

    float nameOutlineWidth;
    float levelOutlineWidth;
    float titleOutlineWidth;
    float outlineWidth;  // Primary (= nameOutlineWidth)

    // Stored for deferred outline width computation
    float baseOutlineWidth;
    float distToPlayer;

    float CalcOutlineWidth(float fontSize) const
    {
        float w = baseOutlineWidth * (fontSize / Settings::NameFontSize);
        if (Settings::Visual().EnableDistanceOutlineScale)
        {
            float distT =
                TextEffects::Saturate((distToPlayer - Settings::FadeStartDistance) /
                                      (Settings::FadeEndDistance - Settings::FadeStartDistance));
            float distMul =
                Settings::Visual().OutlineDistanceMin +
                (Settings::Visual().OutlineDistanceMax - Settings::Visual().OutlineDistanceMin) *
                    distT;
            w *= distMul;
        }
        return w;
    }
};

// Text measurement and position data for a label.
struct LabelLayout
{
    ImFont* fontName;
    ImFont* fontLevel;
    ImFont* fontTitle;
    float nameFontSize;
    float levelFontSize;
    float titleFontSize;

    std::vector<RenderSeg> segments;
    float mainLineWidth;
    float mainLineHeight;
    float segmentPadding;

    std::string titleStr;
    std::string titleDisplayStr;
    ImVec2 titleSize;

    ImVec2 startPos;
    float titleY;
    float mainLineY;
    float totalWidth;

    ImVec2 nameplateCenter;
    float nameplateTop;
    float nameplateBottom;
    float nameplateLeft;
    float nameplateRight;
    float nameplateWidth;
    float nameplateHeight;
};

// Desaturate a color toward white.
static ImVec4 WashColor(ImVec4 base)
{
    const float wash = Settings::ColorWashAmount;
    return ImVec4(base.x + (1.0f - base.x) * wash,
                  base.y + (1.0f - base.y) * wash,
                  base.z + (1.0f - base.z) * wash,
                  base.w);
}

// Replace %n, %l, %t placeholders in a format string.
static std::string FormatString(const std::string& fmt,
                                const std::string_view nameVal,
                                int levelVal,
                                const char* titleVal = nullptr)
{
    std::string s = fmt;

    size_t pos = 0;
    while ((pos = s.find("%n", pos)) != std::string::npos)
    {
        s.replace(pos, 2, nameVal.data(), nameVal.size());
        pos += nameVal.size();
    }

    pos = 0;
    std::string lStr = std::to_string(levelVal);
    while ((pos = s.find("%l", pos)) != std::string::npos)
    {
        s.replace(pos, 2, lStr);
        pos += lStr.length();
    }

    if (titleVal)
    {
        pos = 0;
        while ((pos = s.find("%t", pos)) != std::string::npos)
        {
            s.replace(pos, 2, titleVal);
            pos += strlen(titleVal);
        }
    }
    return s;
}

static const Settings::TierDefinition& GetFallbackTier()
{
    static const Settings::TierDefinition fallback = []
    {
        Settings::TierDefinition t{};
        t.minLevel = 1;
        t.maxLevel = 250;
        t.title = "Unknown";
        t.leftColor = Settings::Color3::White();
        t.rightColor = Settings::Color3::White();
        t.highlightColor = Settings::Color3::White();
        t.titleEffect.type = Settings::EffectType::Gradient;
        t.nameEffect.type = Settings::EffectType::Gradient;
        t.levelEffect.type = Settings::EffectType::Gradient;
        t.particleCount = 0;
        return t;
    }();
    return fallback;
}

static const std::vector<const Settings::SpecialTitleDefinition*>& GetSortedSpecialTitlesForFrame()
{
    static std::vector<const Settings::SpecialTitleDefinition*> sortedSpecials;
    static uint32_t lastFrame = std::numeric_limits<uint32_t>::max();

    if (lastFrame == GetState().frame)
    {
        return sortedSpecials;
    }

    sortedSpecials.clear();
    sortedSpecials.reserve(Settings::SpecialTitles.size());
    for (const auto& st : Settings::SpecialTitles)
    {
        if (!st.keywordLower.empty())
        {
            sortedSpecials.push_back(&st);
        }
    }

    std::sort(sortedSpecials.begin(),
              sortedSpecials.end(),
              [](const auto* a, const auto* b) { return a->priority > b->priority; });

    lastFrame = GetState().frame;
    return sortedSpecials;
}

// Compute all color, tier, and effect data for a label.
static LabelStyle ComputeLabelStyle(const ActorDrawData& d, float alpha, float time)
{
    LabelStyle style{};
    style.alpha = alpha;

    // Disposition color
    if (d.dispo == Disposition::Enemy)
    {
        style.dispoCol = WashColor(ImVec4(.9f, .2f, .2f, alpha));
    }
    else if (d.dispo == Disposition::AllyOrFriend)
    {
        style.dispoCol = WashColor(ImVec4(.2f, .6f, 1.0f, alpha));
    }
    else
    {
        style.dispoCol = WashColor(ImVec4(.9f, .9f, .9f, alpha));
    }

    const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);

    const Settings::TierDefinition* tierPtr = nullptr;
    if (Settings::Tiers.empty())
    {
        style.tierIdx = 0;
        tierPtr = &GetFallbackTier();
    }
    else
    {
        // Find matching tier, or nearest range when no direct match exists.
        int matchedTier = -1;
        for (size_t i = 0; i < Settings::Tiers.size(); ++i)
        {
            if (lv >= Settings::Tiers[i].minLevel && lv <= Settings::Tiers[i].maxLevel)
            {
                matchedTier = static_cast<int>(i);
                break;
            }
        }

        if (matchedTier >= 0)
        {
            style.tierIdx = matchedTier;
        }
        else
        {
            int bestIdx = 0;
            int bestDistance = std::numeric_limits<int>::max();
            for (size_t i = 0; i < Settings::Tiers.size(); ++i)
            {
                const int minLevel = static_cast<int>(Settings::Tiers[i].minLevel);
                const int maxLevel = static_cast<int>(Settings::Tiers[i].maxLevel);
                int distance = 0;
                if (static_cast<int>(lv) < minLevel)
                {
                    distance = minLevel - static_cast<int>(lv);
                }
                else if (static_cast<int>(lv) > maxLevel)
                {
                    distance = static_cast<int>(lv) - maxLevel;
                }

                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    bestIdx = static_cast<int>(i);
                }
            }
            style.tierIdx = bestIdx;
        }

        style.tierIdx = std::clamp(style.tierIdx, 0, static_cast<int>(Settings::Tiers.size()) - 1);
        tierPtr = &Settings::Tiers[style.tierIdx];
    }
    style.tier = tierPtr;
    const Settings::TierDefinition& tier = *tierPtr;

    // Tier effect gating
    style.tierAllowsGlow = !Settings::Visual().EnableTierEffectGating ||
                           style.tierIdx >= Settings::Visual().GlowMinTier;
    style.tierAllowsParticles = !Settings::Visual().EnableTierEffectGating ||
                                style.tierIdx >= Settings::Visual().ParticleMinTier;
    style.tierAllowsOrnaments = !Settings::Visual().EnableTierEffectGating ||
                                style.tierIdx >= Settings::Visual().OrnamentMinTier;

    // Special title matching
    style.specialTitle = nullptr;
    {
        const auto& sortedSpecials = GetSortedSpecialTitlesForFrame();
        if (!sortedSpecials.empty())
        {
            std::string nameLower = d.name;
            std::transform(nameLower.begin(),
                           nameLower.end(),
                           nameLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            for (const auto* st : sortedSpecials)
            {
                if (st->keywordLower.empty())
                {
                    continue;
                }
                if (nameLower.find(st->keywordLower) != std::string::npos)
                {
                    style.specialTitle = st;
                    break;
                }
            }
        }
    }

    // Level position within tier [0, 1]
    float levelT = .0f;
    if (tier.maxLevel > tier.minLevel)
    {
        levelT = (lv <= tier.minLevel) ? .0f
                 : (lv >= tier.maxLevel)
                     ? 1.0f
                     : (float)(lv - tier.minLevel) / (float)(tier.maxLevel - tier.minLevel);
    }
    levelT = std::clamp(levelT, .0f, 1.0f);

    const bool under100 = (lv < 100);
    const float tierIntensity = under100 ? .5f : 1.0f;

    // Pastelize tier colors
    auto Pastelize = [&](const Settings::Color3& c) -> ImVec4
    {
        const float t = Settings::NameColorMix + (1.0f - Settings::NameColorMix) * levelT;
        return ImVec4(
            1.0f + (c.r - 1.0f) * t, 1.0f + (c.g - 1.0f) * t, 1.0f + (c.b - 1.0f) * t, 1.0f);
    };

    style.Lc = Pastelize(tier.leftColor);
    style.Rc = Pastelize(tier.rightColor);

    style.effectAlpha =
        alpha * tierIntensity * (Settings::EffectAlphaMin + Settings::EffectAlphaMax * levelT);

    auto MixToWhite = [](ImVec4 c, float amount)
    {
        amount = std::clamp(amount, .0f, 1.0f);
        return ImVec4(1.0f + (c.x - 1.0f) * amount,
                      1.0f + (c.y - 1.0f) * amount,
                      1.0f + (c.z - 1.0f) * amount,
                      c.w);
    };

    const float baseColorAmount = under100 ? (.35f + .65f * tierIntensity) : 1.0f;

    style.LcLevel = MixToWhite(style.Lc, baseColorAmount);
    style.RcLevel = MixToWhite(style.Rc, baseColorAmount);
    style.LcName = WashColor(style.LcLevel);
    style.RcName = WashColor(style.RcLevel);
    style.LcTitle = WashColor(style.LcName);
    style.RcTitle = WashColor(style.RcName);

    style.specialGlowColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    if (style.specialTitle)
    {
        const auto* st = style.specialTitle;
        ImVec4 specialCol(st->color.r, st->color.g, st->color.b, 1.0f);
        style.specialGlowColor = ImVec4(st->glowColor.r, st->glowColor.g, st->glowColor.b, 1.0f);

        style.Lc = specialCol;
        style.Rc = specialCol;
        style.LcLevel = specialCol;
        style.RcLevel = specialCol;
        style.LcName = specialCol;
        style.RcName = specialCol;
        style.LcTitle = WashColor(specialCol);
        style.RcTitle = WashColor(specialCol);
    }

    // Pack colors to ImU32
    style.colL = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, alpha));
    style.colR = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.RcName.x, style.RcName.y, style.RcName.z, alpha));

    style.titleAlpha = alpha * Settings::Visual().TitleAlphaMultiplier;
    style.levelAlpha = alpha * Settings::Visual().LevelAlphaMultiplier;

    style.colLTitle = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.LcTitle.x, style.LcTitle.y, style.LcTitle.z, style.titleAlpha));
    style.colRTitle = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.RcTitle.x, style.RcTitle.y, style.RcTitle.z, style.titleAlpha));
    style.colLLevel = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.LcLevel.x, style.LcLevel.y, style.LcLevel.z, style.levelAlpha));
    style.colRLevel = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.RcLevel.x, style.RcLevel.y, style.RcLevel.z, style.levelAlpha));
    style.highlight = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, style.effectAlpha));

    const float outlineAlpha = TextEffects::Saturate(alpha);
    const float shadowAlpha = TextEffects::Saturate(alpha * .75f);
    style.outlineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, outlineAlpha));
    style.shadowColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, shadowAlpha));

    // Outline width data (actual widths computed after font sizes are known)
    style.baseOutlineWidth = Settings::OutlineWidthMin + Settings::OutlineWidthMax;
    style.distToPlayer = d.distToPlayer;

    // Animation
    auto frac = [](float x) { return x - std::floor(x); };

    float tierAnimSpeed = Settings::AnimSpeedLowTier;
    if (Settings::Tiers.size() > 1)
    {
        float tierRatio =
            static_cast<float>(style.tierIdx) / static_cast<float>(Settings::Tiers.size() - 1);
        if (tierRatio >= .9f)
        {
            tierAnimSpeed = Settings::AnimSpeedHighTier;
        }
        else if (tierRatio >= .8f)
        {
            tierAnimSpeed = Settings::AnimSpeedMidTier;
        }
    }
    if (under100)
    {
        tierAnimSpeed *= .75f;
    }

    const float phaseSeed = (d.formID & 1023) / 1023.0f;
    style.phase01 = frac(time * tierAnimSpeed + phaseSeed);

    style.strength = tierIntensity * (Settings::StrengthMin + Settings::StrengthMax * levelT);

    return style;
}

// Measure text and compute all positions for a label.
static LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                                      ActorCache& entry,
                                      const LabelStyle& style,
                                      float textSizeScale)
{
    LabelLayout layout{};

    // Typewriter character count
    int typewriterCharsToShow = -1;
    if (Settings::EnableTypewriter && !entry.typewriterComplete)
    {
        float effectiveTime = entry.typewriterTime - Settings::TypewriterDelay;
        if (effectiveTime > .0f)
        {
            typewriterCharsToShow = static_cast<int>(effectiveTime * Settings::TypewriterSpeed);
        }
        else
        {
            typewriterCharsToShow = 0;
        }
    }

    // Fonts
    layout.fontName = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    layout.fontLevel = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    layout.fontTitle = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!layout.fontName || !layout.fontLevel || !layout.fontTitle)
    {
        return layout;
    }

    layout.nameFontSize = layout.fontName->FontSize * textSizeScale;
    layout.levelFontSize = layout.fontLevel->FontSize * textSizeScale;
    layout.titleFontSize = layout.fontTitle->FontSize * textSizeScale;

    const float outlineWidth = style.outlineWidth;

    const char* safeName = d.name.empty() ? " " : d.name.c_str();

    // Build segments
    layout.mainLineWidth = .0f;
    layout.mainLineHeight = .0f;

    const auto& fmtList = Settings::DisplayFormat.empty()
                              ? std::vector<Settings::Segment>{{"%n", false}, {" Lv.%l", true}}
                              : Settings::DisplayFormat;

    int totalCharsProcessed = 0;

    for (const auto& fmt : fmtList)
    {
        RenderSeg seg;
        seg.text = FormatString(fmt.format, safeName, d.level);
        seg.isLevel = fmt.useLevelFont;
        seg.font = seg.isLevel ? layout.fontLevel : layout.fontName;
        seg.fontSize = seg.isLevel ? layout.levelFontSize : layout.nameFontSize;
        seg.size = seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, .0f, seg.text.c_str());

        if (typewriterCharsToShow >= 0)
        {
            size_t segCharCount = Utf8CharCount(seg.text.c_str());
            int charsRemaining = typewriterCharsToShow - totalCharsProcessed;
            if (charsRemaining <= 0)
            {
                seg.displayText = "";
            }
            else if (static_cast<size_t>(charsRemaining) >= segCharCount)
            {
                seg.displayText = seg.text;
            }
            else
            {
                seg.displayText = Utf8Truncate(seg.text.c_str(), charsRemaining);
            }
            totalCharsProcessed += static_cast<int>(segCharCount);
        }
        else
        {
            seg.displayText = seg.text;
        }

        seg.displaySize =
            seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, .0f, seg.displayText.c_str());

        layout.segments.push_back(seg);
        layout.mainLineWidth += seg.size.x;
        if (seg.size.y > layout.mainLineHeight)
        {
            layout.mainLineHeight = seg.size.y;
        }
    }

    layout.segmentPadding = Settings::SegmentPadding;
    if (!layout.segments.empty())
    {
        layout.mainLineWidth += (layout.segments.size() - 1) * layout.segmentPadding;
    }

    // Title
    const char* titleToUse =
        style.specialTitle ? style.specialTitle->displayTitle.c_str() : style.tier->title.c_str();
    layout.titleStr = FormatString(Settings::TitleFormat, safeName, d.level, titleToUse);
    layout.titleDisplayStr = layout.titleStr;

    if (typewriterCharsToShow >= 0)
    {
        size_t titleCharCount = Utf8CharCount(layout.titleStr.c_str());
        int charsRemainingForTitle = typewriterCharsToShow - totalCharsProcessed;
        if (charsRemainingForTitle <= 0)
        {
            layout.titleDisplayStr = "";
        }
        else if (static_cast<size_t>(charsRemainingForTitle) >= titleCharCount)
        {
            layout.titleDisplayStr = layout.titleStr;
        }
        else
        {
            layout.titleDisplayStr = Utf8Truncate(layout.titleStr.c_str(), charsRemainingForTitle);
        }
        totalCharsProcessed += static_cast<int>(titleCharCount);

        if (!entry.typewriterComplete && typewriterCharsToShow >= totalCharsProcessed)
        {
            entry.typewriterComplete = true;
        }
    }

    const char* titleText = layout.titleStr.c_str();

    // Tight vertical bounds
    float titleTop = .0f, titleBottom = .0f;
    if (titleText && *titleText)
    {
        CalcTightYBoundsFromTop(
            layout.fontTitle, layout.titleFontSize, titleText, titleTop, titleBottom);
    }
    layout.titleSize =
        layout.fontTitle->CalcTextSizeA(layout.titleFontSize, FLT_MAX, .0f, titleText);

    float mainTop = +FLT_MAX;
    float mainBottom = -FLT_MAX;
    bool any = false;
    for (const auto& seg : layout.segments)
    {
        float sTop = .0f, sBottom = .0f;
        CalcTightYBoundsFromTop(seg.font, seg.fontSize, seg.text.c_str(), sTop, sBottom);
        float vOffset = (layout.mainLineHeight - seg.size.y) * .5f;
        mainTop = std::min(mainTop, vOffset + sTop);
        mainBottom = std::max(mainBottom, vOffset + sBottom);
        any = true;
    }
    if (!any)
    {
        mainTop = .0f;
        mainBottom = .0f;
    }

    const float titleShadowY = Settings::TitleShadowOffsetY;
    const float mainShadowY = Settings::MainShadowOffsetY;
    float titleBottomDraw = titleBottom + titleShadowY;
    float mainTopDraw = mainTop - outlineWidth;
    float mainBottomDraw = mainBottom + outlineWidth + mainShadowY;

    layout.mainLineY = -mainBottomDraw;
    layout.titleY = layout.mainLineY + mainTopDraw - titleBottomDraw;

    layout.startPos = entry.smooth;
    if (Settings::Visual().EnableOverlapPrevention)
    {
        auto oIt = OverlapOffsets().find(d.formID);
        if (oIt != OverlapOffsets().end())
        {
            layout.startPos.y += oIt->second;
        }
    }

    layout.totalWidth = std::max(layout.mainLineWidth, layout.titleSize.x);

    layout.nameplateTop = layout.startPos.y + layout.titleY + titleTop;
    layout.nameplateBottom = layout.startPos.y + layout.mainLineY + mainBottom;
    layout.nameplateLeft = layout.startPos.x - layout.totalWidth * .5f;
    layout.nameplateRight = layout.startPos.x + layout.totalWidth * .5f;
    layout.nameplateWidth = layout.totalWidth;
    layout.nameplateHeight = layout.nameplateBottom - layout.nameplateTop;
    layout.nameplateCenter =
        ImVec2(layout.startPos.x, (layout.nameplateTop + layout.nameplateBottom) * .5f);

    return layout;
}

// Draw particle aura effects behind the nameplate.
static void DrawParticles(ImDrawList* dl,
                          const ActorDrawData& d,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          float lodEffectsFactor,
                          float time,
                          ImDrawListSplitter* splitter)
{
    splitter->SetCurrentChannel(dl, 0);  // Back layer: particles
    const Settings::TierDefinition& tier = *style.tier;
    const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);

    bool tierHasParticles = !tier.particleTypes.empty() && tier.particleTypes != "None";
    bool globalHasParticles = Settings::EnableOrbs || Settings::EnableWisps ||
                              Settings::EnableRunes || Settings::EnableSparks ||
                              Settings::EnableStars;
    bool hasAnyParticles = tierHasParticles || globalHasParticles;
    bool showParticles =
        ((Settings::EnableParticleAura && hasAnyParticles && style.tierAllowsParticles) ||
         (style.specialTitle && style.specialTitle->forceParticles)) &&
        lodEffectsFactor > .01f;
    if (!showParticles)
    {
        return;
    }

    ImU32 particleColor;
    if (style.specialTitle)
    {
        particleColor = ImGui::ColorConvertFloat4ToU32(ImVec4(style.specialTitle->color.r,
                                                              style.specialTitle->color.g,
                                                              style.specialTitle->color.b,
                                                              1.0f));
    }
    else
    {
        particleColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, 1.0f));
    }

    float spreadX = (layout.nameplateWidth * .5f + Settings::ParticleSpread * 1.4f);
    float spreadY = (layout.nameplateHeight * .5f + Settings::ParticleSpread * 1.1f);

    int particleCount = (tier.particleCount > 0) ? tier.particleCount : Settings::ParticleCount;
    float tierBoost = .0f;
    if (Settings::Tiers.size() > 1)
    {
        tierBoost =
            static_cast<float>(style.tierIdx) / static_cast<float>(Settings::Tiers.size() - 1);
    }
    float levelBoost = TextEffects::Saturate((static_cast<float>(lv) - 100.0f) / 400.0f);
    float particleBoost = 1.0f + .6f * tierBoost + .6f * levelBoost;
    int boostedParticleCount =
        std::clamp(static_cast<int>(std::round(particleCount * particleBoost)), particleCount, 96);
    float boostedParticleSize =
        Settings::ParticleSize * (1.0f + .4f * tierBoost + .35f * levelBoost);
    float boostedParticleAlpha = std::clamp(
        Settings::ParticleAlpha * style.alpha * (.95f + .35f * tierBoost + .35f * levelBoost),
        .0f,
        1.0f);

    bool showOrbs = false, showWisps = false, showRunes = false, showSparks = false,
         showStars = false;
    if (tierHasParticles)
    {
        showOrbs = tier.particleTypes.find("Orbs") != std::string::npos;
        showWisps = tier.particleTypes.find("Wisps") != std::string::npos;
        showRunes = tier.particleTypes.find("Runes") != std::string::npos;
        showSparks = tier.particleTypes.find("Sparks") != std::string::npos;
        showStars = tier.particleTypes.find("Stars") != std::string::npos;
    }
    else
    {
        showOrbs = Settings::EnableOrbs;
        showWisps = Settings::EnableWisps;
        showRunes = Settings::EnableRunes;
        showSparks = Settings::EnableSparks;
        showStars = Settings::EnableStars;
    }

    int enabledStyles =
        (int)showOrbs + (int)showWisps + (int)showRunes + (int)showSparks + (int)showStars;
    int slot = 0;

    if (showOrbs)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       spreadX,
                                       spreadY,
                                       particleColor,
                                       boostedParticleAlpha,
                                       Settings::ParticleStyle::Orbs,
                                       boostedParticleCount,
                                       boostedParticleSize,
                                       Settings::ParticleSpeed,
                                       time,
                                       slot++,
                                       enabledStyles});
    }
    if (showWisps)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       spreadX * 1.15f,
                                       spreadY * 1.15f,
                                       particleColor,
                                       boostedParticleAlpha,
                                       Settings::ParticleStyle::Wisps,
                                       boostedParticleCount,
                                       boostedParticleSize,
                                       Settings::ParticleSpeed,
                                       time,
                                       slot++,
                                       enabledStyles});
    }
    if (showRunes)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       spreadX * .9f,
                                       spreadY * .7f,
                                       particleColor,
                                       boostedParticleAlpha,
                                       Settings::ParticleStyle::Runes,
                                       std::max(4, boostedParticleCount / 2),
                                       boostedParticleSize * 1.2f,
                                       Settings::ParticleSpeed * .6f,
                                       time,
                                       slot++,
                                       enabledStyles});
    }
    if (showSparks)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       spreadX,
                                       spreadY * .8f,
                                       particleColor,
                                       boostedParticleAlpha,
                                       Settings::ParticleStyle::Sparks,
                                       boostedParticleCount,
                                       boostedParticleSize * .7f,
                                       Settings::ParticleSpeed * 1.5f,
                                       time,
                                       slot++,
                                       enabledStyles});
    }
    if (showStars)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       spreadX,
                                       spreadY,
                                       particleColor,
                                       boostedParticleAlpha,
                                       Settings::ParticleStyle::Stars,
                                       boostedParticleCount,
                                       boostedParticleSize,
                                       Settings::ParticleSpeed,
                                       time,
                                       slot++,
                                       enabledStyles});
    }
}

// Draw decorative ornament characters beside the nameplate.
static void DrawOrnaments(ImDrawList* dl,
                          const ActorDrawData& d,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          float lodEffectsFactor,
                          float time,
                          ImDrawListSplitter* splitter)
{
    const Settings::TierDefinition& tier = *style.tier;

    const std::string& leftOrns = (style.specialTitle && !style.specialTitle->leftOrnaments.empty())
                                      ? style.specialTitle->leftOrnaments
                                      : tier.leftOrnaments;
    const std::string& rightOrns =
        (style.specialTitle && !style.specialTitle->rightOrnaments.empty())
            ? style.specialTitle->rightOrnaments
            : tier.rightOrnaments;
    bool hasOrnaments = !leftOrns.empty() || !rightOrns.empty();
    bool showOrnaments =
        ((d.isPlayer && Settings::EnableOrnaments && hasOrnaments && style.tierAllowsOrnaments) ||
         (style.specialTitle && style.specialTitle->forceOrnaments && hasOrnaments)) &&
        lodEffectsFactor > .01f;
    auto& ornIo = ImGui::GetIO();
    ImFont* ornamentFont = (ornIo.Fonts->Fonts.Size > RenderConstants::FONT_INDEX_ORNAMENT)
                               ? ornIo.Fonts->Fonts[RenderConstants::FONT_INDEX_ORNAMENT]
                               : nullptr;
    if (!showOrnaments || Settings::OrnamentFontPath.empty() || !ornamentFont)
    {
        return;
    }

    auto collectDrawableOrnaments = [&](const std::string& raw)
    {
        std::vector<std::string> out;
        const char* p = raw.c_str();
        while (*p)
        {
            unsigned int cp = 0;
            const char* next = Utf8Next(p, cp);
            if (!next || next <= p)
            {
                ++p;
                continue;
            }
            if (cp == 0xFFFD || cp < 0x20)
            {
                p = next;
                continue;
            }

            const ImFontGlyph* glyph = nullptr;
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 18804
            glyph = ornamentFont->FindGlyphNoFallback(static_cast<ImWchar>(cp));
#else
            glyph = ornamentFont->FindGlyph(static_cast<ImWchar>(cp));
#endif
            if (glyph)
                out.emplace_back(p, static_cast<size_t>(next - p));
            p = next;
        }
        return out;
    };

    const auto leftChars = collectDrawableOrnaments(leftOrns);
    const auto rightChars = collectDrawableOrnaments(rightOrns);
    if (leftChars.empty() && rightChars.empty())
    {
        return;
    }

    const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;

    float ornamentScale = .75f;
    if (Settings::Tiers.size() > 1)
    {
        ornamentScale = .75f + .3f * (static_cast<float>(style.tierIdx) /
                                      static_cast<float>(Settings::Tiers.size() - 1));
    }
    float sizeMultiplier = (style.specialTitle != nullptr) ? ornamentScale * 1.3f : ornamentScale;
    float ornamentSize =
        Settings::OrnamentFontSize * Settings::OrnamentScale * sizeMultiplier * textSizeScale;

    float extraPadding = ornamentSize * .30f;
    float totalSpacing = Settings::OrnamentSpacing * 1.35f + extraPadding;
    float ornamentCharGap = std::max(2.0f, ornamentSize * .16f);

    ImU32 ornColL =
        ImGui::ColorConvertFloat4ToU32(ImVec4(style.Lc.x, style.Lc.y, style.Lc.z, style.alpha));
    ImU32 ornColR =
        ImGui::ColorConvertFloat4ToU32(ImVec4(style.Rc.x, style.Rc.y, style.Rc.z, style.alpha));
    ImU32 ornHighlight = ImGui::ColorConvertFloat4ToU32(
        ImVec4(tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, style.alpha));
    ImU32 ornOutline = IM_COL32(0, 0, 0, (int)(style.alpha * 255.0f));
    float ornOutlineWidth = style.outlineWidth * (ornamentSize / layout.nameFontSize);

    ImU32 glowColor =
        ImGui::ColorConvertFloat4ToU32(ImVec4(style.Lc.x, style.Lc.y, style.Lc.z, style.alpha));
    bool showOrnGlow =
        Settings::EnableGlow && Settings::GlowIntensity > .0f && style.tierAllowsGlow;

    auto drawOrnChar = [&](ImVec2 charPos, const char* ch)
    {
        if (showOrnGlow)
        {
            splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
            ParticleTextures::PushAdditiveBlend(dl);
            TextEffects::AddTextGlow(dl,
                                     ornamentFont,
                                     ornamentSize,
                                     charPos,
                                     ch,
                                     glowColor,
                                     Settings::GlowRadius,
                                     Settings::GlowIntensity,
                                     Settings::GlowSamples);
            ParticleTextures::PopBlendState(dl);
        }
        splitter->SetCurrentChannel(dl, 1);  // Front layer: ornament shapes
        ApplyTextEffect(dl,
                        ornamentFont,
                        ornamentSize,
                        charPos,
                        ch,
                        tier.nameEffect,
                        ornColL,
                        ornColR,
                        ornHighlight,
                        ornOutline,
                        ornOutlineWidth,
                        style.phase01,
                        style.strength,
                        textSizeScale,
                        style.alpha);
    };

    if (!leftChars.empty())
    {
        float cursorX = layout.nameplateCenter.x - layout.nameplateWidth * .5f - totalSpacing;
        for (int i = static_cast<int>(leftChars.size()) - 1; i >= 0; --i)
        {
            const std::string& ch = leftChars[i];
            ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, .0f, ch.c_str());
            cursorX -= charSize.x;
            ImVec2 charPos(cursorX, layout.nameplateCenter.y - charSize.y * .5f);
            drawOrnChar(charPos, ch.c_str());
            if (i > 0)
            {
                cursorX -= ornamentCharGap;
            }
        }
    }

    if (!rightChars.empty())
    {
        float cursorX = layout.nameplateCenter.x + layout.nameplateWidth * .5f + totalSpacing;
        for (size_t i = 0; i < rightChars.size(); ++i)
        {
            const std::string& ch = rightChars[i];
            ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, .0f, ch.c_str());
            ImVec2 charPos(cursorX, layout.nameplateCenter.y - charSize.y * .5f);
            drawOrnChar(charPos, ch.c_str());
            cursorX += charSize.x;
            if (i + 1 < rightChars.size())
            {
                cursorX += ornamentCharGap;
            }
        }
    }
}

// Render the title line above the main nameplate line.
static void DrawTitleText(ImDrawList* dl,
                          const ActorDrawData& d,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          float lodTitleFactor,
                          ImDrawListSplitter* splitter)
{
    const char* titleDisplayText = layout.titleDisplayStr.c_str();
    if (!titleDisplayText || !*titleDisplayText || lodTitleFactor <= .01f)
    {
        return;
    }

    float titleOffsetX = (layout.totalWidth - layout.titleSize.x) * .5f;
    ImVec2 titlePos(layout.startPos.x - layout.totalWidth * .5f + titleOffsetX,
                    layout.startPos.y + layout.titleY);

    float lodTitleAlpha = style.alpha * lodTitleFactor;
    ImU32 titleShadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlpha * .5f));

    splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
    if (Settings::EnableGlow && Settings::GlowIntensity > .0f && style.tierAllowsGlow)
    {
        ImVec4 glowColorVec =
            style.specialTitle
                ? ImVec4(style.specialGlowColor.x,
                         style.specialGlowColor.y,
                         style.specialGlowColor.z,
                         style.alpha)
                : ImVec4(style.LcTitle.x, style.LcTitle.y, style.LcTitle.z, style.alpha);
        ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowColorVec);
        float glowIntensity =
            style.specialTitle ? Settings::GlowIntensity * 1.15f : Settings::GlowIntensity;
        float glowRadius = style.specialTitle ? Settings::GlowRadius * 1.1f : Settings::GlowRadius;
        ParticleTextures::PushAdditiveBlend(dl);
        TextEffects::AddTextGlow(dl,
                                 layout.fontTitle,
                                 layout.titleFontSize,
                                 titlePos,
                                 titleDisplayText,
                                 glowColor,
                                 glowRadius,
                                 glowIntensity,
                                 Settings::GlowSamples);
        ParticleTextures::PopBlendState(dl);
    }

    splitter->SetCurrentChannel(dl, 1);  // Front layer: shadow + text
    dl->AddText(layout.fontTitle,
                layout.titleFontSize,
                ImVec2(titlePos.x + Settings::TitleShadowOffsetX,
                       titlePos.y + Settings::TitleShadowOffsetY),
                titleShadow,
                titleDisplayText);

    float lodTitleAlphaFinal = style.titleAlpha * lodTitleFactor;
    float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    if (d.isPlayer)
    {
        ApplyTextEffect(dl,
                        layout.fontTitle,
                        layout.titleFontSize,
                        titlePos,
                        titleDisplayText,
                        style.tier->titleEffect,
                        style.colLTitle,
                        style.colRTitle,
                        style.highlight,
                        style.outlineColor,
                        style.titleOutlineWidth,
                        style.phase01,
                        style.strength,
                        textSizeScale,
                        lodTitleAlphaFinal);
    }
    else
    {
        ImVec4 dColV = WashColor(style.dispoCol);
        dColV.w = lodTitleAlphaFinal;
        ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
        ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlphaFinal));
        TextEffects::AddTextOutline4(dl,
                                     layout.fontTitle,
                                     layout.titleFontSize,
                                     titlePos,
                                     titleDisplayText,
                                     dCol,
                                     npcOutline,
                                     style.titleOutlineWidth);
    }
}

// Render each segment of the main nameplate line.
static void DrawMainLineSegments(ImDrawList* dl,
                                 const ActorDrawData& d,
                                 const LabelStyle& style,
                                 const LabelLayout& layout,
                                 ImDrawListSplitter* splitter)
{
    const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;

    ImVec2 currentPos;
    currentPos.x = layout.startPos.x - layout.totalWidth * .5f +
                   (layout.totalWidth - layout.mainLineWidth) * .5f;
    currentPos.y = layout.startPos.y + layout.mainLineY;

    for (const auto& seg : layout.segments)
    {
        if (seg.displayText.empty())
        {
            currentPos.x += seg.size.x + layout.segmentPadding;
            continue;
        }

        float vOffset = (layout.mainLineHeight - seg.size.y) * .5f;
        ImVec2 pos = ImVec2(currentPos.x, currentPos.y + vOffset);

        splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
        if (Settings::EnableGlow && Settings::GlowIntensity > .0f && style.tierAllowsGlow)
        {
            ImVec4 glowCol =
                style.specialTitle
                    ? ImVec4(style.specialGlowColor.x,
                             style.specialGlowColor.y,
                             style.specialGlowColor.z,
                             style.alpha)
                    : (seg.isLevel
                           ? ImVec4(style.LcLevel.x, style.LcLevel.y, style.LcLevel.z, style.alpha)
                           : ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, style.alpha));
            ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowCol);
            float glowIntensity =
                style.specialTitle ? Settings::GlowIntensity * 1.15f : Settings::GlowIntensity;
            float glowRadius =
                style.specialTitle ? Settings::GlowRadius * 1.1f : Settings::GlowRadius;
            ParticleTextures::PushAdditiveBlend(dl);
            TextEffects::AddTextGlow(dl,
                                     seg.font,
                                     seg.fontSize,
                                     pos,
                                     seg.displayText.c_str(),
                                     glowColor,
                                     glowRadius,
                                     glowIntensity,
                                     Settings::GlowSamples);
            ParticleTextures::PopBlendState(dl);
        }

        splitter->SetCurrentChannel(dl, 1);  // Front layer: shadow + text
        dl->AddText(
            seg.font,
            seg.fontSize,
            ImVec2(pos.x + Settings::MainShadowOffsetX, pos.y + Settings::MainShadowOffsetY),
            style.shadowColor,
            seg.displayText.c_str());

        float segOutlineWidth = seg.isLevel ? style.levelOutlineWidth : style.nameOutlineWidth;

        if (seg.isLevel)
        {
            ApplyTextEffect(dl,
                            seg.font,
                            seg.fontSize,
                            pos,
                            seg.displayText.c_str(),
                            style.tier->levelEffect,
                            style.colLLevel,
                            style.colRLevel,
                            style.highlight,
                            style.outlineColor,
                            segOutlineWidth,
                            style.phase01,
                            style.strength,
                            textSizeScale,
                            style.levelAlpha);
        }
        else
        {
            if (d.isPlayer)
            {
                ApplyTextEffect(dl,
                                seg.font,
                                seg.fontSize,
                                pos,
                                seg.displayText.c_str(),
                                style.tier->nameEffect,
                                style.colL,
                                style.colR,
                                style.highlight,
                                style.outlineColor,
                                segOutlineWidth,
                                style.phase01,
                                style.strength,
                                textSizeScale,
                                style.alpha);
            }
            else
            {
                ImVec4 dColV = style.dispoCol;
                dColV.w = style.alpha;
                ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
                ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, style.alpha));
                TextEffects::AddTextOutline4(dl,
                                             seg.font,
                                             seg.fontSize,
                                             pos,
                                             seg.displayText.c_str(),
                                             dCol,
                                             npcOutline,
                                             segOutlineWidth);
            }
        }

        currentPos.x += seg.size.x + layout.segmentPadding;
    }
}

// Distance-based fade, LOD, and scale factors for a label.
struct DistanceFactors
{
    float alphaTarget;       // Target alpha from distance fade
    float textScaleTarget;   // Target font scale from distance
    float lodTitleFactor;    // LOD multiplier for title visibility [0,1]
    float lodEffectsFactor;  // LOD multiplier for particles/ornaments [0,1]
};

// Compute distance-based visual factors (fade, LOD, scale).
static DistanceFactors ComputeDistanceFactors(const ActorDrawData& d)
{
    DistanceFactors df{};
    const float dist = d.distToPlayer;

    // Alpha fade using squared smoothstep
    const float fadeRange = std::max(1.0f, Settings::FadeEndDistance - Settings::FadeStartDistance);
    float fadeT = TextEffects::SmoothStep((dist - Settings::FadeStartDistance) / fadeRange);
    df.alphaTarget = 1.0f - fadeT;
    df.alphaTarget = df.alphaTarget * df.alphaTarget;

    // LOD factors
    df.lodTitleFactor = 1.0f;
    df.lodEffectsFactor = 1.0f;

    if (Settings::Visual().EnableLOD)
    {
        float transRange = std::max(1.0f, Settings::Visual().LODTransitionRange);
        float titleFadeT =
            TextEffects::Saturate((dist - Settings::Visual().LODFarDistance) / transRange);
        df.lodTitleFactor = 1.0f - TextEffects::SmoothStep(titleFadeT);
        float effectsFadeT =
            TextEffects::Saturate((dist - Settings::Visual().LODMidDistance) / transRange);
        df.lodEffectsFactor = 1.0f - TextEffects::SmoothStep(effectsFadeT);
    }

    // Font size scale with sqrt falloff
    const float scaleRange =
        std::max(1.0f, Settings::ScaleEndDistance - Settings::ScaleStartDistance);
    float scaleT = TextEffects::Saturate((dist - Settings::ScaleStartDistance) / scaleRange);
    constexpr float SCALE_GAMMA = .5f;
    scaleT = std::pow(scaleT, SCALE_GAMMA);
    df.textScaleTarget = 1.0f + (Settings::MinimumScale - 1.0f) * scaleT;

    // Also factor in camera distance for more accurate near-camera scaling
    if (auto pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot)
    {
        RE::NiPoint3 cameraPos = pc->cameraRoot->world.translate;
        const float dx = d.worldPos.x - cameraPos.x;
        const float dy = d.worldPos.y - cameraPos.y;
        const float dz = d.worldPos.z - cameraPos.z;
        float camDist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float camScaleT =
            TextEffects::Saturate((camDist - Settings::ScaleStartDistance) / scaleRange);
        camScaleT = std::pow(camScaleT, SCALE_GAMMA);
        float camTextScale = 1.0f + (Settings::MinimumScale - 1.0f) * camScaleT;
        df.textScaleTarget = std::min(df.textScaleTarget, camTextScale);
    }

    // Enforce minimum readable size
    if (Settings::Visual().MinimumPixelHeight > .0f)
    {
        float minScale = Settings::Visual().MinimumPixelHeight / Settings::NameFontSize;
        df.textScaleTarget = std::max(df.textScaleTarget, minScale);
    }

    return df;
}

// Update cache entry smoothing and return whether the label is visible.
//
// On first frame, snaps values directly. On subsequent frames, applies
// framerate-independent exponential smoothing to alpha, scale, position,
// and occlusion. Returns false if the label should be culled.
static bool UpdateCacheSmoothing(ActorCache& entry,
                                 const ActorDrawData& d,
                                 const DistanceFactors& df,
                                 const RE::NiPoint3& screenPos,
                                 float dt)
{
    float occlusionTarget = d.isOccluded ? .0f : 1.0f;

    if (!entry.initialized)
    {
        entry.initialized = true;
        entry.alphaSmooth = df.alphaTarget;
        entry.textSizeScale = df.textScaleTarget;
        entry.smooth = ImVec2(screenPos.x, screenPos.y);

        ImVec2 initPos(screenPos.x, screenPos.y);
        for (int i = 0; i < ActorCache::HISTORY_SIZE; i++)
        {
            entry.posHistory[i] = initPos;
        }
        entry.historyIndex = 0;
        entry.historyFilled = true;
        entry.occlusionSmooth = occlusionTarget;
        entry.typewriterTime = .0f;
        entry.typewriterComplete = false;
    }
    else
    {
        float aLerp = ExpApproachAlpha(dt, Settings::AlphaSettleTime);
        float sLerp = ExpApproachAlpha(dt, Settings::ScaleSettleTime);
        float pLerp = d.isPlayer ? ExpApproachAlpha(dt, .015f)
                                 : ExpApproachAlpha(dt, Settings::PositionSettleTime);
        float oLerp = ExpApproachAlpha(dt, Settings::OcclusionSettleTime);

        entry.alphaSmooth += (df.alphaTarget - entry.alphaSmooth) * aLerp;
        entry.textSizeScale += (df.textScaleTarget - entry.textSizeScale) * sLerp;
        entry.occlusionSmooth += (occlusionTarget - entry.occlusionSmooth) * oLerp;

        ImVec2 targetPos(screenPos.x, screenPos.y);
        ImVec2 maSmoothed = entry.AddAndGetSmoothed(targetPos);

        ImVec2 expSmoothed;
        expSmoothed.x = entry.smooth.x + (targetPos.x - entry.smooth.x) * pLerp;
        expSmoothed.y = entry.smooth.y + (targetPos.y - entry.smooth.y) * pLerp;

        float blend = Settings::Visual().PositionSmoothingBlend;
        ImVec2 smoothedPos;
        smoothedPos.x = expSmoothed.x + (maSmoothed.x - expSmoothed.x) * blend;
        smoothedPos.y = expSmoothed.y + (maSmoothed.y - expSmoothed.y) * blend;

        float moveDx = targetPos.x - entry.smooth.x;
        float moveDy = targetPos.y - entry.smooth.y;
        float moveDist = std::sqrt(moveDx * moveDx + moveDy * moveDy);

        if (moveDist > Settings::Visual().LargeMovementThreshold)
        {
            entry.smooth.x +=
                (smoothedPos.x - entry.smooth.x) * Settings::Visual().LargeMovementBlend;
            entry.smooth.y +=
                (smoothedPos.y - entry.smooth.y) * Settings::Visual().LargeMovementBlend;
        }
        else
        {
            entry.smooth = smoothedPos;
        }

        if (Settings::EnableTypewriter && !entry.typewriterComplete)
        {
            entry.typewriterTime += dt;
        }
    }

    entry.wasOccluded = d.isOccluded;

    const float alpha = entry.alphaSmooth * entry.occlusionSmooth;
    return alpha > .02f;
}

static void DrawLabel(const ActorDrawData& d, ImDrawList* drawList, ImDrawListSplitter* splitter)
{
    // Look up or create cache entry
    auto it = GetState().cache.find(d.formID);
    if (it == GetState().cache.end())
    {
        ActorCache newEntry{};
        newEntry.lastSeenFrame = GetState().frame;
        it = GetState().cache.emplace(d.formID, newEntry).first;
    }
    auto& entry = it->second;
    const uint32_t prevLastSeenFrame = entry.lastSeenFrame;

    // Detect name changes and reset typewriter
    if (entry.cachedName != d.name)
    {
        entry.cachedName = d.name;
        entry.typewriterTime = .0f;
        entry.typewriterComplete = false;
    }

    // Reset typewriter on re-entry (actor reappearing or becoming unoccluded)
    constexpr uint32_t REENTRY_THRESHOLD = 30;
    if (entry.initialized && entry.typewriterComplete)
    {
        uint32_t framesSinceLastSeen = GetState().frame - prevLastSeenFrame;
        bool becameVisible = entry.wasOccluded && !d.isOccluded;
        if (framesSinceLastSeen >= REENTRY_THRESHOLD || becameVisible)
        {
            entry.typewriterTime = .0f;
            entry.typewriterComplete = false;
        }
    }

    entry.lastSeenFrame = GetState().frame;

    // Compute distance-based visual factors
    DistanceFactors df = ComputeDistanceFactors(d);

    // Project to screen space
    RE::NiPoint3 screenPos;
    if (!WorldToScreen(d.worldPos, screenPos))
    {
        return;
    }

    // Update smoothing and check visibility
    const float dt = ImGui::GetIO().DeltaTime;
    if (!UpdateCacheSmoothing(entry, d, df, screenPos, dt))
    {
        return;
    }

    // Cull off-screen labels
    const float alpha = entry.alphaSmooth * entry.occlusionSmooth;
    const float textSizeScale = entry.textSizeScale;

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return;
    }
    const auto viewSize = renderer->GetScreenSize();
    if (screenPos.z < 0 || screenPos.z > 1.0f || screenPos.x < -100.0f ||
        screenPos.x > viewSize.width + 100.0f || screenPos.y < -100.0f ||
        screenPos.y > viewSize.height + 100.0f)
    {
        return;
    }

    // Compute style, layout, and dispatch to sub-renderers
    const float time = (float)ImGui::GetTime();
    LabelStyle style = ComputeLabelStyle(d, alpha, time);

    ImFont* nameFont = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    ImFont* levelFont = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    ImFont* titleFont = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!nameFont || !levelFont || !titleFont)
    {
        return;
    }
    style.nameOutlineWidth = style.CalcOutlineWidth(nameFont->FontSize * textSizeScale);
    style.levelOutlineWidth = style.CalcOutlineWidth(levelFont->FontSize * textSizeScale);
    style.titleOutlineWidth = style.CalcOutlineWidth(titleFont->FontSize * textSizeScale);
    style.outlineWidth = style.nameOutlineWidth;

    LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale);

    DrawParticles(drawList, d, style, layout, df.lodEffectsFactor, time, splitter);
    DrawOrnaments(drawList, d, style, layout, df.lodEffectsFactor, time, splitter);
    DrawTitleText(drawList, d, style, layout, df.lodTitleFactor, splitter);
    DrawMainLineSegments(drawList, d, style, layout, splitter);
}

// Draw debug overlay with performance stats
static void DrawDebugOverlay()
{
    if (!Settings::EnableDebugOverlay)
    {
        return;
    }

    const float time = static_cast<float>(ImGui::GetTime());
    const float dt = ImGui::GetIO().DeltaTime;

    // Update frame timing stats
    DebugOverlay::UpdateFrameStats(GetState().debugStats,
                                   dt,
                                   time,
                                   GetState().lastDebugUpdateTime,
                                   GetState().updateCounter,
                                   GetState().lastUpdateCount);

    // Update cache stats
    GetState().debugStats.cacheSize = GetState().cache.size();

    // Build context and render
    DebugOverlay::Context ctx;
    ctx.stats = &GetState().debugStats;
    ctx.frameNumber = GetState().frame;
    ctx.postLoadCooldown = GetState().postLoadCooldown;
    ctx.lastReloadTime = GetState().lastReloadTime;
    ctx.actorCacheEntrySize = sizeof(ActorCache);
    ctx.actorDrawDataSize = sizeof(ActorDrawData);
    ctx.occlusionEnabled = Settings::EnableOcclusionCulling;
    ctx.glowEnabled = Settings::EnableGlow;
    ctx.typewriterEnabled = Settings::EnableTypewriter;
    ctx.hidePlayer = Settings::HidePlayer;
    ctx.verticalOffset = Settings::VerticalOffset;
    ctx.tierCount = Settings::Tiers.size();
    ctx.reloadKey = Settings::ReloadKey;

    DebugOverlay::Render(ctx);
}

// Handle hot reload key press (Settings::ReloadKey).
static void HandleHotReload()
{
    if (Settings::ReloadKey <= 0)
    {
        return;
    }

    bool keyDown = (GetAsyncKeyState(Settings::ReloadKey) & 0x8000) != 0;

    if (keyDown && !GetState().reloadKeyWasDown)
    {
        GetState().reloadRequested.store(true, std::memory_order_release);
    }

    if (!GetState().reloadRequested.load(std::memory_order_acquire))
    {
        GetState().reloadKeyWasDown = keyDown;
        return;
    }

    // Defer reload until in-flight snapshot updates are done; no render-thread busy wait.
    const bool queued = GetState().updateQueued.load(std::memory_order_acquire);
    const bool running = GetState().snapshotUpdateRunning.load(std::memory_order_acquire);
    if (queued || running)
    {
        GetState().reloadKeyWasDown = keyDown;
        return;
    }

    // Use seq_cst to ensure the pause flag is globally visible before we
    // re-check for in-flight updates.  A release/acquire pair alone is
    // insufficient here because the store and loads target different
    // atomic variables, so no happens-before is established between them
    // on weakly-ordered architectures.
    GetState().pauseSnapshotUpdates.store(true, std::memory_order_seq_cst);
    const bool queuedAfterPause = GetState().updateQueued.load(std::memory_order_seq_cst);
    const bool runningAfterPause = GetState().snapshotUpdateRunning.load(std::memory_order_seq_cst);
    if (queuedAfterPause || runningAfterPause)
    {
        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
        GetState().reloadKeyWasDown = keyDown;
        return;
    }

    Settings::Load();
    GetState().lastReloadTime = static_cast<float>(ImGui::GetTime());
    GetState().cache.clear();
    GetState().clearOcclusionCacheRequested.store(true, std::memory_order_release);

    if (Settings::TemplateReapplyOnReload && Settings::UseTemplateAppearance)
    {
        AppearanceTemplate::ResetAppliedFlag();
        if (auto* task = SKSE::GetTaskInterface())
        {
            task->AddTask([]() { AppearanceTemplate::ApplyIfConfigured(); });
        }
    }

    GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
    GetState().reloadRequested.store(false, std::memory_order_release);
    GetState().reloadKeyWasDown = keyDown;
}

// Update debug stats from the current snapshot.
static void UpdateDebugStats(const std::vector<ActorDrawData>& snap)
{
    GetState().debugStats.actorCount = static_cast<int>(snap.size());
    GetState().debugStats.visibleActors = 0;
    GetState().debugStats.occludedActors = 0;
    GetState().debugStats.playerVisible = 0;

    for (const auto& d : snap)
    {
        if (d.isPlayer)
        {
            GetState().debugStats.playerVisible = 1;
        }
        if (d.isOccluded)
        {
            GetState().debugStats.occludedActors++;
        }
        else
        {
            GetState().debugStats.visibleActors++;
        }
    }
    ++GetState().updateCounter;
}

// Resolve overlapping labels by pushing lower-priority ones down.
static void ResolveOverlaps(const std::vector<ActorDrawData>& localSnap)
{
    struct LabelRect
    {
        int idx;
        float cy, halfH, dist, yOffset;
        bool isPlayer;
    };
    std::vector<LabelRect> labelRects;

    for (int i = 0; i < static_cast<int>(localSnap.size()); ++i)
    {
        const auto& d = localSnap[i];
        auto cIt = GetState().cache.find(d.formID);
        if (cIt == GetState().cache.end() || !cIt->second.initialized)
        {
            continue;
        }

        const auto& entry = cIt->second;
        if (entry.alphaSmooth * entry.occlusionSmooth <= .02f)
        {
            continue;
        }

        float approxHeight = Settings::NameFontSize * entry.textSizeScale * 1.5f;
        labelRects.push_back(
            {i, entry.smooth.y, approxHeight * .5f, d.distToPlayer, .0f, d.isPlayer});
    }

    // Sort by priority: player first, then closest first
    std::sort(labelRects.begin(),
              labelRects.end(),
              [](const LabelRect& a, const LabelRect& b)
              {
                  if (a.isPlayer != b.isPlayer)
                  {
                      return a.isPlayer > b.isPlayer;
                  }
                  return a.dist < b.dist;
              });

    // Iterative relaxation: push lower-priority labels down
    float padding = Settings::Visual().OverlapPaddingY;
    for (int pass = 0; pass < Settings::Visual().OverlapIterations; ++pass)
    {
        for (int i = 0; i < static_cast<int>(labelRects.size()); ++i)
        {
            for (int j = i + 1; j < static_cast<int>(labelRects.size()); ++j)
            {
                float overlap =
                    (labelRects[i].cy + labelRects[i].yOffset + labelRects[i].halfH + padding) -
                    (labelRects[j].cy + labelRects[j].yOffset - labelRects[j].halfH);
                if (overlap > .0f)
                {
                    labelRects[j].yOffset += overlap;
                }
            }
        }
    }

    // Store offsets for DrawLabel to apply
    for (const auto& lr : labelRects)
    {
        if (std::abs(lr.yOffset) > .01f)
        {
            OverlapOffsets()[localSnap[lr.idx].formID] = lr.yOffset;
        }
    }
}

// Several pipeline stages are gated or tuned by Settings::Visual() overrides
// (LOD, overlap prevention, distance outline scaling, tier effect gating, etc.).
void Draw()
{
    HandleHotReload();
    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());

    if (!GameState::CanDrawOverlay())
    {
        GetState().wasInInvalidState = true;
        return;
    }

    if (GetState().wasInInvalidState)
    {
        GetState().wasInInvalidState = false;
        GetState().postLoadCooldown = 300;
    }

    if (GetState().postLoadCooldown > 0)
    {
        --GetState().postLoadCooldown;
        return;
    }

    QueueSnapshotUpdate_RenderThread();

    auto* bsRenderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!bsRenderer)
    {
        return;
    }

    const auto viewSize = bsRenderer->GetScreenSize();
    ++GetState().frame;

    std::vector<ActorDrawData> localSnap;
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        localSnap = GetState().snapshot;
    }

    if (localSnap.empty())
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)viewSize.width, (float)viewSize.height));
    ImGui::Begin("glyphOverlay",
                 nullptr,
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    if (Settings::EnableDebugOverlay)
    {
        UpdateDebugStats(localSnap);
    }

    OverlapOffsets().clear();
    if (Settings::Visual().EnableOverlapPrevention)
    {
        ResolveOverlaps(localSnap);
    }

    ImDrawListSplitter splitter;
    splitter.Split(drawList, 2);

    for (auto& d : localSnap)
    {
        DrawLabel(d, drawList, &splitter);
    }

    splitter.Merge(drawList);

    ImGui::End();

    DrawDebugOverlay();
    PruneCacheToSnapshot(localSnap);
}

void TickRT()
{
    // Called every frame from render thread
    // Queues updates but keeps the actual work lightweight/throttled
    QueueSnapshotUpdate_RenderThread();
}
}  // namespace Renderer
