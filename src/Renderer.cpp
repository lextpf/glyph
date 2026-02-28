#include "Renderer.h"
#include "TextEffects.h"
#include "ParticleTextures.h"
#include "Settings.h"
#include "Occlusion.h"
#include "RenderConstants.h"
#include "DebugOverlay.h"
#include "AppearanceTemplate.h"

#include <SKSE/SKSE.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Renderer
{
    // Count UTF-8 characters in string
    static size_t Utf8CharCount(const char *s)
    {
        size_t count = 0;
        if (!s)
            return 0;

        while (*s)
        {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80)
            {
                s++;
            }  // 1-byte (ASCII)
            else if (c < 0xE0)
            {
                s += 2;
            }  // 2-byte
            else if (c < 0xF0)
            {
                s += 3;
            }  // 3-byte
            else
            {
                s += 4;
            }  // 4-byte
            count++;
        }
        return count;
    }

    // Truncate UTF-8 string to maxChars codepoints
    static std::string Utf8Truncate(const char *s, size_t maxChars)
    {
        if (!s || maxChars == 0)
            return "";

        const char *start = s;
        size_t count = 0;

        while (*s && count < maxChars)
        {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80)
            {
                s++;
            }  // 1-byte (ASCII)
            else if (c < 0xE0)
            {
                s += 2;
            }  // 2-byte
            else if (c < 0xF0)
            {
                s += 3;
            }  // 3-byte
            else
            {
                s += 4;
            }  // 4-byte
            count++;
        }

        return std::string(start, s - start);
    }

    // Parse next UTF-8 codepoint, returns pointer to next char
    static const char *Utf8Next(const char *s, unsigned int &out)
    {
        out = 0;
        if (!s || !*s)
            return s;

        const unsigned char c = (unsigned char)s[0];

        // Single-byte ASCII character (0x00-0x7F)
        if (c < 0x80)
        {
            out = c;
            return s + 1;
        }

        // Reject continuation bytes (0x80-0xBF) appearing as start bytes
        // These are invalid in UTF-8 when they start a sequence
        if (c < 0xC0)
        {
            out = 0xFFFD;
            return s + 1;
        }  // 0xFFFD = replacement character (U+FFFD)

        // 2-byte sequence (0xC0-0xDF): 110xxxxx 10xxxxxx
        if (c < 0xE0)
        {
            if (!s[1])
            {
                out = 0xFFFD;
                return s + 1;
            }  // Truncated sequence
            const unsigned char c1 = (unsigned char)s[1];
            if ((c1 & 0xC0) != 0x80)
            {
                out = 0xFFFD;
                return s + 1;
            }  // Invalid continuation byte
            // Decode: take 5 bits from first byte, 6 bits from second
            out = ((c & 0x1F) << 6) | (c1 & 0x3F);
            return s + 2;
        }

        // 3-byte sequence (0xE0-0xEF): 1110xxxx 10xxxxxx 10xxxxxx
        if (c < 0xF0)
        {
            if (!s[1] || !s[2])
            {
                out = 0xFFFD;
                return s + 1;
            }
            const unsigned char c1 = (unsigned char)s[1];
            const unsigned char c2 = (unsigned char)s[2];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
            {
                out = 0xFFFD;
                return s + 1;
            }
            // Decode: 4 bits from first, 6 bits each from second and third
            out = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            return s + 3;
        }

        // 4-byte sequence (0xF0-0xF7): 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        if (c < 0xF8)
        {
            if (!s[1] || !s[2] || !s[3])
            {
                out = 0xFFFD;
                return s + 1;
            }
            const unsigned char c1 = (unsigned char)s[1];
            const unsigned char c2 = (unsigned char)s[2];
            const unsigned char c3 = (unsigned char)s[3];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
            {
                out = 0xFFFD;
                return s + 1;
            }
            // Decode: 3 bits from first, 6 bits each from second, third, and fourth
            out = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            return s + 4;
        }

        // Invalid UTF-8 sequence
        out = 0xFFFD;  // Replacement character
        return s + 1;
    }

    // Get byte length of UTF-8 character at position
    static size_t Utf8CharLen(const char* s)
    {
        if (!s || !*s) return 0;
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) return 1;
        if (c < 0xE0) return 2;
        if (c < 0xF0) return 3;
        if (c < 0xF8) return 4;
        return 1;  // Invalid, treat as single byte
    }

    // Calculate tight vertical bounds of text glyphs
    static void CalcTightYBoundsFromTop(ImFont *font, float fontSize, const char *text, float &outTop, float &outBottom)
    {
        // Initialize to extremes so first glyph will replace them
        outTop = +FLT_MAX;
        outBottom = -FLT_MAX;

        if (!font || !text || !*text)
        {
            outTop = 0.0f;
            outBottom = 0.0f;
            return;
        }

        // Calculate scale factor from font's native size to desired size
        const float scale = fontSize / font->FontSize;

        // Iterate through each character in the UTF-8 string
        for (const char *p = text; *p;)
        {
            unsigned int cp;
            p = Utf8Next(p, cp);

            // Skip newlines (shouldn't be in our text, but be safe)
            if (cp == '\n' || cp == '\r')
                continue;

            // Get glyph data for this character
            const ImFontGlyph *g = font->FindGlyph((ImWchar)cp);
            if (!g)
                continue; // Character not in font

            // Y0 and Y1 are relative to the baseline
            // Y0 is negative (above baseline), Y1 is positive (below baseline)
            // We want the tightest bounds, so min of Y0 (most negative), max of Y1 (most positive)
            outTop = std::min(outTop, g->Y0 * scale);
            outBottom = std::max(outBottom, g->Y1 * scale);
        }

        // If no valid glyphs were found, return zeros
        if (outTop == +FLT_MAX)
        {
            outTop = 0.0f;
            outBottom = 0.0f;
        }
    }

    /// Cache entry for smooth actor nameplate animations.
    /// Stores smoothed values and state for position, alpha, and effects.
    struct ActorCache
    {
        ImVec2 smooth{};                       ///< Smoothed screen position (result of moving average)
        float alphaSmooth = 1.0f;              ///< Smoothed alpha for fade transitions
        float textSizeScale = 1.0f;            ///< Smoothed font scale for distance-based sizing
        float occlusionSmooth = 1.0f;          ///< Smoothed occlusion (1.0=visible, 0.0=hidden)

        bool initialized = false;              ///< True after first frame of data
        uint32_t lastSeenFrame = 0;            ///< Frame counter when actor was last in snapshot

        uint32_t lastOcclusionCheckFrame = 0;  ///< Frame when LOS was last checked
        bool cachedOccluded = false;           ///< Cached LOS result
        bool wasOccluded = false;              ///< Previous frame's occlusion state

        static constexpr int kHistorySize = RenderConstants::kPositionHistorySize;
        ImVec2 posHistory[kHistorySize]{};
        int historyIndex = 0;
        bool historyFilled = false;

        float typewriterTime = 0.0f;      ///< Seconds since actor first appeared
        bool typewriterComplete = false;  ///< True when reveal animation finished

        std::string cachedName;           ///< Last known name (to detect changes)

        /// Add position sample to history and return smoothed average.
        ImVec2 AddAndGetSmoothed(const ImVec2& pos)
        {
            posHistory[historyIndex] = pos;
            historyIndex = (historyIndex + 1) % kHistorySize;
            if (historyIndex == 0) historyFilled = true;

            int count = historyFilled ? kHistorySize : historyIndex;
            if (count == 0) return pos;

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
        Neutral,      ///< Neutral NPCs (white/gray)
        Enemy,        ///< Hostile NPCs (red)
        AllyOrFriend  ///< Friendly/allied NPCs (blue)
    };

    // Data for rendering a single actor's nameplate
    struct ActorDrawData
    {
        uint32_t formID{0};                       ///< Actor's form ID (unique identifier)
        RE::NiPoint3 worldPos{};                  ///< World position above actor's head
        std::string name;                         ///< Display name (capitalized)
        uint16_t level{0};                        ///< Actor's level
        float distToPlayer{0.0f};                 ///< Distance to player in units
        Disposition dispo{Disposition::Neutral};  ///< Disposition towards player
        bool isPlayer{false};                     ///< Whether this is the player character
        bool isOccluded{false};                   ///< Whether actor is occluded from view
    };

    /// Encapsulates all mutable renderer state into a single struct.
    struct RendererState
    {
        // Cache
        std::unordered_map<uint32_t, ActorCache> cache;
        uint32_t frame = 0;

        // Snapshot & Thread Safety
        std::vector<ActorDrawData> snapshot;
        std::mutex snapshotLock;
        std::atomic<bool> updateQueued{false};

        // Frame State
        bool wasInInvalidState = true;
        int postLoadCooldown = 0;

        // Debug Stats
        DebugOverlay::Stats debugStats;
        float lastDebugUpdateTime = 0.0f;
        int updateCounter = 0;
        int lastUpdateCount = 0;

        // Hot Reload
        bool reloadKeyWasDown = false;
        float lastReloadTime = -10.0f;

        // Overlay Flags
        std::atomic<bool> allowOverlay{false};
        std::atomic<bool> manualEnabled{true};
    };

    static RendererState s_state;

    static constexpr float kReloadNotificationDuration = RenderConstants::kReloadNotificationDuration;

    /// Per-frame overlap Y offsets, keyed by form ID
    static std::unordered_map<uint32_t, float>& OverlapOffsets() {
        static std::unordered_map<uint32_t, float> offsets;
        return offsets;
    }

    bool IsOverlayAllowedRT() {
        return s_state.manualEnabled.load(std::memory_order_acquire) &&
               s_state.allowOverlay.load(std::memory_order_acquire);
    }

    bool ToggleEnabled() {
        bool newState = !s_state.manualEnabled.load(std::memory_order_acquire);
        s_state.manualEnabled.store(newState, std::memory_order_release);
        return newState;
    }

    // Get player character
    static RE::Actor *GetPlayer()
    {
        return RE::PlayerCharacter::GetSingleton();
    }

    // Capitalize text and trim whitespace
    static std::string Capitalize(const char *text)
    {
        if (!text || !*text)
            return "";
        std::string s = text;

        // Trim leading/trailing whitespace
        size_t first = s.find_first_not_of(" \t\r\n");
        if (std::string::npos == first)
            return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        s = s.substr(first, (last - first + 1));

        // Title Case
        bool newWord = true;
        for (auto &c : s)
        {
            if (isspace(static_cast<unsigned char>(c)))
            {
                newWord = true;
            }
            else if (newWord)
            {
                c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
                newWord = false;
            }
        }
        return s;
    }

    // Get actor's fight reaction towards player
    static RE::FIGHT_REACTION GetReactionToPlayer(RE::Actor *a_actor, RE::Actor *a_player)
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
    static Disposition GetDisposition(RE::Actor *a, RE::Actor *player)
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
    bool WorldToScreen(const RE::NiPoint3 &worldPos, RE::NiPoint3 &screenPos, RE::NiPoint3 *cameraPosOut = nullptr)
    {
        // Get the main world camera
        auto *cam = RE::Main::WorldRootCamera();
        if (!cam)
        {
            return false;
        }

        // Get camera runtime data containing transformation matrices
        const auto &rt = cam->GetRuntimeData();    // Contains worldToCam matrix
        const auto &rt2 = cam->GetRuntimeData2();  // Contains viewport info

        // Optionally output camera position (used for distance calculations elsewhere)
        if (cameraPosOut)
        {
            *cameraPosOut = cam->world.translate;
        }

        // Project world coordinates to normalized screen coordinates [0,1]
        float x = 0.0f, y = 0.0f, z = 0.0f;

        // WorldPtToScreenPt3 returns false if point is behind camera or outside frustum
        // The last parameter (1e-5f) is the epsilon for near-plane clipping
        if (!RE::NiCamera::WorldPtToScreenPt3(rt.worldToCam, rt2.port, worldPos, x, y, z, 1e-5f))
        {
            return false;  // Point not visible (behind camera or clipped)
        }

        // Get actual screen resolution
        auto *renderer = RE::BSGraphics::Renderer::GetSingleton();
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

    /// Check occlusion for an actor, using cached results when available.
    static void UpdateOcclusionForActor(ActorDrawData& d, RE::Actor* a, RE::Actor* player)
    {
        auto cacheIt = s_state.cache.find(d.formID);

        // Use cached result if fresh enough
        if (cacheIt != s_state.cache.end() && cacheIt->second.initialized) {
            uint32_t framesSince = s_state.frame - cacheIt->second.lastOcclusionCheckFrame;
            if (framesSince < static_cast<uint32_t>(Settings::OcclusionCheckInterval)) {
                d.isOccluded = cacheIt->second.cachedOccluded;
                return;
            }
        }

        // Perform fresh occlusion check using nameplate world position
        d.isOccluded = Occlusion::IsActorOccluded(a, player, d.worldPos);

        // Update cache with the fresh result
        if (cacheIt != s_state.cache.end()) {
            cacheIt->second.lastOcclusionCheckFrame = s_state.frame;
            cacheIt->second.cachedOccluded = d.isOccluded;
        }
    }

    static void UpdateSnapshot_GameThread()
    {
        // RAII struct to ensure the update flag is cleared when function exits
        struct ClearFlag
        {
            ~ClearFlag() { s_state.updateQueued.store(false); }
        } _;

        // Check if we're allowed to draw the overlay (not in menus, loading, etc.)
        const bool allow = CanDrawOverlay();
        s_state.allowOverlay.store(allow, std::memory_order_release);

        if (!allow)
        {
            std::lock_guard<std::mutex> lock(s_state.snapshotLock);
            s_state.snapshot.clear();
            return;
        }

        auto *player = GetPlayer();
        auto *pl = RE::ProcessLists::GetSingleton();
        if (!player || !pl)
        {
            std::lock_guard<std::mutex> lock(s_state.snapshotLock);
            s_state.snapshot.clear();
            return;
        }

        constexpr int kMaxActors = RenderConstants::kMaxActors;
        constexpr int kMaxScan = RenderConstants::kMaxScan;
        const float kMaxDistSq = Settings::MaxScanDistance * Settings::MaxScanDistance;

        static std::vector<ActorDrawData> tempBuf;
        tempBuf.clear();
        tempBuf.reserve(kMaxActors);

        const auto playerPos = player->GetPosition();

        // Include the player character first
        if (!Settings::HidePlayer)
        {
            ActorDrawData d;
            d.formID = player->GetFormID();
            d.level = player->GetLevel();
            const char *rawName = player->GetDisplayFullName();
            d.name = rawName ? Capitalize(rawName) : "Player";
            d.worldPos = playerPos;
            d.worldPos.z += player->GetHeight() + Settings::VerticalOffset;
            d.distToPlayer = 0.0f;
            d.isPlayer = true;
            tempBuf.push_back(std::move(d));
        }

        int added = 1;
        int scanned = 0;

        for (auto &h : pl->highActorHandles)
        {
            if (added >= kMaxActors || scanned >= kMaxScan)
                break;
            ++scanned;

            auto aSP = h.get();
            auto *a = aSP.get();
            if (!a || a == player || a->IsDead())
                continue;

            // Skip creatures/animals if HideCreatures is enabled.
            // Prefer ActorTypeNPC on actor, then base, then race for robustness across mods.
            if (Settings::HideCreatures)
            {
                static RE::BGSKeyword *npcKeyword = nullptr;
                if (!npcKeyword)
                {
                    if (auto* dataHandler = RE::TESDataHandler::GetSingleton(); dataHandler) {
                        npcKeyword = dataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
                    }
                }

                bool isHumanoidNPC = false;
                if (npcKeyword) {
                    isHumanoidNPC = a->HasKeyword(npcKeyword);
                    if (!isHumanoidNPC) {
                        if (auto* actorBase = a->GetActorBase(); actorBase) {
                            isHumanoidNPC = actorBase->HasKeyword(npcKeyword);
                            if (!isHumanoidNPC) {
                                if (auto* race = actorBase->GetRace(); race) {
                                    isHumanoidNPC = race->HasKeyword(npcKeyword);
                                }
                            }
                        }
                    }
                }

                if (npcKeyword && !isHumanoidNPC)
                    continue;
            }

            const float distSq = playerPos.GetSquaredDistance(a->GetPosition());
            if (distSq > kMaxDistSq)
                continue;

            ActorDrawData d;
            d.formID = a->GetFormID();
            d.level = a->GetLevel();
            const char *rawName = a->GetDisplayFullName();
            d.name = rawName ? Capitalize(rawName) : "";
            d.worldPos = a->GetPosition();
            d.worldPos.z += a->GetHeight() + Settings::VerticalOffset;
            d.distToPlayer = std::sqrt(distSq);
            d.dispo = GetDisposition(a, player);
            d.isPlayer = false;

            if (Settings::EnableOcclusionCulling)
                UpdateOcclusionForActor(d, a, player);

            tempBuf.push_back(std::move(d));
            ++added;
        }

        {
            std::lock_guard<std::mutex> lock(s_state.snapshotLock);
            s_state.snapshot = tempBuf;
        }
    }

    static void QueueSnapshotUpdate_RenderThread()
    {
        // Check if an update is already queued
        // If exchange returns true, an update is already pending, so skip
        if (s_state.updateQueued.exchange(true))
        {
            return;  // Already queued, don't queue again
        }

        // Schedule the update task on the game thread
        // We can't iterate actors safely from the render thread, so we
        // use SKSE's task interface to run on the game thread instead
        if (auto *task = SKSE::GetTaskInterface())
        {
            task->AddTask([](){ UpdateSnapshot_GameThread(); });
        }
        else
        {
            // Task interface not available, clear the flag
            s_state.updateQueued.store(false);
        }
    }

    static void PruneCacheToSnapshot(const std::vector<ActorDrawData> &snap)
    {
        // Grace period prevents jitter when actors briefly leave the snapshot
        constexpr uint32_t kCacheGraceFrames = RenderConstants::kCacheGraceFrames;

        for (auto it = s_state.cache.begin(); it != s_state.cache.end();)
        {
            bool inSnapshot = false;
            for (auto &d : snap)
            {
                if (d.formID == it->first)
                {
                    inSnapshot = true;
                    it->second.lastSeenFrame = s_state.frame;  // Update last seen
                    break;
                }
            }

            if (!inSnapshot)
            {
                // Check if grace period has expired
                uint32_t framesSinceLastSeen = s_state.frame - it->second.lastSeenFrame;
                if (framesSinceLastSeen > kCacheGraceFrames)
                {
                    it = s_state.cache.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // Compute blend factor for frame-rate independent exponential smoothing.
    // Returns alpha in [0,1] for use with: current = lerp(current, target, alpha)
    static float ExpApproachAlpha(float dt, float settleTime, float epsilon = 0.01f)
    {
        dt = std::max(0.0f, dt);
        settleTime = std::max(1e-5f, settleTime);
        return std::clamp(1.0f - std::pow(epsilon, dt / settleTime), 0.0f, 1.0f);
    }

    static void ApplyTextEffect(
        ImDrawList *drawList,
        ImFont *font,
        float fontSize,
        ImVec2 pos,
        const char *text,
        const Settings::EffectParams &effect,
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
            // Solid color with outline
            TextEffects::AddTextOutline4(drawList, font, fontSize, pos, text,
                                         colL, outlineColor, outlineWidth);
            break;

        case Settings::EffectType::Gradient:
            TextEffects::AddTextOutline4Gradient(drawList, font, fontSize, pos, text,
                                                 colL, colR, outlineColor, outlineWidth);
            break;

        case Settings::EffectType::VerticalGradient:
            TextEffects::AddTextOutline4VerticalGradient(drawList, font, fontSize, pos, text,
                                                         colL, colR, outlineColor, outlineWidth);
            break;

        case Settings::EffectType::DiagonalGradient:
            TextEffects::AddTextOutline4DiagonalGradient(drawList, font, fontSize, pos, text,
                                                         colL, colR, ImVec2(effect.param1, effect.param2), outlineColor, outlineWidth);
            break;

        case Settings::EffectType::RadialGradient:
            TextEffects::AddTextOutline4RadialGradient(drawList, font, fontSize, pos, text,
                                                       colL, colR, outlineColor, outlineWidth, effect.param1);
            break;

        case Settings::EffectType::Shimmer:
            TextEffects::AddTextOutline4Shimmer(drawList, font, fontSize, pos, text,
                                                colL, colR, highlight, outlineColor, outlineWidth,
                                                phase01, effect.param1, effect.param2 > 0.0f ? effect.param2 * strength : strength);
            break;

        case Settings::EffectType::ChromaticShimmer:
            TextEffects::AddTextOutline4ChromaticShimmer(drawList, font, fontSize, pos, text,
                                                         colL, colR, highlight, outlineColor, outlineWidth,
                                                         phase01, effect.param1, effect.param2 * strength, effect.param3 * textSizeScale, effect.param4);
            break;

        case Settings::EffectType::PulseGradient:
        {
            float time = (float)ImGui::GetTime();
            TextEffects::AddTextOutline4PulseGradient(drawList, font, fontSize, pos, text,
                                                      colL, colR, time, effect.param1, effect.param2 * strength, outlineColor, outlineWidth);
        }
        break;

        case Settings::EffectType::RainbowWave:
            // Always draw outline first for consistency
            TextEffects::AddTextOutline4RainbowWave(drawList, font, fontSize, pos, text,
                                                    effect.param1, effect.param2, effect.param3, effect.param4, effect.param5,
                                                    alpha, outlineColor, outlineWidth, effect.useWhiteBase);
            break;

        case Settings::EffectType::ConicRainbow:
            TextEffects::AddTextOutline4ConicRainbow(drawList, font, fontSize, pos, text,
                                                     effect.param1, effect.param2, effect.param3, effect.param4, alpha,
                                                     outlineColor, outlineWidth, effect.useWhiteBase);
            break;

        case Settings::EffectType::Aurora:
            // Aurora effect: param1=speed, param2=waves, param3=intensity, param4=sway
            // Creates flowing northern lights effect with left/right colors
            TextEffects::AddTextOutline4Aurora(drawList, font, fontSize, pos, text,
                                               colL, colR, outlineColor, outlineWidth,
                                               effect.param1 > 0.0f ? effect.param1 : 0.5f,
                                               effect.param2 > 0.0f ? effect.param2 : 3.0f,
                                               effect.param3 > 0.0f ? effect.param3 : 1.0f,
                                               effect.param4 > 0.0f ? effect.param4 : 0.3f);
            break;

        case Settings::EffectType::Sparkle:
            // Sparkle effect: param1=density, param2=speed, param3=intensity
            // Uses highlight color for sparkles
            TextEffects::AddTextOutline4Sparkle(drawList, font, fontSize, pos, text,
                                                colL, colR, highlight, outlineColor, outlineWidth,
                                                effect.param1 > 0.0f ? effect.param1 : 0.3f,
                                                effect.param2 > 0.0f ? effect.param2 : 2.0f,
                                                effect.param3 > 0.0f ? effect.param3 * strength : strength);
            break;

        case Settings::EffectType::Plasma:
            // Plasma effect: param1=freq1, param2=freq2, param3=speed
            TextEffects::AddTextOutline4Plasma(drawList, font, fontSize, pos, text,
                                               colL, colR, outlineColor, outlineWidth,
                                               effect.param1 > 0.0f ? effect.param1 : 2.0f,
                                               effect.param2 > 0.0f ? effect.param2 : 3.0f,
                                               effect.param3 > 0.0f ? effect.param3 : 0.5f);
            break;

        case Settings::EffectType::Scanline:
            // Scanline effect: param1=speed, param2=width, param3=intensity
            // Uses highlight color for scanline
            TextEffects::AddTextOutline4Scanline(drawList, font, fontSize, pos, text,
                                                 colL, colR, highlight, outlineColor, outlineWidth,
                                                 effect.param1 > 0.0f ? effect.param1 : 0.5f,
                                                 effect.param2 > 0.0f ? effect.param2 : 0.15f,
                                                 effect.param3 > 0.0f ? effect.param3 * strength : strength);
            break;
        }
    }

    // =========================================================================
    // Data structures for DrawLabel sub-functions
    // =========================================================================

    /// Describes one formatted text segment on the main nameplate line.
    struct RenderSeg
    {
        std::string text;         ///< Formatted text to display
        std::string displayText;  ///< Text after typewriter truncation
        bool isLevel;             ///< Whether to use level font
        ImFont *font;             ///< Font to use for rendering
        float fontSize;           ///< Scaled font size
        ImVec2 size;              ///< Measured size of this segment
        ImVec2 displaySize;       ///< Measured size of displayText
    };

    /// All color, tier, and effect data computed once per label.
    struct LabelStyle
    {
        int tierIdx;
        const Settings::TierDefinition* tier;
        const Settings::SpecialTitleDefinition* specialTitle;

        ImU32 colL, colR;               // Name gradient (packed)
        ImU32 colLTitle, colRTitle;      // Title gradient
        ImU32 colLLevel, colRLevel;      // Level gradient
        ImU32 highlight;                 // Shimmer / sparkle highlight
        ImU32 outlineColor;              // Black outline (alpha-scaled)
        ImU32 shadowColor;               // Black shadow  (alpha-scaled)

        ImVec4 Lc, Rc;                  // Tier left/right (pasteled)
        ImVec4 LcName, RcName;          // Washed name colors (float, for glow)
        ImVec4 LcTitle, RcTitle;        // Washed title colors (float, for glow)
        ImVec4 LcLevel, RcLevel;        // Softened level colors (float, for glow)
        ImVec4 dispoCol;                // Disposition color
        ImVec4 specialGlowColor;        // Special title glow

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
        float outlineWidth;              // Primary (= nameOutlineWidth)

        // Stored for deferred outline width computation
        float baseOutlineWidth;
        float distToPlayer;

        float CalcOutlineWidth(float fontSize) const {
            float w = baseOutlineWidth * (fontSize / Settings::NameFontSize);
            if (Settings::Visual().EnableDistanceOutlineScale) {
                float distT = TextEffects::Saturate(
                    (distToPlayer - Settings::FadeStartDistance) /
                    (Settings::FadeEndDistance - Settings::FadeStartDistance));
                float distMul = Settings::Visual().OutlineDistanceMin +
                                (Settings::Visual().OutlineDistanceMax - Settings::Visual().OutlineDistanceMin) * distT;
                w *= distMul;
            }
            return w;
        }
    };

    /// Text measurement and position data for a label.
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

    // =========================================================================
    // DrawLabel helper functions
    // =========================================================================

    /// Desaturate a color toward white.
    static ImVec4 WashColor(ImVec4 base)
    {
        const float wash = Settings::ColorWashAmount;
        return ImVec4(
            base.x + (1.0f - base.x) * wash,
            base.y + (1.0f - base.y) * wash,
            base.z + (1.0f - base.z) * wash,
            base.w);
    }

    /// Replace %n, %l, %t placeholders in a format string.
    static std::string FormatString(const std::string &fmt, const std::string_view nameVal, int levelVal, const char *titleVal = nullptr)
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

    /// Compute all color, tier, and effect data for a label.
    static LabelStyle ComputeLabelStyle(const ActorDrawData &d, float alpha, float time)
    {
        LabelStyle style{};
        style.alpha = alpha;

        // Disposition color
        if (d.dispo == Disposition::Enemy)
            style.dispoCol = WashColor(ImVec4(0.9f, 0.2f, 0.2f, alpha));
        else if (d.dispo == Disposition::AllyOrFriend)
            style.dispoCol = WashColor(ImVec4(0.2f, 0.6f, 1.0f, alpha));
        else
            style.dispoCol = WashColor(ImVec4(0.9f, 0.9f, 0.9f, alpha));

        const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);

        // Find matching tier
        style.tierIdx = 0;
        for (size_t i = 0; i < Settings::Tiers.size(); ++i)
        {
            if (lv >= Settings::Tiers[i].minLevel && lv <= Settings::Tiers[i].maxLevel)
            {
                style.tierIdx = static_cast<int>(i);
                break;
            }
        }
        style.tierIdx = std::clamp(style.tierIdx, 0, static_cast<int>(Settings::Tiers.size()) - 1);
        style.tier = &Settings::Tiers[style.tierIdx];
        const Settings::TierDefinition &tier = *style.tier;

        // Tier effect gating
        style.tierAllowsGlow = !Settings::Visual().EnableTierEffectGating || style.tierIdx >= Settings::Visual().GlowMinTier;
        style.tierAllowsParticles = !Settings::Visual().EnableTierEffectGating || style.tierIdx >= Settings::Visual().ParticleMinTier;
        style.tierAllowsOrnaments = !Settings::Visual().EnableTierEffectGating || style.tierIdx >= Settings::Visual().OrnamentMinTier;

        // Special title matching
        style.specialTitle = nullptr;
        {
            std::string nameLower = d.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            std::vector<const Settings::SpecialTitleDefinition*> sortedSpecials;
            for (const auto& st : Settings::SpecialTitles) {
                if (!st.keyword.empty()) {
                    sortedSpecials.push_back(&st);
                }
            }
            std::sort(sortedSpecials.begin(), sortedSpecials.end(),
                      [](const auto* a, const auto* b) { return a->priority > b->priority; });

            for (const auto* st : sortedSpecials) {
                std::string keywordLower = st->keyword;
                std::transform(keywordLower.begin(), keywordLower.end(), keywordLower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (nameLower.find(keywordLower) != std::string::npos) {
                    style.specialTitle = st;
                    break;
                }
            }
        }

        // Level position within tier [0, 1]
        float levelT = 0.0f;
        if (tier.maxLevel > tier.minLevel)
        {
            levelT = (lv <= tier.minLevel)
            ? 0.0f
            : (lv >= tier.maxLevel)
                ? 1.0f
                : (float)(lv - tier.minLevel) / (float)(tier.maxLevel - tier.minLevel);
        }
        levelT = std::clamp(levelT, 0.0f, 1.0f);

        const bool under100 = (lv < 100);
        const float tierIntensity = under100 ? 0.5f : 1.0f;

        // Pastelize tier colors
        auto Pastelize = [&](const float *c) -> ImVec4
        {
            const float t = Settings::NameColorMix + (1.0f - Settings::NameColorMix) * levelT;
            return ImVec4(
                1.0f + (c[0] - 1.0f) * t,
                1.0f + (c[1] - 1.0f) * t,
                1.0f + (c[2] - 1.0f) * t,
                1.0f);
        };

        style.Lc = Pastelize(tier.leftColor);
        style.Rc = Pastelize(tier.rightColor);

        style.effectAlpha = alpha * tierIntensity * (Settings::EffectAlphaMin + Settings::EffectAlphaMax * levelT);

        auto MixToWhite = [](ImVec4 c, float amount)
        {
            amount = std::clamp(amount, 0.0f, 1.0f);
            return ImVec4(
                1.0f + (c.x - 1.0f) * amount,
                1.0f + (c.y - 1.0f) * amount,
                1.0f + (c.z - 1.0f) * amount,
                c.w);
        };

        const float baseColorAmount = under100 ? (0.35f + 0.65f * tierIntensity) : 1.0f;

        style.LcLevel = MixToWhite(style.Lc, baseColorAmount);
        style.RcLevel = MixToWhite(style.Rc, baseColorAmount);
        style.LcName = WashColor(style.LcLevel);
        style.RcName = WashColor(style.RcLevel);
        style.LcTitle = WashColor(style.LcName);
        style.RcTitle = WashColor(style.RcName);

        style.specialGlowColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

        if (style.specialTitle) {
            const auto* st = style.specialTitle;
            ImVec4 specialCol(st->color[0], st->color[1], st->color[2], 1.0f);
            style.specialGlowColor = ImVec4(st->glowColor[0], st->glowColor[1], st->glowColor[2], 1.0f);

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
        style.colL = ImGui::ColorConvertFloat4ToU32(ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, alpha));
        style.colR = ImGui::ColorConvertFloat4ToU32(ImVec4(style.RcName.x, style.RcName.y, style.RcName.z, alpha));

        style.titleAlpha = alpha * Settings::Visual().TitleAlphaMultiplier;
        style.levelAlpha = alpha * Settings::Visual().LevelAlphaMultiplier;

        style.colLTitle = ImGui::ColorConvertFloat4ToU32(ImVec4(style.LcTitle.x, style.LcTitle.y, style.LcTitle.z, style.titleAlpha));
        style.colRTitle = ImGui::ColorConvertFloat4ToU32(ImVec4(style.RcTitle.x, style.RcTitle.y, style.RcTitle.z, style.titleAlpha));
        style.colLLevel = ImGui::ColorConvertFloat4ToU32(ImVec4(style.LcLevel.x, style.LcLevel.y, style.LcLevel.z, style.levelAlpha));
        style.colRLevel = ImGui::ColorConvertFloat4ToU32(ImVec4(style.RcLevel.x, style.RcLevel.y, style.RcLevel.z, style.levelAlpha));
        style.highlight = ImGui::ColorConvertFloat4ToU32(ImVec4(tier.highlightColor[0], tier.highlightColor[1], tier.highlightColor[2], style.effectAlpha));

        const float outlineAlpha = TextEffects::Saturate(alpha);
        const float shadowAlpha = TextEffects::Saturate(alpha * 0.75f);
        style.outlineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, outlineAlpha));
        style.shadowColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, shadowAlpha));

        // Outline width data (actual widths computed after font sizes are known)
        style.baseOutlineWidth = Settings::OutlineWidthMin + Settings::OutlineWidthMax;
        style.distToPlayer = d.distToPlayer;

        // Animation
        auto frac = [](float x) { return x - std::floor(x); };

        float tierAnimSpeed = Settings::AnimSpeedLowTier;
        if (!Settings::Tiers.empty())
        {
            float tierRatio = static_cast<float>(style.tierIdx) / static_cast<float>(Settings::Tiers.size() - 1);
            if (tierRatio >= 0.9f)
                tierAnimSpeed = Settings::AnimSpeedHighTier;
            else if (tierRatio >= 0.8f)
                tierAnimSpeed = Settings::AnimSpeedMidTier;
        }
        if (under100)
            tierAnimSpeed *= 0.75f;

        const float phaseSeed = (d.formID & 1023) / 1023.0f;
        style.phase01 = frac(time * tierAnimSpeed + phaseSeed);

        style.strength = tierIntensity * (Settings::StrengthMin + Settings::StrengthMax * levelT);

        return style;
    }

    /// Measure text and compute all positions for a label.
    static LabelLayout ComputeLabelLayout(const ActorDrawData &d, ActorCache &entry, const LabelStyle &style, float textSizeScale)
    {
        LabelLayout layout{};

        // Typewriter character count
        int typewriterCharsToShow = -1;
        if (Settings::EnableTypewriter && !entry.typewriterComplete)
        {
            float effectiveTime = entry.typewriterTime - Settings::TypewriterDelay;
            if (effectiveTime > 0.0f)
                typewriterCharsToShow = static_cast<int>(effectiveTime * Settings::TypewriterSpeed);
            else
                typewriterCharsToShow = 0;
        }

        // Fonts
        layout.fontName = ImGui::GetIO().Fonts->Fonts[0];
        layout.fontLevel = ImGui::GetIO().Fonts->Fonts[1];
        layout.fontTitle = ImGui::GetIO().Fonts->Fonts[2];

        layout.nameFontSize = layout.fontName->FontSize * textSizeScale;
        layout.levelFontSize = layout.fontLevel->FontSize * textSizeScale;
        layout.titleFontSize = layout.fontTitle->FontSize * textSizeScale;

        const float outlineWidth = style.outlineWidth;

        const char *safeName = d.name.empty() ? " " : d.name.c_str();

        // Build segments
        layout.mainLineWidth = 0.0f;
        layout.mainLineHeight = 0.0f;

        const auto &fmtList = Settings::DisplayFormat.empty() ? std::vector<Settings::Segment>{{"%n", false}, {" Lv.%l", true}} : Settings::DisplayFormat;

        int totalCharsProcessed = 0;

        for (const auto &fmt : fmtList)
        {
            RenderSeg seg;
            seg.text = FormatString(fmt.format, safeName, d.level);
            seg.isLevel = fmt.useLevelFont;
            seg.font = seg.isLevel ? layout.fontLevel : layout.fontName;
            seg.fontSize = seg.isLevel ? layout.levelFontSize : layout.nameFontSize;
            seg.size = seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, 0.0f, seg.text.c_str());

            if (typewriterCharsToShow >= 0)
            {
                size_t segCharCount = Utf8CharCount(seg.text.c_str());
                int charsRemaining = typewriterCharsToShow - totalCharsProcessed;
                if (charsRemaining <= 0)
                    seg.displayText = "";
                else if (static_cast<size_t>(charsRemaining) >= segCharCount)
                    seg.displayText = seg.text;
                else
                    seg.displayText = Utf8Truncate(seg.text.c_str(), charsRemaining);
                totalCharsProcessed += static_cast<int>(segCharCount);
            }
            else
            {
                seg.displayText = seg.text;
            }

            seg.displaySize = seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, 0.0f, seg.displayText.c_str());

            layout.segments.push_back(seg);
            layout.mainLineWidth += seg.size.x;
            if (seg.size.y > layout.mainLineHeight)
                layout.mainLineHeight = seg.size.y;
        }

        layout.segmentPadding = Settings::SegmentPadding;
        if (!layout.segments.empty())
            layout.mainLineWidth += (layout.segments.size() - 1) * layout.segmentPadding;

        // Title
        const char* titleToUse = style.specialTitle ? style.specialTitle->displayTitle.c_str() : style.tier->title.c_str();
        layout.titleStr = FormatString(Settings::TitleFormat, safeName, d.level, titleToUse);
        layout.titleDisplayStr = layout.titleStr;

        if (typewriterCharsToShow >= 0)
        {
            size_t titleCharCount = Utf8CharCount(layout.titleStr.c_str());
            int charsRemainingForTitle = typewriterCharsToShow - totalCharsProcessed;
            if (charsRemainingForTitle <= 0)
                layout.titleDisplayStr = "";
            else if (static_cast<size_t>(charsRemainingForTitle) >= titleCharCount)
                layout.titleDisplayStr = layout.titleStr;
            else
                layout.titleDisplayStr = Utf8Truncate(layout.titleStr.c_str(), charsRemainingForTitle);
            totalCharsProcessed += static_cast<int>(titleCharCount);

            if (!entry.typewriterComplete && typewriterCharsToShow >= totalCharsProcessed)
                entry.typewriterComplete = true;
        }

        const char *titleText = layout.titleStr.c_str();

        // Tight vertical bounds
        float titleTop = 0.0f, titleBottom = 0.0f;
        if (titleText && *titleText)
            CalcTightYBoundsFromTop(layout.fontTitle, layout.titleFontSize, titleText, titleTop, titleBottom);
        layout.titleSize = layout.fontTitle->CalcTextSizeA(layout.titleFontSize, FLT_MAX, 0.0f, titleText);

        float mainTop = +FLT_MAX;
        float mainBottom = -FLT_MAX;
        bool any = false;
        for (const auto &seg : layout.segments)
        {
            float sTop = 0.0f, sBottom = 0.0f;
            CalcTightYBoundsFromTop(seg.font, seg.fontSize, seg.text.c_str(), sTop, sBottom);
            float vOffset = (layout.mainLineHeight - seg.size.y) * 0.5f;
            mainTop = std::min(mainTop, vOffset + sTop);
            mainBottom = std::max(mainBottom, vOffset + sBottom);
            any = true;
        }
        if (!any) { mainTop = 0.0f; mainBottom = 0.0f; }

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
                layout.startPos.y += oIt->second;
        }

        layout.totalWidth = std::max(layout.mainLineWidth, layout.titleSize.x);

        layout.nameplateTop = layout.startPos.y + layout.titleY + titleTop;
        layout.nameplateBottom = layout.startPos.y + layout.mainLineY + mainBottom;
        layout.nameplateLeft = layout.startPos.x - layout.totalWidth * 0.5f;
        layout.nameplateRight = layout.startPos.x + layout.totalWidth * 0.5f;
        layout.nameplateWidth = layout.totalWidth;
        layout.nameplateHeight = layout.nameplateBottom - layout.nameplateTop;
        layout.nameplateCenter = ImVec2(layout.startPos.x, (layout.nameplateTop + layout.nameplateBottom) * 0.5f);

        return layout;
    }

    /// Draw particle aura effects behind the nameplate.
    static void DrawParticles(ImDrawList *dl, const ActorDrawData &d, const LabelStyle &style, const LabelLayout &layout, float lodEffectsFactor, float time, ImDrawListSplitter *splitter)
    {
        splitter->SetCurrentChannel(dl, 0);  // Back layer: particles
        const Settings::TierDefinition &tier = *style.tier;
        const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);

        bool tierHasParticles = !tier.particleTypes.empty() && tier.particleTypes != "None";
        bool globalHasParticles = Settings::EnableOrbs || Settings::EnableWisps || Settings::EnableRunes ||
                                  Settings::EnableSparks || Settings::EnableStars;
        bool hasAnyParticles = tierHasParticles || globalHasParticles;
        bool showParticles = ((Settings::EnableParticleAura && hasAnyParticles && style.tierAllowsParticles)
                          || (style.specialTitle && style.specialTitle->forceParticles))
                          && lodEffectsFactor > 0.01f;
        if (!showParticles)
            return;

        ImU32 particleColor;
        if (style.specialTitle) {
            particleColor = ImGui::ColorConvertFloat4ToU32(
                ImVec4(style.specialTitle->color[0], style.specialTitle->color[1], style.specialTitle->color[2], 1.0f));
        } else {
            particleColor = ImGui::ColorConvertFloat4ToU32(
                ImVec4(tier.highlightColor[0], tier.highlightColor[1], tier.highlightColor[2], 1.0f));
        }

        float spreadX = (layout.nameplateWidth * 0.5f + Settings::ParticleSpread * 1.4f);
        float spreadY = (layout.nameplateHeight * 0.5f + Settings::ParticleSpread * 1.1f);

        int particleCount = (tier.particleCount > 0) ? tier.particleCount : Settings::ParticleCount;
        float tierBoost = 0.0f;
        if (Settings::Tiers.size() > 1)
            tierBoost = static_cast<float>(style.tierIdx) / static_cast<float>(Settings::Tiers.size() - 1);
        float levelBoost = TextEffects::Saturate((static_cast<float>(lv) - 100.0f) / 400.0f);
        float particleBoost = 1.0f + 0.6f * tierBoost + 0.6f * levelBoost;
        int boostedParticleCount = std::clamp(static_cast<int>(std::round(particleCount * particleBoost)), particleCount, 96);
        float boostedParticleSize = Settings::ParticleSize * (1.0f + 0.4f * tierBoost + 0.35f * levelBoost);
        float boostedParticleAlpha = std::clamp(Settings::ParticleAlpha * style.alpha *
                                                (0.95f + 0.35f * tierBoost + 0.35f * levelBoost), 0.0f, 1.0f);

        bool showOrbs = false, showWisps = false, showRunes = false, showSparks = false, showStars = false;
        if (tierHasParticles) {
            showOrbs = tier.particleTypes.find("Orbs") != std::string::npos;
            showWisps = tier.particleTypes.find("Wisps") != std::string::npos;
            showRunes = tier.particleTypes.find("Runes") != std::string::npos;
            showSparks = tier.particleTypes.find("Sparks") != std::string::npos;
            showStars = tier.particleTypes.find("Stars") != std::string::npos;
        } else {
            showOrbs = Settings::EnableOrbs;
            showWisps = Settings::EnableWisps;
            showRunes = Settings::EnableRunes;
            showSparks = Settings::EnableSparks;
            showStars = Settings::EnableStars;
        }

        int enabledStyles = (int)showOrbs + (int)showWisps + (int)showRunes + (int)showSparks + (int)showStars;
        int slot = 0;

        if (showOrbs)
            TextEffects::DrawParticleAura(dl, layout.nameplateCenter, spreadX, spreadY,
                particleColor, boostedParticleAlpha, Settings::ParticleStyle::Orbs, boostedParticleCount,
                boostedParticleSize, Settings::ParticleSpeed, time, slot++, enabledStyles);
        if (showWisps)
            TextEffects::DrawParticleAura(dl, layout.nameplateCenter, spreadX * 1.15f, spreadY * 1.15f,
                particleColor, boostedParticleAlpha, Settings::ParticleStyle::Wisps, boostedParticleCount,
                boostedParticleSize, Settings::ParticleSpeed, time, slot++, enabledStyles);
        if (showRunes)
            TextEffects::DrawParticleAura(dl, layout.nameplateCenter, spreadX * 0.9f, spreadY * 0.7f,
                particleColor, boostedParticleAlpha, Settings::ParticleStyle::Runes, std::max(4, boostedParticleCount / 2),
                boostedParticleSize * 1.2f, Settings::ParticleSpeed * 0.6f, time, slot++, enabledStyles);
        if (showSparks)
            TextEffects::DrawParticleAura(dl, layout.nameplateCenter, spreadX, spreadY * 0.8f,
                particleColor, boostedParticleAlpha, Settings::ParticleStyle::Sparks, boostedParticleCount,
                boostedParticleSize * 0.7f, Settings::ParticleSpeed * 1.5f, time, slot++, enabledStyles);
        if (showStars)
            TextEffects::DrawParticleAura(dl, layout.nameplateCenter, spreadX, spreadY,
                particleColor, boostedParticleAlpha, Settings::ParticleStyle::Stars, boostedParticleCount,
                boostedParticleSize, Settings::ParticleSpeed, time, slot++, enabledStyles);
    }

    /// Draw decorative ornament characters beside the nameplate.
    static void DrawOrnaments(ImDrawList *dl, const ActorDrawData &d, const LabelStyle &style, const LabelLayout &layout, float lodEffectsFactor, float time, ImDrawListSplitter *splitter)
    {
        const Settings::TierDefinition &tier = *style.tier;

        const std::string& leftOrns = (style.specialTitle && !style.specialTitle->leftOrnaments.empty())
            ? style.specialTitle->leftOrnaments : tier.leftOrnaments;
        const std::string& rightOrns = (style.specialTitle && !style.specialTitle->rightOrnaments.empty())
            ? style.specialTitle->rightOrnaments : tier.rightOrnaments;
        bool hasOrnaments = !leftOrns.empty() || !rightOrns.empty();
        bool showOrnaments = ((d.isPlayer && Settings::EnableOrnaments && hasOrnaments && style.tierAllowsOrnaments)
                          || (style.specialTitle && style.specialTitle->forceOrnaments && hasOrnaments))
                          && lodEffectsFactor > 0.01f;
        auto& ornIo = ImGui::GetIO();
        ImFont* ornamentFont = (ornIo.Fonts->Fonts.Size >= 4) ? ornIo.Fonts->Fonts[3] : nullptr;
        if (!showOrnaments || Settings::OrnamentFontPath.empty() || !ornamentFont)
            return;

        auto collectDrawableOrnaments = [&](const std::string& raw) {
            std::vector<std::string> out;
            const char* p = raw.c_str();
            while (*p) {
                unsigned int cp = 0;
                const char* next = Utf8Next(p, cp);
                if (!next || next <= p) { ++p; continue; }
                if (cp == 0xFFFD || cp < 0x20) { p = next; continue; }

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
            return;

        const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;

        float ornamentScale = 0.75f;
        if (Settings::Tiers.size() > 1)
            ornamentScale = 0.75f + 0.3f * (static_cast<float>(style.tierIdx) / static_cast<float>(Settings::Tiers.size() - 1));
        float sizeMultiplier = (style.specialTitle != nullptr) ? ornamentScale * 1.3f : ornamentScale;
        float ornamentSize = Settings::OrnamentFontSize * Settings::OrnamentScale * sizeMultiplier * textSizeScale;

        float extraPadding = ornamentSize * 0.30f;
        float totalSpacing = Settings::OrnamentSpacing * 1.35f + extraPadding;
        float ornamentCharGap = std::max(2.0f, ornamentSize * 0.16f);

        ImU32 ornColL = ImGui::ColorConvertFloat4ToU32(ImVec4(style.Lc.x, style.Lc.y, style.Lc.z, style.alpha));
        ImU32 ornColR = ImGui::ColorConvertFloat4ToU32(ImVec4(style.Rc.x, style.Rc.y, style.Rc.z, style.alpha));
        ImU32 ornHighlight = ImGui::ColorConvertFloat4ToU32(
            ImVec4(tier.highlightColor[0], tier.highlightColor[1], tier.highlightColor[2], style.alpha));
        ImU32 ornOutline = IM_COL32(0, 0, 0, (int)(style.alpha * 255.0f));
        float ornOutlineWidth = style.outlineWidth * (ornamentSize / layout.nameFontSize);

        ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(ImVec4(style.Lc.x, style.Lc.y, style.Lc.z, style.alpha));
        bool showOrnGlow = Settings::EnableGlow && Settings::GlowIntensity > 0.0f && style.tierAllowsGlow;

        auto drawOrnChar = [&](ImVec2 charPos, const char* ch) {
            if (showOrnGlow) {
                splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
                ParticleTextures::PushAdditiveBlend(dl);
                TextEffects::AddTextGlow(dl, ornamentFont, ornamentSize, charPos,
                                         ch, glowColor, Settings::GlowRadius,
                                         Settings::GlowIntensity, Settings::GlowSamples);
                ParticleTextures::PopBlendState(dl);
            }
            splitter->SetCurrentChannel(dl, 1);  // Front layer: ornament shapes
            ApplyTextEffect(dl, ornamentFont, ornamentSize, charPos, ch,
                            tier.nameEffect, ornColL, ornColR, ornHighlight, ornOutline, ornOutlineWidth,
                            style.phase01, style.strength, textSizeScale, style.alpha);
        };

        if (!leftChars.empty())
        {
            float cursorX = layout.nameplateCenter.x - layout.nameplateWidth * 0.5f - totalSpacing;
            for (int i = static_cast<int>(leftChars.size()) - 1; i >= 0; --i)
            {
                const std::string& ch = leftChars[i];
                ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, 0.0f, ch.c_str());
                cursorX -= charSize.x;
                ImVec2 charPos(cursorX, layout.nameplateCenter.y - charSize.y * 0.5f);
                drawOrnChar(charPos, ch.c_str());
                if (i > 0) cursorX -= ornamentCharGap;
            }
        }

        if (!rightChars.empty())
        {
            float cursorX = layout.nameplateCenter.x + layout.nameplateWidth * 0.5f + totalSpacing;
            for (size_t i = 0; i < rightChars.size(); ++i)
            {
                const std::string& ch = rightChars[i];
                ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, 0.0f, ch.c_str());
                ImVec2 charPos(cursorX, layout.nameplateCenter.y - charSize.y * 0.5f);
                drawOrnChar(charPos, ch.c_str());
                cursorX += charSize.x;
                if (i + 1 < rightChars.size()) cursorX += ornamentCharGap;
            }
        }
    }

    /// Render the title line above the main nameplate line.
    static void DrawTitleText(ImDrawList *dl, const ActorDrawData &d, const LabelStyle &style, const LabelLayout &layout, float lodTitleFactor, ImDrawListSplitter *splitter)
    {
        const char *titleDisplayText = layout.titleDisplayStr.c_str();
        if (!titleDisplayText || !*titleDisplayText || lodTitleFactor <= 0.01f)
            return;

        float titleOffsetX = (layout.totalWidth - layout.titleSize.x) * 0.5f;
        ImVec2 titlePos(layout.startPos.x - layout.totalWidth * 0.5f + titleOffsetX,
                        layout.startPos.y + layout.titleY);

        float lodTitleAlpha = style.alpha * lodTitleFactor;
        ImU32 titleShadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlpha * 0.5f));

        splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
        if (Settings::EnableGlow && Settings::GlowIntensity > 0.0f && style.tierAllowsGlow)
        {
            ImVec4 glowColorVec = style.specialTitle
                ? ImVec4(style.specialGlowColor.x, style.specialGlowColor.y, style.specialGlowColor.z, style.alpha)
                : ImVec4(style.LcTitle.x, style.LcTitle.y, style.LcTitle.z, style.alpha);
            ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowColorVec);
            float glowIntensity = style.specialTitle ? Settings::GlowIntensity * 1.15f : Settings::GlowIntensity;
            float glowRadius = style.specialTitle ? Settings::GlowRadius * 1.1f : Settings::GlowRadius;
            ParticleTextures::PushAdditiveBlend(dl);
            TextEffects::AddTextGlow(dl, layout.fontTitle, layout.titleFontSize, titlePos,
                                     titleDisplayText, glowColor, glowRadius,
                                     glowIntensity, Settings::GlowSamples);
            ParticleTextures::PopBlendState(dl);
        }

        splitter->SetCurrentChannel(dl, 1);  // Front layer: shadow + text
        dl->AddText(layout.fontTitle, layout.titleFontSize,
                     ImVec2(titlePos.x + Settings::TitleShadowOffsetX,
                            titlePos.y + Settings::TitleShadowOffsetY),
                     titleShadow, titleDisplayText);

        float lodTitleAlphaFinal = style.titleAlpha * lodTitleFactor;
        float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
        if (d.isPlayer)
        {
            ApplyTextEffect(dl, layout.fontTitle, layout.titleFontSize, titlePos, titleDisplayText,
                            style.tier->titleEffect, style.colLTitle, style.colRTitle, style.highlight, style.outlineColor, style.titleOutlineWidth,
                            style.phase01, style.strength, textSizeScale, lodTitleAlphaFinal);
        }
        else
        {
            ImVec4 dColV = WashColor(style.dispoCol);
            dColV.w = lodTitleAlphaFinal;
            ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
            ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlphaFinal));
            TextEffects::AddTextOutline4(dl, layout.fontTitle, layout.titleFontSize, titlePos, titleDisplayText, dCol, npcOutline, style.titleOutlineWidth);
        }
    }

    /// Render each segment of the main nameplate line.
    static void DrawMainLineSegments(ImDrawList *dl, const ActorDrawData &d, const LabelStyle &style, const LabelLayout &layout, ImDrawListSplitter *splitter)
    {
        const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;

        ImVec2 currentPos;
        currentPos.x = layout.startPos.x - layout.totalWidth * 0.5f + (layout.totalWidth - layout.mainLineWidth) * 0.5f;
        currentPos.y = layout.startPos.y + layout.mainLineY;

        for (const auto &seg : layout.segments)
        {
            if (seg.displayText.empty())
            {
                currentPos.x += seg.size.x + layout.segmentPadding;
                continue;
            }

            float vOffset = (layout.mainLineHeight - seg.size.y) * 0.5f;
            ImVec2 pos = ImVec2(currentPos.x, currentPos.y + vOffset);

            splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
            if (Settings::EnableGlow && Settings::GlowIntensity > 0.0f && style.tierAllowsGlow)
            {
                ImVec4 glowCol = style.specialTitle
                    ? ImVec4(style.specialGlowColor.x, style.specialGlowColor.y, style.specialGlowColor.z, style.alpha)
                    : (seg.isLevel ? ImVec4(style.LcLevel.x, style.LcLevel.y, style.LcLevel.z, style.alpha)
                                   : ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, style.alpha));
                ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowCol);
                float glowIntensity = style.specialTitle ? Settings::GlowIntensity * 1.15f : Settings::GlowIntensity;
                float glowRadius = style.specialTitle ? Settings::GlowRadius * 1.1f : Settings::GlowRadius;
                ParticleTextures::PushAdditiveBlend(dl);
                TextEffects::AddTextGlow(dl, seg.font, seg.fontSize, pos,
                                         seg.displayText.c_str(), glowColor, glowRadius,
                                         glowIntensity, Settings::GlowSamples);
                ParticleTextures::PopBlendState(dl);
            }

            splitter->SetCurrentChannel(dl, 1);  // Front layer: shadow + text
            dl->AddText(seg.font, seg.fontSize,
                        ImVec2(pos.x + Settings::MainShadowOffsetX,
                               pos.y + Settings::MainShadowOffsetY),
                        style.shadowColor, seg.displayText.c_str());

            float segOutlineWidth = seg.isLevel ? style.levelOutlineWidth : style.nameOutlineWidth;

            if (seg.isLevel)
            {
                ApplyTextEffect(dl, seg.font, seg.fontSize, pos, seg.displayText.c_str(),
                                style.tier->levelEffect, style.colLLevel, style.colRLevel, style.highlight, style.outlineColor, segOutlineWidth,
                                style.phase01, style.strength, textSizeScale, style.levelAlpha);
            }
            else
            {
                if (d.isPlayer)
                {
                    ApplyTextEffect(dl, seg.font, seg.fontSize, pos, seg.displayText.c_str(),
                                    style.tier->nameEffect, style.colL, style.colR, style.highlight, style.outlineColor, segOutlineWidth,
                                    style.phase01, style.strength, textSizeScale, style.alpha);
                }
                else
                {
                    ImVec4 dColV = style.dispoCol;
                    dColV.w = style.alpha;
                    ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
                    ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, style.alpha));
                    TextEffects::AddTextOutline4(dl, seg.font, seg.fontSize, pos, seg.displayText.c_str(), dCol, npcOutline, segOutlineWidth);
                }
            }

            currentPos.x += seg.size.x + layout.segmentPadding;
        }
    }

    // =========================================================================
    // DrawLabel - orchestrator
    // =========================================================================
    static void DrawLabel(const ActorDrawData &d, ImDrawList *drawList, ImDrawListSplitter *splitter)
    {
        // ----- Cache management -----
        auto it = s_state.cache.find(d.formID);
        if (it == s_state.cache.end())
        {
            ActorCache newEntry{};
            newEntry.lastSeenFrame = s_state.frame;
            it = s_state.cache.emplace(d.formID, newEntry).first;
        }
        auto &entry = it->second;
        entry.lastSeenFrame = s_state.frame;

        if (entry.cachedName != d.name)
        {
            entry.cachedName = d.name;
            entry.typewriterTime = 0.0f;
            entry.typewriterComplete = false;
        }

        constexpr uint32_t kReentryThreshold = 30;
        if (entry.initialized && entry.typewriterComplete)
        {
            uint32_t framesSinceLastSeen = s_state.frame - entry.lastSeenFrame;
            bool becameVisible = entry.wasOccluded && !d.isOccluded;
            if (framesSinceLastSeen >= kReentryThreshold || becameVisible)
            {
                entry.typewriterTime = 0.0f;
                entry.typewriterComplete = false;
            }
        }

        entry.lastSeenFrame = s_state.frame;

        // ----- Distance / alpha / scale computation -----
        RE::NiPoint3 cameraPos{};
        bool hasCameraPos = false;
        if (auto pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot)
        {
            cameraPos = pc->cameraRoot->world.translate;
            hasCameraPos = true;
        }

        const float dist = d.distToPlayer;
        const float dt = ImGui::GetIO().DeltaTime;

        float fadeT = TextEffects::SmoothStep((dist - Settings::FadeStartDistance) / (Settings::FadeEndDistance - Settings::FadeStartDistance));
        float alphaTarget = 1.0f - fadeT;
        alphaTarget = alphaTarget * alphaTarget;

        float lodTitleFactor = 1.0f;
        float lodEffectsFactor = 1.0f;

        if (Settings::Visual().EnableLOD) {
            float transRange = std::max(1.0f, Settings::Visual().LODTransitionRange);
            float titleFadeT = TextEffects::Saturate(
                (dist - Settings::Visual().LODFarDistance) / transRange);
            lodTitleFactor = 1.0f - TextEffects::SmoothStep(titleFadeT);
            float effectsFadeT = TextEffects::Saturate(
                (dist - Settings::Visual().LODMidDistance) / transRange);
            lodEffectsFactor = 1.0f - TextEffects::SmoothStep(effectsFadeT);
        }

        float scaleT = TextEffects::Saturate((dist - Settings::ScaleStartDistance) / (Settings::ScaleEndDistance - Settings::ScaleStartDistance));
        constexpr float kScaleGamma = 0.5f;
        scaleT = std::pow(scaleT, kScaleGamma);
        float textScaleTarget = 1.0f + (Settings::MinimumScale - 1.0f) * scaleT;

        if (hasCameraPos)
        {
            float camDist = std::sqrt(
                std::pow(d.worldPos.x - cameraPos.x, 2.0f) +
                std::pow(d.worldPos.y - cameraPos.y, 2.0f) +
                std::pow(d.worldPos.z - cameraPos.z, 2.0f));
            float camScaleT = TextEffects::Saturate((camDist - Settings::ScaleStartDistance) / (Settings::ScaleEndDistance - Settings::ScaleStartDistance));
            camScaleT = std::pow(camScaleT, kScaleGamma);
            float camTextScale = 1.0f + (Settings::MinimumScale - 1.0f) * camScaleT;
            textScaleTarget = std::min(textScaleTarget, camTextScale);
        }

        if (Settings::Visual().MinimumPixelHeight > 0.0f) {
            float minScale = Settings::Visual().MinimumPixelHeight / Settings::NameFontSize;
            textScaleTarget = std::max(textScaleTarget, minScale);
        }

        // ----- World projection & smoothing -----
        RE::NiPoint3 screenPos;
        if (!WorldToScreen(d.worldPos, screenPos))
            return;

        float occlusionTarget = d.isOccluded ? 0.0f : 1.0f;

        if (!entry.initialized)
        {
            entry.initialized = true;
            entry.alphaSmooth = alphaTarget;
            entry.textSizeScale = textScaleTarget;
            entry.smooth = ImVec2(screenPos.x, screenPos.y);

            ImVec2 initPos(screenPos.x, screenPos.y);
            for (int i = 0; i < ActorCache::kHistorySize; i++)
                entry.posHistory[i] = initPos;
            entry.historyIndex = 0;
            entry.historyFilled = true;
            entry.occlusionSmooth = 1.0f;
            entry.typewriterTime = 0.0f;
            entry.typewriterComplete = false;
        }
        else
        {
            float aLerp = ExpApproachAlpha(dt, Settings::AlphaSettleTime);
            float sLerp = ExpApproachAlpha(dt, Settings::ScaleSettleTime);
            float pLerp = d.isPlayer ? ExpApproachAlpha(dt, 0.015f) : ExpApproachAlpha(dt, Settings::PositionSettleTime);
            float oLerp = ExpApproachAlpha(dt, Settings::OcclusionSettleTime);

            entry.alphaSmooth += (alphaTarget - entry.alphaSmooth) * aLerp;
            entry.textSizeScale += (textScaleTarget - entry.textSizeScale) * sLerp;
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

            float dx = targetPos.x - entry.smooth.x;
            float dy = targetPos.y - entry.smooth.y;
            float moveDist = std::sqrt(dx * dx + dy * dy);

            if (moveDist > Settings::Visual().LargeMovementThreshold)
            {
                entry.smooth.x += (smoothedPos.x - entry.smooth.x) * Settings::Visual().LargeMovementBlend;
                entry.smooth.y += (smoothedPos.y - entry.smooth.y) * Settings::Visual().LargeMovementBlend;
            }
            else
            {
                entry.smooth = smoothedPos;
            }

            if (Settings::EnableTypewriter && !entry.typewriterComplete)
                entry.typewriterTime += dt;
        }

        entry.wasOccluded = d.isOccluded;

        // ----- Early returns -----
        const float alpha = entry.alphaSmooth * entry.occlusionSmooth;
        if (alpha <= 0.02f)
            return;

        const float textSizeScale = entry.textSizeScale;

        const auto viewSize = RE::BSGraphics::Renderer::GetSingleton()->GetScreenSize();
        if (screenPos.z < 0 || screenPos.z > 1.0f ||
            screenPos.x < -100.0f || screenPos.x > viewSize.width + 100.0f ||
            screenPos.y < -100.0f || screenPos.y > viewSize.height + 100.0f)
            return;

        const float time = (float)ImGui::GetTime();

        // ----- Compute style and layout -----
        LabelStyle style = ComputeLabelStyle(d, alpha, time);

        // Compute per-font outline widths
        style.nameOutlineWidth  = style.CalcOutlineWidth(ImGui::GetIO().Fonts->Fonts[0]->FontSize * textSizeScale);
        style.levelOutlineWidth = style.CalcOutlineWidth(ImGui::GetIO().Fonts->Fonts[1]->FontSize * textSizeScale);
        style.titleOutlineWidth = style.CalcOutlineWidth(ImGui::GetIO().Fonts->Fonts[2]->FontSize * textSizeScale);
        style.outlineWidth      = style.nameOutlineWidth;

        LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale);

        // ----- Draw layers (back to front) -----
        DrawParticles(drawList, d, style, layout, lodEffectsFactor, time, splitter);
        DrawOrnaments(drawList, d, style, layout, lodEffectsFactor, time, splitter);
        DrawTitleText(drawList, d, style, layout, lodTitleFactor, splitter);
        DrawMainLineSegments(drawList, d, style, layout, splitter);
    }

    // Draw debug overlay with performance stats
    static void DrawDebugOverlay()
    {
        if (!Settings::EnableDebugOverlay)
            return;

        const float time = static_cast<float>(ImGui::GetTime());
        const float dt = ImGui::GetIO().DeltaTime;

        // Update frame timing stats
        DebugOverlay::UpdateFrameStats(
            s_state.debugStats, dt, time,
            s_state.lastDebugUpdateTime, s_state.updateCounter, s_state.lastUpdateCount
        );

        // Update cache stats
        s_state.debugStats.cacheSize = s_state.cache.size();

        // Build context and render
        DebugOverlay::Context ctx;
        ctx.stats = &s_state.debugStats;
        ctx.frameNumber = s_state.frame;
        ctx.postLoadCooldown = s_state.postLoadCooldown;
        ctx.lastReloadTime = s_state.lastReloadTime;
        ctx.actorCacheEntrySize = sizeof(ActorCache);
        ctx.actorDrawDataSize = sizeof(ActorDrawData);

        DebugOverlay::Render(ctx);
    }

    /// Handle hot reload key press (Settings::ReloadKey).
    static void HandleHotReload()
    {
        if (Settings::ReloadKey <= 0)
            return;

        bool keyDown = (GetAsyncKeyState(Settings::ReloadKey) & 0x8000) != 0;

        if (keyDown && !s_state.reloadKeyWasDown)
        {
            Settings::Load();
            s_state.lastReloadTime = static_cast<float>(ImGui::GetTime());
            s_state.cache.clear();

            if (Settings::TemplateReapplyOnReload && Settings::UseTemplateAppearance)
            {
                AppearanceTemplate::ResetAppliedFlag();
                SKSE::GetTaskInterface()->AddTask([]() {
                    AppearanceTemplate::ApplyIfConfigured();
                });
            }
        }
        s_state.reloadKeyWasDown = keyDown;
    }

    /// Update debug stats from the current snapshot.
    static void UpdateDebugStats(const std::vector<ActorDrawData>& snap)
    {
        s_state.debugStats.actorCount = static_cast<int>(snap.size());
        s_state.debugStats.visibleActors = 0;
        s_state.debugStats.occludedActors = 0;
        s_state.debugStats.playerVisible = 0;

        for (const auto &d : snap)
        {
            if (d.isPlayer)
                s_state.debugStats.playerVisible = 1;
            if (d.isOccluded)
                s_state.debugStats.occludedActors++;
            else
                s_state.debugStats.visibleActors++;
        }
        ++s_state.updateCounter;
    }

    /// Resolve overlapping labels by pushing lower-priority ones down.
    static void ResolveOverlaps(const std::vector<ActorDrawData>& localSnap)
    {
        struct LabelRect {
            int idx;
            float cy, halfH, dist, yOffset;
            bool isPlayer;
        };
        std::vector<LabelRect> labelRects;

        for (int i = 0; i < static_cast<int>(localSnap.size()); ++i)
        {
            const auto& d = localSnap[i];
            auto cIt = s_state.cache.find(d.formID);
            if (cIt == s_state.cache.end() || !cIt->second.initialized)
                continue;

            const auto& entry = cIt->second;
            if (entry.alphaSmooth * entry.occlusionSmooth <= 0.02f)
                continue;

            float approxHeight = Settings::NameFontSize * entry.textSizeScale * 1.5f;
            labelRects.push_back({i, entry.smooth.y, approxHeight * 0.5f,
                                  d.distToPlayer, 0.0f, d.isPlayer});
        }

        // Sort by priority: player first, then closest first
        std::sort(labelRects.begin(), labelRects.end(), [](const LabelRect& a, const LabelRect& b) {
            if (a.isPlayer != b.isPlayer) return a.isPlayer > b.isPlayer;
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
                    float overlap = (labelRects[i].cy + labelRects[i].yOffset + labelRects[i].halfH + padding)
                                  - (labelRects[j].cy + labelRects[j].yOffset - labelRects[j].halfH);
                    if (overlap > 0.0f)
                        labelRects[j].yOffset += overlap;
                }
            }
        }

        // Store offsets for DrawLabel to apply
        for (const auto& lr : labelRects)
        {
            if (std::abs(lr.yOffset) > 0.01f)
                OverlapOffsets()[localSnap[lr.idx].formID] = lr.yOffset;
        }
    }

    // Several pipeline stages are gated or tuned by Settings::Visual() overrides
    // (LOD, overlap prevention, distance outline scaling, tier effect gating, etc.).
    void Draw()
    {
        HandleHotReload();

        if (!CanDrawOverlay())
        {
            s_state.wasInInvalidState = true;
            return;
        }

        if (s_state.wasInInvalidState)
        {
            s_state.wasInInvalidState = false;
            s_state.postLoadCooldown = 300;
        }

        if (s_state.postLoadCooldown > 0)
        {
            --s_state.postLoadCooldown;
            return;
        }

        QueueSnapshotUpdate_RenderThread();

        auto *bsRenderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!bsRenderer)
            return;

        const auto viewSize = bsRenderer->GetScreenSize();
        ++s_state.frame;

        static std::vector<ActorDrawData> localSnap;
        {
            std::lock_guard<std::mutex> lock(s_state.snapshotLock);
            localSnap = s_state.snapshot;
        }

        if (localSnap.empty())
            return;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)viewSize.width, (float)viewSize.height));
        ImGui::Begin("whoisOverlay", nullptr,
                     ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImDrawList *drawList = ImGui::GetWindowDrawList();

        if (Settings::EnableDebugOverlay)
            UpdateDebugStats(localSnap);

        OverlapOffsets().clear();
        if (Settings::Visual().EnableOverlapPrevention)
            ResolveOverlaps(localSnap);

        ImDrawListSplitter splitter;
        splitter.Split(drawList, 2);

        for (auto &d : localSnap)
            DrawLabel(d, drawList, &splitter);

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
}
