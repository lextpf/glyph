#pragma once

#include "PCH.h"

#include "DebugOverlay.h"
#include "RenderConstants.h"
#include "Settings.h"
#include "TextEffects.h"
#include "Utf8Utils.h"

#include <imgui.h>

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

// ============================================================================
// Render-thread Settings snapshot
// ============================================================================

/// Captures ALL mutable Settings values consumed by the render thread.
/// Populated once per frame under Settings::Mutex() shared_lock in Draw(),
/// then passed by const reference to all render-thread functions.
/// This eliminates lock-free reads of non-POD Settings globals.
struct RenderSettingsSnapshot
{
    // Distance & Fade
    float fadeStartDistance = .0f;
    float fadeEndDistance = .0f;
    float scaleStartDistance = .0f;
    float scaleEndDistance = .0f;
    float minimumScale = .0f;

    // Outline & Shadow
    float outlineWidthMin = .0f;
    float outlineWidthMax = .0f;
    float titleShadowOffsetX = .0f;
    float titleShadowOffsetY = .0f;
    float mainShadowOffsetX = .0f;
    float mainShadowOffsetY = .0f;
    float segmentPadding = .0f;
    bool fastOutlines = false;
    float titleMainGap = .0f;
    float outlineMinScale = .65f;
    bool proportionalSpacing = false;

    // Glow
    bool enableGlow = false;
    float glowRadius = .0f;
    float glowIntensity = .0f;
    int glowSamples = 0;
    float glowDivideStrength = .0f;

    // Outline Glow
    bool enableOutlineGlow = false;
    float outlineGlowScale = 1.6f;
    float outlineGlowAlpha = .1f;
    int outlineGlowRings = 2;
    float outlineGlowR = 1.0f;
    float outlineGlowG = 1.0f;
    float outlineGlowB = 1.0f;
    bool outlineGlowTierTint = false;
    // Dual-tone directional outline
    bool dualOutlineEnabled = false;
    float innerOutlineTint = .3f;
    float innerOutlineAlpha = .5f;
    float innerOutlineScale = .5f;
    float directionalLightAngle = 315.f;
    float directionalLightBias = .15f;

    float outlineColorTint = .0f;
    float shadowColorTint = .0f;

    // Shine Overlay
    bool enableShine = false;
    float shineIntensity = .35f;
    float shineFalloff = 2.0f;
    float textGlowAlpha = .0f;

    // Typewriter
    bool enableTypewriter = false;
    float typewriterSpeed = .0f;
    float typewriterDelay = .0f;

    // Ornaments
    bool enableOrnaments = false;
    float ornamentScale = .0f;
    float ornamentSpacing = .0f;
    std::string ornamentFontPath = {};
    float ornamentFontSize = .0f;
    bool ornamentAnchorToMainLine = true;

    // Particles
    bool useParticleTextures = false;
    bool enableParticleAura = false;
    bool enableStars = false;
    bool enableSparks = false;
    bool enableWisps = false;
    bool enableRunes = false;
    bool enableOrbs = false;
    bool enableCrystals = false;
    int particleCount = 0;
    float particleSize = .0f;
    float particleSpeed = .0f;
    float particleSpread = .0f;
    float particleAlpha = .0f;
    int particleBlendMode = 0;

    // Animation & Color
    float animSpeedLowTier = .0f;
    float animSpeedMidTier = .0f;
    float animSpeedHighTier = .0f;
    float nameColorMix = .0f;
    float effectAlphaMin = .0f;
    float effectAlphaMax = .0f;
    float strengthMin = .0f;
    float strengthMax = .0f;
    float tierVibrancyBoost = .0f;
    float innerTextAlpha = 1.0f;
    float outlineAlpha = 1.0f;
    float textSaturationBoost = .0f;

    // Smoothing
    float alphaSettleTime = .0f;
    float scaleSettleTime = .0f;
    float positionSettleTime = .0f;
    float occlusionSettleTime = .0f;

    // Display
    bool enableDebugOverlay = false;
    bool enableOcclusionCulling = false;
    float verticalOffset = .0f;
    bool hidePlayer = false;
    int reloadKey = 0;

    // Font size (paths are only used during ImGui init, not per-frame)
    float nameFontSize = .0f;

    // Entrance/Exit Transitions
    bool enableEntrance = false;
    int entranceStyle = 0;
    float entranceDuration = .35f;
    float entranceOvershoot = 1.05f;
    bool enableExit = false;
    float exitDuration = .20f;

    // Visual polish settings (value copy)
    Settings::VisualSettings visual = {};

    // Non-POD collections (value copies - small, recaptured on generation change)
    std::vector<Settings::TierDefinition> tiers = {};
    std::string titleFormat = {};
    std::vector<Settings::Segment> displayFormat = {};
    std::vector<Settings::SpecialTitleDefinition> specialTitles = {};

    // Pre-sorted special titles (pointers into specialTitles above).
    // Populated once per snapshot via PopulateSortedSpecialTitles().
    std::vector<const Settings::SpecialTitleDefinition*> sortedSpecialTitles = {};

    // Factory: populate all fields from Settings under shared lock.
    static RenderSettingsSnapshot CaptureFromSettings();

    // Build sortedSpecialTitles from specialTitles.
    // Must be called AFTER the snapshot is in its final storage location
    // (pointers reference elements of the specialTitles member).
    void PopulateSortedSpecialTitles();
};

// ============================================================================
// Structs
// ============================================================================

/// Cache entry for smooth actor nameplate animations.
/// Stores smoothed values and state for position, alpha, and effects.
struct ActorCache
{
    ImVec2 smooth{};               ///< Smoothed screen position (result of moving average)
    float alphaSmooth = 1.0f;      ///< Smoothed alpha for fade transitions
    float textSizeScale = 1.0f;    ///< Smoothed font scale for distance-based sizing
    float occlusionSmooth = 1.0f;  ///< Smoothed occlusion (1.0=visible, 0.0=hidden)

    bool initialized = false;    ///< True after first frame of data
    uint32_t lastSeenFrame = 0;  ///< Frame counter when actor was last in snapshot

    uint32_t lastOcclusionCheckFrame = 0;  ///< Frame when LOS was last checked
    bool cachedOccluded = false;           ///< Cached LOS result
    bool wasOccluded = false;              ///< Previous frame's occlusion state

    static constexpr int HISTORY_SIZE = RenderConstants::POSITION_HISTORY_SIZE;
    ImVec2 posHistory[HISTORY_SIZE]{};  ///< Circular buffer of raw screen positions
    int historyIndex = 0;               ///< Current write index in posHistory
    bool historyFilled = false;         ///< True once posHistory has wrapped at least once

    float typewriterTime = .0f;       ///< Seconds since actor first appeared
    bool typewriterComplete = false;  ///< True when reveal animation finished

    float entrancePhase = .0f;  ///< Entrance animation progress (0=start, 1=complete)
    float exitPhase = .0f;      ///< Exit animation progress (0=visible, 1=fully exited)
    bool entranceDone = false;  ///< True when entrance animation finished

    /// Motion trail history (separate from posHistory used for smoothing).
    static constexpr int TRAIL_HISTORY_SIZE = 8;
    ImVec2 trailHistory[TRAIL_HISTORY_SIZE]{};  ///< Trail ghost positions
    int trailIndex = 0;                         ///< Current write index in trailHistory
    bool trailFilled = false;                   ///< True once trailHistory has wrapped

    std::string cachedName;       ///< Last known name (to detect changes)
    std::string cachedNameLower;  ///< Pre-lowered name for special title matching

    /// Add position sample to history and return smoothed average.
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

/// Actor disposition.
enum class Disposition : std::uint8_t
{
    Neutral,      ///< Neutral NPCs (white/gray)
    Enemy,        ///< Hostile NPCs (red)
    AllyOrFriend  ///< Friendly/allied NPCs (blue)
};

/// Data for rendering a single actor's nameplate.
struct ActorDrawData
{
    uint32_t formID{0};                       ///< Actor's form ID (unique identifier)
    RE::NiPoint3 worldPos{};                  ///< World position above actor's head
    std::string name;                         ///< Display name (capitalized)
    uint16_t level{0};                        ///< Actor's level
    float distToPlayer{.0f};                  ///< Distance to player in units
    Disposition dispo{Disposition::Neutral};  ///< Disposition towards player
    bool isPlayer{false};                     ///< Whether this is the player character
    bool isOccluded{false};                   ///< Whether actor is occluded from view
};

/// Cached line-of-sight result for an actor, checked periodically rather than
/// every frame.
struct OcclusionCacheEntry
{
    uint32_t lastCheckFrame{0};  ///< Frame when LOS was last checked
    bool cachedOccluded{false};  ///< Cached LOS result from that check
};

/// Encapsulates all mutable renderer state into a single struct.
struct RendererState
{
    /// @name Cache
    /// @{
    std::unordered_map<uint32_t, ActorCache> cache;  ///< Per-actor animation cache, keyed by formID
    uint32_t frame = 0;                              ///< Render-thread frame counter
    /// @}

    /// @name Snapshot & Thread Safety
    /// @{
    std::vector<ActorDrawData> snapshot;             ///< Current actor draw data
    std::mutex snapshotLock;                         ///< Guards snapshot reads/writes
    std::atomic<bool> updateQueued{false};           ///< True while a game-thread update is pending
    std::atomic<bool> pauseSnapshotUpdates{false};   ///< Suppress updates during reload
    std::atomic<bool> snapshotUpdateRunning{false};  ///< True while game-thread update is active
    std::atomic<bool> clearOcclusionCacheRequested{false};  ///< Request to clear occlusion cache
    /// @}

    /// @name Frame State
    /// @{
    bool wasInInvalidState = true;  ///< True if previous frame was in an invalid game state
    int postLoadCooldown = 0;       ///< Frames remaining in post-load cooldown
    /// @}

    /// @name Debug Stats
    /// @{
    DebugOverlay::Stats debugStats;   ///< Debug overlay performance metrics
    float lastDebugUpdateTime = .0f;  ///< Time of last debug stats refresh
    int updateCounter = 0;            ///< Total snapshot updates since startup
    int lastUpdateCount = 0;          ///< Update counter at last debug refresh
    /// @}

    /// @name Hot Reload
    /// @{
    bool reloadKeyWasDown = false;             ///< Previous frame's reload key state
    float lastReloadTime = -10.0f;             ///< Time of last settings reload
    std::atomic<bool> reloadRequested{false};  ///< Render thread requests reload
    std::atomic<bool> reloadCompleted{false};  ///< Set by game thread when async Load() finishes
    /// @}

    /// @name Overlay Flags
    /// @{
    std::atomic<bool> allowOverlay{false};  ///< True when game state allows overlay
    std::atomic<bool> manualEnabled{true};  ///< Toggled by user via console/key
    /// @}

    /// Cached settings snapshot (re-captured only when Settings::Generation() changes).
    RenderSettingsSnapshot cachedSnap;
    uint32_t lastSnapGeneration = 0;  ///< Generation counter at last snapshot capture
};

/// Describes one formatted text segment on the main nameplate line.
struct RenderSeg
{
    std::string text;         ///< Formatted text to display
    std::string displayText;  ///< Text after typewriter truncation
    bool isLevel;             ///< Whether to use level font
    ImFont* font;             ///< Font to use for rendering
    float fontSize;           ///< Scaled font size
    ImVec2 size;              ///< Measured size of this segment
    ImVec2 displaySize;       ///< Measured size of displayText
};

/**
 * Color terminology used throughout the rendering pipeline:
 *
 * - **Pastelize**: Blend a color toward white based on the actor's level
 *   position within its tier.  Higher levels get more vivid (less pastel)
 *   colors.  Formula: `result = 1 + (color - 1) * t`.
 * - **MixToWhite**: Blend color toward white by a fixed ratio.  Applied as
 *   a base color
 * damping for lower tiers.
 */

/// All color, tier, and effect data computed once per label.
struct LabelStyle
{
    int tierIdx;
    const Settings::TierDefinition* tier;
    const Settings::SpecialTitleDefinition* specialTitle;

    ImU32 colL, colR;            ///< Name gradient (packed)
    ImU32 colLTitle, colRTitle;  ///< Title gradient
    ImU32 colLLevel, colRLevel;  ///< Level gradient
    ImU32 highlight;             ///< Shimmer / sparkle highlight
    ImU32 outlineColor;          ///< Black outline (alpha-scaled)
    ImU32 shadowColor;           ///< Black shadow  (alpha-scaled)

    ImVec4 Lc, Rc;            ///< Tier left/right color (pasteled float).
    ImVec4 LcName, RcName;    ///< Computed name color (float, for glow).
    ImVec4 LcTitle, RcTitle;  ///< Computed title color (float, for glow).
    ImVec4 LcLevel, RcLevel;  ///< Softened level color (float, for glow).
    ImVec4 dispoCol;          ///< Disposition color.
    ImVec4 specialGlowColor;  ///< Special title glow color.

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

    float CalcOutlineWidth(float fontSize, const RenderSettingsSnapshot& snap) const
    {
        float ratio = std::max(fontSize / snap.nameFontSize, snap.outlineMinScale);
        float w = baseOutlineWidth * ratio;
        if (snap.visual.EnableDistanceOutlineScale)
        {
            float distT = TextEffects::Saturate((distToPlayer - snap.fadeStartDistance) /
                                                (snap.fadeEndDistance - snap.fadeStartDistance));
            float distMul =
                snap.visual.OutlineDistanceMin +
                (snap.visual.OutlineDistanceMax - snap.visual.OutlineDistanceMin) * distT;
            w *= distMul;
        }
        return w;
    }
};

/// Text measurement, font selection, and position data for a label.
/// Coordinates are in screen pixels (Y increases downward).  startPos is
/// the screen-space anchor computed from WorldToScreen.
struct LabelLayout
{
    ImFont* fontName;     ///< Name font pointer
    ImFont* fontLevel;    ///< Level font pointer
    ImFont* fontTitle;    ///< Title font pointer
    float nameFontSize;   ///< Scaled name font size in pixels
    float levelFontSize;  ///< Scaled level font size in pixels
    float titleFontSize;  ///< Scaled title font size in pixels

    std::vector<RenderSeg> segments;  ///< Main line segments (built from DisplayFormat)
    float mainLineWidth;              ///< Total width of all segments plus padding
    float mainLineHeight;             ///< Height of the tallest segment
    float segmentPadding;             ///< Horizontal padding between segments

    std::string titleStr;         ///< Full title text
    std::string titleDisplayStr;  ///< Title text after typewriter truncation
    ImVec2 titleSize;             ///< Measured size of titleDisplayStr

    ImVec2 startPos;   ///< Screen-space anchor from WorldToScreen
    float titleY;      ///< Y position of title line top
    float mainLineY;   ///< Y position of main line top
    float totalWidth;  ///< Width of the wider of title/main line

    ImVec2 nameplateCenter;  ///< Center of the nameplate bounding box
    float nameplateTop;      ///< Top edge of nameplate (screen Y)
    float nameplateBottom;   ///< Bottom edge of nameplate (screen Y)
    float nameplateLeft;     ///< Left edge of nameplate (screen X)
    float nameplateRight;    ///< Right edge of nameplate (screen X)
    float nameplateWidth;    ///< Total nameplate width in pixels
    float nameplateHeight;   ///< Total nameplate height in pixels
    float mainLineCenterY;   ///< Vertical center of main line (for ornament anchoring)
};

/// Distance-based fade, LOD, and scale factors for a label.
struct DistanceFactors
{
    float alphaTarget;       ///< Target alpha from distance fade
    float textScaleTarget;   ///< Target font scale from distance
    float lodTitleFactor;    ///< LOD multiplier for title visibility [0,1]
    float lodEffectsFactor;  ///< LOD multiplier for particles/ornaments [0,1]
};

// ============================================================================
// Snapshot state (game-thread only)
// ============================================================================

/// Encapsulates mutable state used exclusively during snapshot collection
/// on the game thread, wrapped in an accessor per CONTRIBUTING guidelines
/// ("Avoid non-trivial global state").
struct SnapshotState
{
    uint32_t frame = 0;                      ///< Snapshot frame counter
    RE::BGSKeyword* npcKeyword = nullptr;    ///< Cached ActorTypeNPC keyword for creature filtering
    bool npcKeywordLookupAttempted = false;  ///< True after first keyword lookup attempt
    bool npcKeywordMissingLogged = false;    ///< True after logging keyword-not-found warning
};
SnapshotState& GetSnapshotState();

inline constexpr float RELOAD_NOTIFICATION_DURATION = RenderConstants::RELOAD_NOTIFICATION_DURATION;

// ============================================================================
// Accessors
// ============================================================================

/// Returns the singleton RendererState.  Only accessed from the render thread
/// except where noted.
RendererState& GetState();

/// Returns the per-actor occlusion cache.  Game-thread only.
std::unordered_map<uint32_t, OcclusionCacheEntry>& GetOcclusionCache();

/// Returns per-frame Y offsets for overlap resolution, keyed by formID.
/// Render-thread only.
std::unordered_map<uint32_t, float>& OverlapOffsets();

// ============================================================================
// Shared helpers
// ============================================================================

/// Project a world position to screen coordinates.
/// @param worldPos     World-space position.
/// @param[out] screenPos  Screen-space result (x,y in pixels, z=depth).
/// @param[out] cameraPosOut  Optional: camera world position.
/// @return true if the position is on screen.
bool WorldToScreen(const RE::NiPoint3& worldPos,
                   RE::NiPoint3& screenPos,
                   RE::NiPoint3* cameraPosOut = nullptr);

// ============================================================================
// Snapshot functions (RendererSnapshot.cpp)
// ============================================================================

/// ASCII title-case with UTF-8 pass-through.  Multi-byte codepoints are
/// preserved unchanged.
std::string Capitalize(const char* text);

/// Collect visible actor data on the game thread.  Scans nearby actors,
/// sorts by distance, performs occlusion checks, and publishes the snapshot
/// under snapshotLock.
void UpdateSnapshot_GameThread();

/// Schedule a snapshot update via SKSE's task interface.  Called from the
/// render thread; the actual work runs on the game thread.
void QueueSnapshotUpdate_RenderThread();

// ============================================================================
// Layout functions (RendererLayout.cpp)
// ============================================================================

/// Measure the tightest vertical glyph bounds for text.
/// @param[out] outTop     Topmost glyph offset (pixels).
/// @param[out] outBottom  Bottommost glyph offset (pixels).
void CalcTightYBoundsFromTop(
    ImFont* font, float fontSize, const char* text, float& outTop, float& outBottom);

/// Replace %n, %l, %t placeholders in a format string.  Uses single-pass
/// replacement to avoid expanding placeholders embedded in substitution values.
std::string FormatString(const std::string& fmt,
                         const std::string_view nameVal,
                         int levelVal,
                         const char* titleVal = nullptr);

/// Returns a default TierDefinition used when no tiers are configured.
/// Lazily initialized on first call.
const Settings::TierDefinition& GetFallbackTier();

/// Compute all color, tier, effect, and animation data for a label.
/// Performs tier matching, color pastelization, effect gating, and animation
/// phase calculation.
LabelStyle ComputeLabelStyle(const ActorDrawData& d,
                             const std::string& nameLower,
                             float alpha,
                             float time,
                             const RenderSettingsSnapshot& snap);

/// Measure text and compute all positions for a label.  Builds segments,
/// applies typewriter truncation, measures title and main line, and computes
/// the nameplate bounding box.
LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                               ActorCache& entry,
                               const LabelStyle& style,
                               float textSizeScale,
                               const RenderSettingsSnapshot& snap);

// ============================================================================
// Effects functions (RendererEffects.cpp)
// ============================================================================

/// Return the effect parameter if positive, otherwise the fallback.
/// Used to provide defaults for unconfigured effect parameters (where 0.0
/// means "use default").
constexpr float ParamOr(float p, float fallback)
{
    return p > .0f ? p : fallback;
}

/// Central effect dispatch: select and apply a visual effect (gradient,
/// shimmer, rainbow, etc.) to text.
/// @param drawList      Target draw list.
/// @param font          Font for the text.
/// @param fontSize      Scaled font size in pixels.
/// @param pos           Screen-space draw position.
/// @param text          Null-terminated UTF-8 string.
/// @param effect        Effect parameters (type, speed, etc.).
/// @param colL          Left gradient color (packed).
/// @param colR          Right gradient color (packed).
/// @param highlight     Shimmer/sparkle highlight color (packed).
/// @param outlineColor  Outline color (packed, alpha-scaled).
/// @param outlineWidth  Outline thickness in pixels.
/// @param phase01       Animation phase in [0,1].
/// @param strength      Effect strength multiplier.
/// @param textSizeScale Distance-based text scale factor.
/// @param alpha         Overall alpha multiplier.
/// @param fastOutlines  Use 4-dir outlines instead of 8-dir.
void ApplyTextEffect(ImDrawList* drawList,
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
                     float alpha,
                     bool fastOutlines,
                     const TextEffects::OutlineGlowParams* outlineGlow = nullptr,
                     const TextEffects::DualOutlineParams* dualOutline = nullptr,
                     const TextEffects::WaveParams* wave = nullptr,
                     const TextEffects::ShineParams* shine = nullptr);

/// Draw particle aura effects behind the nameplate.  Renders on splitter
/// channel 0 (back layer).
void DrawParticles(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodEffectsFactor,
                   float time,
                   ImDrawListSplitter* splitter,
                   const RenderSettingsSnapshot& snap);

/// Draw decorative ornament characters beside the nameplate.
/// Player-only unless forced by specialTitle.
void DrawOrnaments(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodEffectsFactor,
                   float time,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap);

/// Render the title line above the main nameplate line.
void DrawTitleText(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodTitleFactor,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap);

/// Render each segment of the main nameplate line with its configured effect.
void DrawMainLineSegments(ImDrawList* dl,
                          const ActorDrawData& d,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap);

// ============================================================================
// Coordinator functions (Renderer.cpp)
// ============================================================================

/// Return the ImGui font at the given index, or the default font if out of range.
ImFont* GetFontAt(int index);

/// Remove cache entries for actors not present in the current snapshot,
/// with a grace period to prevent jitter.
void PruneCacheToSnapshot(const std::vector<ActorDrawData>& snap);

/// Compute frame-rate-independent exponential smoothing factor.
/// Returns alpha in [0,1] for use with: current = lerp(current, target, alpha).
/// @param epsilon  Convergence threshold (default 0.01 = settle to 1% of
///                 initial delta).
float ExpApproachAlpha(float dt, float settleTime, float epsilon = .01f);

}  // namespace Renderer
