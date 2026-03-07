#include "Settings.h"
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Settings
{
    // Parser helper forward declaration (used before definition).
    static std::string Trim(const std::string& str);

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
    float FadeStartDistance;
    float FadeEndDistance;
    float ScaleStartDistance;
    float ScaleEndDistance;
    float MinimumScale;
    float MaxScanDistance;

    // Occlusion Settings
    bool  EnableOcclusionCulling = true;
    float OcclusionSettleTime = 0.58f;
    int   OcclusionCheckInterval = 3;

    // Visual Effects
    float TitleShadowOffsetX;
    float TitleShadowOffsetY;
    float MainShadowOffsetX;
    float MainShadowOffsetY;
    float SegmentPadding;
    
    // Outline Settings
    float OutlineWidthMin;
    float OutlineWidthMax;
    bool  FastOutlines = false;

    // Glow Settings
    bool  EnableGlow = false;
    float GlowRadius = 4.0f;
    float GlowIntensity = 0.5f;
    int   GlowSamples = 8;

    // Typewriter Settings
    bool  EnableTypewriter = false;
    float TypewriterSpeed = 30.0f;
    float TypewriterDelay = 0.0f;

    // Debug Settings
    bool  EnableDebugOverlay = false;

    // Side Ornaments
    bool  EnableOrnaments = true;
    float OrnamentScale = 1.0f;
    float OrnamentSpacing = 3.0f;

    // Particle Aura
    bool  EnableParticleAura = true;
    bool  UseParticleTextures = true;
    bool  EnableStars = true;
    bool  EnableSparks = false;
    bool  EnableWisps = false;
    bool  EnableRunes = false;
    bool  EnableOrbs = false;
    int   ParticleCount = 8;
    float ParticleSize = 3.0f;
    float ParticleSpeed = 1.0f;
    float ParticleSpread = 20.0f;
    float ParticleAlpha = 0.8f;
    // Textures loaded from subfolders: Data/SKSE/Plugins/glyph/particles/<type>/

    // Display Options
    float VerticalOffset = 8.0f;
    bool  HidePlayer = false;
    bool  HideCreatures = false;
    int   ReloadKey = 0;  // 0 = disabled, 207 = End key

    // Animation
    float AnimSpeedLowTier;
    float AnimSpeedMidTier;
    float AnimSpeedHighTier;

    // Color & Effects
    float ColorWashAmount;
    float NameColorMix;
    float EffectAlphaMin;
    float EffectAlphaMax;
    float StrengthMin;
    float StrengthMax;

    // Smoothing, settle time in seconds
    float AlphaSettleTime;
    float ScaleSettleTime;
    float PositionSettleTime;

    VisualSettings& Visual() {
        static VisualSettings vs;
        return vs;
    }

    // Font Settings
    std::string NameFontPath;
    float NameFontSize;
    std::string LevelFontPath;
    float LevelFontSize;
    std::string TitleFontPath;
    float TitleFontSize;

    // Ornament font settings
    std::string OrnamentFontPath;
    float OrnamentFontSize;

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

    static TierDefinition MakeDefaultTier()
    {
        TierDefinition tier{};
        tier.minLevel = 1;
        tier.maxLevel = 250;
        tier.title = "Unknown";
        tier.leftColor[0] = tier.leftColor[1] = tier.leftColor[2] = 1.0f;
        tier.rightColor[0] = tier.rightColor[1] = tier.rightColor[2] = 1.0f;
        tier.highlightColor[0] = tier.highlightColor[1] = tier.highlightColor[2] = 1.0f;
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
        for (auto& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    static std::string CanonicalizeKey(const std::string& rawKey)
    {
        static const std::unordered_map<std::string, std::string> kKeyMap = {
            // Tier keys
            { "name", "Name" },
            { "title", "Name" },  // Alias for tier title
            { "minlevel", "MinLevel" },
            { "maxlevel", "MaxLevel" },
            { "leftcolor", "LeftColor" },
            { "rightcolor", "RightColor" },
            { "highlightcolor", "HighlightColor" },
            { "titleeffect", "TitleEffect" },
            { "nameeffect", "NameEffect" },
            { "leveleffect", "LevelEffect" },
            { "ornaments", "Ornaments" },
            { "particletypes", "ParticleTypes" },
            { "particlecount", "ParticleCount" },

            // Special title keys
            { "keyword", "Keyword" },
            { "displaytitle", "DisplayTitle" },
            { "color", "Color" },
            { "glowcolor", "GlowColor" },
            { "forceornaments", "ForceOrnaments" },
            { "forceflourishes", "ForceFlourishes" },
            { "forceparticles", "ForceParticles" },
            { "priority", "Priority" },

            // Display
            { "format", "Format" },

            // Global settings
            { "fadestartdistance", "FadeStartDistance" },
            { "fadeenddistance", "FadeEndDistance" },
            { "scalestartdistance", "ScaleStartDistance" },
            { "scaleenddistance", "ScaleEndDistance" },
            { "minimumscale", "MinimumScale" },
            { "maxscandistance", "MaxScanDistance" },

            { "enableocclusionculling", "EnableOcclusionCulling" },
            { "occlusionsettletime", "OcclusionSettleTime" },
            { "occlusioncheckinterval", "OcclusionCheckInterval" },

            { "titleshadowoffsetx", "TitleShadowOffsetX" },
            { "titleshadowoffsety", "TitleShadowOffsetY" },
            { "mainshadowoffsetx", "MainShadowOffsetX" },
            { "mainshadowoffsety", "MainShadowOffsetY" },
            { "segmentpadding", "SegmentPadding" },

            { "outlinewidthmin", "OutlineWidthMin" },
            { "outlinewidthmax", "OutlineWidthMax" },
            { "fastoutlines", "FastOutlines" },

            { "enableglow", "EnableGlow" },
            { "glowradius", "GlowRadius" },
            { "glowintensity", "GlowIntensity" },
            { "glowsamples", "GlowSamples" },

            { "enabletypewriter", "EnableTypewriter" },
            { "typewriterspeed", "TypewriterSpeed" },
            { "typewriterdelay", "TypewriterDelay" },

            { "enabledebugoverlay", "EnableDebugOverlay" },

            { "enableornaments", "EnableOrnaments" },
            { "enableflourishes", "EnableFlourishes" },
            { "ornamentscale", "OrnamentScale" },
            { "flourishscale", "FlourishScale" },
            { "ornamentspacing", "OrnamentSpacing" },
            { "flourishspacing", "FlourishSpacing" },

            { "enableparticleaura", "EnableParticleAura" },
            { "useparticletextures", "UseParticleTextures" },
            { "enablestars", "EnableStars" },
            { "enablesparks", "EnableSparks" },
            { "enablewisps", "EnableWisps" },
            { "enablerunes", "EnableRunes" },
            { "enableorbs", "EnableOrbs" },
            { "particlesize", "ParticleSize" },
            { "particlespeed", "ParticleSpeed" },
            { "particlespread", "ParticleSpread" },
            { "particlealpha", "ParticleAlpha" },

            { "verticaloffset", "VerticalOffset" },
            { "hideplayer", "HidePlayer" },
            { "hidecreatures", "HideCreatures" },
            { "reloadkey", "ReloadKey" },

            { "animspeedlowtier", "AnimSpeedLowTier" },
            { "animspeedmidtier", "AnimSpeedMidTier" },
            { "animspeedhightier", "AnimSpeedHighTier" },

            { "colorwashamount", "ColorWashAmount" },
            { "namecolormix", "NameColorMix" },
            { "effectalphamin", "EffectAlphaMin" },
            { "effectalphamax", "EffectAlphaMax" },
            { "strengthmin", "StrengthMin" },
            { "strengthmax", "StrengthMax" },

            { "alphasettletime", "AlphaSettleTime" },
            { "scalesettletime", "ScaleSettleTime" },
            { "positionsettletime", "PositionSettleTime" },

            // Visual sub-settings
            { "enabledistanceoutlinescale", "EnableDistanceOutlineScale" },
            { "outlinedistancemin", "OutlineDistanceMin" },
            { "outlinedistancemax", "OutlineDistanceMax" },
            { "minimumpixelheight", "MinimumPixelHeight" },
            { "enablelod", "EnableLOD" },
            { "lodfardistance", "LODFarDistance" },
            { "lodmiddistance", "LODMidDistance" },
            { "lodtransitionrange", "LODTransitionRange" },
            { "titlealphamultiplier", "TitleAlphaMultiplier" },
            { "levelalphamultiplier", "LevelAlphaMultiplier" },
            { "enableoverlapprevention", "EnableOverlapPrevention" },
            { "overlappaddingy", "OverlapPaddingY" },
            { "overlapiterations", "OverlapIterations" },
            { "positionsmoothingblend", "PositionSmoothingBlend" },
            { "largemovementthreshold", "LargeMovementThreshold" },
            { "largemovementblend", "LargeMovementBlend" },
            { "enabletiereffectgating", "EnableTierEffectGating" },
            { "glowmintier", "GlowMinTier" },
            { "particlemintier", "ParticleMinTier" },
            { "ornamentmintier", "OrnamentMinTier" },

            // Fonts
            { "namefontpath", "NameFontPath" },
            { "namefontsize", "NameFontSize" },
            { "levelfontpath", "LevelFontPath" },
            { "levelfontsize", "LevelFontSize" },
            { "titlefontpath", "TitleFontPath" },
            { "titlefontsize", "TitleFontSize" },
            { "ornamentfontpath", "OrnamentFontPath" },
            { "ornamentfontsize", "OrnamentFontSize" },

            // Appearance template
            { "templateformid", "TemplateFormID" },
            { "templateplugin", "TemplatePlugin" },
            { "usetemplateappearance", "UseTemplateAppearance" },
            { "templateincluderace", "TemplateIncludeRace" },
            { "templateincludebody", "TemplateIncludeBody" },
            { "templatecopyfacegen", "TemplateCopyFaceGen" },
            { "templatecopyskin", "TemplateCopySkin" },
            { "templatecopyoverlays", "TemplateCopyOverlays" },
            { "templatecopyoutfit", "TemplateCopyOutfit" },
            { "templatereapplyonreload", "TemplateReapplyOnReload" },
            { "templatefacegenplugin", "TemplateFaceGenPlugin" },
        };

        const std::string lowered = ToLowerAscii(Trim(rawKey));
        if (const auto it = kKeyMap.find(lowered); it != kKeyMap.end()) {
            return it->second;
        }
        return Trim(rawKey);
    }

    static void ResetToDefaults()
    {
        TitleFormat = "%t";
        DisplayFormat = { { "%n", false }, { " Lv.%l", true } };

        Tiers.clear();
        Tiers.push_back(MakeDefaultTier());
        SpecialTitles.clear();

        FadeStartDistance = 200.0f;
        FadeEndDistance = 2500.0f;
        ScaleStartDistance = 200.0f;
        ScaleEndDistance = 2500.0f;
        MinimumScale = 0.1f;
        MaxScanDistance = 3000.0f;

        EnableOcclusionCulling = true;
        OcclusionSettleTime = 0.58f;
        OcclusionCheckInterval = 3;

        TitleShadowOffsetX = 2.0f;
        TitleShadowOffsetY = 2.0f;
        MainShadowOffsetX = 4.0f;
        MainShadowOffsetY = 4.0f;
        SegmentPadding = 4.0f;

        OutlineWidthMin = 2.0f;
        OutlineWidthMax = 2.5f;
        FastOutlines = false;

        EnableGlow = false;
        GlowRadius = 4.0f;
        GlowIntensity = 0.5f;
        GlowSamples = 8;

        EnableTypewriter = false;
        TypewriterSpeed = 30.0f;
        TypewriterDelay = 0.0f;

        EnableDebugOverlay = false;

        EnableOrnaments = true;
        OrnamentScale = 1.0f;
        OrnamentSpacing = 3.0f;

        EnableParticleAura = true;
        UseParticleTextures = true;
        EnableStars = true;
        EnableSparks = false;
        EnableWisps = false;
        EnableRunes = false;
        EnableOrbs = false;
        ParticleCount = 8;
        ParticleSize = 3.0f;
        ParticleSpeed = 1.0f;
        ParticleSpread = 20.0f;
        ParticleAlpha = 0.8f;

        VerticalOffset = 8.0f;
        HidePlayer = false;
        HideCreatures = false;
        ReloadKey = 0;

        AnimSpeedLowTier = 0.35f;
        AnimSpeedMidTier = 0.20f;
        AnimSpeedHighTier = 0.10f;

        ColorWashAmount = 0.50f;
        NameColorMix = 0.35f;
        EffectAlphaMin = 0.20f;
        EffectAlphaMax = 0.60f;
        StrengthMin = 0.15f;
        StrengthMax = 0.60f;

        AlphaSettleTime = 0.46f;
        ScaleSettleTime = 0.46f;
        PositionSettleTime = 0.38f;

        Visual() = VisualSettings{};

        NameFontPath = "Data/SKSE/Plugins/glyph/fonts/bd1aab18-7649-4946-9f7b-6ddd6a81311d.ttf";
        NameFontSize = 122.0f;
        LevelFontPath = "Data/SKSE/Plugins/glyph/fonts/96120cca-4be2-4d10-b10a-b8183ac18467.ttf";
        LevelFontSize = 61.0f;
        TitleFontPath = "Data/SKSE/Plugins/glyph/fonts/56cb786e-c94e-452c-ac54-360c46381de1.ttf";
        TitleFontSize = 42.0f;
        OrnamentFontPath = "Data/SKSE/Plugins/glyph/fonts/050986eb-c23a-4891-a951-9fed313e44c2.otf";
        OrnamentFontSize = 64.0f;

        TemplateFormID.clear();
        TemplatePlugin.clear();
        UseTemplateAppearance = false;
        TemplateIncludeRace = false;
        TemplateIncludeBody = false;
        TemplateCopyFaceGen = true;
        TemplateCopySkin = false;
        TemplateCopyOverlays = false;
        TemplateCopyOutfit = false;
        TemplateReapplyOnReload = false;
        TemplateFaceGenPlugin.clear();
    }

    static void ClampAndValidate()
    {
        FadeStartDistance = std::max(0.0f, FadeStartDistance);
        FadeEndDistance = std::max(FadeStartDistance + 1.0f, FadeEndDistance);

        ScaleStartDistance = std::max(0.0f, ScaleStartDistance);
        ScaleEndDistance = std::max(ScaleStartDistance + 1.0f, ScaleEndDistance);
        MinimumScale = std::clamp(MinimumScale, 0.01f, 5.0f);
        MaxScanDistance = std::max(0.0f, MaxScanDistance);

        OcclusionSettleTime = std::max(0.01f, OcclusionSettleTime);
        OcclusionCheckInterval = std::max(1, OcclusionCheckInterval);

        GlowRadius = std::max(0.0f, GlowRadius);
        GlowIntensity = std::clamp(GlowIntensity, 0.0f, 1.0f);
        GlowSamples = std::clamp(GlowSamples, 1, 64);

        TypewriterSpeed = std::max(0.0f, TypewriterSpeed);
        TypewriterDelay = std::max(0.0f, TypewriterDelay);

        ParticleCount = std::max(0, ParticleCount);
        ParticleSize = std::max(0.0f, ParticleSize);
        ParticleSpeed = std::max(0.0f, ParticleSpeed);
        ParticleSpread = std::max(0.0f, ParticleSpread);
        ParticleAlpha = std::clamp(ParticleAlpha, 0.0f, 1.0f);

        ColorWashAmount = std::clamp(ColorWashAmount, 0.0f, 1.0f);
        NameColorMix = std::clamp(NameColorMix, 0.0f, 1.0f);
        EffectAlphaMin = std::clamp(EffectAlphaMin, 0.0f, 1.0f);
        EffectAlphaMax = std::clamp(EffectAlphaMax, 0.0f, 1.0f);
        StrengthMin = std::max(0.0f, StrengthMin);
        StrengthMax = std::max(0.0f, StrengthMax);

        AlphaSettleTime = std::max(0.01f, AlphaSettleTime);
        ScaleSettleTime = std::max(0.01f, ScaleSettleTime);
        PositionSettleTime = std::max(0.01f, PositionSettleTime);

        Visual().OverlapIterations = std::clamp(Visual().OverlapIterations, 1, 16);
        Visual().LODTransitionRange = std::max(1.0f, Visual().LODTransitionRange);
        Visual().PositionSmoothingBlend = std::clamp(Visual().PositionSmoothingBlend, 0.0f, 1.0f);
        Visual().LargeMovementThreshold = std::max(0.0f, Visual().LargeMovementThreshold);
        Visual().LargeMovementBlend = std::clamp(Visual().LargeMovementBlend, 0.0f, 1.0f);

        if (Tiers.empty()) {
            Tiers.push_back(MakeDefaultTier());
        }

        for (auto& tier : Tiers) {
            if (tier.maxLevel < tier.minLevel) {
                std::swap(tier.maxLevel, tier.minLevel);
            }
            tier.particleCount = std::max(0, tier.particleCount);
            for (int i = 0; i < 3; ++i) {
                tier.leftColor[i] = std::clamp(tier.leftColor[i], 0.0f, 1.0f);
                tier.rightColor[i] = std::clamp(tier.rightColor[i], 0.0f, 1.0f);
                tier.highlightColor[i] = std::clamp(tier.highlightColor[i], 0.0f, 1.0f);
            }
        }

        for (auto& special : SpecialTitles) {
            special.keyword = Trim(special.keyword);
            special.keywordLower = ToLowerAscii(special.keyword);
            for (int i = 0; i < 3; ++i) {
                special.color[i] = std::clamp(special.color[i], 0.0f, 1.0f);
                special.glowColor[i] = std::clamp(special.glowColor[i], 0.0f, 1.0f);
            }
        }
    }

    // Helper function: Remove leading/trailing whitespace
    static std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) return str;
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Helper function: strip inline comments while preserving quoted text.
    static std::string StripInlineComment(const std::string& str) {
        bool inQuote = false;
        bool escaped = false;
        for (size_t i = 0; i < str.size(); ++i) {
            const char c = str[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\' && inQuote) {
                escaped = true;
                continue;
            }
            if (c == '"') {
                inQuote = !inQuote;
                continue;
            }
            if (!inQuote && (c == ';' || c == '#')) {
                return Trim(str.substr(0, i));
            }
        }
        return Trim(str);
    }

    static std::string StripUtf8Bom(const std::string& str) {
        if (str.size() >= 3 &&
            static_cast<unsigned char>(str[0]) == 0xEF &&
            static_cast<unsigned char>(str[1]) == 0xBB &&
            static_cast<unsigned char>(str[2]) == 0xBF) {
            return str.substr(3);
        }
        return str;
    }

    // Helper function: Parse float with fallback default
    static float ParseFloat(const std::string& str, float defaultVal) {
        try {
            return std::stof(str);
        } catch (...) {
            // Return default if parsing fails
            return defaultVal;
        }
    }

    // Helper function: Parse integer with fallback default
    static int ParseInt(const std::string& str, int defaultVal) {
        try {
            return std::stoi(str);
        } catch (...) {
            return defaultVal;
        }
    }

    // Helper function: Parse boolean (true/false, 1/0, yes/no)
    static bool ParseBool(const std::string& str) {
        std::string lower = str;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return (lower == "true" || lower == "1" || lower == "yes" || lower == "on" || lower == "enabled");
    }

    // Helper function: Parse comma-separated RGB color (0.0-1.0)
    static void ParseColor3(const std::string& str, float out[3]) {
        std::istringstream ss(str);
        std::string token;
        int idx = 0;
        // Split by comma and parse each component
        while (std::getline(ss, token, ',') && idx < 3) {
            out[idx++] = ParseFloat(Trim(token), 1.0f);
        }
    }

    // Helper function: Parse effect type name to enum
    static EffectType ParseEffectType(const std::string& str) {
        std::string s = Trim(str);
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        // Map string names to effect types
        if (s == "none") return EffectType::None;
        if (s == "gradient") return EffectType::Gradient;
        if (s == "verticalgradient") return EffectType::VerticalGradient;
        if (s == "diagonalgradient") return EffectType::DiagonalGradient;
        if (s == "radialgradient") return EffectType::RadialGradient;
        if (s == "shimmer") return EffectType::Shimmer;
        if (s == "chromaticshimmer") return EffectType::ChromaticShimmer;
        if (s == "pulsegradient") return EffectType::PulseGradient;
        if (s == "rainbowwave") return EffectType::RainbowWave;
        if (s == "conicrainbow") return EffectType::ConicRainbow;
        if (s == "aurora") return EffectType::Aurora;
        if (s == "sparkle") return EffectType::Sparkle;
        if (s == "plasma") return EffectType::Plasma;
        if (s == "scanline") return EffectType::Scanline;
        // Default to simple gradient if unknown
        return EffectType::Gradient;
    }

    void Load()
    {
        std::unique_lock<std::shared_mutex> settingsWriteLock(Mutex());

        ResetToDefaults();

        // File is located in Skyrim's Data folder under SKSE plugins directory
        std::ifstream file("Data/SKSE/Plugins/glyph.ini");
        if (!file.is_open()) {
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

        auto addWarning = [&](size_t lineNo, const std::string& message) {
            constexpr size_t kMaxWarnings = 48;
            if (parseWarnings.size() < kMaxWarnings) {
                std::ostringstream ss;
                ss << "L" << lineNo << ": " << message;
                parseWarnings.push_back(ss.str());
            }
        };

        // Line-by-line parsing allows for flexible format with sections
        while (std::getline(file, line)) {
            const size_t currentLineNumber = lineNumber + 1;
            if (lineNumber++ == 0) {
                line = StripUtf8Bom(line);
            }
            line = Trim(line);
            line = StripInlineComment(line);

            // Skip empty lines and comments (both ; and # style for user convenience)
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            // Detect section headers like [Tier0], [Tier1], etc.
            // Section headers change the parsing context for subsequent kv pairs
            if (line.size() >= 2 && line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                currentSection = Trim(currentSection);
                currentSectionLower = ToLowerAscii(currentSection);

                // Tier numbers are 0-indexed and can be any non-negative integer
                if (currentSectionLower.size() >= 4 && currentSectionLower.rfind("tier", 0) == 0) {
                    std::string numStr = currentSection.substr(4);
                    currentTier = ParseInt(numStr, -1);
                    currentSpecialTitle = -1;  // Not in a special title section

                    if (currentTier < 0) {
                        currentTier = -1;  // Invalid tier number, treat as non-tier section
                        addWarning(currentLineNumber, "Invalid tier section '" + currentSection + "'");
                    } else {
                        // Dynamically grow the Tiers vector to accommodate the specified tier
                        while (static_cast<int>(Tiers.size()) <= currentTier) {
                            TierDefinition newTier;
                            newTier.minLevel = 1;
                            newTier.maxLevel = 250;
                            newTier.title = "Unknown";
                            // Default to white colors
                            newTier.leftColor[0] = newTier.leftColor[1] = newTier.leftColor[2] = 1.0f;
                            newTier.rightColor[0] = newTier.rightColor[1] = newTier.rightColor[2] = 1.0f;
                            newTier.highlightColor[0] = newTier.highlightColor[1] = newTier.highlightColor[2] = 1.0f;
                            // Default effect is simple gradient
                            newTier.titleEffect.type = EffectType::Gradient;
                            newTier.nameEffect.type = EffectType::Gradient;
                            newTier.levelEffect.type = EffectType::Gradient;
                            // Default particle settings
                            newTier.particleTypes = "";
                            newTier.particleCount = 0;
                            Tiers.push_back(newTier);
                        }
                    }
                }
                // Special title sections like [SpecialTitle0], [SpecialTitle1], etc.
                else if (currentSectionLower.size() >= 12 && currentSectionLower.rfind("specialtitle", 0) == 0) {
                    std::string numStr = currentSection.substr(12);
                    currentSpecialTitle = ParseInt(numStr, -1);
                    currentTier = -1;  // Not in a tier section

                    if (currentSpecialTitle >= 0) {
                        // Dynamically grow the SpecialTitles vector
                        while (static_cast<int>(SpecialTitles.size()) <= currentSpecialTitle) {
                            SpecialTitleDefinition newSpecial;
                            newSpecial.keyword = "";
                            newSpecial.displayTitle = "";
                            newSpecial.color[0] = newSpecial.color[1] = newSpecial.color[2] = 1.0f;
                            newSpecial.glowColor[0] = newSpecial.glowColor[1] = newSpecial.glowColor[2] = 1.0f;
                            newSpecial.forceOrnaments = true;
                            newSpecial.forceParticles = true;
                            newSpecial.priority = 0;
                            SpecialTitles.push_back(newSpecial);
                        }
                    } else {
                        addWarning(currentLineNumber, "Invalid special title section '" + currentSection + "'");
                    }
                } else {
                    currentTier = -1;  // Non-tier section, switch to global context
                    currentSpecialTitle = -1;

                    static const std::unordered_set<std::string> kKnownSections = {
                        "",
                        "general",
                        "display",
                        "appearance",
                        "appearancetemplate",
                        "debug",
                        "visual",
                        "fonts",
                        "particles",
                        "occlusion"
                    };
                    if (kKnownSections.find(currentSectionLower) == kKnownSections.end()) {
                        ++unknownSectionCount;
                        if (warnedUnknownSections.insert(currentSectionLower).second) {
                            addWarning(currentLineNumber, "Unknown section [" + currentSection + "]");
                        }
                    }
                }
                continue;
            }

            // Parse kv pairs
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                ++malformedLineCount;
                addWarning(currentLineNumber, "Ignoring malformed setting line (missing '=')");
                continue;
            }

            std::string keyRaw = Trim(line.substr(0, eq));
            std::string key = CanonicalizeKey(keyRaw);
            std::string val = Trim(line.substr(eq + 1));

            bool handled = false;

            // If we're inside a [TierN] section, all kv pairs apply to that tier
            // This allows each tier to have its own level range, colors, and effects
            if (currentTier >= 0 && currentTier < static_cast<int>(Tiers.size())) {
                if (key == "Name") {
                    // "Name" is displayed as the tier title
                    Tiers[currentTier].title = val;
                    handled = true;
                } else if (key == "MinLevel") {
                    // Inclusive lower bound for this tier's level range
                    const int parsed = ParseInt(val, 1);
                    const int clamped = std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
                    Tiers[currentTier].minLevel = static_cast<uint16_t>(clamped);
                    handled = true;
                } else if (key == "MaxLevel") {
                    // Inclusive upper bound for this tier's level range
                    const int parsed = ParseInt(val, 25);
                    const int clamped = std::clamp(parsed, 0, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
                    Tiers[currentTier].maxLevel = static_cast<uint16_t>(clamped);
                    handled = true;
                } else if (key == "LeftColor") {
                    // Left/start color for gradient effects (RGB floats 0.0-1.0)
                    ParseColor3(val, Tiers[currentTier].leftColor);
                    handled = true;
                } else if (key == "RightColor") {
                    // Right/end color for gradient effects
                    ParseColor3(val, Tiers[currentTier].rightColor);
                    handled = true;
                } else if (key == "HighlightColor") {
                    // Accent color for shimmer, sparkle, and scanline effects
                    ParseColor3(val, Tiers[currentTier].highlightColor);
                    handled = true;
                } else if (key == "TitleEffect" || key == "NameEffect" || key == "LevelEffect") {
                    // Effect syntax: "EffectType param1,param2,param3,param4,param5 [whiteBase]"
                    // Example: "RainbowWave 0.15,0.30,0.60,0.40,1.00 whiteBase"

                    std::istringstream ss(val);
                    std::string effectTypeName;
                    ss >> effectTypeName;  // Extract first word (effect type name)

                    // Select which effect struct to populate based on key name
                    EffectParams& effect = (key == "TitleEffect") ? Tiers[currentTier].titleEffect :
                                          (key == "NameEffect") ? Tiers[currentTier].nameEffect :
                                          Tiers[currentTier].levelEffect;

                    effect.type = ParseEffectType(effectTypeName);

                    // Get the rest of the line
                    std::string paramsStr;
                    std::getline(ss, paramsStr);
                    paramsStr = Trim(paramsStr);

                    // Check for "whiteBase" keyword
                    // When enabled, renders white text layer under effects
                    std::string paramsLower = ToLowerAscii(paramsStr);
                    size_t wbPos = paramsLower.find("whitebase");
                    if (wbPos != std::string::npos) {
                        effect.useWhiteBase = true;
                        paramsStr = paramsStr.substr(0, wbPos);  // Strip flag from params
                    }

                    // Parse comma-separated parameters (up to 5 floats)
                    // Different effects interpret these differently:
                    //   Shimmer: param1=bandWidth, param2=strength
                    //   Aurora: param1=speed, param2=waves, param3=intensity, param4=sway
                    //   RainbowWave: param1=baseHue, param2=hueSpread, param3=speed, param4=sat, param5=val
                    std::istringstream paramStream(paramsStr);
                    std::string token;
                    int paramIdx = 0;
                    while (std::getline(paramStream, token, ',') && paramIdx < 5) {
                        token = Trim(token);
                        if (token.empty()) continue;  // Skip empty tokens (e.g., "1.0,,2.0")

                        float v = ParseFloat(token, 0.0f);
                        switch(paramIdx) {
                            case 0: effect.param1 = v; break;
                            case 1: effect.param2 = v; break;
                            case 2: effect.param3 = v; break;
                            case 3: effect.param4 = v; break;
                            case 4: effect.param5 = v; break;
                        }
                        paramIdx++;
                    }
                    handled = true;
                } else if (key == "Ornaments") {
                    // Ornament format: "LEFT, RIGHT" (e.g., "AC, BD")
                    size_t commaPos = val.find(',');
                    if (commaPos != std::string::npos) {
                        // New format: split on comma
                        Tiers[currentTier].leftOrnaments = Trim(val.substr(0, commaPos));
                        Tiers[currentTier].rightOrnaments = Trim(val.substr(commaPos + 1));
                    } else if (val.length() >= 2) {
                        // Legacy format: first char = left, second char = right
                        Tiers[currentTier].leftOrnaments = val.substr(0, 1);
                        Tiers[currentTier].rightOrnaments = val.substr(1, 1);
                    } else {
                        Tiers[currentTier].leftOrnaments.clear();
                        Tiers[currentTier].rightOrnaments.clear();
                    }
                    handled = true;
                } else if (key == "ParticleTypes") {
                    // Particle types for this tier (e.g., "Stars,Wisps,Orbs")
                    Tiers[currentTier].particleTypes = val;
                    handled = true;
                } else if (key == "ParticleCount") {
                    // Number of particles for this tier (0 = use global)
                    Tiers[currentTier].particleCount = ParseInt(val, 0);
                    handled = true;
                }
            }

            // If we're inside a [SpecialTitleN] section, parse special title properties
            if (!handled && currentSpecialTitle >= 0 && currentSpecialTitle < static_cast<int>(SpecialTitles.size())) {
                if (key == "Keyword") {
                    // Text to match in actor name (case-insensitive)
                    SpecialTitles[currentSpecialTitle].keyword = val;
                    handled = true;
                } else if (key == "DisplayTitle") {
                    // Title shown above name
                    SpecialTitles[currentSpecialTitle].displayTitle = val;
                    handled = true;
                } else if (key == "Color") {
                    // RGB color for nameplate
                    ParseColor3(val, SpecialTitles[currentSpecialTitle].color);
                    handled = true;
                } else if (key == "GlowColor") {
                    // RGB color for glow effect
                    ParseColor3(val, SpecialTitles[currentSpecialTitle].glowColor);
                    handled = true;
                } else if (key == "ForceOrnaments" || key == "ForceFlourishes") {
                    SpecialTitles[currentSpecialTitle].forceOrnaments = ParseBool(val);
                    handled = true;
                } else if (key == "ForceParticles") {
                    // Always show particle aura
                    SpecialTitles[currentSpecialTitle].forceParticles = ParseBool(val);
                    handled = true;
                } else if (key == "Priority") {
                    // Higher priority = checked first
                    SpecialTitles[currentSpecialTitle].priority = ParseInt(val, 0);
                    handled = true;
                } else if (key == "Ornaments") {
                    // Ornament format: "LEFT, RIGHT" (e.g., "AC, BD")
                    size_t commaPos = val.find(',');
                    if (commaPos != std::string::npos) {
                        SpecialTitles[currentSpecialTitle].leftOrnaments = Trim(val.substr(0, commaPos));
                        SpecialTitles[currentSpecialTitle].rightOrnaments = Trim(val.substr(commaPos + 1));
                    } else if (val.length() >= 2) {
                        SpecialTitles[currentSpecialTitle].leftOrnaments = val.substr(0, 1);
                        SpecialTitles[currentSpecialTitle].rightOrnaments = val.substr(1, 1);
                    } else {
                        SpecialTitles[currentSpecialTitle].leftOrnaments.clear();
                        SpecialTitles[currentSpecialTitle].rightOrnaments.clear();
                    }
                    handled = true;
                }
            }

            if (!handled) {
            // Format key requires special parsing because it contains multiple quoted segments
            // Syntax: Format = "%t" "%n" "Lv.%l"
            if (key == "Format") {
                std::vector<Segment> newDisplayFormat;
                std::string newTitleFormat;
                bool titleFound = false;

                // Simple state machine to parse quoted strings
                // Handles escaped quotes (\") within strings
                bool inQuote = false;
                std::string current;
                for (size_t i = 0; i < val.size(); ++i) {
                    char c = val[i];

                    // Handle escape sequences (\" -> literal quote, etc.)
                    if (c == '\\' && i + 1 < val.size()) {
                        if (inQuote) {
                            // Inside quotes, add the escaped character literally
                            current += val[++i];
                        }
                        // Outside quotes, skip escape sequences
                        continue;
                    }

                    if (c == '"') {
                        if (inQuote) {
                            // Closing quote, finish current segment
                            if (current.find("%t") != std::string::npos) {
                                // Segment contains %t -> it's the title format, separate line
                                newTitleFormat = current;
                                titleFound = true;
                            } else {
                                // Regular segment -> add to main line
                                // Use level font if segment contains %l
                                bool isLevel = current.find("%l") != std::string::npos;
                                newDisplayFormat.push_back({current, isLevel});
                            }
                            current.clear();
                            inQuote = false;
                        } else {
                            // Opening quote, start new segment
                            inQuote = true;
                        }
                    } else if (inQuote) {
                        // Inside quotes, accumulate characters
                        current += c;
                    }
                    // Characters outside quotes are ignored
                }

                // Only update if we actually parsed something
                // This prevents empty Format= from clearing the defaults
                if (titleFound) {
                    TitleFormat = newTitleFormat;
                }
                if (!newDisplayFormat.empty()) {
                    DisplayFormat = newDisplayFormat;
                }
            }
            // Float value parsing
            else if (key == "FadeStartDistance") FadeStartDistance = ParseFloat(val, 0.0f);
            else if (key == "FadeEndDistance") FadeEndDistance = ParseFloat(val, 0.0f);
            else if (key == "ScaleStartDistance") ScaleStartDistance = ParseFloat(val, 0.0f);
            else if (key == "ScaleEndDistance") ScaleEndDistance = ParseFloat(val, 0.0f);
            else if (key == "MinimumScale") MinimumScale = ParseFloat(val, 0.0f);
            else if (key == "MaxScanDistance") MaxScanDistance = ParseFloat(val, 0.0f);
            // Occlusion Settings
            else if (key == "EnableOcclusionCulling") EnableOcclusionCulling = ParseBool(val);
            else if (key == "OcclusionSettleTime") OcclusionSettleTime = ParseFloat(val, 0.58f);
            else if (key == "OcclusionCheckInterval") OcclusionCheckInterval = ParseInt(val, 3);
            else if (key == "TitleShadowOffsetX") TitleShadowOffsetX = ParseFloat(val, 0.0f);
            else if (key == "TitleShadowOffsetY") TitleShadowOffsetY = ParseFloat(val, 0.0f);
            else if (key == "MainShadowOffsetX") MainShadowOffsetX = ParseFloat(val, 0.0f);
            else if (key == "MainShadowOffsetY") MainShadowOffsetY = ParseFloat(val, 0.0f);
            else if (key == "SegmentPadding") SegmentPadding = ParseFloat(val, 0.0f);
            else if (key == "OutlineWidthMin") OutlineWidthMin = ParseFloat(val, 0.0f);
            else if (key == "OutlineWidthMax") OutlineWidthMax = ParseFloat(val, 0.0f);
            else if (key == "FastOutlines") FastOutlines = ParseBool(val);
            // Glow Settings
            else if (key == "EnableGlow") EnableGlow = ParseBool(val);
            else if (key == "GlowRadius") GlowRadius = ParseFloat(val, 4.0f);
            else if (key == "GlowIntensity") GlowIntensity = ParseFloat(val, 0.5f);
            else if (key == "GlowSamples") GlowSamples = ParseInt(val, 8);
            // Typewriter Settings
            else if (key == "EnableTypewriter") EnableTypewriter = ParseBool(val);
            else if (key == "TypewriterSpeed") TypewriterSpeed = ParseFloat(val, 30.0f);
            else if (key == "TypewriterDelay") TypewriterDelay = ParseFloat(val, 0.0f);
            // Debug Settings
            else if (key == "EnableDebugOverlay") EnableDebugOverlay = ParseBool(val);
            // Side Ornaments
            else if (key == "EnableOrnaments" || key == "EnableFlourishes") EnableOrnaments = ParseBool(val);
            else if (key == "OrnamentScale" || key == "FlourishScale") OrnamentScale = ParseFloat(val, 1.0f);
            else if (key == "OrnamentSpacing" || key == "FlourishSpacing") OrnamentSpacing = ParseFloat(val, 6.0f);
            // Particle Aura
            else if (key == "EnableParticleAura") EnableParticleAura = ParseBool(val);
            else if (key == "EnableStars") EnableStars = ParseBool(val);
            else if (key == "EnableSparks") EnableSparks = ParseBool(val);
            else if (key == "EnableWisps") EnableWisps = ParseBool(val);
            else if (key == "EnableRunes") EnableRunes = ParseBool(val);
            else if (key == "EnableOrbs") EnableOrbs = ParseBool(val);
            else if (key == "ParticleCount") ParticleCount = ParseInt(val, 8);
            else if (key == "ParticleSize") ParticleSize = ParseFloat(val, 3.0f);
            else if (key == "ParticleSpeed") ParticleSpeed = ParseFloat(val, 1.0f);
            else if (key == "ParticleSpread") ParticleSpread = ParseFloat(val, 20.0f);
            else if (key == "ParticleAlpha") ParticleAlpha = ParseFloat(val, 0.8f);
            else if (key == "UseParticleTextures") UseParticleTextures = ParseBool(val);
            // Textures now auto-loaded from glyph/particles/<type>/ folders
            // Display Options
            else if (key == "VerticalOffset") VerticalOffset = ParseFloat(val, 8.0f);
            else if (key == "HidePlayer") HidePlayer = ParseBool(val);
            else if (key == "HideCreatures") HideCreatures = ParseBool(val);
            else if (key == "ReloadKey") ReloadKey = ParseInt(val, 0);
            else if (key == "AnimSpeedLowTier") AnimSpeedLowTier = ParseFloat(val, 0.0f);
            else if (key == "AnimSpeedMidTier") AnimSpeedMidTier = ParseFloat(val, 0.0f);
            else if (key == "AnimSpeedHighTier") AnimSpeedHighTier = ParseFloat(val, 0.0f);
            else if (key == "ColorWashAmount") ColorWashAmount = ParseFloat(val, 0.0f);
            else if (key == "NameColorMix") NameColorMix = ParseFloat(val, 0.0f);
            else if (key == "EffectAlphaMin") EffectAlphaMin = ParseFloat(val, 0.0f);
            else if (key == "EffectAlphaMax") EffectAlphaMax = ParseFloat(val, 0.0f);
            else if (key == "StrengthMin") StrengthMin = ParseFloat(val, 0.0f);
            else if (key == "StrengthMax") StrengthMax = ParseFloat(val, 0.0f);
            else if (key == "AlphaSettleTime") AlphaSettleTime = ParseFloat(val, 0.46f);
            else if (key == "ScaleSettleTime") ScaleSettleTime = ParseFloat(val, 0.46f);
            else if (key == "PositionSettleTime") PositionSettleTime = ParseFloat(val, 0.38f);
            // Distance-Based Outline
            else if (key == "EnableDistanceOutlineScale") Visual().EnableDistanceOutlineScale = ParseBool(val);
            else if (key == "OutlineDistanceMin") Visual().OutlineDistanceMin = ParseFloat(val, 0.8f);
            else if (key == "OutlineDistanceMax") Visual().OutlineDistanceMax = ParseFloat(val, 1.5f);
            // Minimum Readable Size
            else if (key == "MinimumPixelHeight") Visual().MinimumPixelHeight = ParseFloat(val, 0.0f);
            // LOD by Distance
            else if (key == "EnableLOD") Visual().EnableLOD = ParseBool(val);
            else if (key == "LODFarDistance") Visual().LODFarDistance = ParseFloat(val, 1800.0f);
            else if (key == "LODMidDistance") Visual().LODMidDistance = ParseFloat(val, 800.0f);
            else if (key == "LODTransitionRange") Visual().LODTransitionRange = ParseFloat(val, 200.0f);
            // Visual Hierarchy
            else if (key == "TitleAlphaMultiplier") Visual().TitleAlphaMultiplier = ParseFloat(val, 0.80f);
            else if (key == "LevelAlphaMultiplier") Visual().LevelAlphaMultiplier = ParseFloat(val, 0.85f);
            // Overlap Prevention
            else if (key == "EnableOverlapPrevention") Visual().EnableOverlapPrevention = ParseBool(val);
            else if (key == "OverlapPaddingY") Visual().OverlapPaddingY = ParseFloat(val, 4.0f);
            else if (key == "OverlapIterations") Visual().OverlapIterations = ParseInt(val, 3);
            // Position Smoothing Tuning
            else if (key == "PositionSmoothingBlend") Visual().PositionSmoothingBlend = ParseFloat(val, 1.0f);
            else if (key == "LargeMovementThreshold") Visual().LargeMovementThreshold = ParseFloat(val, 50.0f);
            else if (key == "LargeMovementBlend") Visual().LargeMovementBlend = ParseFloat(val, 0.5f);
            // Tier Effect Gating
            else if (key == "EnableTierEffectGating") Visual().EnableTierEffectGating = ParseBool(val);
            else if (key == "GlowMinTier") Visual().GlowMinTier = ParseInt(val, 5);
            else if (key == "ParticleMinTier") Visual().ParticleMinTier = ParseInt(val, 10);
            else if (key == "OrnamentMinTier") Visual().OrnamentMinTier = ParseInt(val, 10);
            // Font Settings
            else if (key == "NameFontPath") NameFontPath = val;
            else if (key == "NameFontSize") NameFontSize = ParseFloat(val, 0.0f);
            else if (key == "LevelFontPath") LevelFontPath = val;
            else if (key == "LevelFontSize") LevelFontSize = ParseFloat(val, 0.0f);
            else if (key == "TitleFontPath") TitleFontPath = val;
            else if (key == "TitleFontSize") TitleFontSize = ParseFloat(val, 0.0f);
            // Ornament Font Settings
            else if (key == "OrnamentFontPath") OrnamentFontPath = val;
            else if (key == "OrnamentFontSize") OrnamentFontSize = ParseFloat(val, 64.0f);
            // Appearance Template Settings
            else if (key == "TemplateFormID") TemplateFormID = val;
            else if (key == "TemplatePlugin") TemplatePlugin = val;
            else if (key == "UseTemplateAppearance") UseTemplateAppearance = ParseBool(val);
            else if (key == "TemplateIncludeRace") TemplateIncludeRace = ParseBool(val);
            else if (key == "TemplateIncludeBody") TemplateIncludeBody = ParseBool(val);
            else if (key == "TemplateCopyFaceGen") TemplateCopyFaceGen = ParseBool(val);
            else if (key == "TemplateCopySkin") TemplateCopySkin = ParseBool(val);
            else if (key == "TemplateCopyOverlays") TemplateCopyOverlays = ParseBool(val);
            else if (key == "TemplateCopyOutfit") TemplateCopyOutfit = ParseBool(val);
            else if (key == "TemplateReapplyOnReload") TemplateReapplyOnReload = ParseBool(val);
            else if (key == "TemplateFaceGenPlugin") TemplateFaceGenPlugin = val;
            else {
                ++unknownKeyCount;
                std::string sectionName = currentSection.empty() ? "<global>" : currentSection;
                addWarning(currentLineNumber, "Unknown key '" + keyRaw + "' in section " + sectionName);
            }
            }
        }

        ClampAndValidate();

        if (malformedLineCount > 0 || unknownKeyCount > 0 || unknownSectionCount > 0) {
            SKSE::log::warn(
                "Settings: parsed glyph.ini with {} malformed lines, {} unknown keys, {} unknown sections",
                malformedLineCount, unknownKeyCount, unknownSectionCount);
            for (const auto& warning : parseWarnings) {
                SKSE::log::warn("Settings: {}", warning);
            }
            if (parseWarnings.size() == 48) {
                SKSE::log::warn("Settings: warning output truncated");
            }
        }
    }
}
