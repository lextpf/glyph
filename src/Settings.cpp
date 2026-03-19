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
/// Single source of truth for EffectType <-> lowercase string mapping.
static constexpr Stl::EnumStringMap<EffectType, 14> kEffectTypeMap{{
    {{"none", EffectType::None},
     {"gradient", EffectType::Gradient},
     {"verticalgradient", EffectType::VerticalGradient},
     {"diagonalgradient", EffectType::DiagonalGradient},
     {"radialgradient", EffectType::RadialGradient},
     {"shimmer", EffectType::Shimmer},
     {"chromaticshimmer", EffectType::ChromaticShimmer},
     {"pulsegradient", EffectType::PulseGradient},
     {"rainbowwave", EffectType::RainbowWave},
     {"conicrainbow", EffectType::ConicRainbow},
     {"aurora", EffectType::Aurora},
     {"sparkle", EffectType::Sparkle},
     {"plasma", EffectType::Plasma},
     {"scanline", EffectType::Scanline}},
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

std::string TitleFormat;
std::vector<Segment> DisplayFormat;

// Tier Definitions
std::vector<TierDefinition> Tiers;

// Special Titles
std::vector<SpecialTitleDefinition> SpecialTitles;

// Distance & Visibility
float FadeStartDistance = 200.0f;
float FadeEndDistance = 2500.0f;
float ScaleStartDistance = 200.0f;
float ScaleEndDistance = 2500.0f;
float MinimumScale = .1f;
float MaxScanDistance = 3000.0f;

// Occlusion Settings
bool EnableOcclusionCulling = true;
float OcclusionSettleTime = .58f;
int OcclusionCheckInterval = 3;

// Visual Effects
float TitleShadowOffsetX = 2.0f;
float TitleShadowOffsetY = 2.0f;
float MainShadowOffsetX = 4.0f;
float MainShadowOffsetY = 4.0f;
float SegmentPadding = 4.0f;

// Outline Settings
float OutlineWidthMin = 2.0f;
float OutlineWidthMax = 2.5f;
bool FastOutlines = false;

// Glow Settings
bool EnableGlow = false;
float GlowRadius = 4.0f;
float GlowIntensity = .5f;
int GlowSamples = 8;

// Typewriter Settings
bool EnableTypewriter = false;
float TypewriterSpeed = 30.0f;
float TypewriterDelay = .0f;

// Debug Settings
bool EnableDebugOverlay = false;

// Side Ornaments
bool EnableOrnaments = true;
float OrnamentScale = 1.0f;
float OrnamentSpacing = 3.0f;

// Particle Aura
bool EnableParticleAura = true;
bool UseParticleTextures = true;
bool EnableStars = true;
bool EnableSparks = false;
bool EnableWisps = false;
bool EnableRunes = false;
bool EnableOrbs = false;
int ParticleCount = 8;
float ParticleSize = 3.0f;
float ParticleSpeed = 1.0f;
float ParticleSpread = 20.0f;
float ParticleAlpha = .8f;
// Textures loaded from subfolders: Data/SKSE/Plugins/glyph/particles/<type>/

// Display Options
float VerticalOffset = 8.0f;
bool HidePlayer = false;
bool HideCreatures = false;
int ReloadKey = 0;  // 0 = disabled, 207 = End key

// Animation
float AnimSpeedLowTier = .35f;
float AnimSpeedMidTier = .20f;
float AnimSpeedHighTier = .10f;

// Color & Effects
float ColorWashAmount = .50f;
float NameColorMix = .35f;
float EffectAlphaMin = .20f;
float EffectAlphaMax = .60f;
float StrengthMin = .15f;
float StrengthMax = .60f;

// Smoothing, settle time in seconds
float AlphaSettleTime = .46f;
float ScaleSettleTime = .46f;
float PositionSettleTime = .38f;

VisualSettings& Visual()
{
    static VisualSettings vs;
    return vs;
}

// Font Settings
std::string NameFontPath;
float NameFontSize = 122.0f;
std::string LevelFontPath;
float LevelFontSize = 61.0f;
std::string TitleFontPath;
float TitleFontSize = 42.0f;

// Ornament font settings
std::string OrnamentFontPath;
float OrnamentFontSize = 64.0f;

// Appearance template settings
std::string TemplateFormID;
std::string TemplatePlugin;
bool UseTemplateAppearance;
bool TemplateIncludeRace;
bool TemplateIncludeBody;
bool TemplateCopyFaceGen = true;       // Load and apply FaceGen mesh/tint
bool TemplateCopySkin = false;         // Copy skin textures (risky for custom bodies)
bool TemplateCopyOverlays = false;     // Copy RaceMenu overlays if available (requires NiOverride)
bool TemplateCopyOutfit = false;       // Copy equipped armor from template actor
bool TemplateReapplyOnReload = false;  // Re-apply appearance on hot reload
std::string TemplateFaceGenPlugin;     // Optional override for FaceGen plugin (empty = auto-detect)

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

/// Single source of truth for all scalar settings.
/// Each row: key, alias, target ptr, default value, validation rule.
static const auto kSettings = std::to_array<SettingEntry>({
    // Distance & Visibility
    {"FadeStartDistance",       "", &FadeStartDistance,     200.0f,   MinFloat{.0f}},
    {"FadeEndDistance",         "", &FadeEndDistance,       2500.0f,  MinFloat{.0f}},
    {"ScaleStartDistance",      "", &ScaleStartDistance,    200.0f,   MinFloat{.0f}},
    {"ScaleEndDistance",        "", &ScaleEndDistance,      2500.0f,  MinFloat{.0f}},
    {"MinimumScale",           "", &MinimumScale,          .1f,      ClampFloat{.01f, 5.0f}},
    {"MaxScanDistance",         "", &MaxScanDistance,       3000.0f,  MinFloat{.0f}},

    // Occlusion
    {"EnableOcclusionCulling",  "", &EnableOcclusionCulling, true,    NoClamping{}},
    {"OcclusionSettleTime",     "", &OcclusionSettleTime,    .58f,    MinFloat{.01f}},
    {"OcclusionCheckInterval",  "", &OcclusionCheckInterval, 3,       MinInt{1}},

    // Shadow & Outline
    {"TitleShadowOffsetX",     "", &TitleShadowOffsetX,    2.0f,     NoClamping{}},
    {"TitleShadowOffsetY",     "", &TitleShadowOffsetY,    2.0f,     NoClamping{}},
    {"MainShadowOffsetX",      "", &MainShadowOffsetX,     4.0f,     NoClamping{}},
    {"MainShadowOffsetY",      "", &MainShadowOffsetY,     4.0f,     NoClamping{}},
    {"SegmentPadding",         "", &SegmentPadding,        4.0f,     NoClamping{}},
    {"OutlineWidthMin",        "", &OutlineWidthMin,       2.0f,     MinFloat{.0f}},
    {"OutlineWidthMax",        "", &OutlineWidthMax,       2.5f,     MinFloat{.0f}},
    {"FastOutlines",           "", &FastOutlines,          false,    NoClamping{}},

    // Glow
    {"EnableGlow",             "", &EnableGlow,            false,    NoClamping{}},
    {"GlowRadius",             "", &GlowRadius,           4.0f,     MinFloat{.0f}},
    {"GlowIntensity",          "", &GlowIntensity,        .5f,      ClampFloat{.0f, 1.0f}},
    {"GlowSamples",            "", &GlowSamples,          8,        ClampInt{1, 64}},

    // Typewriter
    {"EnableTypewriter",       "", &EnableTypewriter,      false,    NoClamping{}},
    {"TypewriterSpeed",        "", &TypewriterSpeed,       30.0f,    MinFloat{.0f}},
    {"TypewriterDelay",        "", &TypewriterDelay,       .0f,      MinFloat{.0f}},

    // Debug
    {"EnableDebugOverlay",     "", &EnableDebugOverlay,    false,    NoClamping{}},

    // Ornaments
    {"EnableOrnaments",        "EnableFlourishes",   &EnableOrnaments,     true,     NoClamping{}},
    {"OrnamentScale",          "FlourishScale",      &OrnamentScale,       1.0f,     NoClamping{}},
    {"OrnamentSpacing",        "FlourishSpacing",    &OrnamentSpacing,     3.0f,     NoClamping{}},

    // Particle Aura
    {"EnableParticleAura",     "", &EnableParticleAura,    true,     NoClamping{}},
    {"UseParticleTextures",    "", &UseParticleTextures,   true,     NoClamping{}},
    {"EnableStars",            "", &EnableStars,           true,     NoClamping{}},
    {"EnableSparks",           "", &EnableSparks,          false,    NoClamping{}},
    {"EnableWisps",            "", &EnableWisps,           false,    NoClamping{}},
    {"EnableRunes",            "", &EnableRunes,           false,    NoClamping{}},
    {"EnableOrbs",             "", &EnableOrbs,            false,    NoClamping{}},
    {"ParticleCount",          "", &ParticleCount,         8,        MinInt{0}},
    {"ParticleSize",           "", &ParticleSize,          3.0f,     MinFloat{.0f}},
    {"ParticleSpeed",          "", &ParticleSpeed,         1.0f,     MinFloat{.0f}},
    {"ParticleSpread",         "", &ParticleSpread,        20.0f,    MinFloat{.0f}},
    {"ParticleAlpha",          "", &ParticleAlpha,         .8f,      ClampFloat{.0f, 1.0f}},

    // Display Options
    {"VerticalOffset",         "", &VerticalOffset,        8.0f,     NoClamping{}},
    {"HidePlayer",             "", &HidePlayer,            false,    NoClamping{}},
    {"HideCreatures",          "", &HideCreatures,         false,    NoClamping{}},
    {"ReloadKey",              "", &ReloadKey,             0,        NoClamping{}},

    // Animation Speed
    {"AnimSpeedLowTier",       "", &AnimSpeedLowTier,     .35f,     NoClamping{}},
    {"AnimSpeedMidTier",       "", &AnimSpeedMidTier,     .20f,     NoClamping{}},
    {"AnimSpeedHighTier",      "", &AnimSpeedHighTier,    .10f,     NoClamping{}},

    // Color & Effects
    {"ColorWashAmount",        "", &ColorWashAmount,       .50f,     ClampFloat{.0f, 1.0f}},
    {"NameColorMix",           "", &NameColorMix,          .35f,     ClampFloat{.0f, 1.0f}},
    {"EffectAlphaMin",         "", &EffectAlphaMin,        .20f,     ClampFloat{.0f, 1.0f}},
    {"EffectAlphaMax",         "", &EffectAlphaMax,        .60f,     ClampFloat{.0f, 1.0f}},
    {"StrengthMin",            "", &StrengthMin,           .15f,     MinFloat{.0f}},
    {"StrengthMax",            "", &StrengthMax,           .60f,     MinFloat{.0f}},

    // Smoothing
    {"AlphaSettleTime",        "", &AlphaSettleTime,       .46f,     MinFloat{.01f}},
    {"ScaleSettleTime",        "", &ScaleSettleTime,       .46f,     MinFloat{.01f}},
    {"PositionSettleTime",     "", &PositionSettleTime,    .38f,     MinFloat{.01f}},

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
    {"EnableTierEffectGating", "", &Visual().EnableTierEffectGating, false, NoClamping{}},
    {"GlowMinTier",            "", &Visual().GlowMinTier,         5,       NoClamping{}},
    {"ParticleMinTier",        "", &Visual().ParticleMinTier,     10,      NoClamping{}},
    {"OrnamentMinTier",        "", &Visual().OrnamentMinTier,     10,      NoClamping{}},

    // Fonts
    {"NameFontPath",           "", &NameFontPath,          std::string(kDefaultNameFontPath),    NoClamping{}},
    {"NameFontSize",           "", &NameFontSize,          122.0f,   NoClamping{}},
    {"LevelFontPath",          "", &LevelFontPath,         std::string(kDefaultLevelFontPath),   NoClamping{}},
    {"LevelFontSize",          "", &LevelFontSize,         61.0f,    NoClamping{}},
    {"TitleFontPath",          "", &TitleFontPath,         std::string(kDefaultTitleFontPath),   NoClamping{}},
    {"TitleFontSize",          "", &TitleFontSize,         42.0f,    NoClamping{}},
    {"OrnamentFontPath",       "", &OrnamentFontPath,      std::string(kDefaultOrnamentFontPath), NoClamping{}},
    {"OrnamentFontSize",       "", &OrnamentFontSize,      64.0f,    NoClamping{}},

    // Appearance Template
    {"TemplateFormID",         "", &TemplateFormID,        std::string(),  NoClamping{}},
    {"TemplatePlugin",         "", &TemplatePlugin,        std::string(),  NoClamping{}},
    {"UseTemplateAppearance",  "", &UseTemplateAppearance, false,    NoClamping{}},
    {"TemplateIncludeRace",    "", &TemplateIncludeRace,   false,    NoClamping{}},
    {"TemplateIncludeBody",    "", &TemplateIncludeBody,   false,    NoClamping{}},
    {"TemplateCopyFaceGen",    "", &TemplateCopyFaceGen,   true,     NoClamping{}},
    {"TemplateCopySkin",       "", &TemplateCopySkin,      false,    NoClamping{}},
    {"TemplateCopyOverlays",   "", &TemplateCopyOverlays,  false,    NoClamping{}},
    {"TemplateCopyOutfit",     "", &TemplateCopyOutfit,    false,    NoClamping{}},
    {"TemplateReapplyOnReload","", &TemplateReapplyOnReload, false,  NoClamping{}},
    {"TemplateFaceGenPlugin",  "", &TemplateFaceGenPlugin, std::string(),  NoClamping{}},
});

// clang-format on

/// Lazily-built lookup map: lowercase key -> SettingEntry pointer.
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

/// Apply a parsed string value to the correct typed target.
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

/// Reset all table-driven settings to their defaults.
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

/// Apply validation rules from the table.
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

/// Canonicalize a key for tier/special-title section fields (not in the table).
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
    TitleFormat = "%t";
    DisplayFormat = {{"%n", false}, {" Lv.%l", true}};

    Tiers.clear();
    Tiers.push_back(MakeDefaultTier());
    SpecialTitles.clear();

    // All scalar settings are reset from the descriptor table.
    ResetTableDefaults();
}

static void ClampAndValidate()
{
    // Apply per-setting validation from the descriptor table.
    ValidateTableSettings();

    // Cross-field constraints that cannot be expressed per-setting.
    FadeEndDistance = std::max(FadeStartDistance + 1.0f, FadeEndDistance);
    ScaleEndDistance = std::max(ScaleStartDistance + 1.0f, ScaleEndDistance);
    OutlineWidthMax = std::max(OutlineWidthMin, OutlineWidthMax);

    if (EffectAlphaMin > EffectAlphaMax)
    {
        std::swap(EffectAlphaMin, EffectAlphaMax);
    }
    if (StrengthMin > StrengthMax)
    {
        std::swap(StrengthMin, StrengthMax);
    }

    if (Tiers.empty())
    {
        Tiers.push_back(MakeDefaultTier());
    }

    for (auto& tier : Tiers)
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

    for (auto& special : SpecialTitles)
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
                    while (static_cast<int>(Tiers.size()) <= currentTier)
                    {
                        Tiers.emplace_back();
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
                    while (static_cast<int>(SpecialTitles.size()) <= currentSpecialTitle)
                    {
                        SpecialTitleDefinition newSpecial;
                        newSpecial.keyword = "";
                        newSpecial.displayTitle = "";
                        newSpecial.color = Color3::White();
                        newSpecial.glowColor = Color3::White();
                        newSpecial.forceOrnaments = true;
                        newSpecial.forceParticles = true;
                        newSpecial.priority = 0;
                        SpecialTitles.push_back(newSpecial);
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

        // If we're inside a [TierN] section, all kv pairs apply to that tier
        // This allows each tier to have its own level range, colors, and effects
        if (currentTier >= 0 && currentTier < static_cast<int>(Tiers.size()))
        {
            if (key == "Name")
            {
                // "Name" is displayed as the tier title
                Tiers[currentTier].title = val;
                handled = true;
            }
            else if (key == "MinLevel")
            {
                // Inclusive lower bound for this tier's level range
                const int parsed = ParseInt(val, 1);
                const int clamped =
                    std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
                Tiers[currentTier].minLevel = static_cast<uint16_t>(clamped);
                handled = true;
            }
            else if (key == "MaxLevel")
            {
                // Inclusive upper bound for this tier's level range
                const int parsed = ParseInt(val, 25);
                const int clamped =
                    std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
                Tiers[currentTier].maxLevel = static_cast<uint16_t>(clamped);
                handled = true;
            }
            else if (key == "LeftColor")
            {
                // Left/start color for gradient effects (RGB floats 0.0-1.0)
                ParseColor3(val, Tiers[currentTier].leftColor);
                handled = true;
            }
            else if (key == "RightColor")
            {
                // Right/end color for gradient effects
                ParseColor3(val, Tiers[currentTier].rightColor);
                handled = true;
            }
            else if (key == "HighlightColor")
            {
                // Accent color for shimmer, sparkle, and scanline effects
                ParseColor3(val, Tiers[currentTier].highlightColor);
                handled = true;
            }
            else if (key == "TitleEffect" || key == "NameEffect" || key == "LevelEffect")
            {
                // Effect syntax: "EffectType param1,param2,param3,param4,param5 [whiteBase]"
                // Example: "RainbowWave 0.15,0.30,0.60,0.40,1.00 whiteBase"

                std::istringstream ss(val);
                std::string effectTypeName;
                ss >> effectTypeName;  // Extract first word (effect type name)

                // Select which effect struct to populate based on key name
                EffectParams& effect = (key == "TitleEffect")  ? Tiers[currentTier].titleEffect
                                       : (key == "NameEffect") ? Tiers[currentTier].nameEffect
                                                               : Tiers[currentTier].levelEffect;

                effect.type = ParseEffectType(effectTypeName);

                // Get the rest of the line
                std::string paramsStr;
                std::getline(ss, paramsStr);
                paramsStr = Trim(paramsStr);

                // Check for "whiteBase" keyword
                // When enabled, renders white text layer under effects
                std::string paramsLower = ToLowerAscii(paramsStr);
                size_t wbPos = paramsLower.find("whitebase");
                if (wbPos != std::string::npos)
                {
                    effect.useWhiteBase = true;
                    paramsStr = paramsStr.substr(0, wbPos);  // Strip flag from params
                }

                // Parse comma-separated parameters (up to 5 floats)
                // Different effects interpret these differently:
                //   Shimmer: param1=bandWidth, param2=strength
                //   Aurora: param1=speed, param2=waves, param3=intensity, param4=sway
                //   RainbowWave: param1=baseHue, param2=hueSpread, param3=speed, param4=sat,
                //   param5=val
                std::istringstream paramStream(paramsStr);
                std::string token;
                int paramIdx = 0;
                while (std::getline(paramStream, token, ',') && paramIdx < 5)
                {
                    token = Trim(token);
                    if (token.empty())
                    {
                        continue;  // Skip empty tokens (e.g., "1.0,,2.0")
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
                handled = true;
            }
            else if (key == "Ornaments")
            {
                // Ornament format: "LEFT, RIGHT" (e.g., "AC, BD")
                size_t commaPos = val.find(',');
                if (commaPos != std::string::npos)
                {
                    // New format: split on comma
                    Tiers[currentTier].leftOrnaments = Trim(val.substr(0, commaPos));
                    Tiers[currentTier].rightOrnaments = Trim(val.substr(commaPos + 1));
                }
                else if (val.length() >= 2)
                {
                    // Legacy format: first char = left, second char = right
                    Tiers[currentTier].leftOrnaments = val.substr(0, 1);
                    Tiers[currentTier].rightOrnaments = val.substr(1, 1);
                }
                else
                {
                    Tiers[currentTier].leftOrnaments.clear();
                    Tiers[currentTier].rightOrnaments.clear();
                }
                handled = true;
            }
            else if (key == "ParticleTypes")
            {
                // Particle types for this tier (e.g., "Stars,Wisps,Orbs")
                Tiers[currentTier].particleTypes = val;
                handled = true;
            }
            else if (key == "ParticleCount")
            {
                // Number of particles for this tier (0 = use global)
                Tiers[currentTier].particleCount = ParseInt(val, 0);
                handled = true;
            }
        }

        // If we're inside a [SpecialTitleN] section, parse special title properties
        if (!handled && currentSpecialTitle >= 0 &&
            currentSpecialTitle < static_cast<int>(SpecialTitles.size()))
        {
            if (key == "Keyword")
            {
                // Text to match in actor name (case-insensitive)
                SpecialTitles[currentSpecialTitle].keyword = val;
                handled = true;
            }
            else if (key == "DisplayTitle")
            {
                // Title shown above name
                SpecialTitles[currentSpecialTitle].displayTitle = val;
                handled = true;
            }
            else if (key == "Color")
            {
                // RGB color for nameplate
                ParseColor3(val, SpecialTitles[currentSpecialTitle].color);
                handled = true;
            }
            else if (key == "GlowColor")
            {
                // RGB color for glow effect
                ParseColor3(val, SpecialTitles[currentSpecialTitle].glowColor);
                handled = true;
            }
            else if (key == "ForceOrnaments" || key == "ForceFlourishes")
            {
                SpecialTitles[currentSpecialTitle].forceOrnaments = ParseBool(val);
                handled = true;
            }
            else if (key == "ForceParticles")
            {
                // Always show particle aura
                SpecialTitles[currentSpecialTitle].forceParticles = ParseBool(val);
                handled = true;
            }
            else if (key == "Priority")
            {
                // Higher priority = checked first
                SpecialTitles[currentSpecialTitle].priority = ParseInt(val, 0);
                handled = true;
            }
            else if (key == "Ornaments")
            {
                // Ornament format: "LEFT, RIGHT" (e.g., "AC, BD")
                size_t commaPos = val.find(',');
                if (commaPos != std::string::npos)
                {
                    SpecialTitles[currentSpecialTitle].leftOrnaments =
                        Trim(val.substr(0, commaPos));
                    SpecialTitles[currentSpecialTitle].rightOrnaments =
                        Trim(val.substr(commaPos + 1));
                }
                else if (val.length() >= 2)
                {
                    SpecialTitles[currentSpecialTitle].leftOrnaments = val.substr(0, 1);
                    SpecialTitles[currentSpecialTitle].rightOrnaments = val.substr(1, 1);
                }
                else
                {
                    SpecialTitles[currentSpecialTitle].leftOrnaments.clear();
                    SpecialTitles[currentSpecialTitle].rightOrnaments.clear();
                }
                handled = true;
            }
        }

        if (!handled)
        {
            // Format key requires special parsing because it contains multiple quoted segments
            // Syntax: Format = "%t" "%n" "Lv.%l"
            if (key == "Format")
            {
                std::vector<Segment> newDisplayFormat;
                std::string newTitleFormat;
                bool titleFound = false;

                // Simple state machine to parse quoted strings
                // Handles escaped quotes (\") within strings
                bool inQuote = false;
                std::string current;
                for (size_t i = 0; i < val.size(); ++i)
                {
                    char c = val[i];

                    // Handle escape sequences (\" -> literal quote, etc.)
                    if (c == '\\' && i + 1 < val.size())
                    {
                        if (inQuote)
                        {
                            // Inside quotes, add the escaped character literally
                            current += val[++i];
                        }
                        // Outside quotes, skip escape sequences
                        continue;
                    }

                    if (c == '"')
                    {
                        if (inQuote)
                        {
                            // Closing quote, finish current segment
                            if (current.find("%t") != std::string::npos)
                            {
                                // Segment contains %t -> it's the title format, separate line
                                newTitleFormat = current;
                                titleFound = true;
                            }
                            else
                            {
                                // Regular segment -> add to main line
                                // Use level font if segment contains %l
                                bool isLevel = current.find("%l") != std::string::npos;
                                newDisplayFormat.push_back({current, isLevel});
                            }
                            current.clear();
                            inQuote = false;
                        }
                        else
                        {
                            // Opening quote, start new segment
                            inQuote = true;
                        }
                    }
                    else if (inQuote)
                    {
                        // Inside quotes, accumulate characters
                        current += c;
                    }
                    // Characters outside quotes are ignored
                }

                // Only update if we actually parsed something
                // This prevents empty Format= from clearing the defaults
                if (titleFound)
                {
                    TitleFormat = newTitleFormat;
                }
                if (!newDisplayFormat.empty())
                {
                    DisplayFormat = newDisplayFormat;
                }
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
}
}  // namespace Settings
