#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

/**
 * @namespace Settings
 * @brief Configuration management and INI parsing.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Settings
 *
 * Contains all user-configurable settings loaded from `glyph.ini`,
 * including tier definitions, visual effects, fonts, and behavior parameters.
 *
 * ## :material-file-cog-outline: Configuration File
 *
 * Settings are loaded from `Data/SKSE/Plugins/glyph.ini` using a simple
 * key-value format with section headers for tier definitions.
 *
 * | Section            | Purpose                                  |
 * |--------------------|------------------------------------------|
 * | `[General]`        | Core settings, fonts, distances          |
 * | `[TierN]`          | Per-tier colors, effects, ornaments      |
 * | `[SpecialTitleN]`  | Keyword-based title overrides            |
 * | `[Display]`        | Format string for nameplate composition  |
 * | `[Appearance]`     | NPC appearance template settings         |
 *
 * @note Runtime defaults are defined in the `kSettings` binding table in
 * `Settings.cpp`.  `Load()` calls `ResetToDefaults()` first, which applies
 * the `kSettings` defaults to every field, then parses `glyph.ini` to
 * overwrite any keys that are present.  Struct member initializers shown
 * below are compile-time placeholders only and may differ from the
 * operative `kSettings` values.
 *
 * ## :material-refresh: Hot Reload
 *
 * Settings can be reloaded at runtime by pressing the configured `ReloadKey`
 * (default: F7). This calls `Load()` and clears the actor cache.
 *
 * ## :material-blur-linear: Distance Fading
 *
 * Nameplate alpha fades based on distance $d$ from camera to actor using
 * a squared smoothstep for a gentle falloff:
 *
 * $$t = \text{clamp}\!\left(\frac{d - d_{start}}{d_{end} - d_{start}},\; 0,\; 1\right)$$
 *
 * $$\text{smoothstep}(t) = 3t^2 - 2t^3$$
 *
 * $$\alpha = 1 - \text{smoothstep}(t)^2$$
 *
 * This produces near-full opacity at close range, a gradual falloff through
 * mid-range, and rapid fadeout approaching `FadeEndDistance`.
 *
 * ## :material-format-font-size-decrease: Font Scaling
 *
 * Font size scales down with distance using square root falloff, which
 * keeps text readable longer before shrinking:
 *
 * $$t = \text{clamp}\!\left(\frac{d - d_{start}}{d_{end} - d_{start}},\; 0,\; 1\right)$$
 *
 * $$scale = 1 + (scale_{min} - 1) \cdot \sqrt{t}$$
 *
 * At $d = d_{start}$ the scale is $1.0$ (full size). At $d = d_{end}$
 * it reaches $scale_{min}$ (default $0.1$, or 10% of original).
 *
 * ## :material-connection: SKSE Integration
 *
 * | Resource       | Path / API                                                |
 * |----------------|-----------------------------------------------------------|
 * | Settings file  | `Data/SKSE/Plugins/glyph.ini`                             |
 * | Log file       | `Documents/My Games/Skyrim Special Edition/SKSE/glyph.log`|
 * | Actor data     | `RE::TESDataHandler`                                      |
 */
namespace Settings
{
/**
 * RGB color triplet with channel values in [0.0, 1.0].
 *
 * Replaces raw `float[3]` arrays for color fields, providing named access,
 * clamping, and conversion helpers.
 */
struct Color3
{
    float r = 1.f, g = 1.f, b = 1.f;

    constexpr Color3() = default;
    constexpr Color3(float r_, float g_, float b_)
        : r(r_),
          g(g_),
          b(b_)
    {
    }

    constexpr Color3& clamp01()
    {
        r = std::clamp(r, 0.f, 1.f);
        g = std::clamp(g, 0.f, 1.f);
        b = std::clamp(b, 0.f, 1.f);
        return *this;
    }

    static constexpr Color3 White() { return {1.f, 1.f, 1.f}; }
    constexpr bool operator==(const Color3&) const = default;
};

/**
 * Display format segment for nameplate composition.
 *
 * Defines a single segment of the nameplate display. Multiple segments
 * are concatenated horizontally to form the complete nameplate.
 *
 * **Format Placeholders:**
 * - `%n` - Actor's display name
 * - `%l` - Actor's level
 * - `%t` - Tier title (from TierDefinition)
 *
 * ```ini
 * ; Example: "Lydia [42]" with name in name font, level in level font
 * [Display]
 * Segment1 = %n,0
 * Segment2 = " [",0
 * Segment3 = %l,1
 * Segment4 = "]",0
 * ```
 *
 * @see DisplayFormat, TierDefinition
 */
struct Segment
{
    std::string format;  ///< Format string with placeholders (%n, %l, %t)
    bool useLevelFont;   ///< If true, uses level font; otherwise uses name font
};

/**
 * Visual effect types for text rendering.
 *
 * Defines the available visual effects that can be applied to
 * names, levels, and titles. Effects are rendered using ImGui's
 * draw list with per-vertex coloring.
 *
 * **Effect Categories:**
 * - Static: None, Gradient, VerticalGradient, DiagonalGradient, RadialGradient
 * - Animated: Shimmer, ChromaticShimmer, Ember, RainbowWave, ConicRainbow, Aurora
 * - Complex: Sparkle, Plasma, Scanline, Enchant, Frost
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
    ChromaticShimmer,  ///< Chromatic aberration shimmer (param1-4 for tuning)
    Ember,             ///< Warm flickering glow (param1 = speed, param2 = intensity)
    RainbowWave,       ///< Animated rainbow (param1-5 for hue/speed/saturation)
    ConicRainbow,      ///< Circular rainbow rotation (param1-4 for tuning)
    Aurora,  ///< Northern lights effect (param1 = speed, param2 = waves, param3 = intensity, param4
             ///< = sway)
    Sparkle,   ///< Glittering stars (param1 = density, param2 = speed, param3 = intensity)
    Plasma,    ///< Demoscene plasma pattern (param1 = freq1, param2 = freq2, param3 = speed)
    Scanline,  ///< Horizontal scanning bar (param1 = speed, param2 = width, param3 = intensity)
    Enchant,   ///< Flowing magical energy (param1 = speed, param2 = scale, param3 = intensity)
    Frost      ///< Crystalline ice sparkle (param1 = density, param2 = speed, param3 = intensity)
};

/**
 * Parameters for visual effects.
 *
 * Generic parameter structure for effect configuration. Different effects
 * interpret the parameters differently. See EffectType documentation for
 * parameter meanings per effect.
 *
 * ```cpp
 * // Aurora effect: speed=0.5, waves=3.0, intensity=1.0, sway=0.3
 * EffectParams aurora;
 * aurora.type = EffectType::Aurora;
 * aurora.param1 = .5f;   // speed
 * aurora.param2 = 3.0f;   // wave count
 * aurora.param3 = 1.0f;   // intensity
 * aurora.param4 = .3f;   // sway amount
 * ```
 *
 * @see EffectType, TierDefinition, TextEffects::ApplyVertexEffect
 */
struct EffectParams
{
    EffectType type = EffectType::Gradient;  ///< Effect type to apply

    float param1 = .0f;  ///< Effect parameter 1 (meaning varies by effect)
    float param2 = .0f;  ///< Effect parameter 2
    float param3 = .0f;  ///< Effect parameter 3
    float param4 = .0f;  ///< Effect parameter 4
    float param5 = .0f;  ///< Effect parameter 5

    bool useWhiteBase = false;  ///< Draw white base layer under rainbow effects for brightness
};

/**
 * Tier definition for level-based visual styling.
 *
 * Each tier defines visual properties for a range of character levels.
 * Higher tiers typically have more elaborate visual effects. Tiers are
 * defined in `[TierN]` sections of the INI file.
 *
 * **Color Format:**
 * Colors are specified as comma-separated RGB floats in range [0.0, 1.0].
 *
 * ```ini
 * [Tier5]
 * MinLevel = 30
 * MaxLevel = 39
 * Title = Veteran
 * LeftColor = 0.2, 0.6, 1.0
 * RightColor = 0.8, 0.2, 1.0
 * HighlightColor = 1.0, 1.0, 1.0
 * TitleEffect = Shimmer, 0.3, 0.8
 * NameEffect = Gradient
 * LevelEffect = Gradient
 * ```
 *
 * @see Tiers, EffectParams, Renderer::Draw
 */
struct TierDefinition
{
    uint16_t minLevel = 1;          ///< Minimum level for this tier (inclusive)
    uint16_t maxLevel = 250;        ///< Maximum level for this tier (inclusive)
    std::string title = "Unknown";  ///< Title text (e.g., "Novice", "Legend of Tamriel")
    Color3 leftColor;               ///< RGB color for name gradient left/top
    Color3 rightColor;              ///< RGB color for name gradient right/bottom
    Color3 highlightColor;          ///< RGB color for shimmer/sparkle highlights

    /// Per-element color overrides (optional - falls back to derived from leftColor/rightColor)
    std::optional<Color3> titleLeftColor;   ///< Title gradient left (default: washed from name)
    std::optional<Color3> titleRightColor;  ///< Title gradient right
    std::optional<Color3> levelLeftColor;   ///< Level gradient left (default: mixed from name)
    std::optional<Color3> levelRightColor;  ///< Level gradient right
    std::optional<Color3> particleColor;    ///< Particle tint (default: highlightColor)

    EffectParams titleEffect;  ///< Visual effect for title text (player only)
    EffectParams nameEffect;   ///< Visual effect for name text (player only)
    EffectParams levelEffect;  ///< Visual effect for level text (all actors)

    std::string
        leftOrnaments;  ///< Left side ornament characters (e.g., "AC"), empty = no ornaments
    std::string
        rightOrnaments;  ///< Right side ornament characters (e.g., "BD"), empty = no ornaments

    // Per-tier particle settings (empty = use global, "None" = disabled)
    std::string
        particleTypes;      ///< Particle types: "Stars,Wisps,Orbs,Sparks,Runes" (comma-separated)
    int particleCount = 0;  ///< Number of particles (0 = use global setting)
};

/**
 * @brief Special title definition for MMORPG-style staff/VIP nameplates.
 *
 * Special titles override normal tier styling when an actor's name contains
 * the specified keyword. Used for Admin, Moderator, VIP, etc. nameplates.
 *
 * | Field | Description |
 * |-------|-------------|
 * | keyword | Text to match in actor name (case-insensitive) |
 * | displayTitle | Title shown above name (e.g., "[ADMIN]") |
 * | color | RGB color for the nameplate |
 * | glowColor | RGB color for enhanced glow effect |
 * | forceOrnaments | Always show ornaments regardless of tier |
 * | forceParticles | Always show particle effects |
 * | priority | Higher priority special titles take precedence |
 */
struct SpecialTitleDefinition
{
    std::string keyword;         ///< Keyword to match in name (case-insensitive)
    std::string keywordLower;    ///< Cached lowercase keyword for fast runtime matching
    std::string displayTitle;    ///< Title to display
    Color3 color;                ///< RGB color for name/title
    Color3 glowColor;            ///< RGB glow color (more saturated)
    bool forceOrnaments;         ///< Always show ornaments
    bool forceParticles;         ///< Always show particle aura
    int priority;                ///< Higher = checked first
    std::string leftOrnaments;   ///< Left side ornament characters
    std::string rightOrnaments;  ///< Right side ornament characters
};

// Collection accessors (function-local statics for safe destruction order)
std::string& TitleFormat();             ///< Format string for title line (e.g., "%t")
std::vector<Segment>& DisplayFormat();  ///< Segments for main nameplate line
std::vector<TierDefinition>& Tiers();   ///< All tier definitions (indexed by tier number)
std::vector<SpecialTitleDefinition>& SpecialTitles();  ///< Special title overrides

/// Distance and visibility fade/scale settings.
struct DistanceSettings
{
    float FadeStartDistance = 200.0f;   ///< Distance where fade begins
    float FadeEndDistance = 2500.0f;    ///< Distance where fully transparent
    float ScaleStartDistance = 200.0f;  ///< Distance where font size scaling begins
    float ScaleEndDistance = 2500.0f;   ///< Distance where minimum font size is reached
    float MinimumScale = .1f;           ///< Smallest font size multiplier
    float MaxScanDistance = 3000.0f;    ///< Maximum actor scan distance
};
DistanceSettings& Distance();

/// Occlusion culling settings.
struct OcclusionSettings
{
    bool Enabled = true;      ///< Enable LOS-based occlusion
    float SettleTime = .58f;  ///< Fade settle time in seconds
    int CheckInterval = 3;    ///< Frames between LOS checks
};
OcclusionSettings& Occlusion();

/// Shadow offsets, segment padding, and outline settings.
struct ShadowOutlineSettings
{
    float TitleShadowOffsetX = 2.0f;   ///< Title shadow X offset in pixels
    float TitleShadowOffsetY = 2.0f;   ///< Title shadow Y offset in pixels
    float MainShadowOffsetX = 4.0f;    ///< Main text shadow X offset
    float MainShadowOffsetY = 4.0f;    ///< Main text shadow Y offset
    float SegmentPadding = 4.0f;       ///< Horizontal padding between segments
    float OutlineWidthMin = 2.0f;      ///< Base outline width
    float OutlineWidthMax = 2.5f;      ///< Additional width for high tiers
    bool FastOutlines = false;         ///< Use 4-dir outlines instead of 8-dir
    float TitleMainGap = .0f;          ///< Vertical gap between title and main line (pixels)
    float OutlineMinScale = .65f;      ///< Minimum outline width ratio for smaller text
    bool ProportionalSpacing = false;  ///< Scale pixel spacings with text size

    // Outline Glow (white halo behind outline)
    bool OutlineGlowEnabled = false;   ///< Enable white glow behind text outline
    float OutlineGlowScale = 1.4f;     ///< Glow radius as multiplier of outline width
    float OutlineGlowAlpha = .1f;      ///< Peak glow ring opacity 0-1
    int OutlineGlowRings = 2;          ///< Concentric glow rings (1-3)
    float OutlineGlowR = 1.0f;         ///< Glow color red
    float OutlineGlowG = 1.0f;         ///< Glow color green
    float OutlineGlowB = 1.0f;         ///< Glow color blue
    bool OutlineGlowTierTint = false;  ///< Blend glow color with tier color

    // Dual-tone directional outline
    bool DualOutlineEnabled = false;  ///< Enable inner outline tinted with tier color
    float InnerOutlineTint = .3f;     ///< How much to blend toward tier color (0=outline, 1=tier)
    float InnerOutlineAlpha = .5f;    ///< Inner outline opacity multiplier
    float InnerOutlineScale = .5f;    ///< Inner outline width as fraction of outer
    float DirectionalLightAngle = 315.f;  ///< Light direction in degrees (0=right, 90=down)
    float DirectionalLightBias = .15f;    ///< Directional width variation (0=uniform)

    // Color tinting
    float OutlineColorTint = .0f;  ///< Tier-color tint for outlines 0-0.25
    float ShadowColorTint = .0f;   ///< Tier-color tint for shadows 0-0.25
};
ShadowOutlineSettings& ShadowOutline();

/// Glow effect settings.
struct GlowSettings
{
    bool Enabled = false;   ///< Enable glow effect
    float Radius = 4.0f;    ///< Glow spread in pixels
    float Intensity = .5f;  ///< Glow brightness 0-1
    int Samples = 8;        ///< Quality samples 8-16
};
GlowSettings& Glow();

/// Shine overlay effect settings (static top-edge highlight).
struct ShineSettings
{
    bool Enabled = false;    ///< Enable shine overlay on text
    float Intensity = .35f;  ///< Peak brightness at top edge 0-1
    float Falloff = 2.0f;    ///< Vertical falloff exponent (higher = sharper)
    float TextGlowAlpha =
        .0f;  ///< Translucent text body alpha reduction 0-1 (0=opaque, 1=fully translucent)
};
ShineSettings& Shine();

/// Typewriter reveal effect settings.
struct TypewriterSettings
{
    bool Enabled = false;  ///< Enable typewriter reveal
    float Speed = 30.0f;   ///< Characters per second
    float Delay = .0f;     ///< Delay before reveal starts
};
TypewriterSettings& Typewriter();

/// Side ornament settings.
struct OrnamentSettings
{
    bool Enabled = true;     ///< Enable side ornaments
    float Scale = 1.0f;      ///< Size multiplier
    float Spacing = 3.0f;    ///< Distance from text edges
    std::string FontPath;    ///< Path to ornament font (TTF/OTF)
    float FontSize = 64.0f;  ///< Ornament font size in points
    bool AnchorToMainLine =
        true;  ///< Anchor ornaments to main text line instead of nameplate center
};
OrnamentSettings& Ornament();

/**
 * Visual styles for particle aura effects.
 *
 * @see ParticleSettings, TextEffects::DrawParticleAura
 */
enum class ParticleStyle
{
    Stars,    ///< Twinkling blue star points
    Sparks,   ///< Fast, yellowish fire-like sparks
    Wisps,    ///< Slow, ethereal wisps with pale/blue tint
    Runes,    ///< Small magical rune symbols
    Orbs,     ///< Soft glowing orbs
    Crystals  ///< Geometric crystalline shapes
};

/// Particle aura settings.
struct ParticleSettings
{
    bool Enabled = true;              ///< Master enable for particle aura
    bool UseParticleTextures = true;  ///< Use texture sprites instead of shapes
    bool EnableStars = true;          ///< Enable twinkling stars
    bool EnableSparks = false;        ///< Enable fire-like sparks
    bool EnableWisps = false;         ///< Enable ethereal wisps
    bool EnableRunes = false;         ///< Enable magical runes
    bool EnableOrbs = false;          ///< Enable glowing orbs
    bool EnableCrystals = false;      ///< Enable crystalline shapes
    int Count = 8;                    ///< Particles per type
    float Size = 3.0f;                ///< Particle size in pixels
    float Speed = 1.0f;               ///< Animation speed multiplier
    float Spread = 20.0f;             ///< How far particles spread from text
    float Alpha = .8f;                ///< Maximum particle opacity
    int BlendMode = 0;                ///< 0=Additive, 1=Screen, 2=Alpha
};
ParticleSettings& Particle();

/// Display behavior settings.
struct DisplaySettings
{
    float VerticalOffset = 8.0f;      ///< Height above actor's head in units
    bool HidePlayer = false;          ///< Hide player's own nameplate
    bool HideCreatures = false;       ///< Hide nameplates for non-NPC actors
    int ReloadKey = 0;                ///< Virtual key code for hot reload (0 = disabled)
    bool EnableDebugOverlay = false;  ///< Show performance/cache overlay
};
DisplaySettings& Display();

/// Animation speed and color/effect intensity settings.
struct AnimColorSettings
{
    float AnimSpeedLowTier = .35f;    ///< Speed for tiers 0-7
    float AnimSpeedMidTier = .20f;    ///< Speed for tier 8
    float AnimSpeedHighTier = .10f;   ///< Speed for tier 9+
    float ColorWashAmount = .15f;     ///< Desaturation toward white 0-1
    float NameColorMix = .65f;        ///< Base color strength 0-1
    float EffectAlphaMin = .20f;      ///< Minimum effect alpha
    float EffectAlphaMax = .60f;      ///< Maximum effect alpha
    float StrengthMin = .15f;         ///< Minimum effect strength
    float StrengthMax = .60f;         ///< Maximum effect strength
    float AlphaSettleTime = .46f;     ///< Alpha settle time in seconds
    float ScaleSettleTime = .46f;     ///< Font scale settle time in seconds
    float PositionSettleTime = .38f;  ///< Position settle time for NPCs in seconds
    float TierVibrancyBoost = .0f;    ///< Extra color saturation for high tiers 0-1
    float InnerTextAlpha = 1.0f;      ///< Text body alpha multiplier 0-1 (outlines unaffected)
    float TextSaturationBoost = .0f;  ///< Extra color saturation for text body 0-2
};
AnimColorSettings& AnimColor();

/// Font path and size settings.
struct FontSettings
{
    std::string NameFontPath;     ///< Path to name font TTF file
    float NameFontSize = 122.0f;  ///< Name font size in points
    std::string LevelFontPath;    ///< Path to level font TTF file
    float LevelFontSize = 61.0f;  ///< Level font size in points
    std::string TitleFontPath;    ///< Path to title font TTF file
    float TitleFontSize = 42.0f;  ///< Title font size in points
};
FontSettings& Font();

/// Entrance/exit transition animation settings.
struct TransitionSettings
{
    bool EnableEntrance = false;      ///< Enable pop-in/slide entrance animation
    int EntranceStyle = 0;            ///< 0=PopIn, 1=SlideDown, 2=Expand
    float EntranceDuration = .35f;    ///< Entrance animation duration in seconds
    float EntranceOvershoot = 1.05f;  ///< Scale overshoot for PopIn style
    bool EnableExit = false;          ///< Enable exit animation
    float ExitDuration = .20f;        ///< Exit animation duration in seconds
};
TransitionSettings& Transition();

/// NPC appearance template settings.
struct AppearanceSettings
{
    std::string TemplateFormID;            ///< FormID of template NPC (hex, e.g., "0x12345")
    std::string TemplatePlugin;            ///< Plugin file containing template
    bool UseTemplateAppearance = false;    ///< Whether to apply template appearance to player
    bool TemplateIncludeRace = false;      ///< Whether to copy race
    bool TemplateIncludeBody = false;      ///< Whether to copy height/body morphs
    bool TemplateCopyFaceGen = true;       ///< Whether to load and apply FaceGen NIF/tint
    bool TemplateCopySkin = false;         ///< Whether to copy skin textures
    bool TemplateCopyOverlays = false;     ///< Whether to copy RaceMenu overlays
    bool TemplateCopyOutfit = false;       ///< Whether to copy equipped armor
    bool TemplateReapplyOnReload = false;  ///< Whether to re-apply on hot reload
    std::string TemplateFaceGenPlugin;     ///< Optional override for FaceGen plugin path
};
AppearanceSettings& Appearance();

/// Visual polish settings, encapsulated to avoid non-const globals.
struct VisualSettings
{
    // Distance-Based Outline
    bool EnableDistanceOutlineScale = false;  ///< Scale outline width by distance
    float OutlineDistanceMin = .8f;           ///< Outline multiplier at close range
    float OutlineDistanceMax = 1.5f;          ///< Outline multiplier at far range
    // Minimum Readable Size
    float MinimumPixelHeight = .0f;  ///< Min pixel height for name text, 0=disabled
    // LOD by Distance
    bool EnableLOD = false;             ///< Enable distance-based content LOD
    float LODFarDistance = 1800.0f;     ///< Beyond this: name+level only
    float LODMidDistance = 800.0f;      ///< Beyond this: no particles/ornaments
    float LODTransitionRange = 200.0f;  ///< Smooth transition width in game units
    // Visual Hierarchy
    float TitleAlphaMultiplier = .80f;  ///< Alpha multiplier for title text
    float LevelAlphaMultiplier = .85f;  ///< Alpha multiplier for level text
    // Overlap Prevention
    bool EnableOverlapPrevention = false;  ///< Push overlapping labels apart
    float OverlapPaddingY = 4.0f;          ///< Vertical padding between labels
    int OverlapIterations = 3;             ///< Relaxation passes for overlap resolution
    // Position Smoothing Tuning
    float PositionSmoothingBlend = 1.0f;   ///< 1.0=moving-avg, 0.0=exponential
    float LargeMovementThreshold = 50.0f;  ///< Pixel threshold for large movement handling
    float LargeMovementBlend = .5f;        ///< Blend factor for large movements
    // Motion Trail
    bool EnableMotionTrail = false;  ///< Enable afterimage trail on moving nameplates
    int TrailLength = 4;             ///< Number of ghost copies (1-8)
    float TrailAlpha = .3f;          ///< Peak ghost opacity
    float TrailFalloff = 2.0f;       ///< Alpha falloff exponent (higher = faster fade)
    float TrailMinDistance = 2.0f;   ///< Min pixels moved before trail renders
    int TrailMinTier = 0;            ///< Minimum tier index for trail

    // Wave Displacement
    bool EnableWave = false;     ///< Enable per-glyph sine wave displacement
    float WaveAmplitude = 1.5f;  ///< Wave height in pixels
    float WaveFrequency = 3.0f;  ///< Cycles across text width
    float WaveSpeed = 1.0f;      ///< Animation speed multiplier
    int WaveMinTier = 0;         ///< Minimum tier index for wave effect

    // Tier Effect Gating
    bool EnableTierEffectGating = false;  ///< Gate effects by tier index
    int GlowMinTier = 5;                  ///< Minimum tier for glow effects
    int ParticleMinTier = 10;             ///< Minimum tier for particle effects
    int OrnamentMinTier = 10;             ///< Minimum tier for ornament display
};
VisualSettings& Visual();

/**
 * Shared settings mutex.
 *
 * Readers should hold a shared lock while consuming settings during
 * long operations. Settings::Load() acquires a unique lock.
 */
[[nodiscard]] std::shared_mutex& Mutex();

/**
 * Settings generation counter.
 *
 * Incremented each time Load() finishes. Consumers can compare against
 * a cached value to avoid re-copying settings when nothing has changed.
 */
[[nodiscard]] std::atomic<uint32_t>& Generation();

/**
 * Load all settings from glyph.ini.
 *
 * Parses the configuration file and populates all settings variables.
 * Called once during plugin initialization and on hot reload.
 *
 * File Location: Data/SKSE/Plugins/glyph.ini
 *
 * Missing file or invalid values use defaults. No errors are thrown.
 *
 * @post All extern variables in this namespace are populated with values
 *       from the INI file or their defaults.
 * @post Tiers vector contains all `[TierN]` sections sorted by tier number.
 * @post DisplayFormat contains parsed format segments from the `[Display]` section.
 *
 * @see Renderer::Draw, TierDefinition, Segment
 */
void Load();
}  // namespace Settings
