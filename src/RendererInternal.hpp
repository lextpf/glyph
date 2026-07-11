#pragma once

#include "PCH.hpp"

#include "ActorOverrides.hpp"
#include "DebugOverlay.hpp"
#include "RenderConstants.hpp"
#include "Settings.hpp"
#include "TextEffects.hpp"
#include "Utf8Utils.hpp"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @namespace Renderer
 * @brief Shared internal types and state for the Renderer translation units.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Public entry points are in Renderer.hpp. `_GameThread` runs on the game thread;
 * `_RenderThread` and `...RT` run on the render thread. ActorDrawData must not hold
 * engine pointers. Copy the published snapshot and target while holding
 * `snapshotLock`, then release it before layout or GPU work.
 *
 * When both locks are needed, acquire `Settings::Mutex()` before `snapshotLock`.
 * The actor-override store mutex is a leaf lock: acquire no other lock while holding it.
 *
 * ### :material-swap-horizontal: Snapshot handshake
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * sequenceDiagram
 *     participant RT as Render thread
 *     participant Q as SKSE task queue
 *     participant GT as Game thread
 *     RT->>RT: QueueSnapshotUpdate_RenderThread
 *     Note over RT: no-op while pauseSnapshotUpdates is set,<br/>or if updateQueued was set
 *     RT->>Q: AddTask
 *     Q->>GT: UpdateSnapshot_GameThread
 *     GT->>GT: snapshotUpdateRunning = true
 *     GT->>GT: publish allowOverlay and allowDeck
 *     GT->>GT: scan actors, derive plate facts, publish activeRegister
 *     GT->>RT: snapshot and crosshairTarget, under snapshotLock
 *     GT->>GT: snapshotUpdateRunning = false, updateQueued = false
 *     RT->>RT: Draw copies the snapshot under snapshotLock
 * ```
 *
 * ### :material-vector-link: Per-label data derivation
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef state fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef resolved fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef draw fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[ActorDrawData<br/>Game-thread facts]:::input --> R[Frame resolution]:::resolved
 *     S[RenderSettingsSnapshot<br/>Immutable settings]:::input --> R
 *     C[ActorCache<br/>Persistent animation state]:::state --> R
 *     R -.->|advance for the next frame| C
 *     R --> D[DistanceFactors]:::resolved
 *     R --> X[ActorLabelContext<br/>and RenderSeg text]:::resolved
 *     R --> B[BadgeComposition]:::resolved
 *     R --> Y[LabelStyle]:::resolved
 *     D --> L[ComputeLabelLayout]:::resolved
 *     X --> L
 *     B --> L
 *     Y --> L
 *     C --> L
 *     L --> O[LabelLayout<br/>and BadgeDrawItem]:::resolved
 *     O --> P[Draw passes]:::draw
 *     Y --> P
 *     P --> I[ImDrawList]:::draw
 * ```
 */
namespace Renderer
{
using Utf8Utils::Utf8CharCount;
using Utf8Utils::Utf8Next;
using Utf8Utils::Utf8Truncate;

/**
 * @struct RenderSettingsSnapshot
 * @brief Render settings owned by RendererState::cachedSnap.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Recaptured under `Settings::Mutex()` when `Settings::Generation()` changes, then passed
 * by const reference. Settings-backed views expire on refresh, even though cachedSnap lives.
 * Build `sortedSpecialTitles` after the capture is assigned to its final owner; its pointers
 * must refer to that owner's `specialTitles` vector.
 * Deck settings and the reload key are read separately under that lock.
 */
struct RenderSettingsSnapshot
{
    float fadeStartDistance = .0f;
    float fadeEndDistance = .0f;
    float scaleStartDistance = .0f;
    float scaleEndDistance = .0f;
    float minimumScale = .0f;

    float outlineWidthMin = .0f;
    float outlineWidthMax = .0f;
    float titleShadowOffsetX = .0f;
    float titleShadowOffsetY = .0f;
    float mainShadowOffsetX = .0f;
    float mainShadowOffsetY = .0f;
    bool fastOutlines = false;

    bool enableGlow = false;
    float glowRadius = .0f;
    float glowIntensity = .0f;
    int glowSamples = 0;
    float glowDivideStrength = .0f;

    bool enableOutlineGlow = false;
    float outlineGlowScale = 1.6f;
    float outlineGlowAlpha = .1f;
    int outlineGlowRings = 2;
    float outlineGlowR = 1.0f;
    float outlineGlowG = 1.0f;
    float outlineGlowB = 1.0f;
    bool outlineGlowTierTint = false;
    bool dualOutlineEnabled = false;
    float innerOutlineTint = .3f;
    float innerOutlineAlpha = .5f;
    float innerOutlineScale = .5f;
    float directionalLightAngle = 315.f;
    float directionalLightBias = .15f;

    float outlineColorTint = .0f;
    float shadowColorTint = .0f;

    bool softShadowEnabled = false;
    float softShadowDistance = 4.0f;
    float softShadowSoftness = 3.0f;
    float softShadowOpacity = .8f;
    float softShadowAngle = 45.0f;
    int softShadowSamples = 12;

    bool enableShine = false;
    float shineIntensity = .35f;
    float shineFalloff = 2.0f;
    float textGlowAlpha = .0f;

    bool enableTypewriter = false;
    float typewriterSpeed = .0f;
    float typewriterDelay = .0f;

    bool enableOrnaments = false;
    float ornamentScale = .0f;
    float ornamentSpacing = .0f;
    std::string ornamentFontPath = {};
    float ornamentFontSize = .0f;
    bool ornamentAnchorToMainLine = true;
    float ornamentOffsetY = .0f;

    bool useParticleTextures = false;
    bool enableParticleAura = false;
    int particleCount = 0;
    float particleSize = .0f;
    float particleSpeed = .0f;
    float particleSpread = .0f;
    float particleAlpha = .0f;
    int particleBlendMode = 0;
    float particleDepthStrength = .7f;    ///< Depth drive on size, alpha, parallax [0, 1.5]
    float particleColorWarmth = .5f;      ///< warm/cool depth temperature mix [0, 1]
    float particleGlowStrength = .28f;    ///< Additive backlight halo alpha [0, 1]
    float particleGlowSize = 2.2f;        ///< Halo radius, multiple of the sprite [1, 4]
    float particleShineThreshold = .84f;  ///< Glint sine threshold [0, 0.99]; higher = rarer

    float innerTextAlpha = 1.0f;
    float outlineAlpha = 1.0f;

    /**
     * @struct NpcColors
     * @brief Flat white-leaning colors for NPC nameplate text.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @see Settings::NpcColorSettings
     */
    struct NpcColors
    {
        Settings::Color3 neutral{};   ///< Neutral and talkable civilians
        Settings::Color3 hostile{};   ///< Hostiles; slight warm lean
        Settings::Color3 follower{};  ///< Teammates; slight cool lean
        Settings::Color3 level{};     ///< Level readout; dimmed silver
        Settings::Color3 title{};     ///< Title text; soft white
    };
    NpcColors npcColors = {};

    float alphaSettleTime = .0f;
    float scaleSettleTime = .0f;
    float positionSettleTime = .0f;
    float occlusionSettleTime = .0f;

    bool enableDebugOverlay = false;
    bool enableOcclusionCulling = false;
    float verticalOffset = .0f;
    float horizontalOffset = .0f;
    bool hidePlayer = false;
    int maxPlates = RenderConstants::DEFAULT_MAX_PLATES;
    int maxScanActors = RenderConstants::DEFAULT_MAX_SCAN_ACTORS;
    int reloadKey = 0;

    // Font size (paths are only used during ImGui init, not per-frame)
    float nameFontSize = .0f;

    bool enableEntrance = false;
    int entranceStyle = 0;
    float entranceDuration = .35f;
    bool enableExit = false;
    float exitDuration = .20f;
    float entranceStaggerStep = .06f;
    float entranceStaggerMax = .8f;

    Settings::VisualSettings visual = {};

    // Non-POD collections (value copies - small, recaptured on generation change)
    std::vector<Settings::TierDefinition> tiers = {};
    std::string titleFormat = {};
    std::vector<Settings::Segment> displayFormat = {};
    std::vector<Settings::Segment> infoFormat = {};
    std::vector<Settings::SpecialTitleDefinition> specialTitles = {};

    // Pre-sorted special titles (pointers into specialTitles above).
    // Populated once per snapshot via PopulateSortedSpecialTitles().
    std::vector<const Settings::SpecialTitleDefinition*> sortedSpecialTitles = {};

    /**
     * @struct LabelTokens
     * @brief Owned text and thresholds for contextual tokens.
     * @author Alex (<https://github.com/lextpf>)
     *
     * Supports `%r`, `%d`, and `%c`; actor views expire when the snapshot refreshes.
     */
    struct LabelTokens
    {
        std::string relFollower;
        std::string relAlly;
        std::string relNeutral;
        std::string relHostile;
        std::string ldWeak;
        std::string ldEven;
        std::string ldStrong;
        std::string ldDeadly;
        std::string ctNPC;
        std::string ctBeast;
        std::string ctUndead;
        std::string ctDaedra;
        std::string ctDragon;
        int deltaWeakBelow = -5;
        int deltaStrongAbove = 5;
        int deltaDeadlyAbove = 10;
    };
    LabelTokens labels = {};

    /**
     * @struct IconTokens
     * @brief Status-icon configuration with derived colors.
     * @author Alex (<https://github.com/lextpf>)
     *
     * Layout drops unloaded icons. Badges require an enabled gate and an icon folder.
     */
    struct IconTokens
    {
        bool enabled = false;
        float scale = 1.0f;
        bool deadlyPulse = true;
        std::string icoFollower;
        std::string icoAlly;
        std::string icoHostile;
        std::string icoWeak;
        std::string icoStrong;
        std::string icoDeadly;
        std::string icoBeast;
        std::string icoUndead;
        std::string icoDaedra;
        std::string icoDragon;
        Settings::Color3 colFollower{};
        Settings::Color3 colAlly{};
        Settings::Color3 colHostile{};
        Settings::Color3 colWeak{};
        Settings::Color3 colStrong{};
        Settings::Color3 colDeadly{};
        Settings::Color3 colCreature{};

        std::string icoNeutral;   ///< Resting relationship slot (muted)
        std::string icoHumanoid;  ///< Resting creature slot (muted)
        std::string icoEven;      ///< Resting threat slot (muted)
        std::string icoGuard;
        std::string icoMerchant;
        std::string icoCommoner;  ///< Resting role slot (muted)
        std::string icoEssential;
        std::string icoProtected;
        std::string icoMortal;  ///< Resting protection slot (muted)
        std::string icoCombat;
        std::string icoAlert;
        std::string icoIdle;  ///< Resting engagement slot (muted)
        std::string icoSneakHidden;
        std::string icoSneakDetected;
        std::string icoSneakOff;  ///< Resting sneak slot (muted)
        std::string icoEncumbered;
        std::string icoNormalWeight;  ///< Resting encumbrance slot (muted)
        std::string icoWanted;
        std::string icoBountyClear;  ///< Resting bounty slot (muted)
        std::string icoTierLow;
        std::string icoTierMid;
        std::string icoTierHigh;
        Settings::Color3 colGuard{};
        Settings::Color3 colMerchant{};
        Settings::Color3 colEssential{};
        Settings::Color3 colProtected{};
        Settings::Color3 colCombat{};
        Settings::Color3 colAlert{};
        Settings::Color3 colSneakHidden{};
        Settings::Color3 colSneakDetected{};
        Settings::Color3 colEncumbered{};
        Settings::Color3 colWanted{};
        Settings::Color3 colTierLow{};
        Settings::Color3 colTierMid{};
        Settings::Color3 colTierHigh{};
        // Full-color emblem images for the tier badge.  Used instead of the medal/gem/crown
        // icons above when enabled and the images loaded.
        bool tierBadgeImages = false;  ///< Use emblem PNGs for the tier badge
        int tierImageCount = 0;        ///< Emblem images loaded (from BadgeTextures)
        float tierBadgeGamma = 1.0f;   ///< Curve exponent without a Badge key (>1 = high rarer)
        float tierBadgeScale = 1.7f;   ///< Emblem size as a multiple of the status-icon size
        Settings::Color3 colNeutral{};
        Settings::Color3 colHumanoid{};
        Settings::Color3 colCommoner{};
        Settings::Color3 colMortal{};
        Settings::Color3 colEven{};
        Settings::Color3 colIdle{};
        Settings::Color3 colSneakOff{};
        Settings::Color3 colNormalWeight{};
        Settings::Color3 colBountyClear{};
        Settings::Color3 colMuted{};  ///< Unused shared tint; no draw path reads it
        // per-slot enables (a disabled active state drops that actor's badge).
        bool relationshipEnabled = true;
        bool creatureEnabled = true;
        bool threatEnabled = true;
        bool roleEnabled = true;
        bool protectionEnabled = true;
        bool engagementEnabled = true;  ///< Master gate for the NPC engagement slot
        bool combatStateEnabled = true;
        bool alertStateEnabled = true;
        bool sneakEnabled = true;
        bool playerCombatEnabled = true;
        bool encumberedEnabled = true;
        bool bountyEnabled = true;
        bool tierEnabled = true;  ///< Gate for the actor rank badge
        float opacity = 0.92f;
        // Muted styling: alpha multiplier and desaturation strength [0,1].
        float mutedAlpha = 1.0f;
        float mutedDesat = 0.18f;
        // Player-only lighting block behind the badge strip and the emblem (see DrawBadges
        // and DrawTierEmblem).  Colors are optional: empty => derive from the name color.
        bool playerStripBedEnabled = true;
        float playerStripBedAlpha = 0.10f;
        float playerStripBedSize = 2.6f;
        float playerStripBedBreatheHz = 0.14f;
        std::optional<Settings::Color3> playerStripBedColor;
        bool emblemBacklightEnabled = true;
        float emblemBacklightSize = 2.6f;
        float emblemBacklightAlpha = 0.55f;
        float emblemBacklightBreatheHz = 0.167f;
        float emblemCrispAlpha = 0.95f;
        std::optional<Settings::Color3> emblemBacklightColor;

        bool playerRimLightEnabled = true;
        float playerRimAlpha = 0.22f;
        float playerCarveAlpha = 0.26f;
        float playerRimOffset = 1.0f;
        std::optional<Settings::Color3> playerRimColor;
        bool emblemKeyFillEnabled = true;
        float emblemKeyAlpha = 0.35f;
        float emblemFillAlpha = 0.15f;
        float emblemKeyRise = 0.18f;
        float emblemFillDrop = 0.15f;
        std::optional<Settings::Color3> emblemKeyColor;
        std::optional<Settings::Color3> emblemFillColor;
    };
    IconTokens icons = {};

    /// Focus-target expanded-nameplate settings (value copy).
    Settings::FocusSettings focus = {};

    /// Graffito perspective-correct actor-plane typography settings.
    Settings::GraffitoSettings graffito = {};
    float graffitoWrapRadians = .0f;  ///< Cached WrapDegrees conversion for render-time geometry.

    /// Camera-motion quieting settings (value copy).
    Settings::QuietSettings quiet = {};

    /// Death-fade settings, from the DeathRite INI section.
    bool deathRiteEnabled = true;
    float deathRiteDuration = 1.6f;

    /// Context-conditional setting profiles, from the RegisterN INI sections (value copies).
    bool registersEnabled = true;
    float registerTransitionTime = 1.2f;
    std::vector<Settings::RegisterDefinition> registers = {};

    /// HUD-compat yield settings: fade our plate when another HUD mod owns the actor.
    float compatTrueHUDYieldAlpha = .0f;
    float compatYieldSettleTime = .3f;

    /// Scene-adaptive text color settings.
    bool candleEnabled = true;
    float candleStrength = .08f;
    float candleWarmth = .5f;
    float candleSettleTime = .6f;

    /// Per-pixel depth occlusion settings.
    bool depthClipEnabled = true;
    float depthClipFeather = 2.5f;

    /**
     * @fn static RenderSettingsSnapshot CaptureFromSettings()
     * @brief Capture all render settings while a shared settings lock is held.
     * @author Alex (<https://github.com/lextpf>)
     */
    static RenderSettingsSnapshot CaptureFromSettings();

    /**
     * @fn void PopulateSortedSpecialTitles()
     * @brief Sort nonempty special titles by descending priority.
     * @author Alex (<https://github.com/lextpf>)
     *
     * Call after the snapshot reaches its final location. Pointers alias `specialTitles`;
     * copying this snapshot leaves the copy's pointers attached to the original.
     */
    void PopulateSortedSpecialTitles();
};

/**
 * @struct ActorCache
 * @brief Render-thread animation state, keyed by FormID in RendererState::cache.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Position combines a moving average with exponential smoothing. Large jumps
 * use `Visual().LargeMovementBlend` to limit lag. The cache retains no engine pointers.
 */
struct ActorCache
{
    /// Screen-pixel anchor after moving-average and exponential smoothing.
    ImVec2 smooth{};
    float alphaSmooth = 1.0f;      ///< Smoothed alpha for fade transitions
    float textSizeScale = 1.0f;    ///< Smoothed font scale for distance-based sizing
    float occlusionSmooth = 1.0f;  ///< Smoothed occlusion (1.0=visible, 0.0=hidden)

    bool initialized = false;    ///< True after first frame of data
    uint32_t lastSeenFrame = 0;  ///< Frame counter when actor was last in snapshot

    // The next two fields are never read or written.  The live line-of-sight throttle state
    // is OcclusionCacheEntry, on the game thread.
    uint32_t lastOcclusionCheckFrame = 0;  ///< Unused; see OcclusionCacheEntry::lastCheckFrame
    bool cachedOccluded = false;           ///< Unused; see OcclusionCacheEntry::cachedOccluded
    bool wasOccluded = false;              ///< Previous frame's occlusion state

    static constexpr int HISTORY_SIZE = RenderConstants::POSITION_HISTORY_SIZE;
    ImVec2 posHistory[HISTORY_SIZE]{};  ///< Circular buffer of raw screen positions
    int historyIndex = 0;               ///< Current write index in posHistory
    bool historyFilled = false;         ///< True once posHistory has wrapped at least once

    float typewriterTime = .0f;       ///< Seconds since actor first appeared
    bool typewriterComplete = false;  ///< True when reveal animation finished
    /// Console edits rearm reveal, temporarily disabling player/aim forced completion.
    bool revealArmed = false;

    float entrancePhase = .0f;  ///< Entrance animation progress (0=start, 1=complete)
    float exitPhase = .0f;      ///< Exit animation progress (0=visible, 1=fully exited)
    bool entranceDone = false;  ///< True when entrance animation finished

    /// Seconds until entrance; negative means unassigned. Slots follow snapshot order.
    float entranceDelay = -1.0f;

    // It plays only for actors this cache entry saw alive; corpses first seen dead never
    // render it.  deathDone latches, so the fade never repeats for the same corpse while
    // this entry lives.
    bool sawAlive = false;   ///< Entry rendered this actor alive at least once
    float deathPhase = .0f;  ///< Fade progress (0=just died, 1=finished)
    bool deathDone = false;  ///< Fade finished, or cancelled on overlay wake

    /// Focus weight from 0 to 1; controls ambient alpha and title/info visibility.
    float focusSmooth = .0f;

    /// Render-thread yaw smoothing hides game-thread snapshot steps.
    float graffitoYaw = .0f;
    bool graffitoYawInitialized = false;
    bool graffitoForcedFront = false;  ///< Previous live frame was held readable by the aim ray

    // The game thread publishes rendered-head samples.  On render frames that reuse a
    // sample, a bounded velocity estimate bridges the gap.  NPCs do not use these fields.
    RE::NiPoint3 playerPoseSample{};    ///< Last published rendered-head position (world units)
    RE::NiPoint3 playerPoseVelocity{};  ///< Estimated head velocity (world units per second)
    /// Sample time in PoseClockSeconds steady-clock seconds, not ImGui time.
    double playerPoseSampleTime = .0;
    /// Sample interval in seconds, clamped between 0.001 and 0.050; seeded to 1/60.
    double playerPoseInterval = 1.0 / 60.0;
    bool playerPoseInitialized = false;  ///< True once a first sample seeded the tracker

    /// HUD yield weight; approaches 1 over compatYieldSettleTime while covered.
    float yieldSmooth = .0f;

    float bgLum = -1.0f;  ///< Smoothed background luminance (-1 = no sample yet)
    float bgR = .5f;      ///< Smoothed background red
    float bgG = .5f;      ///< Smoothed background green
    float bgB = .5f;      ///< Smoothed background blue

    /// World-space trail history; current-camera reprojection excludes camera-only motion.
    static constexpr int TRAIL_HISTORY_SIZE = 8;
    RE::NiPoint3 trailHistory[TRAIL_HISTORY_SIZE]{};  ///< Trail ghost positions (world space)
    int trailIndex = 0;                               ///< Current write index in trailHistory
    bool trailFilled = false;                         ///< True once trailHistory has wrapped

    std::string cachedName;       ///< Last known name (to detect changes)
    std::string cachedNameLower;  ///< Pre-lowered name for special title matching

    /**
     * @fn ImVec2 AddAndGetSmoothed(const ImVec2& pos)
     * @brief Append a screen-pixel sample and return the filled ring-buffer mean.
     * @author Alex (<https://github.com/lextpf>)
     *
     * The first call returns the sample unchanged.
     */
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

/**
 * @enum RelationshipKind
 * @brief Player-relationship channel for the %r token and the badge strip.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Also selects the folio glass tint and the NPC text support tint
 * (`RenderSettingsSnapshot::npcColors`).
 */
enum class RelationshipKind : std::uint8_t
{
    Hostile,  ///< IsHostileToActor(player)
    Neutral,  ///< Default - neither hostile, nor teammate, nor talkable
    Ally,     ///< CanTalkToPlayer (friendly NPC) but not a teammate
    Follower  ///< IsPlayerTeammate
};

/**
 * @enum LevelDelta
 * @brief Actor level relative to player level for the %d token.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * `Settings::Labels()` requires WeakAtOrBelow < StrongAtOrAbove < DeadlyAtOrAbove.
 * Invalid ordering restores -5, +5, +10. The Even band need not be symmetric.
 */
enum class LevelDelta : std::uint8_t
{
    Weak,    ///< Delta <= WeakAtOrBelow
    Even,    ///< WeakAtOrBelow < delta < StrongAtOrAbove (the band width is user-defined)
    Strong,  ///< StrongAtOrAbove <= delta < DeadlyAtOrAbove
    Deadly   ///< Delta >= DeadlyAtOrAbove (tested first, so it wins over Strong)
};

/**
 * @enum CreatureKind
 * @brief Coarse actor classification for the %c token.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Derived from the `ActorTypeX` keyword set.  NPC is the fallback when no creature keyword
 * matches.
 */
enum class CreatureKind : std::uint8_t
{
    NPC,     ///< Humanoid (ActorTypeNPC) - default
    Beast,   ///< ActorTypeCreature / ActorTypeAnimal
    Undead,  ///< ActorTypeUndead
    Daedra,  ///< ActorTypeDaedra
    Dragon   ///< ActorTypeDragon
};

/**
 * @enum RoleKind
 * @brief Social role for the NPC role badge slot.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Commoner is the muted default; Guard and Merchant render lit.  Derived from `IsGuard()`
 * and vendor-faction membership on the game thread.
 */
enum class RoleKind : std::uint8_t
{
    Commoner,  ///< No notable role - muted
    Merchant,  ///< Member of a vendor faction (TESFaction::IsVendor)
    Guard      ///< Actor::IsGuard()
};

/**
 * @enum ProtectionKind
 * @brief Invulnerability status for the protection badge slot.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Mortal is the muted default.  Essential overrides Protected when both flags are set.
 */
enum class ProtectionKind : std::uint8_t
{
    Mortal,     ///< Killable - muted
    Protected,  ///< Only the player can kill (IsProtected)
    Essential   ///< Cannot be killed (IsEssential)
};

/**
 * @enum EngagementKind
 * @brief Awareness state for the engagement badge slot.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * One mutually exclusive axis: combat supersedes alert, alert supersedes idle.  Idle is
 * muted.
 */
enum class EngagementKind : std::uint8_t
{
    Idle,   ///< Not in combat, not alerted - muted
    Alert,  ///< aware/searching proxy (weapon drawn + detects player)
    Combat  ///< IsInCombat()
};

/**
 * @enum SneakKind
 * @brief Player stealth state for the sneak badge slot.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Off is the muted default (not sneaking); Hidden and Detected render lit while sneaking.
 */
enum class SneakKind : std::uint8_t
{
    Off,      ///< Not sneaking - muted
    Hidden,   ///< Sneaking, undetected
    Detected  ///< Sneaking, detected by a nearby hostile
};

/**
 * @struct ActorDrawData
 * @brief Actor facts published from the game thread to the render thread.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Contains no engine pointers. Dead actors publish identity, pose, targeting bounds,
 * heading, distance, level, uniqueness and `isDead` only. Death fades use the last live
 * `RendererState::lastDrawData` entry for contextual facts.
 * Override records own their strings, so frame copies can outlive the store and
 * release the last record reference on the render thread.
 */
struct ActorDrawData
{
    uint32_t formID{0};       ///< actor's form ID (unique identifier)
    RE::NiPoint3 worldPos{};  ///< Nameplate anchor above the head, in world units
    RE::NiPoint3 feetPos{};   ///< Actor root/feet position for portrait framing
    /// Rendered world-bound center; absent 3D or radius below 4 or above 4096 uses actor height.
    RE::NiPoint3 raycastCenter{};
    float raycastRadius{.0f};   ///< Radius of that sphere, in world units
    float headingRadians{.0f};  ///< Actor world yaw, captured on the game thread
    /// worldPos/feetPos sample time in steady-clock seconds for player extrapolation.
    double poseSampleTime{.0};
    std::string name;         ///< Display name (capitalized); may be empty for an NPC
    uint16_t level{0};        ///< actor's level
    float distToPlayer{.0f};  ///< Distance to the player in world units; 0 for the player
    bool isPlayer{false};     ///< Whether this is the player character
    bool deckOnly{false};     ///< Extra crosshair target retained only for card capture
    bool isUnique{false};     ///< Unique actor base; eligible for collectible rarity rolls
    bool isOccluded{false};   ///< Line-of-sight result; stays false when occlusion is off
    /// Death marker; rendering replays cached live facts.
    bool isDead{false};

    /// Empty means none; replaces tier title, below special-title precedence.
    std::string honorific;

    /// Immutable override record owned across frame copies; null means no override.
    std::shared_ptr<const ActorOverrides::Record> overrides;

    bool yieldPlate{false};  ///< TrueHUD floats a bar here - fade the plate out
    bool yieldLevel{false};  ///< moreHUD shows this target's level - drop ours

    RelationshipKind relationship{RelationshipKind::Neutral};  ///< %r token source
    LevelDelta levelDelta{LevelDelta::Even};                   ///< %d token source
    CreatureKind creatureKind{CreatureKind::NPC};              ///< %c token source

    // NPC slots: role, protection, engagement.  Player slots: sneak, playerInCombat,
    // encumbered, wanted.  Each drives one always-on badge.
    RoleKind role{RoleKind::Commoner};                  ///< NPC role badge
    ProtectionKind protection{ProtectionKind::Mortal};  ///< NPC protection badge
    EngagementKind engagement{EngagementKind::Idle};    ///< NPC engagement badge
    SneakKind sneak{SneakKind::Off};                    ///< Player sneak badge
    bool playerInCombat{false};                         ///< Player engagement badge
    bool encumbered{false};                             ///< Player encumbered badge
    bool wanted{false};                                 ///< Player bounty badge
};

/**
 * @struct ComposedBadgeSlot
 * @brief Badge facts before texture lookup and layout.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * `icon` aliases the active settings snapshot or `ActorOverrides::Record`.
 * Both owners must outlive the slot; settings views also expire on snapshot refresh.
 */
struct ComposedBadgeSlot
{
    std::string_view icon;     ///< Duotone icon name, resolved later by BadgeTextures
    Settings::Color3 color{};  ///< Semantic tint for this slot
    bool muted = false;        ///< Resting state: dimmed and desaturated at draw time
    bool pulse = false;        ///< Breathing alpha (the Deadly threat slot)
    int tierImage = -1;        ///< >=0 -> full-color emblem image in place of an icon
    bool isRank = false;       ///< Rank identity mark, distinct from actor-status slots
};

/**
 * @brief Seven NPC slots plus RenderConstants::MAX_EXTRA_BADGES.
 *
 * Increase the base capacity when adding an NPC slot. Overflow is silently discarded.
 */
inline constexpr int MAX_BADGE_SLOTS = 7 + RenderConstants::MAX_EXTRA_BADGES;

/**
 * @struct BadgeComposition
 * @brief Ordered badges shared by nameplates and Deck cards.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Mutators silently discard empty icons and slots beyond capacity.
 */
struct BadgeComposition
{
    ComposedBadgeSlot slots[MAX_BADGE_SLOTS];  ///< Filled slots, in draw order
    int count = 0;                             ///< Filled slot count, at most MAX_BADGE_SLOTS

    /**
     * @fn void push(std::string_view icon, Settings::Color3 color, bool muted, bool pulse = false)
     * @brief Append a status badge with optional muted tint and alpha pulse.
     * @author Alex (<https://github.com/lextpf>)
     */
    void push(std::string_view icon, Settings::Color3 color, bool muted, bool pulse = false)
    {
        if (count < MAX_BADGE_SLOTS && !icon.empty())
        {
            slots[count++] = {icon, color, muted, pulse};
        }
    }

    /**
     * @fn void pushRank(std::string_view icon, Settings::Color3 color, bool muted, bool pulse =
     *     false)
     * @brief Append an icon as the rank identity slot.
     * @author Alex (<https://github.com/lextpf>)
     */
    void pushRank(std::string_view icon, Settings::Color3 color, bool muted, bool pulse = false)
    {
        if (count < MAX_BADGE_SLOTS && !icon.empty())
        {
            slots[count++] = {icon, color, muted, pulse, -1, true};
        }
    }

    /**
     * @fn void pushTierImage(bool enabled, int imageIndex)
     * @brief Append a full-color rank emblem.
     * @author Alex (<https://github.com/lextpf>)
     *
     * Ignored when disabled, full, or `imageIndex` is negative.
     */
    void pushTierImage(bool enabled, int imageIndex)
    {
        if (enabled && count < MAX_BADGE_SLOTS && imageIndex >= 0)
        {
            slots[count++] = ComposedBadgeSlot{{}, {}, false, false, imageIndex, true};
        }
    }
};

/**
 * @struct OcclusionCacheEntry
 * @brief Game-thread line-of-sight cache keyed by FormID.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Checks are separated by at least one snapshot and `Settings::Occlusion().CheckInterval`.
 * Each snapshot prunes unseen actors. Entries exist only while culling is enabled.
 */
struct OcclusionCacheEntry
{
    /// Last game-thread SnapshotState::frame checked; zero forces a fresh check.
    uint32_t lastCheckFrame{0};
    bool cachedOccluded{false};  ///< Result of that check; reused until the interval expires
};

/**
 * @struct RendererState
 * @brief Mutable renderer state behind GetState().
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Render-thread only unless noted. Game-thread writes require `snapshotLock` or atomics.
 * Draw gates and the active register are published independently of the actor snapshot;
 * they can describe a newer collection pass. Only snapshot and target form one locked pair.
 */
struct RendererState
{
    std::unordered_map<uint32_t, ActorCache> cache;  ///< Per-actor animation cache, keyed by formID
    std::unordered_map<uint32_t, ActorDrawData>
        lastDrawData;    ///< Last live draw data; the death fade and exit animation replay it
    uint32_t frame = 0;  ///< Render-thread frame counter

    std::vector<ActorDrawData> snapshot;  ///< Current actor draw data; guarded by snapshotLock
    /// snapshotLock guards this FormID. Zero unless moreHUD level-yield or Deck resolves it.
    uint32_t crosshairTarget = 0;
    std::mutex snapshotLock;                        ///< Publishes snapshot and target together
    std::atomic<bool> updateQueued{false};          ///< True while a game-thread update is pending
    std::atomic<bool> pauseSnapshotUpdates{false};  ///< Suppress updates during reload
    /// Next draw drops the player cache entry and restarts name reveal.
    std::atomic<bool> pendingIdentityRefresh{false};
    std::atomic<bool> snapshotUpdateRunning{false};         ///< Game-thread update is active
    std::atomic<bool> clearOcclusionCacheRequested{false};  ///< Request to clear occlusion cache

    bool wasInInvalidState = true;  ///< True if previous frame was in an invalid game state
    int postLoadCooldown = 0;       ///< Frames remaining in post-load cooldown

    /// Rearm visible entrances/typewriters on wake; assign stagger slots in snapshot order.
    bool wakeReplayPending = false;
    /// Stagger slots claimed this frame; reset before drawing.
    int entrancesStartedThisFrame = 0;

    // Smoothed quiet factors in [0,1] driven by camera angular speed: 0 = settled, 1 = fully
    // quiet.  The envelope is asymmetric (Quiet().AttackTime rising, the per-row release time
    // falling), so a pan quiets fast and resolves back slowly.
    RE::NiPoint3 prevCamForward{};  ///< Camera forward on the previous frame
    bool prevCamValid = false;      ///< prevCamForward holds a real sample
    /// Advanced each frame but unused by drawing, as is Quiet().NameFloor.
    float quietName = .0f;
    /// Folds title/badges on live and exit plates, info/badges during death rites.
    float quietSub = .0f;

    // The game thread publishes the active register index each snapshot; the render thread
    // eases its effective values toward that register's values (or the 1/1/1/0 base state)
    // so transitions ramp instead of snapping.
    std::atomic<int> activeRegister{-1};  ///< Index into snapshot registers, -1 = none
    float regAlphaMul = 1.0f;             ///< Smoothed overlay-wide alpha multiplier
    /// Multiplies fadeStartDistance/fadeEndDistance, excluding font-scale distances.
    float regFadeMul = 1.0f;
    float regSubLineMul = 1.0f;  ///< Smoothed sub-line alpha multiplier
    float regHideNeutral = .0f;  ///< Smoothed neutral/ally plate hide factor

    /// True when depth SRV and polarity resolve for this render frame.
    bool depthClipFrame = false;

    DebugOverlay::Stats debugStats;   ///< Debug overlay performance metrics
    float lastDebugUpdateTime = .0f;  ///< Time of last debug stats refresh
    int updateCounter = 0;            ///< Total snapshot updates since startup
    int lastUpdateCount = 0;          ///< Update counter at last debug refresh

    bool reloadKeyWasDown = false;             ///< Previous frame's reload key state
    float lastReloadTime = -10.0f;             ///< Time of last settings reload
    std::atomic<bool> reloadRequested{false};  ///< Render thread requests reload
    std::atomic<bool> reloadCompleted{false};  ///< Set by game thread when async Load() finishes

    // allowOverlay and allowDeck are published by UpdateSnapshot_GameThread.
    std::atomic<bool> allowOverlay{false};  ///< True when game state allows overlay
    std::atomic<bool> allowDeck{false};     ///< World is safe for an explicit card capture
    /// Manual gate written by ToggleEnabled and SetEnabled.
    std::atomic<bool> manualEnabled{true};

    /// Cached settings snapshot (re-captured only when Settings::Generation() changes).
    RenderSettingsSnapshot cachedSnap;
    uint32_t lastSnapGeneration = 0;  ///< Generation counter at last snapshot capture
};

/**
 * @struct RenderSeg
 * @brief One formatted text segment on the main or info nameplate line.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 */
struct RenderSeg
{
    std::string text;              ///< Formatted text to display
    std::string displayText;       ///< Text after typewriter truncation
    bool isLevel = false;          ///< Whether to use level font
    bool containsName = false;     ///< Whether the source format contains %n
    ImFont* font = nullptr;        ///< Font to use for rendering
    float fontSize = .0f;          ///< Scaled (and potentially fitted) font size
    float horizontalScale = 1.0f;  ///< Name-only width condensation
    ImVec2 size{};                 ///< Measured size after horizontal fitting
    ImVec2 displaySize{};          ///< Fitted size of displayText
};

/**
 * @fn inline void ScaleNewVerticesX(ImDrawList* drawList, int firstVertex, float anchorX, float
 *     horizontalScale)
 * @brief Scale emitted vertices horizontally around the text anchor.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Keeps fitted text aligned across all draw passes. Null lists and scales >= 0.9999
 * cause no change. Negative `firstVertex` clamps to zero.
 *
 * @param anchorX Horizontal origin, in screen pixels.
 */
inline void ScaleNewVerticesX(ImDrawList* drawList,
                              int firstVertex,
                              float anchorX,
                              float horizontalScale)
{
    if (!drawList || horizontalScale >= .9999f)
    {
        return;
    }

    const int vertexEnd = drawList->VtxBuffer.Size;
    firstVertex = std::max(firstVertex, 0);
    for (int i = firstVertex; i < vertexEnd; ++i)
    {
        float& x = drawList->VtxBuffer[i].pos.x;
        x = anchorX + (x - anchorX) * horizontalScale;
    }
}

/**
 * @struct BadgeDrawItem
 * @brief Loaded badge texture and screen-pixel layout.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Color has no alpha; DrawBadges applies frame alpha and the Deadly pulse.
 */
struct BadgeDrawItem
{
    ImTextureID tex = 0;     ///< Rasterized duotone icon SRV (from BadgeTextures)
    Settings::Color3 color;  ///< Semantic badge tint (ignored when fullColor)
    ImVec2 pos;              ///< Top-left draw position (screen pixels)
    ImVec2 size;             ///< Draw size (square)
    bool pulse;              ///< Deadly skull breathing alpha
    bool muted = false;      ///< Neutral/inactive slot - dimmed + desaturated at draw time
    bool fullColor = false;  ///< True for the emblem tier badge: draw untinted (white multiply)
    bool isRank = false;     ///< Rank identity mark rather than an actor-status indicator
};

/**
 * @struct ActorLabelContext
 * @brief Frame-local views for FormatString.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Relationship, delta and creature views alias the settings snapshot. `name` aliases
 * the frame's ActorDrawData or a static space for an empty name. Owners must outlive use.
 */
struct ActorLabelContext
{
    std::string_view name;          ///< Actor display name (%n)
    int level = 0;                  ///< Actor level (%l)
    const char* title = nullptr;    ///< Tier or special title (%t); nullable
    std::string_view relationship;  ///< Resolved %r label
    std::string_view levelDelta;    ///< Resolved %d label
    std::string_view creatureKind;  ///< Resolved %c label
    std::uint32_t formID = 0;       ///< Actor identity; current token expansion does not use it.
};

/**
 * @struct LabelStyle
 * @brief Colors and effects resolved once per label.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Player colors use the tier palette. NPC names stay white; title and level use tier
 * level colors. NPC colors supply relationship and support tints.
 */
struct LabelStyle
{
    int tierIdx;                           ///< Index into snap.tiers; 0 when no tiers configured
    const Settings::TierDefinition* tier;  ///< Matched tier, or GetFallbackTier(); never null
    const Settings::SpecialTitleDefinition* specialTitle;  ///< Matched special title, or null

    const Settings::EffectParams* nameEffect;
    const Settings::EffectParams* levelEffect;
    const Settings::EffectParams* titleEffect;
    bool usesTierVisuals;  ///< Player / special title: tier effects, wave, shine overlay

    ImU32 colL, colR;            ///< Name gradient (packed)
    ImU32 colLTitle, colRTitle;  ///< Title gradient
    ImU32 colLLevel, colRLevel;  ///< Level gradient
    ImU32 highlight;             ///< Shimmer / sparkle highlight

    ImVec4 LcName, RcName;    ///< Resolved name color (float, for glow).
    ImVec4 LcTitle, RcTitle;  ///< Resolved title color (float, for glow).
    ImVec4 LcLevel, RcLevel;  ///< Resolved level color (float, for glow).
    ImVec4 supportName;       ///< Representative tint for name support layers.
    ImVec4 supportTitle;      ///< Representative tint for title support layers.
    ImVec4 supportLevel;      ///< Representative tint for level support layers.
    ImVec4 specialGlowColor;  ///< Special title glow color.

    float alpha;        ///< Plate alpha for the main line, after distance fade and dimming
    float titleAlpha;   ///< Alpha * Visual().TitleAlphaMultiplier
    float levelAlpha;   ///< Alpha * Visual().LevelAlphaMultiplier
    float effectAlpha;  ///< Alpha for the effect layers: alpha scaled by the tier level band
    float strength;     ///< Effect strength multiplier from tier intensity and level position
    float phase01;      ///< Animation phase in [0,1]; seeded per formID so plates desynchronize

    // Tier effect gates.  Each is true when tier gating is off, or when tierIdx reaches the
    // matching Visual() minimum tier.
    bool tierAllowsGlow;
    bool tierAllowsParticles;
    bool tierAllowsOrnaments;
    bool outlineGlowAllowed = true;  ///< False for restrained Graffito world typography

    float nameOutlineWidth;   ///< Outline width for the name row, in pixels
    float levelOutlineWidth;  ///< Outline width for the level row, in pixels
    float titleOutlineWidth;  ///< Outline width for the title row, in pixels
    float outlineWidth;       ///< Primary outline width (a copy of nameOutlineWidth)

    // Stored for deferred outline width computation (see CalcOutlineWidth).
    float baseOutlineWidth;  ///< OutlineWidthMin + OutlineWidthMax, before per-row scaling
    float distToPlayer;      ///< Distance to the player in game units (~70 per metre)

    /// Info-row opacity from 0 to 1; focus hides it on ambient actors.
    float infoAlphaMul = 1.0f;

    /// Badge opacity multiplier; folds the strip during camera pans.
    float badgeAlphaMul = 1.0f;

    /**
     * @fn float CalcOutlineWidth(float fontSize, const RenderSettingsSnapshot& snap) const
     * @brief Scale outline width for a row's font size.
     * @author Alex (<https://github.com/lextpf>)
     *
     * The size ratio floors at `RenderConstants::OUTLINE_MIN_SCALE`. The optional distance
     * ramp uses snapshot fade distances; register multipliers do not change it.
     *
     * @param fontSize Row font size, in pixels.
     * @return Outline width, in pixels.
     */
    float CalcOutlineWidth(float fontSize, const RenderSettingsSnapshot& snap) const
    {
        float ratio = std::max(fontSize / snap.nameFontSize, RenderConstants::OUTLINE_MIN_SCALE);
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

/**
 * @struct LabelLayout
 * @brief Measured text and positions in screen pixels, with y downward.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * `startPos` is the smoothed anchor plus scaled optical centering and overlap offsets.
 * Row offsets are relative to it; nameplate bounds are absolute.
 */
struct LabelLayout
{
    ImFont* fontName;       ///< Name font pointer
    ImFont* fontLevel;      ///< Level font pointer
    ImFont* fontTitle;      ///< Title font pointer
    float nameFontSize;     ///< Scaled name font size in pixels
    float levelFontSize;    ///< Scaled level font size in pixels
    float titleFontSize;    ///< Scaled title font size in pixels
    bool isPlayer = false;  ///< Copied from ActorDrawData; gates player-only embellishments

    std::vector<RenderSeg> segments;  ///< Main line segments (built from DisplayFormat)
    float mainLineWidth;              ///< Total width of all segments plus padding
    float mainLineHeight;             ///< Height of the tallest segment
    float segmentPadding;             ///< Horizontal padding between segments

    std::vector<RenderSeg> infoSegments;  ///< Info row segments (built from InfoFormat)
    float infoLineWidth = .0f;            ///< Total width of all info segments plus padding
    float infoLineHeight = .0f;           ///< Height of the tallest info segment

    std::vector<BadgeDrawItem> badges;  ///< Status icon badges (built from snap.icons)

    // Full-color rank emblem, on its own row above the icon strip and larger than the icons.
    // Screen-space plates add bloom; Graffito keeps it crisp.
    bool tierEmblemShown = false;
    ImTextureID tierEmblemTex = 0;
    ImVec2 tierEmblemPos{};   ///< Top-left draw position (screen pixels)
    ImVec2 tierEmblemSize{};  ///< Draw size (square)

    std::string titleStr;         ///< Full title text
    std::string titleDisplayStr;  ///< Title text after typewriter truncation
    ImVec2 titleSize;             ///< Measured size of titleDisplayStr

    ImVec2 startPos;  ///< Plate anchor in screen pixels (see the struct doc)
    // the three row offsets below are relative to startPos.y, not absolute screen y: a draw
    // site adds startPos.y.  mainLineY and titleY are negative (rows sit above the anchor)
    // and infoLineY is positive.
    float titleY;           ///< Y offset of the title line top
    float mainLineY;        ///< Y offset of the main line top
    float infoLineY = .0f;  ///< Y offset of the info row top; 0 when infoSegments is empty
    float totalWidth;       ///< Width of the widest of title/main/info line

    ImVec2 nameplateCenter;  ///< Center of the nameplate bounding box
    float nameplateTop;      ///< Top edge of nameplate (screen y)
    float nameplateBottom;   ///< Bottom edge of nameplate (screen y)
    float nameplateLeft;     ///< Left edge of nameplate (screen x)
    float nameplateRight;    ///< Right edge of nameplate (screen x)
    float nameplateWidth;    ///< Total nameplate width in pixels
    float nameplateHeight;   ///< Total nameplate height in pixels
    float mainLineCenterY;   ///< Vertical center of main line (for ornament anchoring)

    /// Text bounds before badge expansion, relative to startPos for later transforms.
    ImVec2 textBoundsMin{};
    ImVec2 textBoundsMax{};
};

/**
 * @struct DistanceFactors
 * @brief Distance-based fade, LOD, and scale factors for one label.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 */
struct DistanceFactors
{
    float alphaTarget;       ///< Target alpha from distance fade
    float textScaleTarget;   ///< Target font scale from distance
    float lodTitleFactor;    ///< LOD multiplier for title visibility [0,1]
    float lodEffectsFactor;  ///< LOD multiplier for particles/ornaments [0,1]
};

/**
 * @struct SnapshotState
 * @brief Game-thread collection state behind GetSnapshotState().
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 */
struct SnapshotState
{
    /// Game-thread counter stored by OcclusionCacheEntry; resets on cache clear.
    uint32_t frame = 0;
    RE::BGSKeyword* npcKeyword = nullptr;    ///< Cached ActorTypeNPC keyword for creature filtering
    bool npcKeywordLookupAttempted = false;  ///< True after first keyword lookup attempt
    bool npcKeywordMissingLogged = false;    ///< True after logging keyword-not-found warning
};
/**
 * @fn SnapshotState& GetSnapshotState()
 * @brief Return the game-thread snapshot-state singleton.
 * @author Alex (<https://github.com/lextpf>)
 */
SnapshotState& GetSnapshotState();

/// Unused alias; DebugOverlay reads RenderConstants directly.
inline constexpr float RELOAD_NOTIFICATION_DURATION = RenderConstants::RELOAD_NOTIFICATION_DURATION;

/**
 * @fn RendererState& GetState()
 * @brief Return the renderer-state singleton.
 * @author Alex (<https://github.com/lextpf>)
 */
RendererState& GetState();

/**
 * @fn std::unordered_map<uint32_t, OcclusionCacheEntry>& GetOcclusionCache()
 * @brief Return the game-thread per-actor occlusion cache.
 * @author Alex (<https://github.com/lextpf>)
 */
std::unordered_map<uint32_t, OcclusionCacheEntry>& GetOcclusionCache();

/**
 * @fn std::unordered_map<uint32_t, float>& OverlapOffsets()
 * @brief Return render-thread overlap offsets keyed by FormID.
 * @author Alex (<https://github.com/lextpf>)
 */
std::unordered_map<uint32_t, float>& OverlapOffsets();

/**
 * @fn bool WorldToScreen(const RE::NiPoint3& worldPos, RE::NiPoint3& screenPos, RE::NiPoint3*
 *     cameraPosOut = nullptr)
 * @brief Project world coordinates to screen pixels.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Render-thread exception: reads the world camera and BSGraphics renderer singletons.
 * Missing singletons or a point on/behind the camera plane return false.
 *
 * @param[out] screenPos x/y pixels with y downward; viewport depth z. Unchanged on failure.
 * @param[out] cameraPosOut Optional camera position; may be written even on failure.
 * @return Projection success. Callers must reject depth outside [0,1] and offscreen x/y.
 */
bool WorldToScreen(const RE::NiPoint3& worldPos,
                   RE::NiPoint3& screenPos,
                   RE::NiPoint3* cameraPosOut = nullptr);

/**
 * @fn std::string Capitalize(const char* text)
 * @brief Title-case ASCII words without changing UTF-8 sequences.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A multibyte codepoint does not start a new word. Trims outer spaces, tabs, CR and LF.
 *
 * @param text Null-terminated UTF-8; null yields an empty string.
 */
std::string Capitalize(const char* text);

/**
 * @fn void UpdateSnapshot_GameThread()
 * @brief Collect actor facts and publish draw gates on the game thread.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Distance filtering precedes full derivation. Priority is player, camera-ray target,
 * private Deck target, then distance. Publishes `crosshairTarget` under `snapshotLock`
 * and `activeRegister`; prunes occlusion and honorific caches.
 *
 * A pause clears both gates and the snapshot. Closed draw gates clear the snapshot
 * without scanning. A missing player or process list also clears the snapshot and Deck gate.
 * Gate publication precedes actor collection; it does not certify a completed snapshot.
 */
void UpdateSnapshot_GameThread();

/**
 * @fn void QueueSnapshotUpdate_RenderThread()
 * @brief Queue at most one game-thread snapshot task.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Render-thread only. `updateQueued` clears on completion or a missing SKSE interface.
 * Does nothing while `pauseSnapshotUpdates` is set.
 */
void QueueSnapshotUpdate_RenderThread();

/**
 * @fn uint32_t SelectFocusedActor(const std::vector<ActorDrawData>& snap, const
 *     RenderSettingsSnapshot& snapSettings)
 * @brief Select the eligible actor nearest the focus-cone center.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Render-thread camera read uses `Occlusion::GetCameraInfo`. Skips players, dead actors,
 * Deck-only entries and disallowed occlusion. Camera distance must be >= 1 world unit;
 * a configured maximum <= 0 adds no limit. Ties use dot product, player distance, FormID.
 *
 * @return FormID, or zero if disabled, camera unavailable, or no actor qualifies.
 */
uint32_t SelectFocusedActor(const std::vector<ActorDrawData>& snap,
                            const RenderSettingsSnapshot& snapSettings);

/**
 * @fn void CalcTightYBoundsFromTop( ImFont* font, float fontSize, const char* text, float& outTop,
 *     float& outBottom)
 * @brief Measure tight vertical glyph bounds.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param fontSize Font size, in pixels.
 * @param text Null-terminated UTF-8.
 * @param outTop Top glyph offset, in pixels.
 * @param outBottom Bottom glyph offset, in pixels.
 */
void CalcTightYBoundsFromTop(
    ImFont* font, float fontSize, const char* text, float& outTop, float& outBottom);

/**
 * @fn std::string FormatString(const std::string& fmt, const ActorLabelContext& ctx)
 * @brief Expand nameplate placeholders once.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Substitutes `%n`, `%l`, `%t`, `%r`, `%d`, `%c` without expanding replacement text.
 * Null `ctx.title` preserves `%t`. Unknown sequences and a trailing `%` stay literal.
 */
std::string FormatString(const std::string& fmt, const ActorLabelContext& ctx);

/**
 * @fn const Settings::TierDefinition& GetFallbackTier()
 * @brief Shared fallback used when no tiers are configured.
 * @author Alex (<https://github.com/lextpf>)
 */
const Settings::TierDefinition& GetFallbackTier();

/**
 * @fn BadgeComposition ComposeBadges(const ActorDrawData& d, const
 *     RenderSettingsSnapshot::IconTokens& cfg, int tierIdx, int tierCount, bool includeRank = true,
 *     int explicitBadge = 0)
 * @brief Compose ordered status badges for nameplates or cards.
 * @author Alex (<https://github.com/lextpf>)
 *
 * | Actor  | Slot order                                                         |
 * |--------|--------------------------------------------------------------------|
 * | NPC    | Rank, relationship, creature, role, protection, threat, engagement |
 * | Player | Rank, sneak, engagement, encumbered, bounty                        |
 *
 * Resting slots are muted. Empty icons are dropped. Overrides can hide, force, replace,
 * or append `RenderConstants::MAX_EXTRA_BADGES` lit extras. Forced states obey INI gates.
 * Disabled icons yield an empty set.
 *
 * Render-thread only: views alias `cfg` or `*d.overrides`; both must outlive the result. Settings
 * views expire on snapshot refresh. Do not call ActorOverrides::Get or build views from temporary
 * strings.
 *
 * @param tierCount <= 1 selects the lowest rank band.
 * @param explicitBadge One-based manifest emblem from the tier Badge key. Zero or an entry past
 *     the loaded emblems selects the TierBadgeGamma curve.
 */
BadgeComposition ComposeBadges(const ActorDrawData& d,
                               const RenderSettingsSnapshot::IconTokens& cfg,
                               int tierIdx,
                               int tierCount,
                               bool includeRank = true,
                               int explicitBadge = 0);

/**
 * @fn LabelStyle ComputeLabelStyle(const ActorDrawData& d, const std::string& nameLower, float
 *     alpha, float time, const RenderSettingsSnapshot& snap)
 * @brief Resolve tier, colors, effect gates and phase for one label.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param nameLower Lowercase name for special-title matching.
 * @param time Animation clock, in seconds.
 */
LabelStyle ComputeLabelStyle(const ActorDrawData& d,
                             const std::string& nameLower,
                             float alpha,
                             float time,
                             const RenderSettingsSnapshot& snap);

/**
 * @fn LabelLayout ComputeLabelLayout(const ActorDrawData& d, ActorCache& entry, const LabelStyle&
 *     style, float textSizeScale, const RenderSettingsSnapshot& snap, int forcedCharsToShow = -1)
 * @brief Measure text and position a label after typewriter truncation.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Missing plate fonts return an empty layout with null `fontName`; no other field is valid.
 *
 * @param forcedCharsToShow Negative uses the cache-driven typewriter budget.
 */
LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                               ActorCache& entry,
                               const LabelStyle& style,
                               float textSizeScale,
                               const RenderSettingsSnapshot& snap,
                               int forcedCharsToShow = -1);

/**
 * @fn void ApplyDeathRiteTint(LabelStyle& style, const ImVec4& target, float mixT, const
 *     RenderSettingsSnapshot& snap)
 * @brief Blend resolved colors toward target and reduce effect strength.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Repacks draw colors. Repeated calls form a multi-stop death-rite ramp.
 */
void ApplyDeathRiteTint(LabelStyle& style,
                        const ImVec4& target,
                        float mixT,
                        const RenderSettingsSnapshot& snap);

/**
 * @fn void ApplyCandlelight(LabelStyle& style, float bgLum, const float bgRGB[3], const
 *     RenderSettingsSnapshot& snap)
 * @brief Dim text over bright scenes and lift/warm it over dark scenes.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Uses smoothed `bgLum` and `bgRGB`; adjustment is capped by `snap.candleStrength`.
 * Repacks draw colors. Keep CandleAdjust in tests/test_utils.cpp synchronized.
 */
void ApplyCandlelight(LabelStyle& style,
                      float bgLum,
                      const float bgRGB[3],
                      const RenderSettingsSnapshot& snap);

/**
 * @fn constexpr float ParamOr(float p, float fallback)
 * @brief Use fallback when the configured effect parameter is not positive.
 * @author Alex (<https://github.com/lextpf>)
 */
constexpr float ParamOr(float p, float fallback)
{
    return p > .0f ? p : fallback;
}

/**
 * @fn void ApplyTextEffect(ImDrawList* drawList, ImFont* font, float fontSize, ImVec2 pos, const
 *     char* text, const Settings::EffectParams& effect, ImU32 colL, ImU32 colR, ImU32 highlight,
 *     ImU32 outlineColor, float outlineWidth, float phase01, float strength, float textSizeScale,
 *     float alpha, bool fastOutlines, const TextEffects::OutlineGlowParams* outlineGlow = nullptr,
 *     const TextEffects::DualOutlineParams* dualOutline = nullptr, const TextEffects::WaveParams*
 *     wave = nullptr, const TextEffects::ShineParams* shine = nullptr)
 * @brief Dispatch one configured text effect.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param fontSize Font size, in pixels.
 * @param pos Screen-pixel anchor.
 * @param text Null-terminated UTF-8.
 * @param outlineWidth Outline thickness, in pixels.
 * @param phase01 Animation phase in [0,1].
 * @param fastOutlines Selects four directions instead of eight.
 * @param outlineGlow Optional outline glow.
 * @param dualOutline Optional dual outline.
 * @param wave Optional wave displacement.
 * @param shine Optional top-edge shine.
 */
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

/**
 * @fn void DrawBackgroundGlow(ImDrawList* dl, const LabelStyle& style, const LabelLayout& layout,
 *     float lodTitleFactor, ImDrawListSplitter* splitter, const RenderSettingsSnapshot& snap)
 * @brief Draw the halo on the glow layer, behind particles, outlines and text.
 * @author Alex (<https://github.com/lextpf>)
 */
void DrawBackgroundGlow(ImDrawList* dl,
                        const LabelStyle& style,
                        const LabelLayout& layout,
                        float lodTitleFactor,
                        ImDrawListSplitter* splitter,
                        const RenderSettingsSnapshot& snap);

/**
 * @fn void DrawParticlesAndOrnaments(ImDrawList* dl, const ActorDrawData& d, const LabelStyle&
 *     style, const LabelLayout& layout, float lodEffectsFactor, float time, ImDrawListSplitter*
 *     splitter, bool fastOutlines, const RenderSettingsSnapshot& snap)
 * @brief Draw aura and ornaments with shared geometry.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Player only unless a special title forces the effect.
 *
 * @param time Animation clock, in seconds.
 */
void DrawParticlesAndOrnaments(ImDrawList* dl,
                               const ActorDrawData& d,
                               const LabelStyle& style,
                               const LabelLayout& layout,
                               float lodEffectsFactor,
                               float time,
                               ImDrawListSplitter* splitter,
                               bool fastOutlines,
                               const RenderSettingsSnapshot& snap);

/**
 * @fn void DrawTitleText(ImDrawList* dl, const LabelStyle& style, const LabelLayout& layout, float
 *     lodTitleFactor, ImDrawListSplitter* splitter, bool fastOutlines, const
 *     RenderSettingsSnapshot& snap)
 * @brief Draw the title above the main line.
 * @author Alex (<https://github.com/lextpf>)
 */
void DrawTitleText(ImDrawList* dl,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodTitleFactor,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap);

/**
 * @fn void DrawMainLineSegments(ImDrawList* dl, const LabelStyle& style, const LabelLayout& layout,
 *     ImDrawListSplitter* splitter, bool fastOutlines, const RenderSettingsSnapshot& snap)
 * @brief Draw the main line's formatted segments.
 * @author Alex (<https://github.com/lextpf>)
 */
void DrawMainLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap);

/**
 * @fn void DrawInfoLineSegments(ImDrawList* dl, const LabelStyle& style, const LabelLayout& layout,
 *     ImDrawListSplitter* splitter, bool fastOutlines, const RenderSettingsSnapshot& snap)
 * @brief Draw the info row when segments remain after drop-if-blank trimming.
 * @author Alex (<https://github.com/lextpf>)
 */
void DrawInfoLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap);

/**
 * @fn void DrawBadges(ImDrawList* dl, const LabelStyle& style, const LabelLayout& layout,
 *     ImDrawListSplitter* splitter, bool fastOutlines, const RenderSettingsSnapshot& snap, bool
 *     restrainedWorld = false)
 * @brief Draw badges whose textures were resolved during layout.
 * @author Alex (<https://github.com/lextpf>)
 *
 * `restrainedWorld` suppresses player lighting, rim light and glow for Graffito planes.
 * Only crisp icon quads remain.
 */
void DrawBadges(ImDrawList* dl,
                const LabelStyle& style,
                const LabelLayout& layout,
                ImDrawListSplitter* splitter,
                bool fastOutlines,
                const RenderSettingsSnapshot& snap,
                bool restrainedWorld = false);

/**
 * @fn void DrawTierEmblem(ImDrawList* dl, const LabelStyle& style, const LabelLayout& layout, float
 *     time, ImDrawListSplitter* splitter, const RenderSettingsSnapshot& snap, bool restrainedWorld
 *     = false)
 * @brief Draw the rank emblem above the status strip.
 * @author Alex (<https://github.com/lextpf>)
 *
 * `restrainedWorld` suppresses screen-space bloom.
 *
 * The emblem alpha is the style alpha times `badgeAlphaMul` times IconOpacity, clamped to 1,
 * which matches the status-icon row. The crisp mark then scales that alpha by EmblemCrispAlpha.
 * With EmblemBacklightEnabled and a soft-glow texture available, the backlight, key and fill
 * alphas scale the glow; otherwise three enlarged copies of the mark stand in at fixed factors.
 *
 * @param time Animation clock, in seconds.
 */
void DrawTierEmblem(ImDrawList* dl,
                    const LabelStyle& style,
                    const LabelLayout& layout,
                    float time,
                    ImDrawListSplitter* splitter,
                    const RenderSettingsSnapshot& snap,
                    bool restrainedWorld = false);

/**
 * @fn ImFont* GetFontAt(int index)
 * @brief Resolve a font slot with the first loaded font as fallback.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return A font borrowed from the current ImGui atlas, or null when no font is loaded.
 * @pre An initialized ImGui context on the render thread.
 */
ImFont* GetFontAt(int index);

/**
 * @fn void PruneCacheToSnapshot(const std::vector<ActorDrawData>& snap)
 * @brief Prune unseen cache and lastDrawData entries.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Grace is `CACHE_GRACE_FRAMES`, tripled during exit (0 < exitPhase < 1).
 * The finite ceiling also removes entries when exit animations are disabled.
 */
void PruneCacheToSnapshot(const std::vector<ActorDrawData>& snap);

/**
 * @fn float ExpApproachAlpha(float dt, float settleTime, float epsilon = .01f)
 * @brief Compute a frame-rate-independent exponential smoothing factor.
 * @author Alex (<https://github.com/lextpf>)
 *
 * $$\alpha = 1 - \epsilon^{\,dt / settleTime}$$
 *
 * @param dt Frame delta, in seconds; negatives clamp to zero.
 * @param settleTime Seconds to reach the residual threshold; floored at 1e-5.
 * @param epsilon Finite residual fraction in (0, 1); 0.01 leaves 1% of the initial delta.
 * @return Factor in [0,1] for `current = lerp(current, target, alpha)`.
 */
float ExpApproachAlpha(float dt, float settleTime, float epsilon = .01f);

}  // namespace Renderer
