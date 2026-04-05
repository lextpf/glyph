#include "Settings.h"

#include "PCH.h"
#include "RenderConstants.h"
#include "SettingsBinding.h"

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
// Single source of truth for EffectType <-> lowercase string mapping.
static constexpr Stl::EnumStringMap<EffectType, 17> kEffectTypeMap{{
    {{"none", EffectType::None},
     {"gradient", EffectType::Gradient},
     {"verticalgradient", EffectType::VerticalGradient},
     {"diagonalgradient", EffectType::DiagonalGradient},
     {"radialgradient", EffectType::RadialGradient},
     {"shimmer", EffectType::Shimmer},
     {"chromaticshimmer", EffectType::ChromaticShimmer},
     {"ember", EffectType::Ember},
     {"pulsegradient", EffectType::Ember},  // backward compat alias
     {"rainbowwave", EffectType::RainbowWave},
     {"conicrainbow", EffectType::ConicRainbow},
     {"aurora", EffectType::Aurora},
     {"sparkle", EffectType::Sparkle},
     {"plasma", EffectType::Plasma},
     {"scanline", EffectType::Scanline},
     {"enchant", EffectType::Enchant},
     {"frost", EffectType::Frost}},
}};

// Parser helper forward declarations (used before definitions).
static std::string Trim(const std::string& str);
static std::string ToLowerAscii(std::string_view input);
static float ParseFloat(const std::string& str, float defaultVal);
static int ParseInt(const std::string& str, int defaultVal);
static bool ParseBool(const std::string& str);

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

// Category struct accessors (function-local statics)
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

AppearanceSettings& Appearance()
{
    static AppearanceSettings s;
    return s;
}

VisualSettings& Visual()
{
    static VisualSettings vs;
    return vs;
}

// Default font paths (shared between table and ResetToDefaults)
static constexpr auto kDefaultNameFontPath =
    "Data/SKSE/Plugins/glyph/fonts/bd1aab18-7649-4946-9f7b-6ddd6a81311d.ttf";
static constexpr auto kDefaultLevelFontPath =
    "Data/SKSE/Plugins/glyph/fonts/96120cca-4be2-4d10-b10a-b8183ac18467.ttf";
static constexpr auto kDefaultTitleFontPath =
    "Data/SKSE/Plugins/glyph/fonts/56cb786e-c94e-452c-ac54-360c46381de1.ttf";
static constexpr auto kDefaultOrnamentFontPath =
    "Data/SKSE/Plugins/glyph/fonts/050986eb-c23a-4891-a951-9fed313e44c2.otf";

// clang-format off

// Single source of truth for all scalar settings.
// Each row: key, alias, target ptr, default value, validation rule.
static const auto kSettings = std::to_array<SettingEntry>({
    // Distance & Visibility
    {"FadeStartDistance",       "", &Distance().FadeStartDistance,     200.0f,   MinFloat{.0f}},
    {"FadeEndDistance",         "", &Distance().FadeEndDistance,       2500.0f,  MinFloat{.0f}},
    {"ScaleStartDistance",      "", &Distance().ScaleStartDistance,    200.0f,   MinFloat{.0f}},
    {"ScaleEndDistance",        "", &Distance().ScaleEndDistance,      2500.0f,  MinFloat{.0f}},
    {"MinimumScale",           "", &Distance().MinimumScale,          .1f,      ClampFloat{.01f, 5.0f}},
    {"MaxScanDistance",         "", &Distance().MaxScanDistance,       3000.0f,  MinFloat{.0f}},

    // Occlusion
    {"EnableOcclusionCulling",  "", &Occlusion().Enabled,              true,    NoClamping{}},
    {"OcclusionSettleTime",     "", &Occlusion().SettleTime,           .58f,    MinFloat{.01f}},
    {"OcclusionCheckInterval",  "", &Occlusion().CheckInterval,        3,       MinInt{1}},

    // Shadow & Outline
    {"TitleShadowOffsetX",     "", &ShadowOutline().TitleShadowOffsetX,    2.0f,     NoClamping{}},
    {"TitleShadowOffsetY",     "", &ShadowOutline().TitleShadowOffsetY,    2.0f,     NoClamping{}},
    {"MainShadowOffsetX",      "", &ShadowOutline().MainShadowOffsetX,     4.0f,     NoClamping{}},
    {"MainShadowOffsetY",      "", &ShadowOutline().MainShadowOffsetY,     4.0f,     NoClamping{}},
    {"SegmentPadding",         "", &ShadowOutline().SegmentPadding,        4.0f,     NoClamping{}},
    {"OutlineWidthMin",        "", &ShadowOutline().OutlineWidthMin,       2.0f,     MinFloat{.0f}},
    {"OutlineWidthMax",        "", &ShadowOutline().OutlineWidthMax,       2.5f,     MinFloat{.0f}},
    {"FastOutlines",           "", &ShadowOutline().FastOutlines,          false,    NoClamping{}},
    {"TitleMainGap",           "", &ShadowOutline().TitleMainGap,          .0f,      MinFloat{.0f}},
    {"OutlineMinScale",        "", &ShadowOutline().OutlineMinScale,       .65f,     ClampFloat{.0f, 1.0f}},
    {"ProportionalSpacing",    "", &ShadowOutline().ProportionalSpacing,   false,    NoClamping{}},

    // Outline Glow
    {"EnableOutlineGlow",      "", &ShadowOutline().OutlineGlowEnabled,    false,    NoClamping{}},
    {"OutlineGlowScale",       "", &ShadowOutline().OutlineGlowScale,      1.4f,     ClampFloat{1.0f, 4.0f}},
    {"OutlineGlowAlpha",       "", &ShadowOutline().OutlineGlowAlpha,      .1f,      ClampFloat{.0f, 1.0f}},
    {"OutlineGlowRings",       "", &ShadowOutline().OutlineGlowRings,      2,        ClampInt{1, 3}},
    {"OutlineGlowR",           "", &ShadowOutline().OutlineGlowR,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowG",           "", &ShadowOutline().OutlineGlowG,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowB",           "", &ShadowOutline().OutlineGlowB,          1.0f,     ClampFloat{.0f, 1.0f}},
    {"OutlineGlowTierTint",    "", &ShadowOutline().OutlineGlowTierTint,   false,    NoClamping{}},
    // Dual-Tone Directional Outline
    {"EnableDualOutline",      "", &ShadowOutline().DualOutlineEnabled,    false,    NoClamping{}},
    {"InnerOutlineTint",       "", &ShadowOutline().InnerOutlineTint,      .3f,      ClampFloat{.0f, 1.0f}},
    {"InnerOutlineAlpha",      "", &ShadowOutline().InnerOutlineAlpha,     .5f,      ClampFloat{.0f, 1.0f}},
    {"InnerOutlineScale",      "", &ShadowOutline().InnerOutlineScale,     .5f,      ClampFloat{.1f, .9f}},
    {"DirectionalLightAngle",  "", &ShadowOutline().DirectionalLightAngle, 315.f,    ClampFloat{.0f, 360.f}},
    {"DirectionalLightBias",   "", &ShadowOutline().DirectionalLightBias,  .15f,     ClampFloat{.0f, .5f}},

    {"OutlineColorTint",       "", &ShadowOutline().OutlineColorTint,      .0f,      ClampFloat{.0f, .25f}},
    {"ShadowColorTint",        "", &ShadowOutline().ShadowColorTint,       .0f,      ClampFloat{.0f, .25f}},

    // Glow
    {"EnableGlow",             "", &Glow().Enabled,                   false,    NoClamping{}},
    {"GlowRadius",             "", &Glow().Radius,                    4.0f,     MinFloat{.0f}},
    {"GlowIntensity",          "", &Glow().Intensity,                 .5f,      ClampFloat{.0f, 1.0f}},
    {"GlowSamples",            "", &Glow().Samples,                   8,        ClampInt{1, 64}},

    // Typewriter
    {"EnableTypewriter",       "", &Typewriter().Enabled,             false,    NoClamping{}},
    {"TypewriterSpeed",        "", &Typewriter().Speed,               30.0f,    MinFloat{.0f}},
    {"TypewriterDelay",        "", &Typewriter().Delay,               .0f,      MinFloat{.0f}},

    // Entrance/Exit Transitions
    {"EnableEntranceAnimation","", &Transition().EnableEntrance,      false,    NoClamping{}},
    {"EntranceStyle",          "", &Transition().EntranceStyle,       0,        ClampInt{0, 2}},
    {"EntranceDuration",       "", &Transition().EntranceDuration,    .35f,     ClampFloat{.05f, 3.0f}},
    {"EntranceOvershoot",      "", &Transition().EntranceOvershoot,   1.05f,    ClampFloat{1.0f, 1.3f}},
    {"EnableExitAnimation",    "", &Transition().EnableExit,          false,    NoClamping{}},
    {"ExitDuration",           "", &Transition().ExitDuration,        .20f,     ClampFloat{.05f, 2.0f}},

    // Debug
    {"EnableDebugOverlay",     "", &Display().EnableDebugOverlay,     false,    NoClamping{}},

    // Ornaments
    {"EnableOrnaments",        "EnableFlourishes",   &Ornament().Enabled,      true,     NoClamping{}},
    {"OrnamentScale",          "FlourishScale",      &Ornament().Scale,        1.0f,     NoClamping{}},
    {"OrnamentSpacing",        "FlourishSpacing",    &Ornament().Spacing,      3.0f,     NoClamping{}},
    {"OrnamentAnchorToMainLine", "",                  &Ornament().AnchorToMainLine, true, NoClamping{}},

    // Particle Aura
    {"EnableParticleAura",     "", &Particle().Enabled,               true,     NoClamping{}},
    {"UseParticleTextures",    "", &Particle().UseParticleTextures,   true,     NoClamping{}},
    {"EnableStars",            "", &Particle().EnableStars,           true,     NoClamping{}},
    {"EnableSparks",           "", &Particle().EnableSparks,          false,    NoClamping{}},
    {"EnableWisps",            "", &Particle().EnableWisps,           false,    NoClamping{}},
    {"EnableRunes",            "", &Particle().EnableRunes,           false,    NoClamping{}},
    {"EnableOrbs",             "", &Particle().EnableOrbs,            false,    NoClamping{}},
    {"EnableCrystals",         "", &Particle().EnableCrystals,        false,    NoClamping{}},
    {"ParticleCount",          "", &Particle().Count,                 8,        MinInt{0}},
    {"ParticleSize",           "", &Particle().Size,                  3.0f,     MinFloat{.0f}},
    {"ParticleSpeed",          "", &Particle().Speed,                 1.0f,     MinFloat{.0f}},
    {"ParticleSpread",         "", &Particle().Spread,                20.0f,    MinFloat{.0f}},
    {"ParticleAlpha",          "", &Particle().Alpha,                 .8f,      ClampFloat{.0f, 1.0f}},
    {"ParticleBlendMode",      "", &Particle().BlendMode,             0,        ClampInt{0, 2}},

    // Display Options
    {"VerticalOffset",         "", &Display().VerticalOffset,         8.0f,     NoClamping{}},
    {"HidePlayer",             "", &Display().HidePlayer,             false,    NoClamping{}},
    {"HideCreatures",          "", &Display().HideCreatures,          false,    NoClamping{}},
    {"ReloadKey",              "", &Display().ReloadKey,              0,        NoClamping{}},

    // Animation Speed
    {"AnimSpeedLowTier",       "", &AnimColor().AnimSpeedLowTier,    .35f,     NoClamping{}},
    {"AnimSpeedMidTier",       "", &AnimColor().AnimSpeedMidTier,    .20f,     NoClamping{}},
    {"AnimSpeedHighTier",      "", &AnimColor().AnimSpeedHighTier,   .10f,     NoClamping{}},

    // Color & Effects
    {"ColorWashAmount",        "", &AnimColor().ColorWashAmount,      .15f,     ClampFloat{.0f, 1.0f}},
    {"NameColorMix",           "", &AnimColor().NameColorMix,         .65f,     ClampFloat{.0f, 1.0f}},
    {"EffectAlphaMin",         "", &AnimColor().EffectAlphaMin,       .20f,     ClampFloat{.0f, 1.0f}},
    {"EffectAlphaMax",         "", &AnimColor().EffectAlphaMax,       .60f,     ClampFloat{.0f, 1.0f}},
    {"StrengthMin",            "", &AnimColor().StrengthMin,          .15f,     MinFloat{.0f}},
    {"StrengthMax",            "", &AnimColor().StrengthMax,          .60f,     MinFloat{.0f}},

    // Smoothing
    {"AlphaSettleTime",        "", &AnimColor().AlphaSettleTime,      .46f,     MinFloat{.01f}},
    {"ScaleSettleTime",        "", &AnimColor().ScaleSettleTime,      .46f,     MinFloat{.01f}},
    {"PositionSettleTime",     "", &AnimColor().PositionSettleTime,   .38f,     MinFloat{.01f}},
    {"TierVibrancyBoost",      "", &AnimColor().TierVibrancyBoost,    .0f,      ClampFloat{.0f, 1.0f}},

    // Visual sub-settings (via Visual() singleton)
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
    // Motion Trail
    {"EnableMotionTrail",      "", &Visual().EnableMotionTrail,      false,   NoClamping{}},
    {"TrailLength",            "", &Visual().TrailLength,             4,       ClampInt{1, 8}},
    {"TrailAlpha",             "", &Visual().TrailAlpha,              .3f,     ClampFloat{.0f, 1.0f}},
    {"TrailFalloff",           "", &Visual().TrailFalloff,            2.0f,    ClampFloat{.5f, 5.0f}},
    {"TrailMinDistance",        "", &Visual().TrailMinDistance,        2.0f,    MinFloat{.0f}},
    {"TrailMinTier",           "", &Visual().TrailMinTier,            0,       MinInt{0}},

    // Wave Displacement
    {"EnableWave",             "", &Visual().EnableWave,             false,   NoClamping{}},
    {"WaveAmplitude",          "", &Visual().WaveAmplitude,          1.5f,    ClampFloat{.0f, 10.0f}},
    {"WaveFrequency",          "", &Visual().WaveFrequency,          3.0f,    ClampFloat{.5f, 20.0f}},
    {"WaveSpeed",              "", &Visual().WaveSpeed,              1.0f,    ClampFloat{.0f, 10.0f}},
    {"WaveMinTier",            "", &Visual().WaveMinTier,            0,       MinInt{0}},

    {"EnableTierEffectGating", "", &Visual().EnableTierEffectGating, false, NoClamping{}},
    {"GlowMinTier",            "", &Visual().GlowMinTier,         5,       NoClamping{}},
    {"ParticleMinTier",        "", &Visual().ParticleMinTier,     10,      NoClamping{}},
    {"OrnamentMinTier",        "", &Visual().OrnamentMinTier,     10,      NoClamping{}},

    // Fonts
    {"NameFontPath",           "", &Font().NameFontPath,           std::string(kDefaultNameFontPath),    NoClamping{}},
    {"NameFontSize",           "", &Font().NameFontSize,           122.0f,   NoClamping{}},
    {"LevelFontPath",          "", &Font().LevelFontPath,          std::string(kDefaultLevelFontPath),   NoClamping{}},
    {"LevelFontSize",          "", &Font().LevelFontSize,          61.0f,    NoClamping{}},
    {"TitleFontPath",          "", &Font().TitleFontPath,          std::string(kDefaultTitleFontPath),   NoClamping{}},
    {"TitleFontSize",          "", &Font().TitleFontSize,          42.0f,    NoClamping{}},
    {"OrnamentFontPath",       "", &Ornament().FontPath,           std::string(kDefaultOrnamentFontPath), NoClamping{}},
    {"OrnamentFontSize",       "", &Ornament().FontSize,           64.0f,    NoClamping{}},

    // Appearance Template
    {"TemplateFormID",         "", &Appearance().TemplateFormID,        std::string(),  NoClamping{}},
    {"TemplatePlugin",         "", &Appearance().TemplatePlugin,        std::string(),  NoClamping{}},
    {"UseTemplateAppearance",  "", &Appearance().UseTemplateAppearance, false,    NoClamping{}},
    {"TemplateIncludeRace",    "", &Appearance().TemplateIncludeRace,   false,    NoClamping{}},
    {"TemplateIncludeBody",    "", &Appearance().TemplateIncludeBody,   false,    NoClamping{}},
    {"TemplateCopyFaceGen",    "", &Appearance().TemplateCopyFaceGen,   true,     NoClamping{}},
    {"TemplateCopySkin",       "", &Appearance().TemplateCopySkin,      false,    NoClamping{}},
    {"TemplateCopyOverlays",   "", &Appearance().TemplateCopyOverlays,  false,    NoClamping{}},
    {"TemplateCopyOutfit",     "", &Appearance().TemplateCopyOutfit,    false,    NoClamping{}},
    {"TemplateReapplyOnReload","", &Appearance().TemplateReapplyOnReload, false,  NoClamping{}},
    {"TemplateFaceGenPlugin",  "", &Appearance().TemplateFaceGenPlugin, std::string(),  NoClamping{}},
});

// clang-format on

// Lazily-built lookup map: lowercase key -> SettingEntry pointer.
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

// Apply a parsed string value to the correct typed target.
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

// Reset all table-driven settings to their defaults.
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

// Apply validation rules from the table.
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

// Canonicalize a key for tier/special-title section fields (not in the table).
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
        {"keyword", "Keyword"},
        {"displaytitle", "DisplayTitle"},
        {"color", "Color"},
        {"glowcolor", "GlowColor"},
        {"forceornaments", "ForceOrnaments"},
        {"forceflourishes", "ForceFlourishes"},
        {"forceparticles", "ForceParticles"},
        {"priority", "Priority"},
        {"format", "Format"},
    };

    const std::string lowered = ToLowerAscii(Trim(rawKey));
    if (const auto it = kStructKeys.find(lowered); it != kStructKeys.end())
    {
        return it->second;
    }
    return Trim(rawKey);
}

static void ResetToDefaults()
{
    TitleFormat() = "%t";
    DisplayFormat() = {{"%n", false}, {" Lv.%l", true}};

    Tiers().clear();
    Tiers().push_back(MakeDefaultTier());
    SpecialTitles().clear();

    // All scalar settings are reset from the descriptor table.
    ResetTableDefaults();
}

static void ClampAndValidate()
{
    // Apply per-setting validation from the descriptor table.
    ValidateTableSettings();

    // Cross-field constraints that cannot be expressed per-setting.
    auto& dist = Distance();
    dist.FadeEndDistance = std::max(dist.FadeStartDistance + 1.0f, dist.FadeEndDistance);
    dist.ScaleEndDistance = std::max(dist.ScaleStartDistance + 1.0f, dist.ScaleEndDistance);
    ShadowOutline().OutlineWidthMax =
        std::max(ShadowOutline().OutlineWidthMin, ShadowOutline().OutlineWidthMax);

    auto& ac = AnimColor();
    if (ac.EffectAlphaMin > ac.EffectAlphaMax)
    {
        std::swap(ac.EffectAlphaMin, ac.EffectAlphaMax);
    }
    if (ac.StrengthMin > ac.StrengthMax)
    {
        std::swap(ac.StrengthMin, ac.StrengthMax);
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
}

// Helper function: Remove leading/trailing whitespace
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

// Helper function: strip inline comments while preserving quoted text.
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

static std::string StripUtf8Bom(const std::string& str)
{
    if (str.size() >= 3 && static_cast<unsigned char>(str[0]) == 0xEF &&
        static_cast<unsigned char>(str[1]) == 0xBB && static_cast<unsigned char>(str[2]) == 0xBF)
    {
        return str.substr(3);
    }
    return str;
}

// Helper function: Parse float with fallback default
static float ParseFloat(const std::string& str, float defaultVal)
{
    try
    {
        return std::stof(str);
    }
    catch (...)
    {
        // Return default if parsing fails
        return defaultVal;
    }
}

// Helper function: Parse integer with fallback default
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

// Helper function: Parse boolean (true/false, 1/0, yes/no)
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

// Helper function: Parse comma-separated RGB color (0.0-1.0)
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

// Helper function: Parse effect type name to enum
static EffectType ParseEffectType(const std::string& str)
{
    return kEffectTypeMap.fromString(ToLowerAscii(Trim(str)), EffectType::Gradient);
}

// Parse an effect string "EffectType param1,param2,... [whiteBase]" into an EffectParams.
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
        if (token.empty())
        {
            continue;
        }

        float v = ParseFloat(token, .0f);
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
        paramIdx++;
    }
}

// Parse ornaments string "LEFT, RIGHT" or legacy "AB" format.
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

// Parse a single key-value pair for a [TierN] section.
static bool ParseTierField(TierDefinition& tier, const std::string& key, const std::string& val)
{
    if (key == "Name")
    {
        tier.title = val;
    }
    else if (key == "MinLevel")
    {
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
    else
    {
        return false;
    }
    return true;
}

// Parse a single key-value pair for a [SpecialTitleN] section.
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

// Parse the Format key: quoted segments forming title and display format.
static void ParseDisplayFormat(const std::string& val)
{
    std::vector<Segment> newDisplayFormat;
    std::string newTitleFormat;
    bool titleFound = false;

    bool inQuote = false;
    std::string current;
    for (size_t i = 0; i < val.size(); ++i)
    {
        char c = val[i];

        if (c == '\\' && i + 1 < val.size())
        {
            if (inQuote)
            {
                current += val[++i];
            }
            continue;
        }

        if (c == '"')
        {
            if (inQuote)
            {
                if (current.find("%t") != std::string::npos)
                {
                    newTitleFormat = current;
                    titleFound = true;
                }
                else
                {
                    bool isLevel = current.find("%l") != std::string::npos;
                    newDisplayFormat.push_back({current, isLevel});
                }
                current.clear();
                inQuote = false;
            }
            else
            {
                inQuote = true;
            }
        }
        else if (inQuote)
        {
            current += c;
        }
    }

    if (titleFound)
    {
        TitleFormat() = newTitleFormat;
    }
    if (!newDisplayFormat.empty())
    {
        DisplayFormat() = newDisplayFormat;
    }
}

void Load()
{
    std::unique_lock<std::shared_mutex> settingsWriteLock(Mutex());

    ResetToDefaults();

    // File is located in Skyrim's Data folder under SKSE plugins directory
    std::ifstream file("Data/SKSE/Plugins/glyph.ini");
    if (!file.is_open())
    {
        ClampAndValidate();
        SKSE::log::warn("Settings: glyph.ini not found, using defaults");
        return;  // Use defaults if file not found
    }

    std::string line;
    std::string currentSection;
    std::string currentSectionLower;
    int currentTier = -1;          // Tracks which tier we're parsing (-1 = global settings)
    int currentSpecialTitle = -1;  // Tracks which special title we're parsing (-1 = none)
    size_t lineNumber = 0;
    size_t malformedLineCount = 0;
    size_t unknownKeyCount = 0;
    size_t unknownSectionCount = 0;
    std::vector<std::string> parseWarnings;
    std::unordered_set<std::string> warnedUnknownSections;

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

    // Line-by-line parsing allows for flexible format with sections
    while (std::getline(file, line))
    {
        const size_t currentLineNumber = lineNumber + 1;
        if (lineNumber++ == 0)
        {
            line = StripUtf8Bom(line);
        }
        line = Trim(line);
        line = StripInlineComment(line);

        // Skip empty lines and comments (both ; and # style for user convenience)
        if (line.empty() || line[0] == ';' || line[0] == '#')
        {
            continue;
        }

        // Detect section headers like [Tier0], [Tier1], etc.
        // Section headers change the parsing context for subsequent kv pairs
        if (line.size() >= 2 && line[0] == '[' && line.back() == ']')
        {
            currentSection = line.substr(1, line.size() - 2);
            currentSection = Trim(currentSection);
            currentSectionLower = ToLowerAscii(currentSection);

            // Tier numbers are 0-indexed and can be any non-negative integer
            if (currentSectionLower.size() >= 4 && currentSectionLower.rfind("tier", 0) == 0)
            {
                std::string numStr = currentSection.substr(4);
                currentTier = ParseInt(numStr, -1);
                currentSpecialTitle = -1;  // Not in a special title section

                if (currentTier < 0 || currentTier > RenderConstants::MAX_TIER_INDEX)
                {
                    currentTier = -1;  // Invalid tier number, treat as non-tier section
                    addWarning(currentLineNumber,
                               "Invalid or out-of-range tier section '" + currentSection + "'");
                }
                else
                {
                    // Dynamically grow the Tiers vector to accommodate the specified tier
                    while (static_cast<int>(Tiers().size()) <= currentTier)
                    {
                        Tiers().emplace_back();
                    }
                }
            }
            // Special title sections like [SpecialTitle0], [SpecialTitle1], etc.
            else if (currentSectionLower.size() >= 12 &&
                     currentSectionLower.rfind("specialtitle", 0) == 0)
            {
                std::string numStr = currentSection.substr(12);
                currentSpecialTitle = ParseInt(numStr, -1);
                currentTier = -1;  // Not in a tier section

                if (currentSpecialTitle >= 0 &&
                    currentSpecialTitle <= RenderConstants::MAX_SPECIAL_TITLE_INDEX)
                {
                    // Dynamically grow the SpecialTitles vector
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
            else
            {
                currentTier = -1;  // Non-tier section, switch to global context
                currentSpecialTitle = -1;

                static const std::unordered_set<std::string> kKnownSections = {"",
                                                                               "general",
                                                                               "display",
                                                                               "appearance",
                                                                               "appearancetemplate",
                                                                               "debug",
                                                                               "visual",
                                                                               "fonts",
                                                                               "particles",
                                                                               "occlusion"};
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

        // Parse kv pairs
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

        if (!handled)
        {
            if (key == "Format")
            {
                ParseDisplayFormat(val);
            }
            // Table-driven lookup for all scalar settings.
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

    Generation().fetch_add(1, std::memory_order_release);
}
}  // namespace Settings
