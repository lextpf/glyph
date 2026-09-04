// Load() holds Mutex() uniquely; callers must not already hold it. The parser
// dereferences no game objects, including during the render-thread reload fallback.
//
// Load() pipeline, in order:
//
//   ResetToDefaults()   Format/InfoFormat, the four indexed vectors, then
//        |              ResetTableDefaults() for every kSettings row.
//        v
//   line loop           Trim -> StripInlineComment -> section header or key=value.
//        |              A header selects one indexed family ([TierN], [SpecialTitleN],
//        |              [HonorificN], [RegisterN]) or the global context. A key is
//        |              offered to the active indexed parser first, then to
//        |              Format/InfoFormat, then to the kSettings map keyed on the raw
//        |              INI key. Anything left over is counted and warned about.
//        v
//   ClampAndValidate()  per-row validation rules, then cross-field constraints, then
//        |              the string-to-Color3 derivations.
//        v
//   Generation()++      release store. The render thread re-captures its
//                       RenderSettingsSnapshot when this counter changes.
//
// A missing glyph.ini short-circuits after ResetToDefaults() and ClampAndValidate(),
// and does not advance Generation().

#include "Settings.hpp"

#include "PCH.hpp"
#include "RenderConstants.hpp"
#include "SettingsBinding.hpp"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Settings
{

static constexpr Stl::EnumStringMap<EffectType, 18> kEffectTypeMap{{
    {{"none", EffectType::None},
     {"gradient", EffectType::Gradient},
     {"verticalgradient", EffectType::VerticalGradient},
     {"diagonalgradient", EffectType::DiagonalGradient},
     {"radialgradient", EffectType::RadialGradient},
     {"shimmer", EffectType::Shimmer},
     {"ember", EffectType::Ember},
     {"aurora", EffectType::Aurora},
     {"sparkle", EffectType::Sparkle},
     {"enchant", EffectType::Enchant},
     {"frost", EffectType::Frost},
     {"breathe", EffectType::Breathe},
     {"drift", EffectType::Drift},
     {"mote", EffectType::Mote},
     {"wander", EffectType::Wander},
     {"eclipse", EffectType::Eclipse},
     {"pulse", EffectType::Pulse},
     {"electric", EffectType::Electric}},
}};

/**
 * @fn std::string Trim(const std::string& str)
 * @brief Remove surrounding spaces, tabs, carriage returns, and line feeds.
 * @author Alex (<https://github.com/lextpf>)
 */
static std::string Trim(const std::string& str);
/**
 * @fn std::string ToLowerAscii(std::string_view input)
 * @brief Fold key bytes with the current C locale.
 * @author Alex (<https://github.com/lextpf>)
 */
static std::string ToLowerAscii(std::string_view input);
/**
 * @fn float ParseFloat(const std::string& str, float defaultVal)
 * @brief Parse a floating-point prefix or use the supplied fallback.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Numeric prefixes are accepted: `2.5px` gives 2.5. No prefix or float overflow uses
 * `defaultVal`. Non-finite values accepted by `std::stof` pass through unchanged.
 */
static float ParseFloat(const std::string& str, float defaultVal);
/**
 * @fn int ParseInt(const std::string& str, int defaultVal)
 * @brief Parse a decimal integer prefix or use the supplied fallback.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Numeric prefixes are accepted: `10 plates` gives 10 and `1.9` gives 1.
 * No prefix or integer overflow uses `defaultVal`.
 */
static int ParseInt(const std::string& str, int defaultVal);
/**
 * @fn bool ParseBool(const std::string& str)
 * @brief Accept the case-insensitive true tokens used by INI settings.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Accept `true`, `1`, `yes`, `on`, and `enabled`. Every other value is false.
 */
static bool ParseBool(const std::string& str);
/**
 * @fn void ParseColor3(const std::string& str, Color3& out)
 * @brief Update up to three RGB channels from comma-separated text.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Missing channels retain their input values. Invalid present channels become 1.0;
 * ignore channels after the third. For example, `0.5,,0.2` sets green to 1.0.
 * Callers apply clamping.
 */
static void ParseColor3(const std::string& str, Color3& out);

std::shared_mutex& Mutex()
{
    static std::shared_mutex settingsMutex;
    return settingsMutex;
}

std::atomic<uint32_t>& Generation()
{
    static std::atomic<uint32_t> gen{0};
    return gen;
}

std::string& TitleFormat()
{
    static std::string s;
    return s;
}

std::vector<Segment>& DisplayFormat()
{
    static std::vector<Segment> v;
    return v;
}

std::vector<Segment>& InfoFormat()
{
    static std::vector<Segment> v;
    return v;
}

std::vector<TierDefinition>& Tiers()
{
    static std::vector<TierDefinition> v;
    return v;
}

std::vector<SpecialTitleDefinition>& SpecialTitles()
{
    static std::vector<SpecialTitleDefinition> v;
    return v;
}

std::vector<HonorificDefinition>& Honorifics()
{
    static std::vector<HonorificDefinition> v;
    return v;
}

std::vector<RegisterDefinition>& Registers()
{
    static std::vector<RegisterDefinition> v;
    return v;
}

RegisterSettings& RegisterConfig()
{
    static RegisterSettings s;
    return s;
}

DistanceSettings& Distance()
{
    static DistanceSettings s;
    return s;
}

OcclusionSettings& Occlusion()
{
    static OcclusionSettings s;
    return s;
}

ShadowOutlineSettings& ShadowOutline()
{
    static ShadowOutlineSettings s;
    return s;
}

GlowSettings& Glow()
{
    static GlowSettings s;
    return s;
}

ShineSettings& Shine()
{
    static ShineSettings s;
    return s;
}

TypewriterSettings& Typewriter()
{
    static TypewriterSettings s;
    return s;
}

OrnamentSettings& Ornament()
{
    static OrnamentSettings s;
    return s;
}

ParticleSettings& Particle()
{
    static ParticleSettings s;
    return s;
}

DisplaySettings& Display()
{
    static DisplaySettings s;
    return s;
}

DeckSettings& Deck()
{
    static DeckSettings s;
    return s;
}

AnimColorSettings& AnimColor()
{
    static AnimColorSettings s;
    return s;
}

FontSettings& Font()
{
    static FontSettings s;
    return s;
}

TransitionSettings& Transition()
{
    static TransitionSettings s;
    return s;
}

VisualSettings& Visual()
{
    static VisualSettings vs;
    return vs;
}

LabelSettings& Labels()
{
    static LabelSettings s;
    return s;
}

FocusSettings& Focus()
{
    static FocusSettings s;
    return s;
}

GraffitoSettings& Graffito()
{
    static GraffitoSettings s;
    return s;
}

IconSettings& Icons()
{
    static IconSettings s;
    return s;
}

NpcColorSettings& NpcColors()
{
    static NpcColorSettings s;
    return s;
}

QuietSettings& Quiet()
{
    static QuietSettings s;
    return s;
}

DeathRiteSettings& DeathRite()
{
    static DeathRiteSettings s;
    return s;
}

CompatSettings& Compat()
{
    static CompatSettings s;
    return s;
}

CandlelightSettings& Candlelight()
{
    static CandlelightSettings s;
    return s;
}

DepthClipSettings& DepthClipConfig()
{
    static DepthClipSettings s;
    return s;
}

// Font fallbacks when the manifest has no role entry.
static constexpr auto kDefaultNameFontPath =
    "Data/SKSE/Plugins/glyph/fonts/bd1aab18-7649-4946-9f7b-6ddd6a81311d.ttf";
static constexpr auto kDefaultLevelFontPath =
    "Data/SKSE/Plugins/glyph/fonts/96120cca-4be2-4d10-b10a-b8183ac18467.ttf";
static constexpr auto kDefaultTitleFontPath =
    "Data/SKSE/Plugins/glyph/fonts/56cb786e-c94e-452c-ac54-360c46381de1.ttf";
static constexpr auto kDefaultOrnamentFontPath =
    "Data/SKSE/Plugins/glyph/fonts/050986eb-c23a-4891-a951-9fed313e44c2.otf";

// clang-format off

// Scalar defaults and validation; duplicate keys or aliases keep the last row.
// Default variant types must match target pointees; see SettingEntry.
static const auto kSettings = std::to_array<SettingEntry>({
    {"FadeStartDistance",       "", &Distance().FadeStartDistance,     200.0f,   MinFloat{.0f}},
    {"FadeEndDistance",         "", &Distance().FadeEndDistance,       2500.0f,  MinFloat{.0f}},
    {"ScaleStartDistance",      "", &Distance().ScaleStartDistance,    200.0f,   MinFloat{.0f}},
    {"ScaleEndDistance",        "", &Distance().ScaleEndDistance,      2500.0f,  MinFloat{.0f}},
    {"MinimumScale",           "", &Distance().MinimumScale,          .1f,      ClampFloat{.01f, 5.0f}},
    {"MaxScanDistance",         "", &Distance().MaxScanDistance,       3000.0f,  MinFloat{.0f}},

    {"EnableOcclusionCulling",  "", &Occlusion().Enabled,              true,    NoClamping{}},
    {"OcclusionSettleTime",     "", &Occlusion().SettleTime,           .58f,    MinFloat{.01f}},
    {"OcclusionCheckInterval",  "", &Occlusion().CheckInterval,        3,       MinInt{1}},

    {"TitleShadowOffsetX",     "", &ShadowOutline().TitleShadowOffsetX,    2.0f,     NoClamping{}},
    {"TitleShadowOffsetY",     "", &ShadowOutline().TitleShadowOffsetY,    2.0f,     NoClamping{}},
    {"MainShadowOffsetX",      "", &ShadowOutline().MainShadowOffsetX,     4.0f,     NoClamping{}},
    {"MainShadowOffsetY",      "", &ShadowOutline().MainShadowOffsetY,     4.0f,     NoClamping{}},
    {"OutlineWidthMin",        "", &ShadowOutline().OutlineWidthMin,       2.0f,     MinFloat{.0f}},
    {"OutlineWidthMax",        "", &ShadowOutline().OutlineWidthMax,       2.5f,     MinFloat{.0f}},
    {"FastOutlines",           "", &ShadowOutline().FastOutlines,          false,    NoClamping{}},

    {"EnableOutlineGlow",      "", &ShadowOutline().OutlineGlowEnabled,    false,    NoClamping{}},
    {"OutlineGlowScale",       "", &ShadowOutline().OutlineGlowScale,      1.4f,     ClampFloat{1.0f, 4.0f}},
    {"OutlineGlowAlpha",       "", &ShadowOutline().OutlineGlowAlpha,      .1f,      ClampFloat{.0f, 1.0f}},
    {"OutlineGlowRings",       "", &ShadowOutline().OutlineGlowRings,      2,        ClampInt{1, 3}},
    {"OutlineGlowR",           "", &ShadowOutline().OutlineGlowR,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowG",           "", &ShadowOutline().OutlineGlowG,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowB",           "", &ShadowOutline().OutlineGlowB,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowTierTint",    "", &ShadowOutline().OutlineGlowTierTint,   false,    NoClamping{}},
    {"EnableDualOutline",      "", &ShadowOutline().DualOutlineEnabled,    false,    NoClamping{}},
    {"InnerOutlineTint",       "", &ShadowOutline().InnerOutlineTint,      .3f,      ClampFloat{.0f, 1.0f}},
    {"InnerOutlineAlpha",      "", &ShadowOutline().InnerOutlineAlpha,     .5f,      ClampFloat{.0f, 1.0f}},
    {"InnerOutlineScale",      "", &ShadowOutline().InnerOutlineScale,     .5f,      ClampFloat{.1f, .9f}},
    {"DirectionalLightAngle",  "", &ShadowOutline().DirectionalLightAngle, 315.f,    ClampFloat{.0f, 360.f}},
    {"DirectionalLightBias",   "", &ShadowOutline().DirectionalLightBias,  .15f,     ClampFloat{.0f, .5f}},

    {"OutlineColorTint",       "", &ShadowOutline().OutlineColorTint,      .0f,      ClampFloat{.0f, .25f}},
    {"ShadowColorTint",        "", &ShadowOutline().ShadowColorTint,       .0f,      ClampFloat{.0f, .25f}},

    {"EnableSoftShadow",       "", &ShadowOutline().SoftShadowEnabled,     false,    NoClamping{}},
    {"SoftShadowDistance",     "", &ShadowOutline().SoftShadowDistance,    4.0f,     ClampFloat{.0f, 16.0f}},
    {"SoftShadowSoftness",     "", &ShadowOutline().SoftShadowSoftness,    3.0f,     ClampFloat{.0f, 12.0f}},
    {"SoftShadowOpacity",      "", &ShadowOutline().SoftShadowOpacity,     .8f,      ClampFloat{.0f, 1.0f}},
    {"SoftShadowAngle",        "", &ShadowOutline().SoftShadowAngle,       45.0f,    ClampFloat{.0f, 360.0f}},
    {"SoftShadowSamples",      "", &ShadowOutline().SoftShadowSamples,     12,       ClampInt{4, 24}},

    {"EnableGlow",             "", &Glow().Enabled,                   false,    NoClamping{}},
    {"GlowRadius",             "", &Glow().Radius,                    4.0f,     MinFloat{.0f}},
    {"GlowIntensity",          "", &Glow().Intensity,                 .5f,      ClampFloat{.0f, 1.0f}},
    {"GlowSamples",            "", &Glow().Samples,                   8,        ClampInt{1, 64}},
    {"GlowDivideStrength",     "", &Glow().DivideStrength,            .0f,      ClampFloat{.0f, 1.0f}},

    {"EnableShine",            "", &Shine().Enabled,                  false,    NoClamping{}},
    {"ShineIntensity",         "", &Shine().Intensity,                .35f,     ClampFloat{.0f, 1.0f}},
    {"ShineFalloff",           "", &Shine().Falloff,                  2.0f,     ClampFloat{.5f, 8.0f}},
    {"TextGlowAlpha",          "", &Shine().TextGlowAlpha,            .0f,      ClampFloat{.0f, 1.0f}},

    {"EnableTypewriter",       "", &Typewriter().Enabled,             false,    NoClamping{}},
    {"TypewriterSpeed",        "", &Typewriter().Speed,               30.0f,    MinFloat{.0f}},
    {"TypewriterDelay",        "", &Typewriter().Delay,               .0f,      MinFloat{.0f}},

    {"EnableEntranceAnimation","", &Transition().EnableEntrance,      false,    NoClamping{}},
    {"EntranceStyle",          "", &Transition().EntranceStyle,       0,        ClampInt{0, 2}},
    {"EntranceDuration",       "", &Transition().EntranceDuration,    .35f,     ClampFloat{.05f, 3.0f}},
    {"EnableExitAnimation",    "", &Transition().EnableExit,          false,    NoClamping{}},
    {"ExitDuration",           "", &Transition().ExitDuration,        .20f,     ClampFloat{.05f, 2.0f}},
    {"EntranceStaggerStep",    "", &Transition().EntranceStaggerStep, .06f,     ClampFloat{.0f, .5f}},
    {"EntranceStaggerMax",     "", &Transition().EntranceStaggerMax,  .8f,      ClampFloat{.0f, 3.0f}},

    {"EnableDebugOverlay",     "", &Display().EnableDebugOverlay,     false,    NoClamping{}},

    {"EnableOrnaments",        "EnableFlourishes",   &Ornament().Enabled,      true,     NoClamping{}},
    {"OrnamentScale",          "FlourishScale",      &Ornament().Scale,        1.0f,     NoClamping{}},
    {"OrnamentSpacing",        "FlourishSpacing",    &Ornament().Spacing,      3.0f,     NoClamping{}},
    {"OrnamentAnchorToMainLine", "",                  &Ornament().AnchorToMainLine, true, NoClamping{}},
    {"OrnamentOffsetY",        "FlourishOffsetY",    &Ornament().OffsetY,      .0f,      NoClamping{}},

    {"EnableParticleAura",     "", &Particle().Enabled,               true,     NoClamping{}},
    {"UseParticleTextures",    "", &Particle().UseParticleTextures,   true,     NoClamping{}},
    {"ParticleCount",          "", &Particle().Count,                 8,        MinInt{0}},
    {"ParticleSize",           "", &Particle().Size,                  4.2f,     MinFloat{.0f}},
    {"ParticleSpeed",          "", &Particle().Speed,                 1.0f,     MinFloat{.0f}},
    {"ParticleSpread",         "", &Particle().Spread,                20.0f,    MinFloat{.0f}},
    {"ParticleAlpha",          "", &Particle().Alpha,                 .8f,      ClampFloat{.0f, 1.0f}},
    {"ParticleBlendMode",      "", &Particle().BlendMode,             1,        ClampInt{0, 2}},
    {"ParticleDepthStrength",  "", &Particle().DepthStrength,         .7f,      ClampFloat{.0f, 1.5f}},
    {"ParticleColorWarmth",    "", &Particle().ColorWarmth,           .5f,      ClampFloat{.0f, 1.0f}},
    {"ParticleGlowStrength",   "", &Particle().GlowStrength,          .28f,     ClampFloat{.0f, 1.0f}},
    {"ParticleGlowSize",       "", &Particle().GlowSize,              2.2f,     ClampFloat{1.0f, 4.0f}},
    {"ParticleShineThreshold", "", &Particle().ShineThreshold,        .84f,     ClampFloat{.0f, .99f}},

    {"VerticalOffset",         "", &Display().VerticalOffset,         8.0f,     NoClamping{}},
    {"HorizontalOffset",       "", &Display().HorizontalOffset,      -10.0f,    ClampFloat{-200.0f, 200.0f}},
    {"HidePlayer",             "", &Display().HidePlayer,             false,    NoClamping{}},
    {"HideCreatures",          "", &Display().HideCreatures,          false,    NoClamping{}},
    {"MaxPlates",              "", &Display().MaxPlates,              RenderConstants::DEFAULT_MAX_PLATES,
                                                                              ClampInt{RenderConstants::MIN_PLATES,
                                                                                       RenderConstants::MAX_PLATES}},
    {"MaxScanActors",          "", &Display().MaxScanActors,          RenderConstants::DEFAULT_MAX_SCAN_ACTORS,
                                                                              ClampInt{RenderConstants::MIN_SCAN_ACTORS,
                                                                                       RenderConstants::MAX_SCAN_ACTORS}},
    {"ReloadKey",              "", &Display().ReloadKey,              0,        NoClamping{}},

    {"DeckEnabled",            "", &Deck().Enabled,                    true,     NoClamping{}},
    {"DeckKey",                "", &Deck().Key,                        119,      NoClamping{}},
    {"DeckOutputFolder",       "", &Deck().OutputFolder,               std::string("Data/SKSE/Plugins/glyph/cards"), NoClamping{}},
    {"DeckCardWidth",          "", &Deck().CardWidth,                  750,      ClampInt{384, 2048}},
    {"DeckCardHeight",         "", &Deck().CardHeight,                 1050,     ClampInt{538, 2867}},
    {"DeckTargetRadius",       "", &Deck().TargetRadius,               220,      ClampInt{32, 1000}},
    {"DeckPlayerFallback",     "", &Deck().PlayerFallback,             true,     NoClamping{}},
    {"DeckRarityRolls",        "", &Deck().RarityRolls,                true,     NoClamping{}},

    {"AlphaSettleTime",        "", &AnimColor().AlphaSettleTime,      .46f,     MinFloat{.01f}},
    {"ScaleSettleTime",        "", &AnimColor().ScaleSettleTime,      .46f,     MinFloat{.01f}},
    {"PositionSettleTime",     "", &AnimColor().PositionSettleTime,   .38f,     MinFloat{.01f}},
    {"InnerTextAlpha",         "", &AnimColor().InnerTextAlpha,        1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineAlpha",           "", &AnimColor().OutlineAlpha,          1.0f,     ClampFloat{.0f, 1.0f}},

    {"EnableDistanceOutlineScale", "", &Visual().EnableDistanceOutlineScale, false, NoClamping{}},
    {"OutlineDistanceMin",     "", &Visual().OutlineDistanceMin,   .8f,     NoClamping{}},
    {"OutlineDistanceMax",     "", &Visual().OutlineDistanceMax,   1.5f,    NoClamping{}},
    {"MinimumPixelHeight",     "", &Visual().MinimumPixelHeight,   .0f,     NoClamping{}},
    {"EnableLOD",              "", &Visual().EnableLOD,            false,   NoClamping{}},
    {"LODFarDistance",         "", &Visual().LODFarDistance,       1800.0f, NoClamping{}},
    {"LODMidDistance",         "", &Visual().LODMidDistance,       800.0f,  NoClamping{}},
    {"LODTransitionRange",     "", &Visual().LODTransitionRange,  200.0f,  MinFloat{1.0f}},
    {"TitleAlphaMultiplier",   "", &Visual().TitleAlphaMultiplier, .80f,   NoClamping{}},
    {"LevelAlphaMultiplier",   "", &Visual().LevelAlphaMultiplier, .85f,   NoClamping{}},
    {"EnableOverlapPrevention","", &Visual().EnableOverlapPrevention, false, NoClamping{}},
    {"OverlapPaddingY",        "", &Visual().OverlapPaddingY,     4.0f,    NoClamping{}},
    {"OverlapIterations",      "", &Visual().OverlapIterations,   3,       ClampInt{1, 16}},
    {"PositionSmoothingBlend", "", &Visual().PositionSmoothingBlend, 1.0f, ClampFloat{.0f, 1.0f}},
    {"LargeMovementThreshold", "", &Visual().LargeMovementThreshold, 50.0f, MinFloat{.0f}},
    {"LargeMovementBlend",     "", &Visual().LargeMovementBlend,  .5f,     ClampFloat{.0f, 1.0f}},
    {"EnableMotionTrail",      "", &Visual().EnableMotionTrail,      false,   NoClamping{}},
    {"TrailLength",            "", &Visual().TrailLength,             4,       ClampInt{1, 8}},
    {"TrailAlpha",             "", &Visual().TrailAlpha,              .3f,     ClampFloat{.0f, 1.0f}},
    {"TrailFalloff",           "", &Visual().TrailFalloff,            2.0f,    ClampFloat{.5f, 5.0f}},
    {"TrailMinDistance",        "", &Visual().TrailMinDistance,        2.0f,    MinFloat{.0f}},
    {"TrailMinTier",           "", &Visual().TrailMinTier,            0,       MinInt{0}},

    {"EnableWave",             "", &Visual().EnableWave,             false,   NoClamping{}},
    {"WaveAmplitude",          "", &Visual().WaveAmplitude,          1.5f,    ClampFloat{.0f, 10.0f}},
    {"WaveFrequency",          "", &Visual().WaveFrequency,          3.0f,    ClampFloat{.5f, 20.0f}},
    {"WaveSpeed",              "", &Visual().WaveSpeed,              1.0f,    ClampFloat{.0f, 10.0f}},
    {"WaveMinTier",            "", &Visual().WaveMinTier,            0,       MinInt{0}},

    {"EnableTierEffectGating", "", &Visual().EnableTierEffectGating, false, NoClamping{}},
    {"GlowMinTier",            "", &Visual().GlowMinTier,         5,       NoClamping{}},
    {"ParticleMinTier",        "", &Visual().ParticleMinTier,     10,      NoClamping{}},
    {"OrnamentMinTier",        "", &Visual().OrnamentMinTier,     10,      NoClamping{}},

    {"NameFontPath",           "", &Font().NameFontPath,           std::string(kDefaultNameFontPath),    NoClamping{}},
    {"NameFontSize",           "", &Font().NameFontSize,           122.0f,   NoClamping{}},
    {"LevelFontPath",          "", &Font().LevelFontPath,          std::string(kDefaultLevelFontPath),   NoClamping{}},
    {"LevelFontSize",          "", &Font().LevelFontSize,          61.0f,    NoClamping{}},
    {"TitleFontPath",          "", &Font().TitleFontPath,          std::string(kDefaultTitleFontPath),   NoClamping{}},
    {"TitleFontSize",          "", &Font().TitleFontSize,          42.0f,    NoClamping{}},
    {"OrnamentFontPath",       "", &Ornament().FontPath,           std::string(kDefaultOrnamentFontPath), NoClamping{}},
    {"OrnamentFontSize",       "", &Ornament().FontSize,           64.0f,    NoClamping{}},

    // Empty label defaults pair with a trailing ? To drop blank Format/InfoFormat segments.
    {"RelationshipFollower",   "", &Labels().RelationshipFollower,  std::string("Follower"), NoClamping{}},
    {"RelationshipAlly",       "", &Labels().RelationshipAlly,      std::string("Ally"),     NoClamping{}},
    {"RelationshipNeutral",    "", &Labels().RelationshipNeutral,   std::string(),           NoClamping{}},
    {"RelationshipHostile",    "", &Labels().RelationshipHostile,   std::string("Hostile"),  NoClamping{}},
    {"LevelDeltaWeak",         "", &Labels().LevelDeltaWeak,        std::string("Weak"),     NoClamping{}},
    {"LevelDeltaEven",         "", &Labels().LevelDeltaEven,        std::string(),           NoClamping{}},
    {"LevelDeltaStrong",       "", &Labels().LevelDeltaStrong,      std::string("Strong"),   NoClamping{}},
    {"LevelDeltaDeadly",       "", &Labels().LevelDeltaDeadly,      std::string("Deadly"),   NoClamping{}},
    {"CreatureTypeNPC",        "", &Labels().CreatureTypeNPC,       std::string(),           NoClamping{}},
    {"CreatureTypeBeast",      "", &Labels().CreatureTypeBeast,     std::string("Beast"),    NoClamping{}},
    {"CreatureTypeUndead",     "", &Labels().CreatureTypeUndead,    std::string("Undead"),   NoClamping{}},
    {"CreatureTypeDaedra",     "", &Labels().CreatureTypeDaedra,    std::string("Daedra"),   NoClamping{}},
    {"CreatureTypeDragon",     "", &Labels().CreatureTypeDragon,    std::string("Dragon"),   NoClamping{}},

    {"WeakAtOrBelow",          "", &Labels().WeakAtOrBelow,         -5,                      NoClamping{}},
    {"StrongAtOrAbove",        "", &Labels().StrongAtOrAbove,        5,                      NoClamping{}},
    {"DeadlyAtOrAbove",        "", &Labels().DeadlyAtOrAbove,       10,                      NoClamping{}},

    {"FocusEnabled",           "", &Focus().Enabled,                 false,                  NoClamping{}},
    {"FocusConeAngleDegrees",  "", &Focus().ConeAngleDegrees,        8.0f,                   ClampFloat{.5f, 45.0f}},
    {"FocusMaxDistance",       "", &Focus().MaxDistance,             .0f,                    MinFloat{.0f}},
    {"FocusAmbientDimFactor",  "", &Focus().AmbientDimFactor,        .55f,                   ClampFloat{.05f, 1.0f}},
    {"FocusSettleTime",        "", &Focus().SettleTime,              .25f,                   ClampFloat{.0f, 2.0f}},
    {"FocusIgnoreOccluded",    "", &Focus().IgnoreOccluded,          true,                   NoClamping{}},

    {"GraffitoEnabled",               "", &Graffito().Enabled,               false,   NoClamping{}},
    {"GraffitoScale",                 "", &Graffito().Scale,                 1.0f,    ClampFloat{.25f, 4.0f}},
    {"GraffitoPlayerScale",           "", &Graffito().PlayerScale,           .72f,    ClampFloat{.25f, 2.0f}},
    {"GraffitoForwardOffset",         "", &Graffito().ForwardOffset,         4.0f,    ClampFloat{.0f, 32.0f}},
    {"GraffitoFacingFadeDegrees",     "", &Graffito().FacingFadeDegrees,     15.0f,   ClampFloat{.0f, 45.0f}},
    {"GraffitoBacksideBleedAlpha",    "", &Graffito().BacksideBleedAlpha,    .12f,    ClampFloat{.0f, .25f}},
    {"GraffitoEdgeSeamAlpha",         "", &Graffito().EdgeSeamAlpha,         .22f,    ClampFloat{.0f, .4f}},
    {"GraffitoFolioEnabled",           "", &Graffito().FolioEnabled,           true,    NoClamping{}},
    {"GraffitoFolioReverseAlpha",      "", &Graffito().FolioReverseAlpha,      .72f,    ClampFloat{.0f, 1.0f}},
    {"GraffitoFolioSpineAlpha",        "", &Graffito().FolioSpineAlpha,        .88f,    ClampFloat{.0f, 1.0f}},
    {"GraffitoFolioDepth",             "", &Graffito().FolioDepth,             2.0f,    ClampFloat{.0f, 28.0f}},
    {"GraffitoWrapDegrees",            "", &Graffito().WrapDegrees,            55.0f,   ClampFloat{.0f, 140.0f}},
    {"GraffitoFisheyeStrength",        "", &Graffito().FisheyeStrength,        .62f,    ClampFloat{.0f, 1.0f}},
    {"GraffitoLayerDepth",             "", &Graffito().LayerDepth,             .0f,     ClampFloat{.0f, .06f}},
    {"GraffitoEdgeSheen",              "", &Graffito().EdgeSheen,              .14f,    ClampFloat{.0f, .4f}},
    {"GraffitoOrientationSettleTime", "", &Graffito().OrientationSettleTime, .18f,    ClampFloat{.0f, 2.0f}},
    {"GraffitoMaxDistance",           "", &Graffito().MaxDistance,           1200.0f, MinFloat{.0f}},
    {"GraffitoFallenEpitaphEnabled",  "", &Graffito().FallenEpitaphEnabled,  true,    NoClamping{}},
    {"GraffitoEpitaphGroundLift",     "", &Graffito().EpitaphGroundLift,     2.0f,    ClampFloat{.0f, 16.0f}},

    {"IconFolder",             "", &Icons().Folder,           std::string("Data/SKSE/Plugins/glyph/duotone"), NoClamping{}},
    {"IconsEnabled",           "", &Icons().Enabled,          true,                             NoClamping{}},
    {"IconScale",              "", &Icons().Scale,            1.0f,                             ClampFloat{.5f, 2.0f}},
    {"IconDeadlyPulse",        "", &Icons().DeadlyPulse,      true,                             NoClamping{}},
    {"IconFollower",           "", &Icons().FollowerIcon,     std::string("shield-halved"),     NoClamping{}},
    {"IconAlly",               "", &Icons().AllyIcon,         std::string("handshake"),         NoClamping{}},
    {"IconHostile",            "", &Icons().HostileIcon,      std::string("skull-crossbones"),  NoClamping{}},
    {"IconWeak",               "", &Icons().WeakIcon,         std::string("caret-down"),        NoClamping{}},
    {"IconStrong",             "", &Icons().StrongIcon,       std::string("caret-up"),          NoClamping{}},
    {"IconDeadly",             "", &Icons().DeadlyIcon,       std::string("skull"),             NoClamping{}},
    {"IconBeast",              "", &Icons().BeastIcon,        std::string("paw"),               NoClamping{}},
    {"IconUndead",             "", &Icons().UndeadIcon,       std::string("ghost"),             NoClamping{}},
    {"IconDaedra",             "", &Icons().DaedraIcon,       std::string("fire"),              NoClamping{}},
    {"IconDragon",             "", &Icons().DragonIcon,       std::string("dragon"),            NoClamping{}},
    {"IconFollowerColor",      "", &Icons().FollowerColorStr, std::string("0.46, 0.68, 0.84"),  NoClamping{}},
    {"IconAllyColor",          "", &Icons().AllyColorStr,     std::string("0.52, 0.74, 0.50"),  NoClamping{}},
    {"IconHostileColor",       "", &Icons().HostileColorStr,  std::string("0.86, 0.36, 0.32"),  NoClamping{}},
    {"IconWeakColor",          "", &Icons().WeakColorStr,     std::string("0.54, 0.66, 0.80"),  NoClamping{}},
    {"IconStrongColor",        "", &Icons().StrongColorStr,   std::string("0.86, 0.62, 0.32"),  NoClamping{}},
    {"IconDeadlyColor",        "", &Icons().DeadlyColorStr,   std::string("0.90, 0.28, 0.24"),  NoClamping{}},
    {"IconCreatureColor",      "", &Icons().CreatureColorStr, std::string("0.80, 0.74, 0.62"),  NoClamping{}},

    {"IconNeutral",            "", &Icons().NeutralIcon,       std::string("circle"),            NoClamping{}},
    {"IconHumanoid",           "", &Icons().HumanoidIcon,      std::string("user"),              NoClamping{}},
    {"IconEven",               "", &Icons().EvenIcon,          std::string("equals"),            NoClamping{}},
    {"IconGuard",              "", &Icons().GuardIcon,         std::string("helmet-battle"),     NoClamping{}},
    {"IconMerchant",           "", &Icons().MerchantIcon,      std::string("coins"),             NoClamping{}},
    {"IconCommoner",           "", &Icons().CommonerIcon,      std::string("house"),             NoClamping{}},
    {"IconEssential",          "", &Icons().EssentialIcon,     std::string("certificate"),       NoClamping{}},
    {"IconProtected",          "", &Icons().ProtectedIcon,     std::string("shield-check"),      NoClamping{}},
    {"IconMortal",             "", &Icons().MortalIcon,        std::string("heart"),             NoClamping{}},
    {"IconCombat",             "", &Icons().CombatIcon,        std::string("swords"),            NoClamping{}},
    {"IconAlert",              "", &Icons().AlertIcon,         std::string("eye"),               NoClamping{}},
    {"IconIdle",               "", &Icons().IdleIcon,          std::string("moon"),              NoClamping{}},
    {"IconSneakHidden",        "", &Icons().SneakHiddenIcon,   std::string("eye-slash"),         NoClamping{}},
    {"IconSneakDetected",      "", &Icons().SneakDetectedIcon, std::string("eye"),               NoClamping{}},
    {"IconSneakOff",           "", &Icons().SneakOffIcon,      std::string("person-walking"),    NoClamping{}},
    {"IconEncumbered",         "", &Icons().EncumberedIcon,    std::string("weight-hanging"),    NoClamping{}},
    {"IconNormalWeight",       "", &Icons().NormalWeightIcon,  std::string("feather"),           NoClamping{}},
    {"IconWanted",             "", &Icons().WantedIcon,        std::string("gavel"),             NoClamping{}},
    {"IconBountyClear",        "", &Icons().BountyClearIcon,   std::string("scale-balanced"),    NoClamping{}},
    {"IconTierLow",            "", &Icons().TierLowIcon,       std::string("medal"),             NoClamping{}},
    {"IconTierMid",            "", &Icons().TierMidIcon,       std::string("gem"),               NoClamping{}},
    {"IconTierHigh",           "", &Icons().TierHighIcon,      std::string("crown"),             NoClamping{}},
    {"TierBadgeImages",        "", &Icons().TierBadgeImages,   true,                             NoClamping{}},
    {"TierBadgeFolder",        "", &Icons().TierBadgeFolder,   std::string("Data/SKSE/Plugins/glyph/badges"), NoClamping{}},
    {"TierBadgeGamma",         "", &Icons().TierBadgeGamma,    1.0f,                             ClampFloat{.5f, 4.0f}},
    {"TierBadgeScale",         "", &Icons().TierBadgeScale,    1.7f,                             ClampFloat{1.0f, 4.0f}},

    {"PlayerStripBedEnabled",   "", &Icons().PlayerStripBedEnabled,   true,             NoClamping{}},
    {"PlayerStripBedAlpha",     "", &Icons().PlayerStripBedAlpha,     0.10f,            ClampFloat{0.0f, 0.2f}},
    {"PlayerStripBedSize",      "", &Icons().PlayerStripBedSize,      2.6f,             ClampFloat{1.8f, 4.0f}},
    {"PlayerStripBedBreatheHz", "", &Icons().PlayerStripBedBreatheHz, 0.14f,            ClampFloat{0.0f, 2.0f}},
    {"PlayerStripBedColor",     "", &Icons().PlayerStripBedColorStr,  std::string(""),  NoClamping{}},
    {"EmblemBacklightEnabled",  "", &Icons().EmblemBacklightEnabled,  true,             NoClamping{}},
    {"EmblemBacklightSize",     "", &Icons().EmblemBacklightSize,     2.6f,             ClampFloat{1.8f, 4.0f}},
    {"EmblemBacklightAlpha",    "", &Icons().EmblemBacklightAlpha,    0.55f,            ClampFloat{0.0f, 1.0f}},
    {"EmblemBacklightBreatheHz","", &Icons().EmblemBacklightBreatheHz,0.167f,           ClampFloat{0.0f, 2.0f}},
    {"EmblemCrispAlpha",        "", &Icons().EmblemCrispAlpha,        0.95f,            ClampFloat{0.5f, 1.0f}},
    {"EmblemBacklightColor",    "", &Icons().EmblemBacklightColorStr, std::string(""),  NoClamping{}},

    {"PlayerRimLightEnabled",   "", &Icons().PlayerRimLightEnabled,  true,             NoClamping{}},
    {"PlayerRimAlpha",          "", &Icons().PlayerRimAlpha,         0.22f,            ClampFloat{0.0f, 0.6f}},
    {"PlayerCarveAlpha",        "", &Icons().PlayerCarveAlpha,       0.26f,            ClampFloat{0.0f, 0.6f}},
    {"PlayerRimOffset",         "", &Icons().PlayerRimOffset,        1.0f,             ClampFloat{0.0f, 3.0f}},
    {"PlayerRimColor",          "", &Icons().PlayerRimColorStr,      std::string(""),  NoClamping{}},
    {"EmblemKeyFillEnabled",    "", &Icons().EmblemKeyFillEnabled,   true,             NoClamping{}},
    {"EmblemKeyAlpha",          "", &Icons().EmblemKeyAlpha,         0.35f,            ClampFloat{0.0f, 1.0f}},
    {"EmblemFillAlpha",         "", &Icons().EmblemFillAlpha,        0.15f,            ClampFloat{0.0f, 1.0f}},
    {"EmblemKeyRise",           "", &Icons().EmblemKeyRise,          0.18f,            ClampFloat{0.0f, 0.6f}},
    {"EmblemFillDrop",          "", &Icons().EmblemFillDrop,         0.15f,            ClampFloat{0.0f, 0.6f}},
    {"EmblemKeyColor",          "", &Icons().EmblemKeyColorStr,      std::string(""),  NoClamping{}},
    {"EmblemFillColor",         "", &Icons().EmblemFillColorStr,     std::string(""),  NoClamping{}},

    {"IconGuardColor",         "", &Icons().GuardColorStr,         std::string("0.60, 0.68, 0.84"),  NoClamping{}},
    {"IconMerchantColor",      "", &Icons().MerchantColorStr,      std::string("0.84, 0.74, 0.42"),  NoClamping{}},
    {"IconEssentialColor",     "", &Icons().EssentialColorStr,     std::string("0.86, 0.78, 0.46"),  NoClamping{}},
    {"IconProtectedColor",     "", &Icons().ProtectedColorStr,     std::string("0.54, 0.72, 0.86"),  NoClamping{}},
    {"IconCombatColor",        "", &Icons().CombatColorStr,        std::string("0.88, 0.42, 0.30"),  NoClamping{}},
    {"IconAlertColor",         "", &Icons().AlertColorStr,         std::string("0.86, 0.76, 0.40"),  NoClamping{}},
    {"IconSneakHiddenColor",   "", &Icons().SneakHiddenColorStr,   std::string("0.50, 0.64, 0.84"),  NoClamping{}},
    {"IconSneakDetectedColor", "", &Icons().SneakDetectedColorStr, std::string("0.86, 0.36, 0.32"),  NoClamping{}},
    {"IconEncumberedColor",    "", &Icons().EncumberedColorStr,    std::string("0.82, 0.64, 0.40"),  NoClamping{}},
    {"IconWantedColor",        "", &Icons().WantedColorStr,        std::string("0.84, 0.34, 0.30"),  NoClamping{}},
    {"IconTierLowColor",       "", &Icons().TierLowColorStr,       std::string("0.70, 0.62, 0.52"),  NoClamping{}},
    {"IconTierMidColor",       "", &Icons().TierMidColorStr,       std::string("0.62, 0.70, 0.80"),  NoClamping{}},
    {"IconTierHighColor",      "", &Icons().TierHighColorStr,      std::string("0.86, 0.74, 0.46"),  NoClamping{}},
    {"IconNeutralColor",       "", &Icons().NeutralColorStr,      std::string("0.56, 0.62, 0.70"),  NoClamping{}},
    {"IconHumanoidColor",      "", &Icons().HumanoidColorStr,     std::string("0.74, 0.68, 0.58"),  NoClamping{}},
    {"IconCommonerColor",      "", &Icons().CommonerColorStr,     std::string("0.60, 0.68, 0.54"),  NoClamping{}},
    {"IconMortalColor",        "", &Icons().MortalColorStr,       std::string("0.76, 0.58, 0.60"),  NoClamping{}},
    {"IconEvenColor",          "", &Icons().EvenColorStr,         std::string("0.60, 0.70, 0.72"),  NoClamping{}},
    {"IconIdleColor",          "", &Icons().IdleColorStr,         std::string("0.56, 0.60, 0.76"),  NoClamping{}},
    {"IconSneakOffColor",      "", &Icons().SneakOffColorStr,     std::string("0.64, 0.68, 0.60"),  NoClamping{}},
    {"IconNormalWeightColor",  "", &Icons().NormalWeightColorStr, std::string("0.64, 0.76, 0.70"),  NoClamping{}},
    {"IconBountyClearColor",   "", &Icons().BountyClearColorStr,  std::string("0.50, 0.70, 0.68"),  NoClamping{}},
    {"IconMutedColor",         "", &Icons().MutedColorStr,         std::string("0.62, 0.64, 0.68"),  NoClamping{}},

    {"IconRelationshipEnabled","", &Icons().RelationshipEnabled, true,                            NoClamping{}},
    {"IconCreatureEnabled",    "", &Icons().CreatureEnabled,     true,                            NoClamping{}},
    {"IconThreatEnabled",      "", &Icons().ThreatEnabled,       true,                            NoClamping{}},
    {"IconRoleEnabled",        "", &Icons().RoleEnabled,         true,                            NoClamping{}},
    {"IconProtectionEnabled",  "", &Icons().ProtectionEnabled,   true,                            NoClamping{}},
    {"IconEngagementEnabled",  "", &Icons().EngagementEnabled,   true,                            NoClamping{}},
    {"IconCombatStateEnabled", "", &Icons().CombatStateEnabled,  true,                            NoClamping{}},
    {"IconAlertStateEnabled",  "", &Icons().AlertStateEnabled,   true,                            NoClamping{}},
    {"IconSneakEnabled",       "", &Icons().SneakEnabled,        true,                            NoClamping{}},
    {"IconPlayerCombatEnabled","", &Icons().PlayerCombatEnabled, true,                            NoClamping{}},
    {"IconEncumberedEnabled",  "", &Icons().EncumberedEnabled,   true,                            NoClamping{}},
    {"IconBountyEnabled",      "", &Icons().BountyEnabled,       true,                            NoClamping{}},
    {"IconTierEnabled",        "", &Icons().TierEnabled,         true,                            NoClamping{}},

    {"IconMutedAlpha",         "", &Icons().MutedAlpha,          1.0f,             ClampFloat{.0f, 1.0f}},
    {"IconMutedDesat",         "", &Icons().MutedDesat,          0.18f,            ClampFloat{.0f, 1.0f}},
    {"IconOpacity",            "", &Icons().Opacity,             0.92f,            ClampFloat{.5f, 2.0f}},

    {"DeathRiteEnabled",       "", &DeathRite().Enabled,          true,     NoClamping{}},
    {"DeathRiteDuration",      "", &DeathRite().Duration,         1.6f,     ClampFloat{.4f, 4.0f}},

    {"CompatYieldToTrueHUD",     "", &Compat().YieldToTrueHUD,     true,   NoClamping{}},
    {"CompatTrueHUDYieldAlpha",  "", &Compat().TrueHUDYieldAlpha,  .0f,    ClampFloat{.0f, 1.0f}},
    {"CompatYieldLevelToMoreHUD","", &Compat().YieldLevelToMoreHUD,true,   NoClamping{}},
    {"CompatYieldSettleTime",    "", &Compat().YieldSettleTime,    .3f,    ClampFloat{.01f, 2.0f}},

    {"RegistersEnabled",         "", &RegisterConfig().Enabled,          true,   NoClamping{}},
    {"RegisterTransitionTime",   "", &RegisterConfig().TransitionTime,   1.2f,   ClampFloat{.05f, 5.0f}},
    {"RegisterCrowdedThreshold", "", &RegisterConfig().CrowdedThreshold, 12,     MinInt{2}},

    {"DepthClipEnabled",       "", &DepthClipConfig().Enabled,    true,     NoClamping{}},
    {"DepthClipFeather",       "", &DepthClipConfig().Feather,    2.5f,     ClampFloat{.0f, 8.0f}},

    {"CandlelightEnabled",     "", &Candlelight().Enabled,        true,     NoClamping{}},
    {"CandlelightStrength",    "", &Candlelight().Strength,       .08f,     ClampFloat{.0f, .15f}},
    {"CandlelightWarmth",      "", &Candlelight().Warmth,         .5f,      ClampFloat{.0f, 1.0f}},
    {"CandlelightSettleTime",  "", &Candlelight().SettleTime,     .6f,      ClampFloat{.05f, 3.0f}},

    {"QuietFrameEnabled",      "", &Quiet().Enabled,              true,     NoClamping{}},
    {"QuietPanThresholdLo",    "", &Quiet().PanThresholdLo,       40.0f,    ClampFloat{1.0f, 720.0f}},
    {"QuietPanThresholdHi",    "", &Quiet().PanThresholdHi,       160.0f,   ClampFloat{2.0f, 1440.0f}},
    {"QuietAttackTime",        "", &Quiet().AttackTime,           .10f,     ClampFloat{.01f, 1.0f}},
    {"QuietNameReleaseTime",   "", &Quiet().NameReleaseTime,      .28f,     ClampFloat{.01f, 2.0f}},
    {"QuietSubReleaseTime",    "", &Quiet().SubReleaseTime,       .50f,     ClampFloat{.01f, 3.0f}},
    {"QuietNameFloor",         "", &Quiet().NameFloor,            .35f,     ClampFloat{.0f, 1.0f}},

    {"NpcNeutralColor",        "", &NpcColors().NeutralColorStr,  std::string("1.0, 1.0, 1.0"),    NoClamping{}},
    {"NpcHostileColor",        "", &NpcColors().HostileColorStr,  std::string("1.0, 0.86, 0.84"),  NoClamping{}},
    {"NpcFollowerColor",       "", &NpcColors().FollowerColorStr, std::string("0.86, 0.91, 1.0"),  NoClamping{}},
    {"NpcLevelColor",          "", &NpcColors().LevelColorStr,    std::string("0.80, 0.82, 0.86"), NoClamping{}},
    {"NpcTitleColor",          "", &NpcColors().TitleColorStr,    std::string("0.92, 0.93, 0.95"), NoClamping{}},
});

// clang-format on

/**
 * @fn const std::unordered_map<std::string, const SettingEntry*>& GetKeyMap()
 * @brief Index scalar keys and aliases for case-insensitive lookup.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Built once; accessor targets remain stable across reloads. Raw lowercase keys bypass
 * CanonicalizeStructKey, so indexed-field spelling rules cannot rename scalars.
 */
static const std::unordered_map<std::string, const SettingEntry*>& GetKeyMap()
{
    static const auto map = []
    {
        std::unordered_map<std::string, const SettingEntry*> m;
        m.reserve(kSettings.size() * 2);
        for (const auto& s : kSettings)
        {
            m[ToLowerAscii(s.key)] = &s;
            if (!s.alias.empty())
            {
                m[ToLowerAscii(s.alias)] = &s;
            }
        }
        return m;
    }();
    return map;
}

/**
 * @fn void ApplySettingValue(const SettingEntry& entry, const std::string& val)
 * @brief Write a scalar value through its typed table binding.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Invalid numeric text becomes zero before validation; strings retain surrounding quotes.
 */
static void ApplySettingValue(const SettingEntry& entry, const std::string& val)
{
    std::visit(
        overloaded{
            [&](float* p) { *p = ParseFloat(val, .0f); },
            [&](bool* p) { *p = ParseBool(val); },
            [&](int* p) { *p = ParseInt(val, 0); },
            [&](std::string* p) { *p = val; },
        },
        entry.target);
}

/**
 * @fn void ResetTableDefaults()
 * @brief Restore each scalar target from its matching default variant.
 * @author Alex (<https://github.com/lextpf>)
 */
static void ResetTableDefaults()
{
    for (const auto& s : kSettings)
    {
        std::visit(
            [&](auto* ptr)
            {
                using T = std::remove_pointer_t<decltype(ptr)>;
                *ptr = std::get<T>(s.defaultValue);
            },
            s.target);
    }
}

/**
 * @fn void ValidateTableSettings()
 * @brief Apply each numeric validation rule to its matching target type.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Ignore mismatched rules and bool/string targets without diagnostics.
 */
static void ValidateTableSettings()
{
    for (const auto& s : kSettings)
    {
        std::visit(
            overloaded{
                [&](float* p)
                {
                    std::visit(
                        overloaded{
                            [p](ClampFloat c) { *p = std::clamp(*p, c.lo, c.hi); },
                            [p](MinFloat c) { *p = std::max(c.lo, *p); },
                            [](auto) {},
                        },
                        s.validation);
                },
                [&](int* p)
                {
                    std::visit(
                        overloaded{
                            [p](ClampInt c) { *p = std::clamp(*p, c.lo, c.hi); },
                            [p](MinInt c) { *p = std::max(c.lo, *p); },
                            [](auto) {},
                        },
                        s.validation);
                },
                [](auto*) {},
            },
            s.target);
    }
}

/**
 * @fn TierDefinition MakeDefaultTier()
 * @brief Build the fallback style used for missing tiers.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Default and gap tiers cover levels 1-250; first-match lookup can shadow later tiers.
 */
static TierDefinition MakeDefaultTier()
{
    TierDefinition tier{};
    tier.minLevel = 1;
    tier.maxLevel = 250;
    tier.title = "Unknown";
    tier.leftColor = Color3::White();
    tier.rightColor = Color3::White();
    tier.highlightColor = Color3::White();
    tier.titleEffect.type = EffectType::Gradient;
    tier.nameEffect.type = EffectType::Gradient;
    tier.levelEffect.type = EffectType::Gradient;
    tier.leftOrnaments.clear();
    tier.rightOrnaments.clear();
    tier.particleTypes.clear();
    tier.particleCount = 0;
    tier.badgeIndex = 0;
    return tier;
}

static std::string ToLowerAscii(std::string_view input)
{
    std::string out(input);
    for (auto& c : out)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/**
 * @fn std::string CanonicalizeStructKey(const std::string& rawKey)
 * @brief Normalize known indexed-field names and format keys.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Canonicalize indexed fields and Format; title aliases Name. Unmapped names retain case,
 * so the optional tier color keys omitted here remain case-sensitive.
 */
static std::string CanonicalizeStructKey(const std::string& rawKey)
{
    static const std::unordered_map<std::string, std::string> kStructKeys = {
        {"name", "Name"},
        {"title", "Name"},
        {"minlevel", "MinLevel"},
        {"maxlevel", "MaxLevel"},
        {"leftcolor", "LeftColor"},
        {"rightcolor", "RightColor"},
        {"highlightcolor", "HighlightColor"},
        {"titleeffect", "TitleEffect"},
        {"nameeffect", "NameEffect"},
        {"leveleffect", "LevelEffect"},
        {"ornaments", "Ornaments"},
        {"particletypes", "ParticleTypes"},
        {"particlecount", "ParticleCount"},
        {"badge", "Badge"},
        {"ornamentleftcolor", "OrnamentLeftColor"},
        {"ornamentrightcolor", "OrnamentRightColor"},
        {"keyword", "Keyword"},
        {"displaytitle", "DisplayTitle"},
        {"color", "Color"},
        {"glowcolor", "GlowColor"},
        {"forceornaments", "ForceOrnaments"},
        {"forceflourishes", "ForceFlourishes"},
        {"forceparticles", "ForceParticles"},
        {"priority", "Priority"},
        {"format", "Format"},
        {"faction", "Faction"},
        {"minrank", "MinRank"},
        {"playeronly", "PlayerOnly"},
        {"npconly", "NpcOnly"},
        {"when", "When"},
        {"alphamultiplier", "AlphaMultiplier"},
        {"fadedistancemultiplier", "FadeDistanceMultiplier"},
        {"sublinealphamultiplier", "SubLineAlphaMultiplier"},
        {"hideneutral", "HideNeutral"},
    };

    const std::string lowered = ToLowerAscii(Trim(rawKey));
    if (const auto it = kStructKeys.find(lowered); it != kStructKeys.end())
    {
        return it->second;
    }
    return Trim(rawKey);
}

/**
 * @fn void ResetToDefaults()
 * @brief Reset formats, indexed records, and scalar settings before parsing.
 * @author Alex (<https://github.com/lextpf>)
 */
static void ResetToDefaults()
{
    TitleFormat() = "%t";
    DisplayFormat() = {{"%n", false, false}, {" Lv.%l", true, false}};
    // An explicit InfoFormat restores the text row alongside badges.
    InfoFormat().clear();

    Tiers().clear();
    Tiers().push_back(MakeDefaultTier());
    SpecialTitles().clear();
    Honorifics().clear();
    Registers().clear();

    ResetTableDefaults();
}

/**
 * @fn void ClampAndValidate()
 * @brief Apply numeric limits, cross-field constraints, and derived colors.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Validate rows before cross-field constraints, which read clamped values. Color
 * derivation follows; this also runs on the missing-file path with pure defaults.
 */
static void ClampAndValidate()
{
    ValidateTableSettings();

    auto& dist = Distance();
    dist.FadeEndDistance = std::max(dist.FadeStartDistance + 1.0f, dist.FadeEndDistance);
    dist.ScaleEndDistance = std::max(dist.ScaleStartDistance + 1.0f, dist.ScaleEndDistance);
    ShadowOutline().OutlineWidthMax =
        std::max(ShadowOutline().OutlineWidthMin, ShadowOutline().OutlineWidthMax);

    auto& display = Display();
    const auto actorLimits =
        RenderConstants::ClampActorLimits(display.MaxPlates, display.MaxScanActors);
    display.MaxPlates = actorLimits.maxPlates;
    display.MaxScanActors = actorLimits.maxScanActors;

    // Invalid weak < strong < deadly ordering restores defaults to keep every bucket reachable.
    auto& lb = Labels();
    if (lb.WeakAtOrBelow >= lb.StrongAtOrAbove || lb.StrongAtOrAbove >= lb.DeadlyAtOrAbove)
    {
        SKSE::log::warn(
            "Settings: LevelDelta thresholds out of order (Weak={}, Strong={}, Deadly={}); "
            "resetting to defaults",
            lb.WeakAtOrBelow,
            lb.StrongAtOrAbove,
            lb.DeadlyAtOrAbove);
        lb.WeakAtOrBelow = -5;
        lb.StrongAtOrAbove = 5;
        lb.DeadlyAtOrAbove = 10;
    }

    if (Tiers().empty())
    {
        Tiers().push_back(MakeDefaultTier());
    }

    for (auto& tier : Tiers())
    {
        if (tier.maxLevel < tier.minLevel)
        {
            std::swap(tier.maxLevel, tier.minLevel);
        }
        tier.particleCount = std::max(0, tier.particleCount);
        // No upper bound: the manifest emblem count is unknown here, so the renderer
        // falls back to the TierBadgeGamma curve when the index exceeds it.
        tier.badgeIndex = std::max(0, tier.badgeIndex);
        // Optional per-element colors retain INI values; only required colors are clamped.
        tier.leftColor.clamp01();
        tier.rightColor.clamp01();
        tier.highlightColor.clamp01();
    }

    for (auto& special : SpecialTitles())
    {
        special.keyword = Trim(special.keyword);
        special.keywordLower = ToLowerAscii(special.keyword);
        special.color.clamp01();
        special.glowColor.clamp01();
    }

    for (auto& honorific : Honorifics())
    {
        honorific.factionSpec = Trim(honorific.factionSpec);
        honorific.title = Trim(honorific.title);
        honorific.minRank = std::max(0, honorific.minRank);
    }

    for (auto& reg : Registers())
    {
        reg.alphaMul = std::clamp(reg.alphaMul, .0f, 1.0f);
        reg.fadeMul = std::clamp(reg.fadeMul, .2f, 2.0f);
        reg.subLineMul = std::clamp(reg.subLineMul, .0f, 1.0f);
    }

    // Missing components retain white; invalid present components become 1.0.
    auto& ic = Icons();
    const auto deriveColor = [](const std::string& str, Color3& out)
    {
        out = Color3::White();
        ParseColor3(str, out);
        out.clamp01();
    };
    deriveColor(ic.FollowerColorStr, ic.FollowerColor);
    deriveColor(ic.AllyColorStr, ic.AllyColor);
    deriveColor(ic.HostileColorStr, ic.HostileColor);
    deriveColor(ic.WeakColorStr, ic.WeakColor);
    deriveColor(ic.StrongColorStr, ic.StrongColor);
    deriveColor(ic.DeadlyColorStr, ic.DeadlyColor);
    deriveColor(ic.CreatureColorStr, ic.CreatureColor);
    deriveColor(ic.GuardColorStr, ic.GuardColor);
    deriveColor(ic.MerchantColorStr, ic.MerchantColor);
    deriveColor(ic.EssentialColorStr, ic.EssentialColor);
    deriveColor(ic.ProtectedColorStr, ic.ProtectedColor);
    deriveColor(ic.CombatColorStr, ic.CombatColor);
    deriveColor(ic.AlertColorStr, ic.AlertColor);
    deriveColor(ic.SneakHiddenColorStr, ic.SneakHiddenColor);
    deriveColor(ic.SneakDetectedColorStr, ic.SneakDetectedColor);
    deriveColor(ic.EncumberedColorStr, ic.EncumberedColor);
    deriveColor(ic.WantedColorStr, ic.WantedColor);
    deriveColor(ic.TierLowColorStr, ic.TierLowColor);
    deriveColor(ic.TierMidColorStr, ic.TierMidColor);
    deriveColor(ic.TierHighColorStr, ic.TierHighColor);
    deriveColor(ic.NeutralColorStr, ic.NeutralColor);
    deriveColor(ic.HumanoidColorStr, ic.HumanoidColor);
    deriveColor(ic.CommonerColorStr, ic.CommonerColor);
    deriveColor(ic.MortalColorStr, ic.MortalColor);
    deriveColor(ic.EvenColorStr, ic.EvenColor);
    deriveColor(ic.IdleColorStr, ic.IdleColor);
    deriveColor(ic.SneakOffColorStr, ic.SneakOffColor);
    deriveColor(ic.NormalWeightColorStr, ic.NormalWeightColor);
    deriveColor(ic.BountyClearColorStr, ic.BountyClearColor);
    deriveColor(ic.MutedColorStr, ic.MutedColor);

    // Empty accents remain unset for tier-derived colors at draw time. Nonempty values
    // are parsed and clamped; deriveColor would incorrectly force empty values to white.
    const auto deriveOptionalColor = [](const std::string& str, std::optional<Color3>& out)
    {
        if (Trim(str).empty())
        {
            out.reset();
            return;
        }
        Color3 c = Color3::White();
        ParseColor3(str, c);
        c.clamp01();
        out = c;
    };
    deriveOptionalColor(ic.PlayerStripBedColorStr, ic.PlayerStripBedColor);
    deriveOptionalColor(ic.EmblemBacklightColorStr, ic.EmblemBacklightColor);
    deriveOptionalColor(ic.PlayerRimColorStr, ic.PlayerRimColor);
    deriveOptionalColor(ic.EmblemKeyColorStr, ic.EmblemKeyColor);
    deriveOptionalColor(ic.EmblemFillColorStr, ic.EmblemFillColor);

    auto& nc = NpcColors();
    deriveColor(nc.NeutralColorStr, nc.NeutralColor);
    deriveColor(nc.HostileColorStr, nc.HostileColor);
    deriveColor(nc.FollowerColorStr, nc.FollowerColor);
    deriveColor(nc.LevelColorStr, nc.LevelColor);
    deriveColor(nc.TitleColorStr, nc.TitleColor);
}

static std::string Trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
    {
        return {};
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

/**
 * @fn std::string StripInlineComment(const std::string& str)
 * @brief Remove comment text outside double-quoted runs.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Strip ; and # outside quoted runs. Retain quotes and escapes for ParseQuotedSegments;
 * an unterminated quote keeps the rest of the line literal.
 */
static std::string StripInlineComment(const std::string& str)
{
    bool inQuote = false;
    bool escaped = false;
    for (size_t i = 0; i < str.size(); ++i)
    {
        const char c = str[i];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (c == '\\' && inQuote)
        {
            escaped = true;
            continue;
        }
        if (c == '"')
        {
            inQuote = !inQuote;
            continue;
        }
        if (!inQuote && (c == ';' || c == '#'))
        {
            return Trim(str.substr(0, i));
        }
    }
    return Trim(str);
}

/**
 * @fn std::string StripUtf8Bom(const std::string& str)
 * @brief Remove a leading UTF-8 byte-order mark.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Strip the first-line UTF-8 BOM before parsing its key or section.
 */
static std::string StripUtf8Bom(const std::string& str)
{
    if (str.size() >= 3 && static_cast<unsigned char>(str[0]) == 0xEF &&
        static_cast<unsigned char>(str[1]) == 0xBB && static_cast<unsigned char>(str[2]) == 0xBF)
    {
        return str.substr(3);
    }
    return str;
}

static float ParseFloat(const std::string& str, float defaultVal)
{
    try
    {
        return std::stof(str);
    }
    catch (...)
    {
        return defaultVal;
    }
}

static int ParseInt(const std::string& str, int defaultVal)
{
    try
    {
        return std::stoi(str);
    }
    catch (...)
    {
        return defaultVal;
    }
}

static bool ParseBool(const std::string& str)
{
    std::string lower = str;
    for (auto& c : lower)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return (lower == "true" || lower == "1" || lower == "yes" || lower == "on" ||
            lower == "enabled");
}

static void ParseColor3(const std::string& str, Color3& out)
{
    std::istringstream ss(str);
    std::string token;
    int idx = 0;
    float rgb[3] = {out.r, out.g, out.b};
    while (std::getline(ss, token, ',') && idx < 3)
    {
        rgb[idx++] = ParseFloat(Trim(token), 1.0f);
    }
    out = Color3(rgb[0], rgb[1], rgb[2]);
}

/**
 * @fn EffectType ParseEffectType(const std::string& str)
 * @brief Resolve an effect name or use Gradient for unknown text.
 * @author Alex (<https://github.com/lextpf>)
 */
static EffectType ParseEffectType(const std::string& str)
{
    return kEffectTypeMap.fromString(ToLowerAscii(Trim(str)), EffectType::Gradient);
}

/**
 * @fn void ParseEffectString(const std::string& val, EffectParams& effect)
 * @brief Parse an effect name and its optional numeric parameters.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Whitespace separates the effect name from up to five comma-separated parameters.
 * A comma attached to the name causes Gradient fallback; whiteBase truncates remaining text.
 */
static void ParseEffectString(const std::string& val, EffectParams& effect)
{
    std::istringstream ss(val);
    std::string effectTypeName;
    ss >> effectTypeName;

    effect.type = ParseEffectType(effectTypeName);

    std::string paramsStr;
    std::getline(ss, paramsStr);
    paramsStr = Trim(paramsStr);

    std::string paramsLower = ToLowerAscii(paramsStr);
    size_t wbPos = paramsLower.find("whitebase");
    if (wbPos != std::string::npos)
    {
        effect.useWhiteBase = true;
        paramsStr = paramsStr.substr(0, wbPos);
    }

    std::istringstream paramStream(paramsStr);
    std::string token;
    int paramIdx = 0;
    while (std::getline(paramStream, token, ',') && paramIdx < 5)
    {
        token = Trim(token);
        if (!token.empty())
        {
            const float v = ParseFloat(token, .0f);
            switch (paramIdx)
            {
                case 0:
                    effect.param1 = v;
                    break;
                case 1:
                    effect.param2 = v;
                    break;
                case 2:
                    effect.param3 = v;
                    break;
                case 3:
                    effect.param4 = v;
                    break;
                case 4:
                    effect.param5 = v;
                    break;
            }
        }
        // Preserve empty parameter slots: "Aurora 0.5,,0.85" assigns 0.85 to param3,
        // leaving param2 at its default.
        paramIdx++;
    }
}

/**
 * @fn void ParseOrnaments(const std::string& val, std::string& leftOrnaments, std::string&
 *     rightOrnaments)
 * @brief Split ornament codes into left and right strings.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Comma-separated sides retain all characters. Bare "ab" takes two single-byte codes;
 * fewer than two bytes clears both sides.
 */
static void ParseOrnaments(const std::string& val,
                           std::string& leftOrnaments,
                           std::string& rightOrnaments)
{
    size_t commaPos = val.find(',');
    if (commaPos != std::string::npos)
    {
        leftOrnaments = Trim(val.substr(0, commaPos));
        rightOrnaments = Trim(val.substr(commaPos + 1));
    }
    else if (val.length() >= 2)
    {
        leftOrnaments = val.substr(0, 1);
        rightOrnaments = val.substr(1, 1);
    }
    else
    {
        leftOrnaments.clear();
        rightOrnaments.clear();
    }
}

/**
 * @fn bool ParseTierField(TierDefinition& tier, const std::string& key, const std::string& val)
 * @brief Apply a recognized tier field and report whether it consumed the key.
 * @author Alex (<https://github.com/lextpf>)
 *
 * False lets Load offer the line to scalar lookup. Optional color keys omitted by
 * CanonicalizeStructKey require exact casing.
 */
static bool ParseTierField(TierDefinition& tier, const std::string& key, const std::string& val)
{
    if (key == "Name")
    {
        tier.title = val;
    }
    else if (key == "MinLevel")
    {
        // Invalid min/max use 1/25; validation clamps to uint16_t and swaps inverted bounds.
        const int parsed = ParseInt(val, 1);
        const int clamped =
            std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
        tier.minLevel = static_cast<uint16_t>(clamped);
    }
    else if (key == "MaxLevel")
    {
        const int parsed = ParseInt(val, 25);
        const int clamped =
            std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
        tier.maxLevel = static_cast<uint16_t>(clamped);
    }
    else if (key == "LeftColor")
    {
        ParseColor3(val, tier.leftColor);
    }
    else if (key == "RightColor")
    {
        ParseColor3(val, tier.rightColor);
    }
    else if (key == "HighlightColor")
    {
        ParseColor3(val, tier.highlightColor);
    }
    else if (key == "TitleLeftColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.titleLeftColor = c;
    }
    else if (key == "TitleRightColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.titleRightColor = c;
    }
    else if (key == "LevelLeftColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.levelLeftColor = c;
    }
    else if (key == "LevelRightColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.levelRightColor = c;
    }
    else if (key == "ParticleColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.particleColor = c;
    }
    else if (key == "OrnamentLeftColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.ornamentLeftColor = c;
    }
    else if (key == "OrnamentRightColor")
    {
        Color3 c;
        ParseColor3(val, c);
        tier.ornamentRightColor = c;
    }
    else if (key == "TitleEffect" || key == "NameEffect" || key == "LevelEffect")
    {
        EffectParams& effect = (key == "TitleEffect")  ? tier.titleEffect
                               : (key == "NameEffect") ? tier.nameEffect
                                                       : tier.levelEffect;
        ParseEffectString(val, effect);
    }
    else if (key == "Ornaments")
    {
        ParseOrnaments(val, tier.leftOrnaments, tier.rightOrnaments);
    }
    else if (key == "ParticleTypes")
    {
        tier.particleTypes = val;
    }
    else if (key == "ParticleCount")
    {
        tier.particleCount = ParseInt(val, 0);
    }
    else if (key == "Badge")
    {
        tier.badgeIndex = ParseInt(val, 0);
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @fn bool ParseSpecialTitleField(SpecialTitleDefinition& st, const std::string& key, const
 *     std::string& val)
 * @brief Apply a recognized name-keyword override field.
 * @author Alex (<https://github.com/lextpf>)
 */
static bool ParseSpecialTitleField(SpecialTitleDefinition& st,
                                   const std::string& key,
                                   const std::string& val)
{
    if (key == "Keyword")
    {
        st.keyword = val;
    }
    else if (key == "DisplayTitle")
    {
        st.displayTitle = val;
    }
    else if (key == "Color")
    {
        ParseColor3(val, st.color);
    }
    else if (key == "GlowColor")
    {
        ParseColor3(val, st.glowColor);
    }
    else if (key == "ForceOrnaments" || key == "ForceFlourishes")
    {
        st.forceOrnaments = ParseBool(val);
    }
    else if (key == "ForceParticles")
    {
        st.forceParticles = ParseBool(val);
    }
    else if (key == "Priority")
    {
        st.priority = ParseInt(val, 0);
    }
    else if (key == "Ornaments")
    {
        ParseOrnaments(val, st.leftOrnaments, st.rightOrnaments);
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @fn void ParseWhenTokens(const std::string& val, uint32_t& whenMask, uint32_t& whenNotMask)
 * @brief Build required and forbidden context masks from comma-separated tokens.
 * @author Alex (<https://github.com/lextpf>)
 *
 * When tokens: interior, exterior, night, day, city, sneaking, dialogue, crowded.
 * ! negates; exterior/day mean !interior/!night. Unknown tokens are ignored.
 * Keep the mirror in tests/test_settings.cpp synchronized.
 */
static void ParseWhenTokens(const std::string& val, uint32_t& whenMask, uint32_t& whenNotMask)
{
    whenMask = 0;
    whenNotMask = 0;
    std::istringstream ss(val);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        token = ToLowerAscii(Trim(token));
        bool negate = false;
        if (!token.empty() && token[0] == '!')
        {
            negate = true;
            token = Trim(token.substr(1));
        }
        uint32_t bit = 0;
        if (token == "interior")
        {
            bit = Context::Interior;
        }
        else if (token == "exterior")
        {
            bit = Context::Interior;
            negate = !negate;
        }
        else if (token == "night")
        {
            bit = Context::Night;
        }
        else if (token == "day")
        {
            bit = Context::Night;
            negate = !negate;
        }
        else if (token == "city")
        {
            bit = Context::City;
        }
        else if (token == "sneaking")
        {
            bit = Context::Sneaking;
        }
        else if (token == "dialogue")
        {
            bit = Context::Dialogue;
        }
        else if (token == "crowded")
        {
            bit = Context::Crowded;
        }
        if (bit == 0)
        {
            continue;
        }
        (negate ? whenNotMask : whenMask) |= bit;
    }
}

/**
 * @fn bool ParseRegisterField(RegisterDefinition& r, const std::string& key, const std::string&
 *     val)
 * @brief Apply a recognized profile field and activate the profile.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A recognized key activates the otherwise inert gap placeholder.
 */
static bool ParseRegisterField(RegisterDefinition& r,
                               const std::string& key,
                               const std::string& val)
{
    if (key == "Name")
    {
        r.name = val;
    }
    else if (key == "When")
    {
        ParseWhenTokens(val, r.whenMask, r.whenNotMask);
    }
    else if (key == "AlphaMultiplier")
    {
        r.alphaMul = ParseFloat(val, 1.0f);
    }
    else if (key == "FadeDistanceMultiplier")
    {
        r.fadeMul = ParseFloat(val, 1.0f);
    }
    else if (key == "SubLineAlphaMultiplier")
    {
        r.subLineMul = ParseFloat(val, 1.0f);
    }
    else if (key == "HideNeutral")
    {
        r.hideNeutral = ParseBool(val);
    }
    else if (key == "Priority")
    {
        r.priority = ParseInt(val, 0);
    }
    else
    {
        return false;
    }
    r.configured = true;
    return true;
}

/**
 * @fn bool ParseHonorificField(HonorificDefinition& h, const std::string& key, const std::string&
 *     val)
 * @brief Apply a recognized faction-title field.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The title INI key is canonicalized to Name.
 */
static bool ParseHonorificField(HonorificDefinition& h,
                                const std::string& key,
                                const std::string& val)
{
    if (key == "Faction")
    {
        h.factionSpec = val;
    }
    else if (key == "Name")
    {
        h.title = val;
    }
    else if (key == "MinRank")
    {
        h.minRank = ParseInt(val, 0);
    }
    else if (key == "Priority")
    {
        h.priority = ParseInt(val, 0);
    }
    else if (key == "PlayerOnly")
    {
        h.playerOnly = ParseBool(val);
    }
    else if (key == "NpcOnly")
    {
        h.npcOnly = ParseBool(val);
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @fn void ParseQuotedSegments(const std::string& val, std::vector<Segment>& out, std::string*
 *     outTitle, bool forceLevelFont)
 * @brief Extract row segments and optionally separate the title format.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clear both outputs first. If `outTitle` is non-null, move the last `%t` segment to it.
 * Ignore unquoted text except `?` directly after a closing quote. Title segments cannot
 * be droppable. Inside quotes, a backslash copies the next byte literally.
 * Discard a final segment with no closing quote.
 *
 * @param outTitle Optional title output; null keeps `%t` segments in the row.
 * @param forceLevelFont Select the level font for every segment, regardless of `%l`.
 */
static void ParseQuotedSegments(const std::string& val,
                                std::vector<Segment>& out,
                                std::string* outTitle,
                                bool forceLevelFont)
{
    out.clear();
    if (outTitle != nullptr)
    {
        outTitle->clear();
    }

    bool inQuote = false;
    bool justClosed = false;  // True only for the character immediately after a closing `"`.
    std::string current;
    Segment* lastPushed = nullptr;

    for (size_t i = 0; i < val.size(); ++i)
    {
        const char c = val[i];

        if (c == '\\' && i + 1 < val.size())
        {
            if (inQuote)
            {
                current += val[++i];
            }
            justClosed = false;
            continue;
        }

        if (c == '"')
        {
            if (inQuote)
            {
                if (outTitle != nullptr && current.find("%t") != std::string::npos)
                {
                    *outTitle = current;
                    lastPushed = nullptr;
                }
                else
                {
                    const bool isLevel = forceLevelFont || current.find("%l") != std::string::npos;
                    out.push_back({current, isLevel, false});
                    lastPushed = &out.back();
                }
                current.clear();
                inQuote = false;
                justClosed = true;  // Allow trailing `?` on the very next character.
            }
            else
            {
                inQuote = true;
                justClosed = false;
            }
            continue;
        }

        if (inQuote)
        {
            current += c;
            justClosed = false;
            continue;
        }

        if (justClosed && c == '?')
        {
            if (lastPushed != nullptr)
            {
                lastPushed->dropIfBlank = true;
            }
        }
        justClosed = false;
    }
}

/**
 * @fn void ParseDisplayFormat(const std::string& val)
 * @brief Replace nonempty main and title rows from quoted segments.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Assign each row only if nonempty; empty or unquoted Format leaves defaults intact.
 */
static void ParseDisplayFormat(const std::string& val)
{
    std::vector<Segment> newDisplayFormat;
    std::string newTitleFormat;
    ParseQuotedSegments(val, newDisplayFormat, &newTitleFormat, false);

    if (!newTitleFormat.empty())
    {
        TitleFormat() = newTitleFormat;
    }
    if (!newDisplayFormat.empty())
    {
        DisplayFormat() = newDisplayFormat;
    }
}

/**
 * @fn void ParseInfoFormat(const std::string& val)
 * @brief Replace the info row, including an explicitly empty row.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Always assign so an empty InfoFormat disables the row.
 */
static void ParseInfoFormat(const std::string& val)
{
    std::vector<Segment> newInfoFormat;
    ParseQuotedSegments(val, newInfoFormat, nullptr, true);
    InfoFormat() = newInfoFormat;
}

void Load()
{
    std::unique_lock<std::shared_mutex> settingsWriteLock(Mutex());

    ResetToDefaults();

    std::ifstream file("Data/SKSE/Plugins/glyph.ini");
    if (!file.is_open())
    {
        ClampAndValidate();
        SKSE::log::warn("Settings: glyph.ini not found, using defaults");
        // Missing files leave Generation unchanged, so consumers keep cached copies.
        return;
    }

    std::string line;
    std::string currentSection;
    std::string currentSectionLower;
    int currentTier = -1;          // Tracks which tier we're parsing (-1 = global settings)
    int currentSpecialTitle = -1;  // Tracks which special title we're parsing (-1 = none)
    int currentHonorific = -1;     // Tracks which honorific we're parsing (-1 = none)
    int currentRegister = -1;      // Tracks which register we're parsing (-1 = none)
    size_t lineNumber = 0;
    size_t malformedLineCount = 0;
    size_t unknownKeyCount = 0;
    size_t unknownSectionCount = 0;
    std::vector<std::string> parseWarnings;
    std::unordered_set<std::string> warnedUnknownSections;

    // Cap detailed warnings; counters continue for the summary.
    auto addWarning = [&](size_t lineNo, const std::string& message)
    {
        constexpr size_t MAX_WARNINGS = 48;
        if (parseWarnings.size() < MAX_WARNINGS)
        {
            std::ostringstream ss;
            ss << "L" << lineNo << ": " << message;
            parseWarnings.push_back(ss.str());
        }
    };

    while (std::getline(file, line))
    {
        const size_t currentLineNumber = lineNumber + 1;
        if (lineNumber++ == 0)
        {
            line = StripUtf8Bom(line);
        }
        line = Trim(line);
        line = StripInlineComment(line);

        if (line.empty() || line[0] == ';' || line[0] == '#')
        {
            continue;
        }

        if (line.size() >= 2 && line[0] == '[' && line.back() == ']')
        {
            currentSection = line.substr(1, line.size() - 2);
            currentSection = Trim(currentSection);
            currentSectionLower = ToLowerAscii(currentSection);

            // Reject excessive indices to bound vector growth after INI typos.
            if (currentSectionLower.size() >= 4 && currentSectionLower.rfind("tier", 0) == 0)
            {
                std::string numStr = currentSection.substr(4);
                currentTier = ParseInt(numStr, -1);
                currentSpecialTitle = -1;  // Not in a special title section
                currentHonorific = -1;
                currentRegister = -1;

                if (currentTier < 0 || currentTier > RenderConstants::MAX_TIER_INDEX)
                {
                    currentTier = -1;  // Invalid tier number, treat as non-tier section
                    addWarning(currentLineNumber,
                               "Invalid or out-of-range tier section '" + currentSection + "'");
                }
                else
                {
                    // Gap tiers span levels 1-250 and shadow higher tiers in first-match lookup;
                    // warn on gaps.
                    const int oldTierCount = static_cast<int>(Tiers().size());
                    if (oldTierCount < currentTier)
                    {
                        addWarning(currentLineNumber,
                                   "Tier section '" + currentSection + "' leaves tiers " +
                                       std::to_string(oldTierCount) + "-" +
                                       std::to_string(currentTier - 1) +
                                       " undefined; they default to 'Unknown' (level range "
                                       "1-250) and will shadow this and higher tiers");
                    }
                    while (static_cast<int>(Tiers().size()) <= currentTier)
                    {
                        Tiers().emplace_back();
                    }
                }
            }

            else if (currentSectionLower.size() >= 12 &&
                     currentSectionLower.rfind("specialtitle", 0) == 0)
            {
                std::string numStr = currentSection.substr(12);
                currentSpecialTitle = ParseInt(numStr, -1);
                currentTier = -1;  // Not in a tier section
                currentHonorific = -1;
                currentRegister = -1;

                if (currentSpecialTitle >= 0 &&
                    currentSpecialTitle <= RenderConstants::MAX_SPECIAL_TITLE_INDEX)
                {
                    while (static_cast<int>(SpecialTitles().size()) <= currentSpecialTitle)
                    {
                        SpecialTitleDefinition newSpecial;
                        newSpecial.keyword = "";
                        newSpecial.displayTitle = "";
                        newSpecial.color = Color3::White();
                        newSpecial.glowColor = Color3::White();
                        newSpecial.forceOrnaments = true;
                        newSpecial.forceParticles = true;
                        newSpecial.priority = 0;
                        SpecialTitles().push_back(newSpecial);
                    }
                }
                else
                {
                    addWarning(
                        currentLineNumber,
                        "Invalid or out-of-range special title section '" + currentSection + "'");
                    currentSpecialTitle = -1;
                }
            }

            else if (currentSectionLower.size() >= 9 &&
                     currentSectionLower.rfind("honorific", 0) == 0)
            {
                std::string numStr = currentSection.substr(9);
                currentHonorific = ParseInt(numStr, -1);
                currentTier = -1;
                currentSpecialTitle = -1;
                currentRegister = -1;

                if (currentHonorific >= 0 &&
                    currentHonorific <= RenderConstants::MAX_HONORIFIC_INDEX)
                {
                    while (static_cast<int>(Honorifics().size()) <= currentHonorific)
                    {
                        Honorifics().emplace_back();
                    }
                }
                else
                {
                    addWarning(
                        currentLineNumber,
                        "Invalid or out-of-range honorific section '" + currentSection + "'");
                    currentHonorific = -1;
                }
            }

            else if (currentSectionLower.size() >= 8 &&
                     currentSectionLower.rfind("register", 0) == 0)
            {
                std::string numStr = currentSection.substr(8);
                currentRegister = ParseInt(numStr, -1);
                currentTier = -1;
                currentSpecialTitle = -1;
                currentHonorific = -1;

                if (currentRegister >= 0 && currentRegister <= RenderConstants::MAX_REGISTER_INDEX)
                {
                    const int oldCount = static_cast<int>(Registers().size());
                    if (oldCount < currentRegister)
                    {
                        addWarning(currentLineNumber,
                                   "Register section '" + currentSection + "' leaves registers " +
                                       std::to_string(oldCount) + "-" +
                                       std::to_string(currentRegister - 1) +
                                       " undefined; they stay inert until given keys");
                    }
                    while (static_cast<int>(Registers().size()) <= currentRegister)
                    {
                        Registers().emplace_back();
                    }
                }
                else
                {
                    addWarning(currentLineNumber,
                               "Invalid or out-of-range register section '" + currentSection + "'");
                    currentRegister = -1;
                }
            }
            else
            {
                currentTier = -1;  // Non-tier section, switch to global context
                currentSpecialTitle = -1;
                currentHonorific = -1;
                currentRegister = -1;

                // Unknown sections warn but still accept scalars by key name.
                static const std::unordered_set<std::string> kKnownSections = {"",
                                                                               "general",
                                                                               "display",
                                                                               "deck",
                                                                               "debug",
                                                                               "visual",
                                                                               "fonts",
                                                                               "particles",
                                                                               "occlusion",
                                                                               "labels",
                                                                               "leveldelta",
                                                                               "icons",
                                                                               "focus",
                                                                               "graffito",
                                                                               "quiet",
                                                                               "deathrite",
                                                                               "compat",
                                                                               "candlelight",
                                                                               "depthclip"};
                if (kKnownSections.find(currentSectionLower) == kKnownSections.end())
                {
                    ++unknownSectionCount;
                    if (warnedUnknownSections.insert(currentSectionLower).second)
                    {
                        addWarning(currentLineNumber, "Unknown section [" + currentSection + "]");
                    }
                }
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            ++malformedLineCount;
            addWarning(currentLineNumber, "Ignoring malformed setting line (missing '=')");
            continue;
        }

        std::string keyRaw = Trim(line.substr(0, eq));
        std::string key = CanonicalizeStructKey(keyRaw);
        std::string val = Trim(line.substr(eq + 1));

        // Offer indexed fields first, then formats, then raw scalar keys. At most one indexed
        // parser is active; canonicalization must not rename a scalar.
        bool handled = false;

        if (currentTier >= 0 && currentTier < static_cast<int>(Tiers().size()))
        {
            handled = ParseTierField(Tiers()[currentTier], key, val);
        }

        if (!handled && currentSpecialTitle >= 0 &&
            currentSpecialTitle < static_cast<int>(SpecialTitles().size()))
        {
            handled = ParseSpecialTitleField(SpecialTitles()[currentSpecialTitle], key, val);
        }

        if (!handled && currentHonorific >= 0 &&
            currentHonorific < static_cast<int>(Honorifics().size()))
        {
            handled = ParseHonorificField(Honorifics()[currentHonorific], key, val);
        }

        if (!handled && currentRegister >= 0 &&
            currentRegister < static_cast<int>(Registers().size()))
        {
            handled = ParseRegisterField(Registers()[currentRegister], key, val);
        }

        if (!handled)
        {
            if (key == "Format")
            {
                ParseDisplayFormat(val);
            }
            else if (keyRaw == "InfoFormat" || ToLowerAscii(keyRaw) == "infoformat")
            {
                ParseInfoFormat(val);
            }

            else if (auto it = GetKeyMap().find(ToLowerAscii(keyRaw)); it != GetKeyMap().end())
            {
                ApplySettingValue(*it->second, val);
            }
            else
            {
                ++unknownKeyCount;
                std::string sectionName = currentSection.empty() ? "<global>" : currentSection;
                addWarning(currentLineNumber,
                           "Unknown key '" + keyRaw + "' in section " + sectionName);
            }
        }
    }

    ClampAndValidate();

    if (malformedLineCount > 0 || unknownKeyCount > 0 || unknownSectionCount > 0)
    {
        SKSE::log::warn(
            "Settings: parsed glyph.ini with {} malformed lines, {} unknown keys, {} unknown "
            "sections",
            malformedLineCount,
            unknownKeyCount,
            unknownSectionCount);
        for (const auto& warning : parseWarnings)
        {
            SKSE::log::warn("Settings: {}", warning);
        }

        if (parseWarnings.size() == 48)
        {
            SKSE::log::warn("Settings: warning output truncated");
        }
    }

    // Publish under the write lock; the render-thread acquire observes all updated values.
    Generation().fetch_add(1, std::memory_order_release);
}
}  // namespace Settings
