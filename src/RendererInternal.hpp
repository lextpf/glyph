#pragma once

#include "PCH.hpp"

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
    bool fastOutlines = false;

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

    // Soft directional drop-shadow
    bool softShadowEnabled = false;
    float softShadowDistance = 4.0f;
    float softShadowSoftness = 3.0f;
    float softShadowOpacity = .8f;
    float softShadowAngle = 45.0f;
    int softShadowSamples = 12;

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
    float ornamentOffsetY = .0f;

    // Particles
    bool useParticleTextures = false;
    bool enableParticleAura = false;
    int particleCount = 0;
    float particleSize = .0f;
    float particleSpeed = .0f;
    float particleSpread = .0f;
    float particleAlpha = .0f;
    int particleBlendMode = 0;
    float particleDepthStrength = .7f;    ///< Scales the 3D depth read (F1/F2/F6)
    float particleColorWarmth = .5f;      ///< Warm/cool depth temperature mix (C1/C4)
    float particleGlowStrength = .28f;    ///< Additive backlight halo alpha (G1/G5)
    float particleGlowSize = 2.2f;        ///< Halo radius vs. crisp sprite (G1/G5)
    float particleShineThreshold = .84f;  ///< Rare specular glint threshold (G3/G5)

    // Animation & Color
    float innerTextAlpha = 1.0f;
    float outlineAlpha = 1.0f;

    /// NPC nameplate text colors (flat, white-leaning; see Settings::NpcColorSettings).
    struct NpcColors
    {
        Settings::Color3 neutral{};   ///< Neutral + talkable civilians
        Settings::Color3 hostile{};   ///< Slight warm lean for hostiles
        Settings::Color3 follower{};  ///< Slight cool lean for teammates
        Settings::Color3 level{};     ///< Dimmed silver level readout
        Settings::Color3 title{};     ///< Soft white title text
    };
    NpcColors npcColors = {};

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
    bool enableExit = false;
    float exitDuration = .20f;
    float entranceStaggerStep = .06f;
    float entranceStaggerMax = .8f;

    // Visual polish settings (value copy)
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

    /// Resolved label strings + classification thresholds for the contextual
    /// nameplate tokens (`%r`, `%d`, `%c`).  Snapshotted as plain values so
    /// render helpers can grab a `string_view` per actor without re-locking.
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

    /// Resolved status-icon badge configuration.  Colors are pre-derived in
    /// Settings::ClampAndValidate(); icon names map to duotone SVG textures
    /// loaded by BadgeTextures (an unloaded name simply drops that badge at
    /// layout time).  `enabled` is false when badges are master-toggled off
    /// or no icon folder is configured.
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

        // Always-on neutral states (previously blank) + new NPC categories.
        std::string icoNeutral;   // muted relationship
        std::string icoHumanoid;  // muted creature
        std::string icoEven;      // muted threat
        std::string icoGuard;
        std::string icoMerchant;
        std::string icoCommoner;  // muted role
        std::string icoEssential;
        std::string icoProtected;
        std::string icoMortal;  // muted protection
        std::string icoCombat;
        std::string icoAlert;
        std::string icoIdle;  // muted engagement
        // Player slot icons.
        std::string icoSneakHidden;
        std::string icoSneakDetected;
        std::string icoSneakOff;  // muted
        std::string icoEncumbered;
        std::string icoNormalWeight;  // muted
        std::string icoWanted;
        std::string icoBountyClear;  // muted
        // Player tier-band prestige badge (low / mid / high ladder thirds).
        std::string icoTierLow;
        std::string icoTierMid;
        std::string icoTierHigh;
        // Lit colors for the active states.
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
        // Full-color prestige emblem images for the tier badge (replaces the
        // medal/gem/crown icons above when enabled and images loaded).
        bool tierBadgeImages = false;  ///< Use emblem PNGs for the tier badge
        int tierImageCount = 0;        ///< Emblem images loaded (from BadgeTextures)
        float tierBadgeGamma = 1.8f;   ///< Top-weighting exponent (>1 = high emblems rarer)
        float tierBadgeScale = 1.7f;   ///< Emblem size as a multiple of the status-icon size
        // Per-slot resting colors (each "muted" slot carries its own calm hue).
        Settings::Color3 colNeutral{};
        Settings::Color3 colHumanoid{};
        Settings::Color3 colCommoner{};
        Settings::Color3 colMortal{};
        Settings::Color3 colEven{};
        Settings::Color3 colIdle{};
        Settings::Color3 colSneakOff{};
        Settings::Color3 colNormalWeight{};
        Settings::Color3 colBountyClear{};
        Settings::Color3 colMuted{};  // deprecated shared tint (unused)
        // Per-slot enables (a disabled active state drops that actor's badge).
        bool relationshipEnabled = true;
        bool creatureEnabled = true;
        bool threatEnabled = true;
        bool roleEnabled = true;
        bool protectionEnabled = true;
        bool engagementEnabled = true;  // master for the NPC engagement slot
        bool combatStateEnabled = true;
        bool alertStateEnabled = true;
        bool sneakEnabled = true;
        bool playerCombatEnabled = true;
        bool encumberedEnabled = true;
        bool bountyEnabled = true;
        bool tierEnabled = true;  // player tier-band prestige badge
        float opacity = 1.15f;
        // Muted styling: alpha multiplier and desaturation strength [0,1].
        float mutedAlpha = 0.45f;
        float mutedDesat = 0.18f;
    };
    IconTokens icons = {};

    /// Focus-target expanded-nameplate settings (value copy).
    Settings::FocusSettings focus = {};

    /// Quiet Frame camera-motion quieting settings (value copy).
    Settings::QuietSettings quiet = {};

    /// Last Rites death valediction settings.
    bool deathRiteEnabled = true;
    float deathRiteDuration = 1.6f;

    /// Registers -- context-conditional profiles (value copies).
    bool registersEnabled = true;
    float registerTransitionTime = 1.2f;
    std::vector<Settings::RegisterDefinition> registers = {};

    /// One Voice Per Actor -- HUD-compat yield settings.
    float compatTrueHUDYieldAlpha = .0f;
    float compatYieldSettleTime = .3f;

    /// Candlelight Metering -- exposure-adaptive ink settings.
    bool candleEnabled = true;
    float candleStrength = .08f;
    float candleWarmth = .5f;
    float candleSettleTime = .6f;

    /// Cut by the World -- per-pixel depth occlusion settings.
    bool depthClipEnabled = true;
    float depthClipFeather = 2.5f;

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

    /// Roll Call stagger: seconds this plate still waits before its entrance
    /// begins.  Negative means "not yet assigned" -- the entrance block claims
    /// the next distance-ordered slot the first frame it runs.
    float entranceDelay = -1.0f;

    /// @name Last Rites -- one-shot death valediction
    /// The rite plays only for actors this cache entry saw alive; corpses
    /// first seen dead never render.  deathDone latches so a rite never
    /// repeats for the same corpse while its entry lives.
    /// @{
    bool sawAlive = false;   ///< Entry rendered this actor alive at least once
    float deathPhase = .0f;  ///< Rite progress (0=just died, 1=finished)
    bool deathDone = false;  ///< Rite finished (or cancelled on overlay wake)
    /// @}

    /// Smoothed focus state for the focus-target expanded nameplate feature.
    /// Targets 1.0 while this actor is the cone winner, 0.0 otherwise.
    /// Drives both the ambient dim multiplier and the title/info row fade.
    float focusSmooth = .0f;

    /// One Voice Per Actor: smoothed yield state.  Targets 1.0 while another
    /// HUD mod (TrueHUD) floats a widget over this actor, fading the plate
    /// to the configured yield alpha and back with the same weight as focus.
    float yieldSmooth = .0f;

    /// @name Candlelight Metering -- smoothed scene sample behind this plate
    /// @{
    float bgLum = -1.0f;  ///< Smoothed background luminance (-1 = no sample yet)
    float bgR = .5f;      ///< Smoothed background red
    float bgG = .5f;      ///< Smoothed background green
    float bgB = .5f;      ///< Smoothed background blue
    /// @}

    /// Motion trail history (separate from posHistory used for smoothing).
    /// Stored in WORLD space and reprojected through the current camera each
    /// frame, so a pure camera pan maps every ghost identically and they
    /// collapse onto the head -- only real actor movement leaves a trail.
    static constexpr int TRAIL_HISTORY_SIZE = 8;
    RE::NiPoint3 trailHistory[TRAIL_HISTORY_SIZE]{};  ///< Trail ghost positions (world space)
    int trailIndex = 0;                               ///< Current write index in trailHistory
    bool trailFilled = false;                         ///< True once trailHistory has wrapped

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

/// Player-relationship channel for the `%r` token, badges, and NPC text
/// color (see `RenderSettingsSnapshot::npcColors`).
enum class RelationshipKind : std::uint8_t
{
    Hostile,  ///< IsHostileToActor(player)
    Neutral,  ///< Default -- neither hostile, nor teammate, nor talkable
    Ally,     ///< CanTalkToPlayer (friendly NPC) but not a teammate
    Follower  ///< IsPlayerTeammate
};

/// Actor level relative to player level for the `%d` token.  Thresholds
/// live in `Settings::Labels()` and default to <=-5, >=+5, >=+10.
enum class LevelDelta : std::uint8_t
{
    Weak,    ///< Far below player level
    Even,    ///< Within +/-(StrongAtOrAbove - 1) of player
    Strong,  ///< Notably above player
    Deadly   ///< Far above player (overrides Strong)
};

/// Coarse actor classification for the `%c` token, derived from the
/// `ActorTypeX` keyword set.  NPC is the fallback when no creature
/// keyword matches.
enum class CreatureKind : std::uint8_t
{
    NPC,     ///< Humanoid (ActorTypeNPC) -- default
    Beast,   ///< ActorTypeCreature / ActorTypeAnimal
    Undead,  ///< ActorTypeUndead
    Daedra,  ///< ActorTypeDaedra
    Dragon   ///< ActorTypeDragon
};

/// Social role for the NPC role badge slot.  Commoner is the muted default;
/// Guard and Merchant render lit.  Derived from `IsGuard()` and vendor-faction
/// membership on the game thread.
enum class RoleKind : std::uint8_t
{
    Commoner,  ///< No notable role -- muted
    Merchant,  ///< Member of a vendor faction (TESFaction::IsVendor)
    Guard      ///< Actor::IsGuard()
};

/// Invulnerability status for the protection badge slot.  Mortal is the muted
/// default.  Essential overrides Protected when both flags are set.
enum class ProtectionKind : std::uint8_t
{
    Mortal,     ///< Killable -- muted
    Protected,  ///< Only the player can kill (IsProtected)
    Essential   ///< Cannot be killed (IsEssential)
};

/// Awareness/engagement state for the engagement badge slot.  One mutually
/// exclusive axis (combat supersedes alert supersedes idle).  Idle is muted.
enum class EngagementKind : std::uint8_t
{
    Idle,   ///< Not in combat, not alerted -- muted
    Alert,  ///< Aware/searching proxy (weapon drawn + detects player)
    Combat  ///< IsInCombat()
};

/// Player stealth state for the sneak badge slot.  Off is the muted default
/// (not sneaking); Hidden/Detected render lit while sneaking.
enum class SneakKind : std::uint8_t
{
    Off,      ///< Not sneaking -- muted
    Hidden,   ///< Sneaking, undetected
    Detected  ///< Sneaking, detected by a nearby hostile
};

/// Data for rendering a single actor's nameplate.
struct ActorDrawData
{
    uint32_t formID{0};       ///< Actor's form ID (unique identifier)
    RE::NiPoint3 worldPos{};  ///< World position above actor's head
    std::string name;         ///< Display name (capitalized)
    uint16_t level{0};        ///< Actor's level
    float distToPlayer{.0f};  ///< Distance to player in units
    bool isPlayer{false};     ///< Whether this is the player character
    bool isOccluded{false};   ///< Whether actor is occluded from view
    bool isDead{false};       ///< Actor is dead -- signals the Last Rites pass;
                              ///< the rite itself replays the last live facts

    /// Deeds, Not Words: faction-earned honorific resolved on the game
    /// thread; empty = none.  Replaces the tier title in the title slot
    /// (special titles still win).
    std::string honorific;

    /// @name One Voice Per Actor (HUD-compat facts, resolved on game thread)
    /// @{
    bool yieldPlate{false};  ///< TrueHUD floats a bar here -- fade the plate out
    bool yieldLevel{false};  ///< moreHUD shows this target's level -- drop ours
    /// @}

    /// @name Contextual nameplate facts (resolved on game thread)
    /// @{
    RelationshipKind relationship{RelationshipKind::Neutral};  ///< %r token source
    LevelDelta levelDelta{LevelDelta::Even};                   ///< %d token source
    CreatureKind creatureKind{CreatureKind::NPC};              ///< %c token source
    /// @}

    /// @name Status badge facts (resolved on game thread)
    /// NPC slots: role, protection, engagement.  Player slots: sneak,
    /// playerInCombat, encumbered, wanted.  Each drives one always-on badge.
    /// @{
    RoleKind role{RoleKind::Commoner};                  ///< NPC role badge
    ProtectionKind protection{ProtectionKind::Mortal};  ///< NPC protection badge
    EngagementKind engagement{EngagementKind::Idle};    ///< NPC engagement badge
    SneakKind sneak{SneakKind::Off};                    ///< Player sneak badge
    bool playerInCombat{false};                         ///< Player engagement badge
    bool encumbered{false};                             ///< Player encumbered badge
    bool wanted{false};                                 ///< Player bounty badge
    /// @}
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
    std::unordered_map<uint32_t, ActorDrawData>
        lastDrawData;    ///< Last live draw data per actor, replayed by the exit animation
    uint32_t frame = 0;  ///< Render-thread frame counter
    /// @}

    /// @name Snapshot & Thread Safety
    /// @{
    std::vector<ActorDrawData> snapshot;            ///< Current actor draw data
    std::mutex snapshotLock;                        ///< Guards snapshot reads/writes
    std::atomic<bool> updateQueued{false};          ///< True while a game-thread update is pending
    std::atomic<bool> pauseSnapshotUpdates{false};  ///< Suppress updates during reload
    std::atomic<bool> pendingIdentityRefresh{false};  ///< RaceMenu rename -> force player re-read
    std::atomic<bool> snapshotUpdateRunning{false};   ///< True while game-thread update is active
    std::atomic<bool> clearOcclusionCacheRequested{false};  ///< Request to clear occlusion cache
    /// @}

    /// @name Frame State
    /// @{
    bool wasInInvalidState = true;  ///< True if previous frame was in an invalid game state
    int postLoadCooldown = 0;       ///< Frames remaining in post-load cooldown
    /// @}

    /// @name Roll Call -- orchestrated re-entry
    /// @{
    /// Set when the overlay resumes after suppression (combat, menus, loads).
    /// The next drawn frame replays every visible plate's entrance so the
    /// scene re-introduces itself as a staggered near-to-far cascade.
    bool wakeReplayPending = false;
    /// Entrances that began this frame; each new start claims the next
    /// stagger slot.  Reset at the top of every drawn frame.
    int entrancesStartedThisFrame = 0;
    /// @}

    /// @name Quiet Frame -- camera-motion quieting
    /// Smoothed [0,1] quiet factors driven by camera angular speed with an
    /// asymmetric envelope (fast attack, slow release).  quietSub releases
    /// slower than quietName, so names resolve back before sub-lines.
    /// @{
    RE::NiPoint3 prevCamForward{};  ///< Camera forward on the previous frame
    bool prevCamValid = false;      ///< prevCamForward holds a real sample
    float quietName = .0f;          ///< Name-row quiet factor (thins to a floor)
    float quietSub = .0f;           ///< Sub-line quiet factor (folds to zero)
    /// @}

    /// @name Registers -- context-conditional profiles
    /// The game thread publishes the active register index each snapshot;
    /// the render thread eases its effective knobs toward that register's
    /// values (or the 1/1/1/0 base state) so scene transitions swell and
    /// recede instead of snapping.
    /// @{
    std::atomic<int> activeRegister{-1};  ///< Index into snapshot registers, -1 = none
    float regAlphaMul = 1.0f;             ///< Smoothed overlay-wide alpha multiplier
    float regFadeMul = 1.0f;              ///< Smoothed fade/scale distance multiplier
    float regSubLineMul = 1.0f;           ///< Smoothed sub-line alpha multiplier
    float regHideNeutral = .0f;           ///< Smoothed neutral/ally plate hide factor
    /// @}

    /// Cut by the World: true while this frame's draws are depth-clipped
    /// (depth SRV located, polarity resolved).  Render-thread only.
    bool depthClipFrame = false;

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

/// One resolved status badge icon, fully positioned during layout.
/// Color carries no alpha -- DrawBadges packs it with the per-frame
/// style alpha (plus the Deadly pulse) at draw time.
struct BadgeDrawItem
{
    ImTextureID tex = 0;     ///< Rasterized duotone icon SRV (from BadgeTextures)
    Settings::Color3 color;  ///< Semantic badge tint (ignored when fullColor)
    ImVec2 pos;              ///< Top-left draw position (screen pixels)
    ImVec2 size;             ///< Draw size (square)
    bool pulse;              ///< Deadly skull breathing alpha
    bool muted = false;      ///< Neutral/inactive slot -- dimmed + desaturated at draw time
    bool fullColor = false;  ///< True for the emblem tier badge: draw untinted (white multiply)
};

/// Bundle of resolved actor-fact values passed to `FormatString` for one
/// expansion call.  All string_views point into the active
/// `RenderSettingsSnapshot::labels` (lives for the frame, per-frame
/// snapshot cached at `RendererState::cachedSnap`), so the views are
/// safe to read for the duration of any single `FormatString` call.
struct ActorLabelContext
{
    std::string_view name;          ///< Actor display name (%n)
    int level = 0;                  ///< Actor level (%l)
    const char* title = nullptr;    ///< Tier or special title (%t); nullable
    std::string_view relationship;  ///< Resolved %r label
    std::string_view levelDelta;    ///< Resolved %d label
    std::string_view creatureKind;  ///< Resolved %c label
    std::uint32_t formID = 0;       ///< For deterministic per-actor effects in future tokens
};

/**
 * Color resolution model:
 *
 * Colors are resolved once per label into per-role values (name, level,
 * title) plus per-role text effects.  The player and special titles use the
 * tier palette, displayed **as authored** in the INI; NPCs resolve to flat
 * white-leaning colors from `RenderSettingsSnapshot::npcColors`, keyed by
 * `RelationshipKind`.  Draw code applies the resolved style uniformly and
 * contains no per-actor color branches.
 */

/// All color, tier, and effect data computed once per label.
struct LabelStyle
{
    int tierIdx;
    const Settings::TierDefinition* tier;
    const Settings::SpecialTitleDefinition* specialTitle;

    /// @name Resolved per-role text effects (tier effects, or None for NPCs)
    /// @{
    const Settings::EffectParams* nameEffect;
    const Settings::EffectParams* levelEffect;
    const Settings::EffectParams* titleEffect;
    bool usesTierVisuals;  ///< Player / special title: tier effects, wave, shine overlay
    /// @}

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

    /// Extra alpha multiplier applied to the info row (and only the info row)
    /// so it can be hidden on ambient actors and faded in on focus changes.
    /// 1.0 = no effect; 0.0 = info row fully hidden.
    float infoAlphaMul = 1.0f;

    /// Extra alpha multiplier for the status badge strip.  The Quiet Frame
    /// folds the badge strip away with the title during fast camera pans;
    /// the name and level stay full.
    float badgeAlphaMul = 1.0f;

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

    std::vector<RenderSeg> infoSegments;  ///< Info row segments (built from InfoFormat)
    float infoLineWidth = .0f;            ///< Total width of all info segments plus padding
    float infoLineHeight = .0f;           ///< Height of the tallest info segment

    std::vector<BadgeDrawItem> badges;  ///< Status icon badges (built from snap.icons)

    // Player tier emblem (full-color prestige badge): laid on its own row above
    // the icon strip, larger than the icons, drawn with a bloom + breathe glow.
    bool tierEmblemShown = false;
    ImTextureID tierEmblemTex = 0;
    ImVec2 tierEmblemPos{};   ///< Top-left draw position (screen pixels)
    ImVec2 tierEmblemSize{};  ///< Draw size (square)

    std::string titleStr;         ///< Full title text
    std::string titleDisplayStr;  ///< Title text after typewriter truncation
    ImVec2 titleSize;             ///< Measured size of titleDisplayStr

    ImVec2 startPos;        ///< Screen-space anchor from WorldToScreen
    float titleY;           ///< Y position of title line top
    float mainLineY;        ///< Y position of main line top
    float infoLineY = .0f;  ///< Y position of info row top (only valid if infoSegments non-empty)
    float totalWidth;       ///< Width of the widest of title/main/info line

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
// Focus selection (Renderer.cpp)
// ============================================================================

/// Pick the formID of the actor whose direction from the camera lies inside
/// the configured cone with the smallest angular offset, or 0 if no actor
/// qualifies.  Render-thread only -- reads camera pose via
/// `Occlusion::GetCameraInfo`.  The player is never picked.
uint32_t SelectFocusedActor(const std::vector<ActorDrawData>& snap,
                            const RenderSettingsSnapshot& snapSettings);

// ============================================================================
// Layout functions (RendererLayout.cpp)
// ============================================================================

/// Measure the tightest vertical glyph bounds for text.
/// @param[out] outTop     Topmost glyph offset (pixels).
/// @param[out] outBottom  Bottommost glyph offset (pixels).
void CalcTightYBoundsFromTop(
    ImFont* font, float fontSize, const char* text, float& outTop, float& outBottom);

/// Replace placeholders in a format string (`%n`, `%l`, `%t`, `%r`, `%d`, `%c`)
/// with values from the supplied `ActorLabelContext`.  Single-pass replacement
/// avoids expanding placeholders embedded in substitution values.
std::string FormatString(const std::string& fmt, const ActorLabelContext& ctx);

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
/// @param forcedCharsToShow  When >= 0, overrides the typewriter character
///        budget (used by Last Rites to crumble text in reverse); -1 keeps
///        the entry-driven typewriter behavior.
LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                               ActorCache& entry,
                               const LabelStyle& style,
                               float textSizeScale,
                               const RenderSettingsSnapshot& snap,
                               int forcedCharsToShow = -1);

/// Last Rites: pull every resolved style color toward `target` by `mixT`,
/// calm the effect strength to match, and re-pack the draw-ready colors.
/// Composable -- call repeatedly to stage multi-stop ramps (sear -> dark).
void ApplyDeathRiteTint(LabelStyle& style,
                        const ImVec4& target,
                        float mixT,
                        const RenderSettingsSnapshot& snap);

/// Candlelight Metering: adapt the resolved ink to the scene behind the
/// plate -- dim a touch over bright backgrounds, lift and warm slightly over
/// dark ones -- then re-pack the draw-ready colors.  `bgLum`/`bgRGB` are the
/// smoothed per-actor scene sample; adjustments are capped by
/// `snap.candleStrength`.  Pure color math -- mirrored conceptually in
/// tests/test_utils.cpp (CandleAdjust); keep the mapping in sync.
void ApplyCandlelight(LabelStyle& style,
                      float bgLum,
                      const float bgRGB[3],
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

/// Draw a soft backplate halo behind the full nameplate, title, and main line.
/// Uses the glow layer so it stays behind particles, outlines, and text.
void DrawBackgroundGlow(ImDrawList* dl,
                        const LabelStyle& style,
                        const LabelLayout& layout,
                        float lodTitleFactor,
                        ImDrawListSplitter* splitter,
                        const RenderSettingsSnapshot& snap);

/// Draw the particle aura (back layer, splitter channel 0) and ornament glyphs
/// for one nameplate, computing the shared ornament block geometry a single time
/// so the particle-aura sizer and the ornament draw pass agree without rebuilding
/// it twice per label.  Player-only unless forced by specialTitle.
void DrawParticlesAndOrnaments(ImDrawList* dl,
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
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodTitleFactor,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap);

/// Render each segment of the main nameplate line with its configured effect.
void DrawMainLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap);

/// Render the contextual info row below the main line.  No-op when no info
/// segments survived the per-segment drop-if-blank trimming.
void DrawInfoLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap);

/// Render resolved status badge glyphs (relationship, threat pip, creature)
/// around the main line.  No-op when the layout produced no badge items or
/// the icon font is unavailable.
void DrawBadges(ImDrawList* dl,
                const LabelStyle& style,
                const LabelLayout& layout,
                ImDrawListSplitter* splitter,
                bool fastOutlines,
                const RenderSettingsSnapshot& snap);

/// Render the player tier emblem on its own row above the icon strip, with a
/// soft emblem-colored bloom + breathe glow.  No-op when the layout produced no
/// emblem.
void DrawTierEmblem(ImDrawList* dl,
                    const LabelStyle& style,
                    const LabelLayout& layout,
                    float time,
                    ImDrawListSplitter* splitter,
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
