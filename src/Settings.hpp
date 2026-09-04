#pragma once

#include "RenderConstants.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

// clang-format off
/**
 * @namespace Settings
 * @brief INI configuration and runtime defaults.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * `Load()` resets defaults, then reads `Data/SKSE/Plugins/glyph.ini`.
 * `kSettings` in Settings.cpp owns scalar defaults; member initializers are placeholders.
 * `MakeDefaultTier()` and the indexed-section parsers own indexed defaults.
 *
 * Scalar keys match in any section. Indexed fields in `TierN`, `SpecialTitleN`,
 * `HonorificN`, and `RegisterN` take precedence over scalar keys. An unknown section
 * warns once per name but still accepts scalars. Keys before the first section are valid.
 *
 * ### :material-refresh: Hot reload
 *
 * `ReloadKey` is a Win32 virtual key; values <= 0 disable it. The runtime default is 0;
 * glyph.ini sets 118 (F7). The render thread queues `Load()` on the game thread and
 * pauses snapshots until completion. Without the task interface, it loads directly;
 * the parser dereferences no game objects.
 *
 * ```mermaid
 * sequenceDiagram
 *     participant R as Render thread
 *     participant G as Game thread
 *     R->>R: ReloadKey edge sets reloadRequested
 *     R->>R: In-flight snapshot task drains, then pauseSnapshotUpdates
 *     R->>G: AddTask(Settings::Load)
 *     G->>G: Load() under a unique Mutex() lock
 *     G->>G: Advance Generation() if the INI opened
 *     G-->>R: reloadCompleted
 *     R->>R: Clear actor cache and occlusion cache, resume snapshots
 * ```
 *
 * ### :material-blur-linear: Distance curves
 *
 * Alpha uses player-to-actor distance and a squared quintic smoothstep. `m_fade` is
 * the active register's smoothed fade multiplier, or 1 when no register applies.
 * Font scale uses the smaller player-distance and camera-distance result; skip the
 * camera term when unavailable. Both denominators have a floor of 1 game unit.
 * `MinimumPixelHeight` applies last when positive. Register fade multipliers affect
 * alpha only. The formulas below follow that order.
 *
 *
 * $$d'_{start} = d_{start} \cdot m_{fade}, \quad d'_{end} = d_{end} \cdot m_{fade}$$
 *
 * $$t = \text{clamp}\!\left(\frac{d - d'_{start}}{d'_{end} - d'_{start}},\; 0,\; 1\right)$$
 *
 * $$\text{smoothstep}(t) = 6t^5 - 15t^4 + 10t^3$$
 *
 * $$\alpha = \left(1 - \text{smoothstep}(t)\right)^2$$
 *
 * $$t = \text{clamp}\!\left(\frac{d - d_{start}}{d_{end} - d_{start}},\; 0,\; 1\right)$$
 *
 * $$scale = 1 + (scale_{min} - 1) \cdot \sqrt{t}$$
 *
 * $$scale = \min\left(scale_{player},\; scale_{camera}\right)$$
 *
 * $$scale = \max\left(scale,\; \text{MinimumPixelHeight} / \text{NameFontSize}\right)$$
 */
// clang-format on
namespace Settings
{
/**
 * @struct Color3
 * @brief RGB channels with an explicit unit-range clamp.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 */
struct Color3
{
    float r = 1.f, g = 1.f, b = 1.f;

    constexpr Color3() = default;
    /**
     * @fn constexpr Color3(float r_, float g_, float b_)
     * @brief Store RGB channels without clamping.
     * @author Alex (<https://github.com/lextpf>)
     */
    constexpr Color3(float r_, float g_, float b_)
        : r(r_),
          g(g_),
          b(b_)
    {
    }

    /**
     * @fn constexpr Color3& clamp01()
     * @brief Clamp all channels to the inclusive range from zero to one.
     * @author Alex (<https://github.com/lextpf>)
     */
    constexpr Color3& clamp01()
    {
        r = std::clamp(r, 0.f, 1.f);
        g = std::clamp(g, 0.f, 1.f);
        b = std::clamp(b, 0.f, 1.f);
        return *this;
    }

    /**
     * @fn constexpr Color3 White()
     * @brief Return full-intensity white.
     * @author Alex (<https://github.com/lextpf>)
     */
    static constexpr Color3 White() { return {1.f, 1.f, 1.f}; }
    constexpr bool operator==(const Color3&) const = default;
};

/**
 * @struct Segment
 * @brief One segment of a nameplate row.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Substitution is single-pass: `%n` name, `%l` level, `%r` relationship,
 * `%d` level delta, `%c` creature kind. `%t` expands only on the title line.
 * `Format` and `InfoFormat` contain double-quoted segments.
 *
 * | Condition                      | Result                                       |
 * |--------------------------------|----------------------------------------------|
 * | `Format` segment contains `%t` | Title line; last such segment wins           |
 * | `InfoFormat` contains `%t`     | Literal `%t` on the info row                 |
 * | Empty `Format`                 | Keep the previous main row                   |
 * | Empty `InfoFormat`             | Clear the info row                           |
 * | Segment contains `%l`          | Level font; all info segments use this font  |
 * | `?` follows the closing quote  | Omit the segment when its expansion is blank |
 *
 * @code{.ini}
 * ; Three segments. The first holds %t and becomes the title line, with a
 * ; Literal quote on each side. The other two form the main row: Lydia|lv.42
 * Format = "\"%t\"" "%n" "|lv.%l"
 * @endcode
 */
struct Segment
{
    std::string format;
    bool useLevelFont = false;  ///< False selects the name font
    /// Trailing ? Omits this segment when its expansion is blank.
    bool dropIfBlank = false;
};

/**
 * @enum EffectType
 * @brief Text effects and their parameter meanings.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * @see EffectParams, TextEffects::ApplyVertexEffect
 */
enum class EffectType
{
    None,              ///< No effect, solid color
    Gradient,          ///< Horizontal gradient (left to right)
    VerticalGradient,  ///< Vertical gradient (top to bottom)
    DiagonalGradient,  ///< Diagonal gradient (requires direction in param1, param2)
    RadialGradient,    ///< Radial gradient from center (param1 = gamma)
    Shimmer,           ///< Moving highlight band (param1 = width, param2 = strength)
    Ember,             ///< Warm flickering glow (param1 = speed, param2 = intensity)
    /**
     * Northern lights effect (param1 = speed, param2 = waves, param3 = intensity,
     * param4 = sway)
     */
    Aurora,
    Sparkle,  ///< Glittering stars (param1 = density, param2 = speed, param3 = intensity)
    Enchant,  ///< Flowing magical energy (param1 = speed, param2 = scale, param3 = intensity)
    Frost,    ///< Crystalline ice sparkle (param1 = density, param2 = speed, param3 = intensity)
    Breathe,  ///< Slow uniform brightness pulse (param1 = speed Hz, param2 = amplitude)
    Drift,    ///< Slow uniform hue wander (param1 = speed Hz, param2 = hue range degrees)
    Mote,     ///< Rare single twinkle (param1 = period s, param2 = peak alpha)
    /**
     * Per-character asynchronous breathing (param1 = speed Hz, param2 = amplitude,
     * param3 = phase spread)
     */
    Wander,
    /**
     * Shadow band sweeping with a hot leading rim (param1 = width,
     * param2 = strength)
     */
    Eclipse,
    Pulse,    ///< Weighty two-beat heartbeat glow (param1 = rate Hz, param2 = amplitude)
    Electric  ///< Rare crackling arc sweeping the text (param1 = rate Hz, param2 = intensity)
};

/**
 * @struct EffectParams
 * @brief Effect parameters interpreted by EffectType.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * @see EffectType
 *
 * @code{.cpp}
 * // Aurora effect: speed=0.5, waves=3.0, intensity=1.0, sway=0.3
 * EffectParams aurora;
 * aurora.type = EffectType::Aurora;
 * aurora.param1 = .5f;   // Speed
 * aurora.param2 = 3.0f;   // Wave count
 * aurora.param3 = 1.0f;   // Intensity
 * aurora.param4 = .3f;   // Sway amount
 * @endcode
 */
struct EffectParams
{
    EffectType type = EffectType::Gradient;

    float param1 = .0f;
    float param2 = .0f;
    float param3 = .0f;
    float param4 = .0f;
    float param5 = .0f;  ///< Parsed; no effect reads it

    /// Parsed whiteBase marker; no draw path reads it.
    bool useWhiteBase = false;
};

/**
 * @struct TierDefinition
 * @brief Level interval and visual style from a TierN section.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * RGB values are comma-separated floats in [0, 1]. Effect syntax is
 * `name p1,p2,...`: whitespace must separate the name from at most five parameters.
 * A comma after the name causes lookup to fall back to `EffectType::Gradient`.
 * Case-insensitive `whiteBase` truncates the remaining parameter text and sets
 * `EffectParams::useWhiteBase`.
 *
 * @code{.ini}
 * [Tier5]
 * MinLevel = 30
 * MaxLevel = 39
 * Title = Veteran
 * LeftColor = 0.2, 0.6, 1.0
 * RightColor = 0.8, 0.2, 1.0
 * HighlightColor = 1.0, 1.0, 1.0
 * TitleEffect = Shimmer 0.3,0.8
 * NameEffect = Gradient
 * LevelEffect = Gradient
 * Badge = 3
 * @endcode
 */
struct TierDefinition
{
    uint16_t minLevel = 1;    ///< Minimum level for this tier (inclusive)
    uint16_t maxLevel = 250;  ///< Maximum level for this tier (inclusive)
    std::string title = "Unknown";
    Color3 leftColor;       ///< RGB color for name gradient left/top
    Color3 rightColor;      ///< RGB color for name gradient right/bottom
    Color3 highlightColor;  ///< RGB color for shimmer/sparkle highlights

    std::optional<Color3>
        titleLeftColor;  ///< Title gradient left (default: leftColor blended 25% toward white)
    std::optional<Color3>
        titleRightColor;  ///< Title gradient right (default: rightColor blended 25% toward white)
    std::optional<Color3>
        levelLeftColor;  ///< Level gradient left (default: leftColor blended 40% toward white)
    std::optional<Color3> levelRightColor;  ///< Level gradient right (rightColor, 40% toward white)
    std::optional<Color3> particleColor;    ///< Particle tint (default: highlightColor)
    /**
     * Empty: title-left mixed 32% toward highlight, 15% toward name, saturation x1.25.
     * Deck uses title-left directly.
     */
    std::optional<Color3> ornamentLeftColor;
    /// Same fallback as the left, with 42% highlight mixing.
    std::optional<Color3> ornamentRightColor;

    EffectParams titleEffect;
    EffectParams nameEffect;
    EffectParams levelEffect;

    std::string
        leftOrnaments;  ///< Left side ornament characters (e.g., "ac"), empty = no ornaments
    std::string
        rightOrnaments;  ///< Right side ornament characters (e.g., "bd"), empty = no ornaments

    /**
     * @brief Weighted particle style list.
     *
     * Case-insensitive `token[:weight]`; weights clamp to [0.1, 10] and normalize by
     * the mean to preserve total budget. Unknown tokens render nothing; empty/None disables.
     */
    std::string particleTypes;
    int particleCount = 0;  ///< Number of particles (0 = use the global ParticleCount)
    int badgeIndex = 0;     ///< 1-based manifest tierBadges entry (0 = TierBadgeGamma curve)
};

/**
 * @struct SpecialTitleDefinition
 * @brief Case-insensitive name-keyword replacement for tier styling.
 * @author Alex (<https://github.com/lextpf>)
 */
struct SpecialTitleDefinition
{
    std::string keyword;       ///< Keyword to match in name (case-insensitive)
    std::string keywordLower;  ///< Cached lowercase keyword for fast runtime matching
    std::string displayTitle;
    Color3 color;      ///< RGB color for name/title
    Color3 glowColor;  ///< RGB glow color (more saturated)
    /// Default true when Load creates a special-title entry; ForceFlourishes aliases this.
    bool forceOrnaments;
    /// Default true when Load creates an entry; the only NPC aura route.
    bool forceParticles;
    int priority;                ///< Higher = checked first (0 when the section omits priority)
    std::string leftOrnaments;   ///< Left side ornament characters
    std::string rightOrnaments;  ///< Right side ornament characters
};

/**
 * @struct HonorificDefinition
 * @brief Faction title selected by rank and priority.
 * @author Alex (<https://github.com/lextpf>)
 *
 * `HonorificN` applies to actors at or above `minRank`; special titles take precedence.
 * `factionSpec` is `0xFORMID` or `0xFORMID@Plugin.esp`, defaulting to skyrim.esm.
 * Resolve it on the game thread; unresolved specs warn once per generation and never match.
 * Highest priority wins. Ties use the first faction visited, then the lowest
 * honorific index within that faction.
 */
struct HonorificDefinition
{
    std::string factionSpec;  ///< "0xFORMID[@plugin.esp]" faction reference
    std::string title;
    int minRank = 0;          ///< Minimum faction rank required
    int priority = 0;         ///< Higher wins when several factions match
    bool playerOnly = false;  ///< Only the player's plate may earn this
    bool npcOnly = false;     ///< Only NPC plates may earn this
};

/**
 * @namespace Settings::Context
 * @brief Scene predicates computed once per game-thread snapshot.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A register requires every `whenMask` bit and forbids every `whenNotMask` bit.
 */
namespace Context
{
inline constexpr uint32_t Interior = 1u << 0;  ///< Player's cell is interior
inline constexpr uint32_t Night = 1u << 1;     ///< Game hour outside [6, 20)
inline constexpr uint32_t City = 1u << 2;      ///< Location keyword LocTypeCity/town
inline constexpr uint32_t Sneaking = 1u << 3;  ///< Player is sneaking
inline constexpr uint32_t Dialogue = 1u << 4;  ///< Dialogue menu speaker active
inline constexpr uint32_t Crowded = 1u << 5;   ///< Visible plates >= threshold
}  // namespace Context

/**
 * @struct RegisterDefinition
 * @brief Overlay profile selected by scene predicates.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Highest priority wins; an empty `When` matches every scene. The render thread
 * eases transitions over `RegisterSettings::TransitionTime`.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 *
 * flowchart TD
 *     C[Game thread computes the context mask] --> G{Registers enabled?}
 *     G
 * -- No --> N[Publish activeRegister = -1]
 *     G -- Yes --> R[Scan configured entries in index
 * order]
 *     R --> M{All required bits set<br/>and all forbidden bits clear?}
 *     M -- No -->
 * Q{More entries?}
 *     M -- Yes --> P{Priority greater than the current best?}
 *     P -- Yes
 * --> B[Replace the best match]
 *     P -- No --> K[Keep the earlier match]
 *     B --> Q
 * K
 * --> Q
 *     Q -- Yes --> R
 *     Q -- No --> U[Publish the best index, or -1]
 *     U -->
 * T[Render thread reads activeRegister]
 *     N --> T
 *     T --> E[Ease alpha, fade, sub-line,
 * and hide-neutral values]
 * ```
 */
struct RegisterDefinition
{
    std::string name;          ///< Optional label (logs/debugging)
    uint32_t whenMask = 0;     ///< Context bits that must all be set
    uint32_t whenNotMask = 0;  ///< Context bits that must all be clear
    float alphaMul = 1.0f;     ///< Overlay-wide alpha multiplier [0,1]
    /// Alpha-distance multiplier from 0.2 to 2; INI key FadeDistanceMultiplier.
    float fadeMul = 1.0f;
    float subLineMul = 1.0f;   ///< Title/info/level/badge alpha multiplier [0,1]
    bool hideNeutral = false;  ///< Hide neutral + ally NPC plates entirely
    int priority = 0;          ///< Highest-priority match wins

    /// False for numbering gaps, which must not match an empty when on priority ties.
    bool configured = false;
};

/**
 * @struct RegisterSettings
 * @brief Controls shared by the profiles in Registers.
 * @author Alex (<https://github.com/lextpf>)
 */
struct RegisterSettings
{
    bool Enabled = true;          ///< Master toggle for the register system
    float TransitionTime = 1.2f;  ///< Seconds for knob transitions to settle
    int CrowdedThreshold = 12;    ///< Visible plates that count as "crowded"
};
/**
 * @fn RegisterSettings& RegisterConfig()
 * @brief Access the shared controls for register selection.
 * @author Alex (<https://github.com/lextpf>)
 */
RegisterSettings& RegisterConfig();

/**
 * @fn std::string& TitleFormat()
 * @brief Access the title-row format string.
 * @author Alex (<https://github.com/lextpf>)
 */
std::string& TitleFormat();
/**
 * @fn std::vector<Segment>& DisplayFormat()
 * @brief Access the main-row segments.
 * @author Alex (<https://github.com/lextpf>)
 */
std::vector<Segment>& DisplayFormat();
/**
 * @fn std::vector<Segment>& InfoFormat()
 * @brief Access the optional info-row segments.
 * @author Alex (<https://github.com/lextpf>)
 */
std::vector<Segment>& InfoFormat();
/**
 * @fn std::vector<TierDefinition>& Tiers()
 * @brief Access tier definitions in section-number order.
 * @author Alex (<https://github.com/lextpf>)
 */
std::vector<TierDefinition>& Tiers();
/**
 * @fn std::vector<SpecialTitleDefinition>& SpecialTitles()
 * @brief Access name-keyword overrides in section-number order.
 * @author Alex (<https://github.com/lextpf>)
 */
std::vector<SpecialTitleDefinition>& SpecialTitles();
/**
 * @fn std::vector<HonorificDefinition>& Honorifics()
 * @brief Access faction-title rules in section-number order.
 * @author Alex (<https://github.com/lextpf>)
 */
std::vector<HonorificDefinition>& Honorifics();
/**
 * @fn std::vector<RegisterDefinition>& Registers()
 * @brief Access context profiles in section-number order.
 * @author Alex (<https://github.com/lextpf>)
 */
std::vector<RegisterDefinition>& Registers();

/**
 * @struct DistanceSettings
 * @brief Visibility and scale distances in game units.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Fade and scan limits use player distance. Scale uses the smaller result from player
 * and camera distances. Validation keeps each end at least one unit above its start.
 */
struct DistanceSettings
{
    float FadeStartDistance = 200.0f;
    float FadeEndDistance = 2500.0f;
    float ScaleStartDistance = 200.0f;
    float ScaleEndDistance = 2500.0f;
    float MinimumScale = .1f;  ///< Smallest font size multiplier, clamped to 0.01 - 5
    /// Player-distance cutoff for plates. Deck crosshair targets beyond it are capture-only.
    float MaxScanDistance = 3000.0f;
};
/**
 * @fn DistanceSettings& Distance()
 * @brief Access the shared distance settings.
 * @author Alex (<https://github.com/lextpf>)
 */
DistanceSettings& Distance();

/**
 * @struct OcclusionSettings
 * @brief Settings for line-of-sight occlusion culling.
 * @author Alex (<https://github.com/lextpf>)
 */
struct OcclusionSettings
{
    bool Enabled = true;      ///< Enable LOS-based occlusion
    float SettleTime = .58f;  ///< Fade settle time in seconds
    int CheckInterval = 3;    ///< Frames between LOS checks
};
/**
 * @fn OcclusionSettings& Occlusion()
 * @brief Access the shared occlusion settings.
 * @author Alex (<https://github.com/lextpf>)
 */
OcclusionSettings& Occlusion();

/**
 * @struct ShadowOutlineSettings
 * @brief Shadow and outline geometry in full-scale pixels.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Offsets, widths, and radii scale with the plate.
 */
struct ShadowOutlineSettings
{
    float TitleShadowOffsetX = 2.0f;  ///< Title shadow X offset in pixels
    float TitleShadowOffsetY = 2.0f;  ///< Title shadow Y offset in pixels
    float MainShadowOffsetX = 4.0f;   ///< Main text shadow X offset
    float MainShadowOffsetY = 4.0f;   ///< Main text shadow Y offset
    /// First summand of the base outline width, in pixels.
    float OutlineWidthMin = 2.0f;
    /**
     * Second base-width summand; every plate uses OutlineWidthMin + OutlineWidthMax.
     * Validation keeps this at least OutlineWidthMin.
     */
    float OutlineWidthMax = 2.5f;
    bool FastOutlines = false;  ///< Use 4-dir outlines instead of 8-dir

    bool OutlineGlowEnabled = false;  ///< Enable white glow behind text outline
    float OutlineGlowScale = 1.4f;    ///< Glow radius as multiplier of outline width
    float OutlineGlowAlpha = .1f;     ///< Peak glow ring opacity 0-1
    int OutlineGlowRings = 2;         ///< Concentric glow rings (1-3)
    float OutlineGlowR = 1.0f;
    float OutlineGlowG = 1.0f;
    float OutlineGlowB = 1.0f;
    bool OutlineGlowTierTint = false;  ///< Blend glow color with tier color

    bool DualOutlineEnabled = false;  ///< Enable inner outline tinted with tier color
    float InnerOutlineTint = .3f;     ///< How much to blend toward tier color (0=outline, 1=tier)
    float InnerOutlineAlpha = .5f;    ///< Inner outline opacity multiplier
    float InnerOutlineScale = .5f;    ///< Inner outline width as fraction of outer
    float DirectionalLightAngle = 315.f;  ///< Light direction in degrees (0=right, 90=down)
    float DirectionalLightBias = .15f;    ///< Directional width variation (0=uniform)

    float OutlineColorTint = .0f;  ///< Tier-color tint for outlines 0-0.25
    float ShadowColorTint = .0f;   ///< Tier-color tint for shadows 0-0.25

    bool SoftShadowEnabled = false;   ///< Use a soft directional shadow instead of a hard offset
    float SoftShadowDistance = 4.0f;  ///< Offset distance along the cast angle (pixels)
    float SoftShadowSoftness = 3.0f;  ///< Feather/blur radius of the shadow disc (pixels)
    float SoftShadowOpacity = .8f;    ///< Peak shadow opacity multiplier 0-1
    float SoftShadowAngle = 45.0f;    ///< Shadow cast direction in degrees (0=right, 90=down)
    int SoftShadowSamples = 12;       ///< Feather sample count 4-24 (higher = smoother)
};
/**
 * @fn ShadowOutlineSettings& ShadowOutline()
 * @brief Access the shared shadow outline settings.
 * @author Alex (<https://github.com/lextpf>)
 */
ShadowOutlineSettings& ShadowOutline();

/**
 * @struct GlowSettings
 * @brief Settings for text glow and color-divide effects.
 * @author Alex (<https://github.com/lextpf>)
 */
struct GlowSettings
{
    bool Enabled = false;
    float Radius = 4.0f;    ///< Glow spread in pixels
    float Intensity = .5f;  ///< Glow brightness 0-1
    int Samples = 8;        ///< Quality samples 1-64 (recommended: 8-16)
    /**
     * @brief Independent color-divide pass.
     *
     * GlowDivideStrength in [0, 1]; zero disables. Pixel weight is smoothstep(0.10, 0.25, luma).
     * Independent of EnableGlow; no effect without TextPostProcess initialization.
     */
    float DivideStrength = 0;
};
/**
 * @fn GlowSettings& Glow()
 * @brief Access the shared glow settings.
 * @author Alex (<https://github.com/lextpf>)
 */
GlowSettings& Glow();

/**
 * @struct ShineSettings
 * @brief Settings for the static top-edge shine overlay.
 * @author Alex (<https://github.com/lextpf>)
 */
struct ShineSettings
{
    bool Enabled = false;
    float Intensity = .35f;  ///< Peak brightness at top edge 0-1
    float Falloff = 2.0f;    ///< Vertical falloff exponent (higher = sharper)
    float TextGlowAlpha =
        .0f;  ///< Translucent text body alpha reduction 0-1 (0=opaque, 1=fully translucent)
};
/**
 * @fn ShineSettings& Shine()
 * @brief Access the shared shine settings.
 * @author Alex (<https://github.com/lextpf>)
 */
ShineSettings& Shine();

/**
 * @struct TypewriterSettings
 * @brief Settings for the typewriter reveal effect.
 * @author Alex (<https://github.com/lextpf>)
 */
struct TypewriterSettings
{
    bool Enabled = false;
    float Speed = 30.0f;  ///< Characters per second
    float Delay = .0f;    ///< Seconds before reveal
};
/**
 * @fn TypewriterSettings& Typewriter()
 * @brief Access the shared typewriter settings.
 * @author Alex (<https://github.com/lextpf>)
 */
TypewriterSettings& Typewriter();

/**
 * @struct OrnamentSettings
 * @brief Settings for side ornaments.
 * @author Alex (<https://github.com/lextpf>)
 */
struct OrnamentSettings
{
    bool Enabled = true;
    float Scale = 1.0f;      ///< Size multiplier
    float Spacing = 3.0f;    ///< Full-scale pixels from text edges
    std::string FontPath;    ///< Path to ornament font (TTF/OTF)
    float FontSize = 64.0f;  ///< Ornament font size in pixels (ImGui size_pixels)
    bool AnchorToMainLine =
        true;  ///< Anchor ornaments to main text line instead of nameplate center
    /// Pixels after anchoring, scaled with text size; negative moves up.
    float OffsetY = .0f;
};
/**
 * @fn OrnamentSettings& Ornament()
 * @brief Access the shared ornament settings.
 * @author Alex (<https://github.com/lextpf>)
 */
OrnamentSettings& Ornament();

/**
 * @enum ParticleStyle
 * @brief Particle styles in sprite-table order.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Append styles to preserve ordinals. `kParticleStyleTokens` maps each ordinal to its
 * INI token; dependent tables check their size against `kParticleStyleCount`.
 */
enum class ParticleStyle
{
    Firefly,        ///< Slow wandering glow that flickers in place
    Snow,           ///< Snowflakes drifting around a broad slow orbit
    Smoke,          ///< Rising, expanding, fading puffs
    Spark,          ///< Orbiting embers that periodically flare outward hot
    Wisp,           ///< Serpentine trails
    Leaf,           ///< Orbiting leaves with in/out drift and fluttering tumble
    Aurora,         ///< Wavy horizontal shimmer band
    CherryBlossom,  ///< Orbiting petals with a gentle bob and slow spin
    Dust,           ///< Very slow floating motes
    Mote,           ///< Soft glowing motes that gently drift and pulse
    Arcane,         ///< Orbiting rune, slow spin, glow pulse
    Ash,            ///< Slow-falling embers with a fading glow pulse
    Bat,            ///< Bats circling the plate with swooping flight (flap strip)
    Bubble,         ///< Rising wobbling bubbles that pop at the top
    Butterfly,      ///< Slow fluttering orbit with figure-eight bob (flap strip)
    Coin,           ///< Slowly orbiting coins spinning on their axis (spin strip)
    Confetti,       ///< Tumbling festive scraps drifting down
    Constellation,  ///< Near-static twinkling star clusters, high band
    Curse,          ///< Skittering dark sigils on a slow low orbit
    Enchant,        ///< Orbiting arcane sparkles with a shimmer weave
    Fairy,          ///< Bright darting wanderer, firefly-class but livelier
    Fog,            ///< Slow horizontal haze bank along the lower edge
    Gem,            ///< Upright orbiting jewels with a facet glint
    Glitter,        ///< Anchored sparkle field, twinkling in place
    Heart,          ///< Rising hearts with a heartbeat pulse
    Hex,            ///< Slow orbiting hex marks with green flame flicker
    Ink,            ///< Matte ink marks creeping around a low slow orbit
    Moon,           ///< A single slow crescent arcing across the top band
    Planet,         ///< A single ringed planet on a slow deep orbit
    Pollen,         ///< Air-borne golden specks on a very slow sway-fall
    Soul,           ///< Ghostly wisps rising and fading out
    Steam,          ///< Brisk narrow rising vapor that expands
    Void,           ///< Dark swirls orbiting with an inward breathing pull
    Vortex,         ///< Coherent spinning spirals on a brisk shared orbit
    Wind,           ///< Fast horizontal gust streaks across the plate
    Zap,            ///< Electric arcs jumping between hashed positions
    Zzz,            ///< Sleepy Z glyphs drifting up and to the side
    Ember,          ///< Rising fire embers that flicker as they cool
    Pixiedust,      ///< Pastel sparkle motes sprinkling down, twinkling
    Runes,          ///< Floating glyphs that morph between rune shapes
    Sand            ///< Wind-blown grains gusting across the lower band
};

/**
 * @struct ParticleStyleToken
 * @brief Particle style and manifest token in enum order.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Tokens match `ParticleTypes` case-insensitively and select the `particles` manifest
 * entry. Files use guid names; image dimensions determine flipbook frame counts.
 */
struct ParticleStyleToken
{
    ParticleStyle style;
    const char* token;
};
inline constexpr ParticleStyleToken kParticleStyleTokens[] = {
    {ParticleStyle::Firefly, "firefly"},
    {ParticleStyle::Snow, "snow"},
    {ParticleStyle::Smoke, "smoke"},
    {ParticleStyle::Spark, "spark"},
    {ParticleStyle::Wisp, "wisp"},
    {ParticleStyle::Leaf, "leaf"},
    {ParticleStyle::Aurora, "aurora"},
    {ParticleStyle::CherryBlossom, "cherryblossom"},
    {ParticleStyle::Dust, "dust"},
    {ParticleStyle::Mote, "mote"},
    {ParticleStyle::Arcane, "arcane"},
    {ParticleStyle::Ash, "ash"},
    {ParticleStyle::Bat, "bat"},
    {ParticleStyle::Bubble, "bubble"},
    {ParticleStyle::Butterfly, "butterfly"},
    {ParticleStyle::Coin, "coin"},
    {ParticleStyle::Confetti, "confetti"},
    {ParticleStyle::Constellation, "constellation"},
    {ParticleStyle::Curse, "curse"},
    {ParticleStyle::Enchant, "enchant"},
    {ParticleStyle::Fairy, "fairy"},
    {ParticleStyle::Fog, "fog"},
    {ParticleStyle::Gem, "gem"},
    {ParticleStyle::Glitter, "glitter"},
    {ParticleStyle::Heart, "heart"},
    {ParticleStyle::Hex, "hex"},
    {ParticleStyle::Ink, "ink"},
    {ParticleStyle::Moon, "moon"},
    {ParticleStyle::Planet, "planet"},
    {ParticleStyle::Pollen, "pollen"},
    {ParticleStyle::Soul, "soul"},
    {ParticleStyle::Steam, "steam"},
    {ParticleStyle::Void, "void"},
    {ParticleStyle::Vortex, "vortex"},
    {ParticleStyle::Wind, "wind"},
    {ParticleStyle::Zap, "zap"},
    {ParticleStyle::Zzz, "zzz"},
    {ParticleStyle::Ember, "ember"},
    {ParticleStyle::Pixiedust, "pixiedust"},
    {ParticleStyle::Runes, "runes"},
    {ParticleStyle::Sand, "sand"},
};
inline constexpr int kParticleStyleCount =
    static_cast<int>(sizeof(kParticleStyleTokens) / sizeof(kParticleStyleTokens[0]));
static_assert(
    []() consteval
    {
        for (int i = 0; i < kParticleStyleCount; ++i)
        {
            if (static_cast<int>(kParticleStyleTokens[i].style) != i)
            {
                return false;
            }
        }
        return true;
    }(),
    "kParticleStyleTokens rows must be listed in ParticleStyle enum order");

/**
 * @struct ParticleSettings
 * @brief Particle aura controls.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Empty or `None` tier styles disable the aura. Tier auras apply only to the player;
 * NPCs need a matched special title with `ForceParticles = 1`. When LOD is enabled,
 * particles stop beyond `LODMidDistance + LODTransitionRange`.
 */
struct ParticleSettings
{
    bool Enabled = true;
    bool UseParticleTextures = true;  ///< Use texture sprites instead of shapes
    int Count = 8;                    ///< Particles per type
    float Size = 3.5f;                ///< Particle size in pixels
    float Speed = 1.0f;               ///< Animation speed multiplier
    float Spread = 20.0f;             ///< Full-scale pixels from text
    float Alpha = .8f;                ///< Maximum particle opacity
    int BlendMode = 0;                ///< 0=additive, 1=screen, 2=Alpha
    float DepthStrength = .7f;        ///< Scales the 3D depth read (size/alpha/parallax)
    float ColorWarmth = .5f;          ///< Warm/cool depth temperature mix [0,1]
    float GlowStrength = .28f;        ///< Additive backlight halo alpha (0 disables glow pass)
    float GlowSize = 2.2f;            ///< Halo radius as a multiple of the crisp sprite size
    float ShineThreshold = .84f;      ///< Sine threshold for the rare specular glint
};
/**
 * @fn ParticleSettings& Particle()
 * @brief Access the shared particle settings.
 * @author Alex (<https://github.com/lextpf>)
 */
ParticleSettings& Particle();

/**
 * @struct DisplaySettings
 * @brief Settings for display placement and actor limits.
 * @author Alex (<https://github.com/lextpf>)
 */
struct DisplaySettings
{
    float VerticalOffset = 8.0f;      ///< Height above actor's head in game units
    float HorizontalOffset = -10.0f;  ///< Screen-space X correction at full scale
    bool HidePlayer = false;          ///< Hide player's own nameplate
    bool HideCreatures = false;       ///< Hide nameplates for non-NPC actors
    int MaxPlates = RenderConstants::DEFAULT_MAX_PLATES;  ///< Visible nameplate limit
    /// Handles inspected per snapshot; validation raises this to at least MaxPlates.
    int MaxScanActors = RenderConstants::DEFAULT_MAX_SCAN_ACTORS;
    int ReloadKey = 0;                ///< Hot-reload virtual key (0 or negative = disabled)
    bool EnableDebugOverlay = false;  ///< Show performance/cache overlay
};
/**
 * @fn DisplaySettings& Display()
 * @brief Access the shared display settings.
 * @author Alex (<https://github.com/lextpf>)
 */
DisplaySettings& Display();

/**
 * @struct DeckSettings
 * @brief Settings for collectible character-card capture.
 * @author Alex (<https://github.com/lextpf>)
 */
struct DeckSettings
{
    bool Enabled = true;
    /// Win32 capture key; 119 is F8, values <= 0 disable.
    int Key = 119;
    std::string OutputFolder =
        "Data/SKSE/Plugins/glyph/cards";  ///< Folder where generated PNG cards are written
    int CardWidth = 750;                  ///< Output card width in pixels
    int CardHeight = 1050;                ///< Output card height in pixels
    /// Pixels around screen center, used only when the engine crosshair pick has no actor.
    int TargetRadius = 220;
    bool PlayerFallback = true;  ///< Capture the player when no actor is targeted
    bool RarityRolls = true;     ///< Let unique actors roll collectible rarity upgrades
};
/**
 * @fn DeckSettings& Deck()
 * @brief Access the shared deck settings.
 * @author Alex (<https://github.com/lextpf>)
 */
DeckSettings& Deck();

/**
 * @struct AnimColorSettings
 * @brief Settings for animation timing and color intensity.
 * @author Alex (<https://github.com/lextpf>)
 */
struct AnimColorSettings
{
    float AlphaSettleTime = .46f;     ///< Alpha settle time in seconds
    float ScaleSettleTime = .46f;     ///< Font scale settle time in seconds
    float PositionSettleTime = .38f;  ///< Position settle time for NPCs in seconds
    float InnerTextAlpha = 1.0f;      ///< Text body alpha multiplier 0-1 (outlines unaffected)
    float OutlineAlpha = 1.0f;        ///< Outline and shadow alpha multiplier 0-1
};
/**
 * @fn AnimColorSettings& AnimColor()
 * @brief Access the shared animation and color settings.
 * @author Alex (<https://github.com/lextpf>)
 */
AnimColorSettings& AnimColor();

/**
 * @struct FontSettings
 * @brief Settings for font paths and sizes.
 * @author Alex (<https://github.com/lextpf>)
 */
struct FontSettings
{
    std::string NameFontPath;     ///< Path to name font TTF file
    float NameFontSize = 122.0f;  ///< Name font size in pixels (ImGui size_pixels)
    std::string LevelFontPath;    ///< Path to level font TTF file
    float LevelFontSize = 61.0f;  ///< Level font size in pixels (ImGui size_pixels)
    std::string TitleFontPath;    ///< Path to title font TTF file
    float TitleFontSize = 42.0f;  ///< Title font size in pixels (ImGui size_pixels)
};
/**
 * @fn FontSettings& Font()
 * @brief Access the shared font settings.
 * @author Alex (<https://github.com/lextpf>)
 */
FontSettings& Font();

/**
 * @struct TransitionSettings
 * @brief Settings for entrance and exit transitions.
 * @author Alex (<https://github.com/lextpf>)
 */
struct TransitionSettings
{
    bool EnableEntrance = false;    ///< Enable pop-in/slide entrance animation
    int EntranceStyle = 0;          ///< 0=PopIn, 1=SlideDown, 2=expand
    float EntranceDuration = .35f;  ///< Entrance animation duration in seconds
    bool EnableExit = false;        ///< Enable exit animation
    float ExitDuration = .20f;      ///< Exit animation duration in seconds

    // Simultaneous entrances stagger nearest first; step zero disables the delay.
    float EntranceStaggerStep = .06f;  ///< Delay between successive entrances (s)
    float EntranceStaggerMax = .8f;    ///< Ceiling for any single plate's delay (s)
};
/**
 * @fn TransitionSettings& Transition()
 * @brief Access the shared transition settings.
 * @author Alex (<https://github.com/lextpf>)
 */
TransitionSettings& Transition();

/**
 * @struct VisualSettings
 * @brief Optional text placement and color adjustments.
 * @author Alex (<https://github.com/lextpf>)
 */
struct VisualSettings
{
    bool EnableDistanceOutlineScale = false;  ///< Scale outline width by distance
    float OutlineDistanceMin = .8f;           ///< Outline multiplier at close range
    float OutlineDistanceMax = 1.5f;          ///< Outline multiplier at far range
    float MinimumPixelHeight = .0f;           ///< Min pixel height for name text, 0=disabled
    bool EnableLOD = false;                   ///< Enable distance-based content LOD
    float LODFarDistance = 1800.0f;           ///< Beyond this: name+level only
    float LODMidDistance = 800.0f;            ///< Beyond this: no particles/ornaments
    float LODTransitionRange = 200.0f;        ///< Smooth transition width in game units
    float TitleAlphaMultiplier = .80f;        ///< Alpha multiplier for title text
    float LevelAlphaMultiplier = .85f;        ///< Alpha multiplier for level text
    bool EnableOverlapPrevention = false;     ///< Push overlapping labels apart
    float OverlapPaddingY = 4.0f;             ///< Vertical padding between labels
    int OverlapIterations = 3;                ///< Relaxation passes for overlap resolution
    float PositionSmoothingBlend = 1.0f;      ///< 1.0=moving-avg, 0.0=exponential
    float LargeMovementThreshold = 50.0f;     ///< Pixel threshold for large movement handling
    float LargeMovementBlend = .5f;           ///< Blend factor for large movements
    bool EnableMotionTrail = false;           ///< Enable afterimage trail on moving nameplates
    int TrailLength = 4;                      ///< Number of ghost copies (1-8)
    float TrailAlpha = .3f;                   ///< Peak ghost opacity
    float TrailFalloff = 2.0f;                ///< Alpha falloff exponent (higher = faster fade)
    float TrailMinDistance = 2.0f;            ///< Min pixels moved before trail renders
    int TrailMinTier = 0;                     ///< Minimum tier index for trail

    bool EnableWave = false;     ///< Enable per-glyph sine wave displacement
    float WaveAmplitude = 1.5f;  ///< Wave height in pixels
    float WaveFrequency = 3.0f;  ///< Cycles across text width
    float WaveSpeed = 1.0f;      ///< Animation speed multiplier
    int WaveMinTier = 0;         ///< Minimum tier index for wave effect

    bool EnableTierEffectGating = false;  ///< Gate effects by tier index
    int GlowMinTier = 5;                  ///< Minimum tier for glow effects
    int ParticleMinTier = 10;             ///< Minimum tier for particle effects
    int OrnamentMinTier = 10;             ///< Minimum tier for ornament display
};
/**
 * @fn VisualSettings& Visual()
 * @brief Access the shared visual settings.
 * @author Alex (<https://github.com/lextpf>)
 */
VisualSettings& Visual();

/**
 * @struct LabelSettings
 * @brief Label strings and thresholds for the contextual nameplate tokens.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Covers `%r` (relationship), `%d` (level delta) and `%c` (creature type).
 *
 * Use a trailing `?` in `Format` / `InfoFormat` to omit blank label segments.
 *
 * @see Segment::dropIfBlank, RelationshipKind, LevelDelta, CreatureKind
 */
struct LabelSettings
{
    std::string RelationshipFollower = "Follower";  ///< Player teammate
    std::string RelationshipAlly = "Ally";          ///< Friendly NPC that can talk
    std::string RelationshipNeutral;                ///< Default: empty
    std::string RelationshipHostile = "Hostile";    ///< Actively hostile to player

    std::string LevelDeltaWeak = "Weak";      ///< Far below player
    std::string LevelDeltaEven;               ///< Default: empty (similar level)
    std::string LevelDeltaStrong = "Strong";  ///< Notably above player
    std::string LevelDeltaDeadly = "Deadly";  ///< Far above player

    std::string CreatureTypeNPC;                ///< Default: empty
    std::string CreatureTypeBeast = "Beast";    ///< ActorTypeCreature / ActorTypeAnimal
    std::string CreatureTypeUndead = "Undead";  ///< ActorTypeUndead
    std::string CreatureTypeDaedra = "Daedra";  ///< ActorTypeDaedra
    std::string CreatureTypeDragon = "Dragon";  ///< ActorTypeDragon

    // Delta is actor level minus player level.
    int WeakAtOrBelow = -5;    ///< Delta <= this -> weak
    int StrongAtOrAbove = 5;   ///< Delta >= this -> strong
    int DeadlyAtOrAbove = 10;  ///< Delta >= this -> deadly (overrides strong)
};
/**
 * @fn LabelSettings& Labels()
 * @brief Access the shared label text and thresholds.
 * @author Alex (<https://github.com/lextpf>)
 */
LabelSettings& Labels();

/**
 * @struct IconSettings
 * @brief Duotone badge names, colors, and rank-emblem controls.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Names omit `.svg`; empty names hide their state. An empty folder disables badges.
 * `ClampAndValidate()` derives RGB members from strings. `kSettings` defines INI names;
 * most use an `Icon` prefix, but tier-emblem and player-lighting keys do not.
 * The licensed Font Awesome Pro assets are excluded from the public repository.
 */
struct IconSettings
{
    std::string Folder = "Data/SKSE/Plugins/glyph/duotone";  ///< SVG folder; empty disables
    bool Enabled = true;      ///< Master toggle (effective only with a folder)
    float Scale = 1.0f;       ///< Badge size relative to the level font size
    bool DeadlyPulse = true;  ///< Subtle alpha pulse on the deadly skull

    std::string FollowerIcon = "shield-halved";
    std::string AllyIcon = "handshake";
    std::string HostileIcon = "skull-crossbones";
    std::string WeakIcon = "caret-down";
    std::string StrongIcon = "caret-up";
    std::string DeadlyIcon = "skull";
    std::string BeastIcon = "paw";
    std::string UndeadIcon = "ghost";
    std::string DaedraIcon = "fire";
    std::string DragonIcon = "dragon";

    std::string FollowerColorStr = "0.46, 0.68, 0.84";
    std::string AllyColorStr = "0.52, 0.74, 0.50";
    std::string HostileColorStr = "0.86, 0.36, 0.32";
    std::string WeakColorStr = "0.54, 0.66, 0.80";
    std::string StrongColorStr = "0.86, 0.62, 0.32";
    std::string DeadlyColorStr = "0.90, 0.28, 0.24";
    std::string CreatureColorStr = "0.80, 0.74, 0.62";

    Color3 FollowerColor;
    Color3 AllyColor;
    Color3 HostileColor;
    Color3 WeakColor;
    Color3 StrongColor;
    Color3 DeadlyColor;
    Color3 CreatureColor;

    // Empty icon names hide a state; slot toggles hide the slot. Inactive states use their
    // own resting tint with MutedAlpha and MutedDesat.
    std::string NeutralIcon = "circle";  ///< Muted relationship
    std::string HumanoidIcon = "user";   ///< Muted creature
    std::string EvenIcon = "equals";     ///< Muted threat
    std::string GuardIcon = "helmet-battle";
    std::string MerchantIcon = "coins";
    std::string CommonerIcon = "house";  ///< Muted role
    std::string EssentialIcon = "certificate";
    std::string ProtectedIcon = "shield-check";
    std::string MortalIcon = "heart";  ///< Muted protection
    std::string CombatIcon = "swords";
    std::string AlertIcon = "eye";
    std::string IdleIcon = "moon";  ///< Muted engagement
    std::string SneakHiddenIcon = "eye-slash";
    std::string SneakDetectedIcon = "eye";
    std::string SneakOffIcon = "person-walking";  ///< Muted
    std::string EncumberedIcon = "weight-hanging";
    std::string NormalWeightIcon = "feather";  ///< Muted
    std::string WantedIcon = "gavel";
    std::string BountyClearIcon = "scale-balanced";  ///< Muted
    std::string TierLowIcon = "medal";
    std::string TierMidIcon = "gem";
    std::string TierHighIcon = "crown";
    // Manifest tierBadges replace rank icons when at least one emblem loads.
    bool TierBadgeImages = true;
    /// Unused; emblem paths come from the manifest.
    std::string TierBadgeFolder = "Data/SKSE/Plugins/glyph/badges";
    /// Emblem curve for tiers with no Badge key. 1.0 is even; above 1 makes high emblems rarer.
    float TierBadgeGamma = 1.0f;
    float TierBadgeScale = 1.7f;  ///< Emblem size as a multiple of the status-icon size

    bool PlayerStripBedEnabled = true;
    float PlayerStripBedAlpha = 0.10f;          ///< Per-disc additive alpha (overlap sums)
    float PlayerStripBedSize = 2.6f;            ///< Disc edge as a multiple of row height
    float PlayerStripBedBreatheHz = 0.14f;      ///< Bed breathe frequency (alpha only)
    std::string PlayerStripBedColorStr;         ///< Empty => derive near-neutral from Name
    std::optional<Color3> PlayerStripBedColor;  ///< Empty => derive at draw time
    bool EmblemBacklightEnabled = true;
    float EmblemBacklightSize = 2.6f;            ///< Disc edge as a multiple of emblem edge
    float EmblemBacklightAlpha = 0.55f;          ///< Peak backlight alpha (x breathe)
    float EmblemBacklightBreatheHz = 0.167f;     ///< Backlight breathe freq (=> sin(t*1.05))
    float EmblemCrispAlpha = 0.95f;              ///< After IconOpacity; nameplate emblem only
    std::string EmblemBacklightColorStr;         ///< Empty => derive near-neutral tier accent
    std::optional<Color3> EmblemBacklightColor;  ///< Empty => derive at draw time

    // Player resting-icon emboss: warm top rim plus carved bottom shadow.
    bool PlayerRimLightEnabled = true;
    float PlayerRimAlpha = 0.22f;    ///< Top-rim highlight alpha (x muted alpha)
    float PlayerCarveAlpha = 0.26f;  ///< Bottom carve-shadow alpha (x muted alpha)
    float PlayerRimOffset = 1.0f;    ///< Emboss offset in px (scaled by text size)
    std::string PlayerRimColorStr;   ///< Empty => derive warm-white from Name
    std::optional<Color3> PlayerRimColor;
    // Emblem directional lights: warm key above and cool fill below the backlight.
    bool EmblemKeyFillEnabled = true;
    float EmblemKeyAlpha = 0.35f;   ///< Key (top) light alpha (x breathe)
    float EmblemFillAlpha = 0.15f;  ///< Fill (bottom) bounce alpha (x breathe)
    float EmblemKeyRise = 0.18f;    ///< Key offset above center (fraction of emblem edge)
    float EmblemFillDrop = 0.15f;   ///< Fill offset below center (fraction of emblem edge)
    std::string EmblemKeyColorStr;  ///< Empty => derive warm from Name
    std::optional<Color3> EmblemKeyColor;
    std::string EmblemFillColorStr;  ///< Empty => derive cool from Name
    std::optional<Color3> EmblemFillColor;

    std::string GuardColorStr = "0.60, 0.68, 0.84";
    std::string MerchantColorStr = "0.84, 0.74, 0.42";
    std::string EssentialColorStr = "0.86, 0.78, 0.46";
    std::string ProtectedColorStr = "0.54, 0.72, 0.86";
    std::string CombatColorStr = "0.88, 0.42, 0.30";
    std::string AlertColorStr = "0.86, 0.76, 0.40";
    std::string SneakHiddenColorStr = "0.50, 0.64, 0.84";
    std::string SneakDetectedColorStr = "0.86, 0.36, 0.32";
    std::string EncumberedColorStr = "0.82, 0.64, 0.40";
    std::string WantedColorStr = "0.84, 0.34, 0.30";
    // Tier colors progress bronze -> silver-blue -> gold.
    std::string TierLowColorStr = "0.70, 0.62, 0.52";
    std::string TierMidColorStr = "0.62, 0.70, 0.80";
    std::string TierHighColorStr = "0.86, 0.74, 0.46";
    std::string NeutralColorStr = "0.56, 0.62, 0.70";
    std::string HumanoidColorStr = "0.74, 0.68, 0.58";
    std::string CommonerColorStr = "0.60, 0.68, 0.54";
    std::string MortalColorStr = "0.76, 0.58, 0.60";
    std::string EvenColorStr = "0.60, 0.70, 0.72";
    std::string IdleColorStr = "0.56, 0.60, 0.76";
    std::string SneakOffColorStr = "0.64, 0.68, 0.60";      ///< Sneak off (player)
    std::string NormalWeightColorStr = "0.64, 0.76, 0.70";  ///< Normal weight (player)
    std::string BountyClearColorStr = "0.50, 0.70, 0.68";   ///< Bounty clear (player)
    std::string MutedColorStr = "0.62, 0.64, 0.68";         ///< Shared tint, unused

    Color3 GuardColor;
    Color3 MerchantColor;
    Color3 EssentialColor;
    Color3 ProtectedColor;
    Color3 CombatColor;
    Color3 AlertColor;
    Color3 SneakHiddenColor;
    Color3 SneakDetectedColor;
    Color3 EncumberedColor;
    Color3 WantedColor;
    Color3 TierLowColor;
    Color3 TierMidColor;
    Color3 TierHighColor;
    Color3 NeutralColor;
    Color3 HumanoidColor;
    Color3 CommonerColor;
    Color3 MortalColor;
    Color3 EvenColor;
    Color3 IdleColor;
    Color3 SneakOffColor;
    Color3 NormalWeightColor;
    Color3 BountyClearColor;
    Color3 MutedColor;

    // Per-slot enables (a disabled active state drops that actor's badge).
    bool RelationshipEnabled = true;
    bool CreatureEnabled = true;
    bool ThreatEnabled = true;
    bool RoleEnabled = true;
    bool ProtectionEnabled = true;
    bool EngagementEnabled = true;  ///< Master for the NPC engagement slot
    bool CombatStateEnabled = true;
    bool AlertStateEnabled = true;
    bool SneakEnabled = true;
    bool PlayerCombatEnabled = true;
    bool EncumberedEnabled = true;
    bool BountyEnabled = true;
    bool TierEnabled = true;  ///< Rank badge for player and NPCs

    // Clamp status-row alpha to 1.0 before applying MutedAlpha to resting slots.
    float Opacity = 0.92f;

    float MutedAlpha = 1.0f;
    /// Desaturation strength in [0, 1]; zero preserves the resting hue.
    float MutedDesat = 0.18f;
};
/**
 * @fn IconSettings& Icons()
 * @brief Access the shared badge settings.
 * @author Alex (<https://github.com/lextpf>)
 */
IconSettings& Icons();

/**
 * @struct NpcColorSettings
 * @brief NPC outline, shadow, and glow tints.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Name fill stays white; title and level fill use the tier's level gradient.
 * `ClampAndValidate()` parses the RGB strings into color members in [0, 1].
 */
struct NpcColorSettings
{
    std::string NeutralColorStr = "1.0, 1.0, 1.0";
    std::string HostileColorStr = "1.0, 0.72, 0.68";
    std::string FollowerColorStr = "0.72, 0.84, 1.0";
    std::string LevelColorStr = "0.82, 0.84, 0.88";
    std::string TitleColorStr = "0.92, 0.93, 0.95";

    Color3 NeutralColor;
    Color3 HostileColor;
    Color3 FollowerColor;
    Color3 LevelColor;
    Color3 TitleColor;
};
/**
 * @fn NpcColorSettings& NpcColors()
 * @brief Access the shared NPC color settings.
 * @author Alex (<https://github.com/lextpf>)
 */
NpcColorSettings& NpcColors();

/**
 * @struct FocusSettings
 * @brief Expanded content for the actor nearest camera-forward.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Pick at most one actor inside the cone; skip the player, dead actors, and capture-only
 * entries. Other NPCs show only the dimmed main row; the player remains unchanged.
 * A Graffito raycast target, or crosshair target when Graffito is off, expands immediately.
 * Other transitions use `SettleTime`.
 */
struct FocusSettings
{
    bool Enabled = false;           ///< Master toggle (off = no focus pick, no dimming)
    float ConeAngleDegrees = 8.0f;  ///< Cone half-angle for selection [0.5, 45]
    /// Game units; zero adds no limit beyond MaxScanDistance.
    float MaxDistance = .0f;
    float AmbientDimFactor = .55f;  ///< Alpha multiplier for non-focused actors [0.05, 1]
    float SettleTime = .25f;        ///< Seconds for focusSmooth crossfade [0, 2]
    bool IgnoreOccluded = true;     ///< Skip occluded actors when picking focus
};
/**
 * @fn FocusSettings& Focus()
 * @brief Access the shared focus settings.
 * @author Alex (<https://github.com/lextpf>)
 */
FocusSettings& Focus();

// clang-format off
/**
 * @struct GraffitoSettings
 * @brief Text on an actor-aligned world plane.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Folio mode adds front relief and compact side/rear facets. Single-sheet mode has
 * front, seam, and mirrored-back states. `FacingFadeDegrees` controls transitions
 * near edge-on, including folio collapse; zero selects hard states.
 * `Scale` and `PlayerScale` preserve authored layout ratios.
 */
struct GraffitoSettings
{
    bool Enabled = false;                 ///< Master toggle (off = billboard nameplates)
    float Scale = 1.0f;                   ///< Physical text-size multiplier [0.25, 4]
    float PlayerScale = .72f;             ///< Uniform player-only multiplier [0.25, 2]
    float ForwardOffset = 4.0f;           ///< Game units along actor-forward [0, 32]
    float FacingFadeDegrees = 15.0f;      ///< Single-sheet edge fade in degrees [0, 45]
    float BacksideBleedAlpha = .12f;      ///< Single-sheet mirrored ink opacity [0, .25]
    float EdgeSeamAlpha = .22f;           ///< Single-sheet edge-seam opacity [0, .4]
    bool FolioEnabled = true;             ///< Relief front plus one side/rear head marker
    float FolioReverseAlpha = .72f;       ///< Head-marker opacity from the rear [0, 1]
    float FolioSpineAlpha = .88f;         ///< Head-marker opacity edge-on [0, 1]
    float FolioDepth = 2.0f;              ///< Relief plane spacing in source pixels [0, 28]
    float WrapDegrees = 55.0f;            ///< Total cylindrical arc across content [0, 140]
    float FisheyeStrength = .62f;         ///< Text-row midpoint magnification [0, 1]
    float LayerDepth = .0f;               ///< Role separation, fraction of view distance [0, .06]
    float EdgeSheen = .14f;               ///< Grazing highlight strength on crowned wings [0, .4]
    float OrientationSettleTime = .18f;   ///< Actor-plane orientation smoothing [0, 2] seconds
    float MaxDistance = 1200.0f;          ///< Effect range; 0 = normal nameplate distance
    bool FallenEpitaphEnabled = true;     ///< Hinge death-rite text onto the ground plane
    float EpitaphGroundLift = 2.0f;       ///< Game units above ground against z-fighting [0, 16]
};
/**
 * @fn GraffitoSettings& Graffito()
 * @brief Access the shared graffito settings.
 * @author Alex (<https://github.com/lextpf>)
 */
GraffitoSettings& Graffito();

/**
 * @struct CompatSettings
 * @brief TrueHUD and moreHUD visibility adjustments.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Absent mods leave plates unchanged. Only TrueHUD floating bars cause yielding;
 * docked boss bars do not.
 * @see HudCompat
 */
// clang-format on
struct CompatSettings
{
    bool YieldToTrueHUD = true;       ///< Yield plates to TrueHUD floating bars
    float TrueHUDYieldAlpha = .0f;    ///< Plate alpha while yielded [0,1]
    bool YieldLevelToMoreHUD = true;  ///< Drop level for moreHUD's crosshair target
    float YieldSettleTime = .3f;      ///< Seconds for the yield crossfade
};
/**
 * @fn CompatSettings& Compat()
 * @brief Access the shared HUD compatibility settings.
 * @author Alex (<https://github.com/lextpf>)
 */
CompatSettings& Compat();

/**
 * @struct DeathRiteSettings
 * @brief Death animation for actors first seen alive.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Play once per corpse. Cancel pending animations when the overlay resumes after suppression.
 */
struct DeathRiteSettings
{
    bool Enabled = true;
    float Duration = 1.6f;  ///< Full animation length in seconds
};
/**
 * @fn DeathRiteSettings& DeathRite()
 * @brief Access the shared death rite settings.
 * @author Alex (<https://github.com/lextpf>)
 */
DeathRiteSettings& DeathRite();

/**
 * @struct QuietSettings
 * @brief Camera-motion suppression of secondary content.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Quiet hides sub-lines and dims names. Smoothstep blends between the pan thresholds;
 * names return before sub-lines when the camera settles.
 */
struct QuietSettings
{
    bool Enabled = true;
    float PanThresholdLo = 40.0f;   ///< Deg/s where quieting begins
    float PanThresholdHi = 160.0f;  ///< Deg/s where fully quiet
    float AttackTime = .10f;        ///< Settle time receding into quiet (s)
    float NameReleaseTime = .28f;   ///< Name resolve-back settle time (s)
    float SubReleaseTime = .50f;    ///< Sub-line resolve-back settle time (s)
    float NameFloor = .35f;         ///< Name alpha multiplier at full quiet
};
/**
 * @fn QuietSettings& Quiet()
 * @brief Access the shared quiet settings.
 * @author Alex (<https://github.com/lextpf>)
 */
QuietSettings& Quiet();

/**
 * @struct CandlelightSettings
 * @brief Scene-based brightness and warmth adjustment.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Adjustments are capped and smoothed per actor. Unavailable metering logs once and
 * leaves the image unchanged.
 */
struct CandlelightSettings
{
    bool Enabled = true;
    float Strength = .08f;   ///< Max brightness adjustment (fraction, [0, 0.15])
    float Warmth = .5f;      ///< Chroma pull toward the scene in dark shots [0,1]
    float SettleTime = .6f;  ///< Per-actor smoothing settle time (s)
};
/**
 * @fn CandlelightSettings& Candlelight()
 * @brief Access the shared candlelight settings.
 * @author Alex (<https://github.com/lextpf>)
 */
CandlelightSettings& Candlelight();

/**
 * @struct DepthClipSettings
 * @brief Soft per-pixel occlusion against scene depth.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Settings
 *
 * Line-of-sight culling remains active. Unavailable depth logs once and disables
 * only the per-pixel test.
 */
struct DepthClipSettings
{
    bool Enabled = true;
    float Feather = 2.5f;  ///< Feather radius at the occlusion edge (px, [0,8])
};
/**
 * @fn DepthClipSettings& DepthClipConfig()
 * @brief Access the shared depth-clipping settings.
 * @author Alex (<https://github.com/lextpf>)
 */
DepthClipSettings& DepthClipConfig();

/**
 * @fn std::shared_mutex& Mutex()
 * @brief Lock for settings accessor references.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Accessors return function-local statics. Readers need a shared lock; writers need
 * a unique lock. `Load()` rewrites all settings; the console changes the debug flag.
 * The render thread copies `RenderSettingsSnapshot` when `Generation()` changes.
 * Returning a reference does not acquire this lock. Keep it held while reading or writing
 * through that reference; copy strings and vectors before releasing it.
 */
[[nodiscard]] std::shared_mutex& Mutex();

/**
 * @fn std::atomic<uint32_t>& Generation()
 * @brief Version used to refresh cached settings.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Advances after a complete INI parse or a console debug-flag edit. A missing file applies
 * validated defaults but leaves the counter unchanged. On a missing-INI startup,
 * generation-gated consumers therefore keep their default-constructed copies.
 */
[[nodiscard]] std::atomic<uint32_t>& Generation();

/**
 * @fn void Load()
 * @brief Load and validate glyph.ini under the settings write lock.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Missing keys retain defaults. Malformed scalar numbers become zero before clamping;
 * indexed fields use their own fallbacks. Unknown keys and malformed lines are logged.
 * An invalid descriptor default type can throw; see `SettingEntry`.
 *
 * Any thread may call this parser; it dereferences no game objects.
 *
 * @pre The caller holds no lock on `Mutex()`.
 * @post Tiers remain nonempty and indexed by section number. Gaps create default
 * tiers covering levels 1-250, which shadow higher tiers and produce a warning.
 * @post `Format` supplies title and main rows; `InfoFormat` supplies the optional info row.
 * @post `Generation()` advances unless the file is missing.
 */
void Load();
}  // namespace Settings
