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
 * | Section              | Purpose                                  |
 * |----------------------|------------------------------------------|
 * | `[General]`          | Core settings, fonts, distances          |
 * | `[TierN]`            | Per-tier colors, effects, ornaments      |
 * | `[SpecialTitleN]`    | Keyword-based title overrides            |
 * | `[Display]`          | Format string for nameplate composition  |
 * | `[Visual]`           | LOD, overlap, motion trail, wave         |
 * | `[Fonts]`            |
 * Font paths and sizes                     | | `[Particles]`        | Particle aura settings | |
 * `[Occlusion]`        | Line-of-sight culling settings           | | `[Debug]`            | Debug
 * overlay toggle                     |
 *
 * @note Scalar settings are matched by key name regardless of which
 * section they appear in.
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
 * (default: disabled; set to a virtual key code, e.g. `118` for F7).
 * This calls `Load()` and clears the actor cache.
 *
 * ## :material-blur-linear: Distance Fading
 *
 * Nameplate alpha fades based on distance $d$ from camera to actor using
 * a squared quintic smoothstep for a gentle falloff:
 *
 * $$t = \text{clamp}\!\left(\frac{d - d_{start}}{d_{end} - d_{start}},\; 0,\; 1\right)$$
 *
 * $$\text{smoothstep}(t) = 6t^5 - 15t^4 + 10t^3$$
 *
 * $$\alpha = \left(1 - \text{smoothstep}(t)\right)^2$$
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
    std::string format;         ///< Format string with placeholders (%n, %l, %t, %r, %d, %c)
    bool useLevelFont = false;  ///< If true, uses level font; otherwise uses name font
    bool dropIfBlank = false;   ///< Set by trailing `?` in Format / InfoFormat -- segment is
                                ///< omitted when its expansion trims to empty.
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
 * - Animated: Shimmer, Ember, Aurora, Breathe, Mote, Wander
 * - Complex: Sparkle, Enchant, Frost, Drift
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
    Aurora,   ///< Northern lights effect (param1 = speed, param2 = waves, param3 = intensity,
              ///< param4 = sway)
    Sparkle,  ///< Glittering stars (param1 = density, param2 = speed, param3 = intensity)
    Enchant,  ///< Flowing magical energy (param1 = speed, param2 = scale, param3 = intensity)
    Frost,    ///< Crystalline ice sparkle (param1 = density, param2 = speed, param3 = intensity)
    Breathe,  ///< Slow uniform brightness pulse (param1 = speed Hz, param2 = amplitude)
    Drift,    ///< Slow uniform hue wander (param1 = speed Hz, param2 = hue range degrees)
    Mote,     ///< Rare single twinkle (param1 = period s, param2 = peak alpha)
    Wander,   ///< Per-character asynchronous breathing (param1 = speed Hz, param2 = amplitude,
              ///< param3 = phase spread)
    Eclipse,  ///< Shadow band sweeping with a hot leading rim (param1 = width,
              ///< param2 = strength)
    Pulse,    ///< Weighty two-beat heartbeat glow (param1 = rate Hz, param2 = amplitude)
    Electric  ///< Rare crackling arc sweeping the text (param1 = rate Hz, param2 = intensity)
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

    /// Per-element color overrides (optional - falls back to derived tier accents)
    std::optional<Color3>
        titleLeftColor;  ///< Title gradient left (default: companion accent from tier palette)
    std::optional<Color3> titleRightColor;  ///< Title gradient right
    std::optional<Color3>
        levelLeftColor;  ///< Level gradient left (default: softened tier/highlight blend)
    std::optional<Color3> levelRightColor;  ///< Level gradient right
    std::optional<Color3> particleColor;    ///< Particle tint (default: highlightColor)
    std::optional<Color3>
        ornamentLeftColor;  ///< Ornament gradient left (default: vivid title/highlight accent)
    std::optional<Color3>
        ornamentRightColor;  ///< Ornament gradient right (default: vivid title/highlight accent)

    EffectParams titleEffect;  ///< Visual effect for title text (player / special titles)
    EffectParams nameEffect;   ///< Visual effect for name text (player / special titles)
    EffectParams levelEffect;  ///< Visual effect for level text (player / special titles)

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

/**
 * @brief Deeds, Not Words -- a faction-driven honorific title.
 *
 * Honorifics replace the static tier title with one the game state actually
 * granted: an actor (player included) that holds at least `minRank` in the
 * resolved faction shows `title` in the title slot the moment the deed is
 * done.  Defined in `[HonorificN]` INI sections; the highest-priority match
 * wins; special titles (name-keyword styling) still take precedence.
 *
 * `factionSpec` is `0xFORMID` or `0xFORMID@Plugin.esp` (plugin defaults to
 * Skyrim.esm), resolved against the load order on the game thread.
 */
struct HonorificDefinition
{
    std::string factionSpec;  ///< "0xFORMID[@Plugin.esp]" faction reference
    std::string title;        ///< Honorific text shown in the title slot
    int minRank = 0;          ///< Minimum faction rank required
    int priority = 0;         ///< Higher wins when several factions match
    bool playerOnly = false;  ///< Only the player's plate may earn this
    bool npcOnly = false;     ///< Only NPC plates may earn this
};

/// Scene-context predicate bits for Registers (context-conditional profiles).
/// The game thread computes the current mask once per snapshot; a register
/// matches when all `whenMask` bits are set and no `whenNotMask` bit is.
namespace Context
{
inline constexpr uint32_t Interior = 1u << 0;  ///< Player's cell is interior
inline constexpr uint32_t Night = 1u << 1;     ///< Game hour outside [6, 20)
inline constexpr uint32_t City = 1u << 2;      ///< Location keyword LocTypeCity/Town
inline constexpr uint32_t Sneaking = 1u << 3;  ///< Player is sneaking
inline constexpr uint32_t Dialogue = 1u << 4;  ///< Dialogue menu speaker active
inline constexpr uint32_t Crowded = 1u << 5;   ///< Visible plates >= threshold
}  // namespace Context

/**
 * @brief A Register -- one context-conditional overlay profile.
 *
 * Registers bind a small, curated knob set to scene predicates so the
 * overlay swells and recedes with the scene instead of asking the player to
 * retune per situation: dimmer at night, fewer plates in a packed city,
 * quieter sub-lines in dialogue.  The highest-priority matching register is
 * active; transitions ease over `RegisterSettings::TransitionTime` on the
 * render thread.  An empty `When` matches every scene (a base register).
 */
struct RegisterDefinition
{
    std::string name;          ///< Optional label (logs/debugging)
    uint32_t whenMask = 0;     ///< Context bits that must all be set
    uint32_t whenNotMask = 0;  ///< Context bits that must all be clear
    float alphaMul = 1.0f;     ///< Overlay-wide alpha multiplier [0,1]
    float fadeMul = 1.0f;      ///< Fade/scale distance multiplier [0.2,2]
    float subLineMul = 1.0f;   ///< Title/info/level/badge alpha multiplier [0,1]
    bool hideNeutral = false;  ///< Hide neutral + ally NPC plates entirely
    int priority = 0;          ///< Highest-priority match wins

    /// True once any key parsed inside this section.  Index gaps in
    /// [RegisterN] numbering back-fill with defaults whose empty `When`
    /// would otherwise match every scene and shadow the real register on
    /// priority ties -- unconfigured entries never match.
    bool configured = false;
};

/// Register globals (the profiles themselves live in Registers()).
struct RegisterSettings
{
    bool Enabled = true;          ///< Master toggle for the register system
    float TransitionTime = 1.2f;  ///< Seconds for knob transitions to settle
    int CrowdedThreshold = 12;    ///< Visible plates that count as "crowded"
};
RegisterSettings& RegisterConfig();

// Collection accessors (function-local statics for safe destruction order)
std::string& TitleFormat();             ///< Format string for title line (e.g., "%t")
std::vector<Segment>& DisplayFormat();  ///< Segments for main nameplate line (row 2)
std::vector<Segment>& InfoFormat();     ///< Segments for contextual info row (row 3, below main)
std::vector<TierDefinition>& Tiers();   ///< All tier definitions (indexed by tier number)
std::vector<SpecialTitleDefinition>& SpecialTitles();  ///< Special title overrides
std::vector<HonorificDefinition>& Honorifics();        ///< Faction-driven honorific titles
std::vector<RegisterDefinition>& Registers();          ///< Context-conditional profiles

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
    float TitleShadowOffsetX = 2.0f;  ///< Title shadow X offset in pixels
    float TitleShadowOffsetY = 2.0f;  ///< Title shadow Y offset in pixels
    float MainShadowOffsetX = 4.0f;   ///< Main text shadow X offset
    float MainShadowOffsetY = 4.0f;   ///< Main text shadow Y offset
    float OutlineWidthMin = 2.0f;     ///< Base outline width
    float OutlineWidthMax = 2.5f;     ///< Additional width for high tiers
    bool FastOutlines = false;        ///< Use 4-dir outlines instead of 8-dir

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

    // Soft directional drop-shadow (feathered; replaces the hard offset shadow)
    bool SoftShadowEnabled = false;   ///< Use a soft directional shadow instead of a hard offset
    float SoftShadowDistance = 4.0f;  ///< Offset distance along the cast angle (pixels)
    float SoftShadowSoftness = 3.0f;  ///< Feather/blur radius of the shadow disc (pixels)
    float SoftShadowOpacity = .8f;    ///< Peak shadow opacity multiplier 0-1
    float SoftShadowAngle = 45.0f;    ///< Shadow cast direction in degrees (0=right, 90=down)
    int SoftShadowSamples = 12;       ///< Feather sample count 4-24 (higher = smoother)
};
ShadowOutlineSettings& ShadowOutline();

/// Glow effect settings.
struct GlowSettings
{
    bool Enabled = false;      ///< Enable glow effect
    float Radius = 4.0f;       ///< Glow spread in pixels
    float Intensity = .5f;     ///< Glow brightness 0-1
    int Samples = 8;           ///< Quality samples 1-64 (recommended: 8-16)
    float DivideStrength = 0;  ///< Color-divide blend 0-1 (0 = additive glow, 1 = full divide)
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
        true;             ///< Anchor ornaments to main text line instead of nameplate center
    float OffsetY = .0f;  ///< Manual vertical nudge in pixels (negative = up),
                          ///< scaled with text size, applied after anchoring
};
OrnamentSettings& Ornament();

/**
 * Visual styles for particle aura effects.
 *
 * @see ParticleSettings, TextEffects::DrawParticleAura
 */
/// @note The ordinal is significant: it indexes the per-type sprite set in
/// `ParticleTextures`. The single source of truth linking each style to its
/// INI token / sprite filename base is `kParticleStyleTokens` below -- every
/// dependent table static_asserts against `kParticleStyleCount`.
enum class ParticleStyle
{
    Firefly,        ///< Slow wandering glow that flickers in place
    Snow,           ///< Snowflakes drifting around a broad slow orbit
    Smoke,          ///< Rising, expanding, fading puffs
    Spark,          ///< Orbiting embers that periodically flare outward hot
    Wisp,           ///< Serpentine ethereal trails
    Leaf,           ///< Orbiting leaves with in/out drift and fluttering tumble
    Aurora,         ///< Wavy horizontal shimmer band
    CherryBlossom,  ///< Orbiting petals with a gentle bob and slow spin
    Dust,           ///< Very slow floating motes
    Mote,           ///< Soft glowing motes that gently drift and pulse
    // -- 2026-07 sprite expansion. Appended so existing ordinals stay stable. --
    Arcane,         ///< Stately orbiting rune, slow spin, glow pulse
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
    Planet,         ///< A single majestic ringed planet on a slow deep orbit
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

/// Canonical style -> token mapping. The token is simultaneously the
/// case-insensitive `ParticleTypes` INI name and the sprite filename base in
/// `Data/SKSE/Plugins/glyph/particles/` (`<token>.png`, `<token>2.png`,
/// `<token>_strip.png`, ...). Order must match the enum; dependent tables
/// (`ParticleTextures`, `RendererEffects`, the motion spec table) size-check
/// against `kParticleStyleCount`.
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

/// Particle aura settings.
///
/// Which particle styles actually render is determined per tier via
/// `TierDefinition::particleTypes` (e.g. `ParticleTypes = Snow,Firefly`).
/// `Enabled` is the master toggle; a tier with `ParticleTypes = None` (or
/// empty) renders no particles regardless of this setting.
struct ParticleSettings
{
    bool Enabled = true;              ///< Master enable for particle aura
    bool UseParticleTextures = true;  ///< Use texture sprites instead of shapes
    int Count = 8;                    ///< Particles per type
    float Size = 3.5f;                ///< Particle size in pixels
    float Speed = 1.0f;               ///< Animation speed multiplier
    float Spread = 20.0f;             ///< How far particles spread from text
    float Alpha = .8f;                ///< Maximum particle opacity
    int BlendMode = 0;                ///< 0=Additive, 1=Screen, 2=Alpha
    float DepthStrength = .7f;        ///< Scales the 3D depth read (size/alpha/parallax)
    float ColorWarmth = .5f;          ///< Warm/cool depth temperature mix [0,1]
    float GlowStrength = .28f;        ///< Additive backlight halo alpha (0 disables glow pass)
    float GlowSize = 2.2f;            ///< Halo radius as a multiple of the crisp sprite size
    float ShineThreshold = .84f;      ///< Sine threshold for the rare specular glint
};
ParticleSettings& Particle();

/// Display behavior settings.
struct DisplaySettings
{
    float VerticalOffset = 8.0f;      ///< Height above actor's head in game units
    float HorizontalOffset = -10.0f;  ///< Screen-space X correction in pixels at full scale
    bool HidePlayer = false;          ///< Hide player's own nameplate
    bool HideCreatures = false;       ///< Hide nameplates for non-NPC actors
    int ReloadKey = 0;                ///< Virtual key code for hot reload (0 = disabled)
    bool EnableDebugOverlay = false;  ///< Show performance/cache overlay
};
DisplaySettings& Display();

/// Animation speed and color/effect intensity settings.
struct AnimColorSettings
{
    float AlphaSettleTime = .46f;     ///< Alpha settle time in seconds
    float ScaleSettleTime = .46f;     ///< Font scale settle time in seconds
    float PositionSettleTime = .38f;  ///< Position settle time for NPCs in seconds
    float InnerTextAlpha = 1.0f;      ///< Text body alpha multiplier 0-1 (outlines unaffected)
    float OutlineAlpha = 1.0f;        ///< Outline and shadow alpha multiplier 0-1
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
    bool EnableEntrance = false;    ///< Enable pop-in/slide entrance animation
    int EntranceStyle = 0;          ///< 0=PopIn, 1=SlideDown, 2=Expand
    float EntranceDuration = .35f;  ///< Entrance animation duration in seconds
    bool EnableExit = false;        ///< Enable exit animation
    float ExitDuration = .20f;      ///< Exit animation duration in seconds

    // Roll Call -- staggered entrance cascade
    // When several plates begin their entrance on the same frame (overlay
    // wake after combat/menus/loads, hot reload, a group entering range),
    // each successive plate waits one step longer, nearest first, so the
    // scene introduces itself as a quiet near-to-far wave instead of a
    // simultaneous pop.  Step 0 disables staggering.
    float EntranceStaggerStep = .06f;  ///< Delay between successive entrances (s)
    float EntranceStaggerMax = .8f;    ///< Ceiling for any single plate's delay (s)
};
TransitionSettings& Transition();

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
 * Configurable label strings and thresholds for the contextual nameplate tokens
 * (`%r` relationship, `%d` level delta, `%c` creature type).
 *
 * Empty strings render as nothing -- pair with a trailing `?` segment marker in
 * `Format` / `InfoFormat` to drop the segment entirely when its tokens expand
 * to whitespace.
 *
 * @see Segment::dropIfBlank, RelationshipKind, LevelDelta, CreatureKind
 */
struct LabelSettings
{
    // Relationship labels (`%r`)
    std::string RelationshipFollower = "Follower";  ///< Player teammate
    std::string RelationshipAlly = "Ally";          ///< Friendly NPC that can talk
    std::string RelationshipNeutral;                ///< Default: empty
    std::string RelationshipHostile = "Hostile";    ///< Actively hostile to player

    // Level delta labels (`%d`, actor level vs. player level)
    std::string LevelDeltaWeak = "Weak";      ///< Far below player
    std::string LevelDeltaEven;               ///< Default: empty (similar level)
    std::string LevelDeltaStrong = "Strong";  ///< Notably above player
    std::string LevelDeltaDeadly = "Deadly";  ///< Far above player

    // Creature type labels (`%c`)
    std::string CreatureTypeNPC;                ///< Default: empty
    std::string CreatureTypeBeast = "Beast";    ///< ActorTypeCreature / ActorTypeAnimal
    std::string CreatureTypeUndead = "Undead";  ///< ActorTypeUndead
    std::string CreatureTypeDaedra = "Daedra";  ///< ActorTypeDaedra
    std::string CreatureTypeDragon = "Dragon";  ///< ActorTypeDragon

    // Level delta classification thresholds (actor level minus player level)
    int WeakAtOrBelow = -5;    ///< delta <= this -> Weak
    int StrongAtOrAbove = 5;   ///< delta >= this -> Strong
    int DeadlyAtOrAbove = 10;  ///< delta >= this -> Deadly (overrides Strong)
};
LabelSettings& Labels();

/**
 * Status icon badge settings.
 *
 * Renders the contextual facts (relationship, level delta, creature kind)
 * as a compact strip of icon indicators below the name/level row instead of
 * the text-based info row.  Icons are duotone SVGs (Font Awesome naming)
 * rasterized at load time from `IconFolder`; each `Icon*` value is a file
 * name without the `.svg` extension.  Empty names hide that badge.
 *
 * Colors are comma-separated RGB floats in [0,1]; the `*Color` members are
 * derived from the string forms in `ClampAndValidate()`.
 *
 * @note The bundled duotone set is Font Awesome Pro (licensed per-seat) and
 *       is excluded from the public repository.  An empty `IconFolder`
 *       disables badges entirely.
 */
struct IconSettings
{
    std::string Folder = "Data/SKSE/Plugins/glyph/duotone";  ///< SVG folder; empty disables
    bool Enabled = true;      ///< Master toggle (effective only with a folder)
    float Scale = 1.0f;       ///< Badge size relative to the level font size
    bool DeadlyPulse = true;  ///< Subtle alpha pulse on the Deadly skull

    // Icon names (SVG file names without extension -- the INI surface)
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

    // Badge colors (comma-separated RGB strings -- the INI surface)
    std::string FollowerColorStr = "0.46, 0.68, 0.84";
    std::string AllyColorStr = "0.52, 0.74, 0.50";
    std::string HostileColorStr = "0.86, 0.36, 0.32";
    std::string WeakColorStr = "0.54, 0.66, 0.80";
    std::string StrongColorStr = "0.86, 0.62, 0.32";
    std::string DeadlyColorStr = "0.90, 0.28, 0.24";
    std::string CreatureColorStr = "0.80, 0.74, 0.62";

    // Derived values (computed in ClampAndValidate, not INI keys)
    Color3 FollowerColor;
    Color3 AllyColor;
    Color3 HostileColor;
    Color3 WeakColor;
    Color3 StrongColor;
    Color3 DeadlyColor;
    Color3 CreatureColor;

    // Expanded always-on slots (more NPC + player indicators)
    // New status slots render alongside the original three.  Neutral/inactive
    // states show muted (MutedColor, dimmed via MutedAlpha + desaturated via
    // MutedDesat at draw time); active states show lit in their own color.
    // Set any `*Icon` empty to hide that state; toggle a whole slot via its
    // `*Enabled`.  Icon names require matching SVGs in the IconFolder.
    // Always-on neutral states (previously blank) + new NPC categories.
    std::string NeutralIcon = "circle";  ///< muted relationship
    std::string HumanoidIcon = "user";   ///< muted creature
    std::string EvenIcon = "equals";     ///< muted threat
    std::string GuardIcon = "helmet-battle";
    std::string MerchantIcon = "coins";
    std::string CommonerIcon = "house";  ///< muted role
    std::string EssentialIcon = "certificate";
    std::string ProtectedIcon = "shield-check";
    std::string MortalIcon = "heart";  ///< muted protection
    std::string CombatIcon = "swords";
    std::string AlertIcon = "eye";
    std::string IdleIcon = "moon";  ///< muted engagement
    // Player slot icons.
    std::string SneakHiddenIcon = "eye-slash";
    std::string SneakDetectedIcon = "eye";
    std::string SneakOffIcon = "person-walking";  ///< muted
    std::string EncumberedIcon = "weight-hanging";
    std::string NormalWeightIcon = "feather";  ///< muted
    std::string WantedIcon = "gavel";
    std::string BountyClearIcon = "scale-balanced";  ///< muted
    // Player tier-band prestige badge (low / mid / high thirds of the ladder).
    std::string TierLowIcon = "medal";
    std::string TierMidIcon = "gem";
    std::string TierHighIcon = "crown";
    // Full-color prestige emblem images for the tier badge.  When enabled and at
    // least one emblem loads, they replace the medal/gem/crown icons above with
    // a top-weighted rank climb (see BadgeTextures::InitializeTierImages).
    // Emblems are PNGs named 1.png, 2.png, ... in TierBadgeFolder.
    bool TierBadgeImages = true;
    std::string TierBadgeFolder = "Data/SKSE/Plugins/glyph/badges";
    float TierBadgeGamma = 1.8f;  ///< Emblem->tier top-weighting (>1 = high emblems rarer)
    float TierBadgeScale = 1.7f;  ///< Emblem size as a multiple of the status-icon size

    // --- Player "Seat of Light" block elevation (player-only) ---
    // Strip light bed: a soft breathing pool behind the player's icon strip.
    bool PlayerStripBedEnabled = true;
    float PlayerStripBedAlpha = 0.10f;          ///< Per-disc additive alpha (overlap sums)
    float PlayerStripBedSize = 2.6f;            ///< Disc EDGE as a multiple of row height
    float PlayerStripBedBreatheHz = 0.14f;      ///< Bed breathe frequency (alpha only)
    std::string PlayerStripBedColorStr;         ///< Empty => derive near-neutral from Name
    std::optional<Color3> PlayerStripBedColor;  ///< Empty => derive at draw time
    // Emblem backlight: one true radial disc replacing the ghosted art copies.
    bool EmblemBacklightEnabled = true;
    float EmblemBacklightSize = 2.6f;            ///< Disc EDGE as a multiple of emblem edge
    float EmblemBacklightAlpha = 0.55f;          ///< Peak backlight alpha (x breathe)
    float EmblemBacklightBreatheHz = 0.167f;     ///< Backlight breathe freq (=> sin(t*1.05))
    float EmblemCrispAlpha = 0.92f;              ///< Crisp emblem alpha on the backlight path
    std::string EmblemBacklightColorStr;         ///< Empty => derive near-neutral tier accent
    std::optional<Color3> EmblemBacklightColor;  ///< Empty => derive at draw time

    // --- B "Struck Metal" optional layers (default ON, subtle values) ---
    // Player resting-icon emboss: warm top rim + carved bottom shadow.
    bool PlayerRimLightEnabled = true;
    float PlayerRimAlpha = 0.22f;    ///< Top-rim highlight alpha (x muted alpha)
    float PlayerCarveAlpha = 0.26f;  ///< Bottom carve-shadow alpha (x muted alpha)
    float PlayerRimOffset = 1.0f;    ///< Emboss offset in px (scaled by text size)
    std::string PlayerRimColorStr;   ///< Empty => derive warm-white from Name
    std::optional<Color3> PlayerRimColor;
    // Emblem directional lights: warm KEY above + cool FILL below the backlight.
    bool EmblemKeyFillEnabled = true;
    float EmblemKeyAlpha = 0.35f;   ///< Key (top) light alpha (x breathe)
    float EmblemFillAlpha = 0.15f;  ///< Fill (bottom) bounce alpha (x breathe)
    float EmblemKeyRise = 0.18f;    ///< Key offset above center (fraction of emblem edge)
    float EmblemFillDrop = 0.15f;   ///< Fill offset below center (fraction of emblem edge)
    std::string EmblemKeyColorStr;  ///< Empty => derive warm from Name
    std::optional<Color3> EmblemKeyColor;
    std::string EmblemFillColorStr;  ///< Empty => derive cool from Name
    std::optional<Color3> EmblemFillColor;

    // Lit colors for the active states.
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
    // Tier-band prestige colors: bronze -> silver-blue -> calm gold, in the
    // same muted-saturation family as the other lit colors.
    std::string TierLowColorStr = "0.70, 0.62, 0.52";
    std::string TierMidColorStr = "0.62, 0.70, 0.80";
    std::string TierHighColorStr = "0.86, 0.74, 0.46";
    // Resting-state colors: each "muted" slot carries its own calm hue so the
    // always-on strip reads as a colored spectrum rather than uniform grey.
    std::string NeutralColorStr = "0.56, 0.62, 0.70";       ///< neutral relationship
    std::string HumanoidColorStr = "0.74, 0.68, 0.58";      ///< humanoid creature
    std::string CommonerColorStr = "0.60, 0.68, 0.54";      ///< commoner role
    std::string MortalColorStr = "0.76, 0.58, 0.60";        ///< mortal protection
    std::string EvenColorStr = "0.60, 0.70, 0.72";          ///< even threat
    std::string IdleColorStr = "0.56, 0.60, 0.76";          ///< idle engagement
    std::string SneakOffColorStr = "0.64, 0.68, 0.60";      ///< sneak off (player)
    std::string NormalWeightColorStr = "0.64, 0.76, 0.70";  ///< normal weight (player)
    std::string BountyClearColorStr = "0.50, 0.70, 0.68";   ///< bounty clear (player)
    std::string MutedColorStr = "0.62, 0.64, 0.68";         ///< deprecated shared tint (unused)

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
    bool EngagementEnabled = true;  ///< master for the NPC engagement slot
    bool CombatStateEnabled = true;
    bool AlertStateEnabled = true;
    bool SneakEnabled = true;
    bool PlayerCombatEnabled = true;
    bool EncumberedEnabled = true;
    bool BountyEnabled = true;
    bool TierEnabled = true;  ///< player tier-band prestige badge

    // Lit-badge opacity gain, multiplied into the icon tint alpha (clamped to
    // 1.0 at draw). Raises overall icon visibility; muted slots use MutedAlpha.
    float Opacity = 1.15f;

    // Resting styling: alpha multiplier and desaturation strength [0,1].
    // Lower desat keeps each resting slot's hue; alpha keeps it subordinate.
    float MutedAlpha = 0.45f;
    float MutedDesat = 0.18f;
};
IconSettings& Icons();

/**
 * NPC nameplate text colors.
 *
 * NPCs render flat, white-leaning text; the tier palettes apply only to the
 * player (and special titles).  The name color is keyed by the actor's
 * relationship to the player: hostiles lean warm, teammates lean cool, and
 * everyone else -- including merely talkable civilians -- stays neutral.
 *
 * Colors are comma-separated RGB floats in [0,1]; the `*Color` members are
 * derived from the string forms in `ClampAndValidate()`.
 */
struct NpcColorSettings
{
    // Colors (comma-separated RGB strings -- the INI surface)
    std::string NeutralColorStr = "1.0, 1.0, 1.0";     ///< Neutral + talkable civilians
    std::string HostileColorStr = "1.0, 0.72, 0.68";   ///< Slight warm lean for hostiles
    std::string FollowerColorStr = "0.72, 0.84, 1.0";  ///< Slight cool lean for teammates
    std::string LevelColorStr = "0.82, 0.84, 0.88";    ///< Dimmed silver level readout
    std::string TitleColorStr = "0.92, 0.93, 0.95";    ///< Soft white title text

    // Derived values (computed in ClampAndValidate, not INI keys)
    Color3 NeutralColor;
    Color3 HostileColor;
    Color3 FollowerColor;
    Color3 LevelColor;
    Color3 TitleColor;
};
NpcColorSettings& NpcColors();

/**
 * Focus-target expanded-nameplate settings.
 *
 * When `Enabled`, the render thread picks at most one actor per frame --
 * the one whose direction from the camera lies inside a cone around
 * camera-forward with the smallest angular offset.  That actor renders
 * the full title + main + info row stack; every other actor renders
 * only the main line at a reduced alpha (`AmbientDimFactor`).
 *
 * The player is never picked as the focus target and is never dimmed.
 * Transitions between ambient and focused are smoothed via a per-actor
 * value driven by `Renderer::ExpApproachAlpha` with `SettleTime`.
 *
 * @see Renderer::SelectFocusedActor, RendererInternal::ActorCache::focusSmooth
 */
struct FocusSettings
{
    bool Enabled = false;           ///< Master toggle (off = legacy behavior)
    float ConeAngleDegrees = 8.0f;  ///< Cone half-angle for selection [0.5, 45]
    float MaxDistance = .0f;        ///< Max world distance; 0 = reuse Distance().MaxScanDistance
    float AmbientDimFactor = .55f;  ///< Alpha multiplier for non-focused actors [0.05, 1]
    float SettleTime = .25f;        ///< Seconds for focusSmooth crossfade [0, 2]
    bool IgnoreOccluded = true;     ///< Skip occluded actors when picking focus
};
FocusSettings& Focus();

/**
 * One Voice Per Actor -- TrueHUD / moreHUD deconfliction.
 *
 * When TrueHUD floats an info/boss bar over an actor, glyph yields that
 * actor's plate (fading it to `TrueHUDYieldAlpha`); when moreHUD's
 * crosshair readout already shows the target's level, glyph drops its own
 * level segment for that actor.  Both handshakes are automatic -- absent
 * mods simply leave the behavior off.
 *
 * @see HudCompat
 */
struct CompatSettings
{
    bool YieldToTrueHUD = true;       ///< Yield plates to TrueHUD floating bars
    float TrueHUDYieldAlpha = .0f;    ///< Plate alpha while yielded [0,1]
    bool YieldLevelToMoreHUD = true;  ///< Drop level for moreHUD's crosshair target
    float YieldSettleTime = .3f;      ///< Seconds for the yield crossfade
};
CompatSettings& Compat();

/**
 * Last Rites -- a one-shot death valediction.
 *
 * A tracked actor that dies in view no longer pops out: the plate holds
 * perfectly still for a beat, its ink drains to ash, and the name takes a
 * creature-keyed farewell -- a mortal's gutters out and sinks, a draugr's
 * crumbles letter by letter, a dragon's sears bright before going dark.
 * Plays once per corpse, never for corpses first seen dead, and rites
 * pending while the overlay was suppressed (player combat) are cancelled
 * on wake rather than replayed stale.
 */
struct DeathRiteSettings
{
    bool Enabled = true;    ///< Master toggle
    float Duration = 1.6f;  ///< Full rite length in seconds
};
DeathRiteSettings& DeathRite();

/**
 * The Quiet Frame -- camera-motion quieting.
 *
 * During fast camera pans nothing on screen is readable, so the overlay
 * exhales: sub-lines (title, info row, level, badges) fold away entirely
 * and names thin to a low-alpha trace.  When the camera settles the
 * information re-resolves in a damped cadence -- names first, sub-lines
 * breathing back after -- via an asymmetric envelope (fast attack toward
 * quiet, slow weighty release back).
 *
 * Angular speed below `PanThresholdLo` leaves the overlay untouched; above
 * `PanThresholdHi` it is fully quiet; smoothstep in between.
 *
 * @see Renderer::UpdateQuietFrame, RendererState::quietName
 */
struct QuietSettings
{
    bool Enabled = true;            ///< Master toggle
    float PanThresholdLo = 40.0f;   ///< deg/s where quieting begins
    float PanThresholdHi = 160.0f;  ///< deg/s where fully quiet
    float AttackTime = .10f;        ///< Settle time receding into quiet (s)
    float NameReleaseTime = .28f;   ///< Name resolve-back settle time (s)
    float SubReleaseTime = .50f;    ///< Sub-line resolve-back settle time (s)
    float NameFloor = .35f;         ///< Name alpha multiplier at full quiet
};
QuietSettings& Quiet();

/**
 * Candlelight Metering -- exposure-adaptive ink.
 *
 * The renderer meters the scene behind each plate like a film exposure
 * meter: over a bright snowfield the ink dims a touch; in a torchlit crypt
 * it lifts a few percent and picks up a whisper of the scene's warmth.
 * All adjustments are hard-capped (`Strength`) and smoothed per actor, so
 * the typography participates in the scene's lighting without ever calling
 * attention to itself.  Unsupported render setups disable the feature
 * silently (one log line, zero visual change).
 */
struct CandlelightSettings
{
    bool Enabled = true;     ///< Master toggle
    float Strength = .08f;   ///< Max brightness adjustment (fraction, [0, 0.15])
    float Warmth = .5f;      ///< Chroma pull toward the scene in dark shots [0,1]
    float SettleTime = .6f;  ///< Per-actor smoothing settle time (s)
};
CandlelightSettings& Candlelight();

/**
 * Cut by the World -- per-pixel depth occlusion.
 *
 * Plates are depth-tested per pixel against the game's own depth buffer, so
 * a name is physically sliced by a doorframe as an actor walks behind it
 * instead of the whole plate popping.  The soft feather hides the raw
 * intersection edge.  The coarse line-of-sight culling stays on regardless;
 * this is the fine, sub-plate truth layered on top.  Setups where the depth
 * buffer is unavailable (some ENB/upscaler stacks) fall back to LOS-only
 * occlusion automatically, with one log line.
 */
struct DepthClipSettings
{
    bool Enabled = true;   ///< Master toggle
    float Feather = 2.5f;  ///< Feather radius at the occlusion edge (px, [0,8])
};
DepthClipSettings& DepthClipConfig();

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
