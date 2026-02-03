#include "RendererInternal.h"

#include "ParticleTextures.h"

namespace Renderer
{
// Common parameters passed to each per-effect helper.
struct EffectArgs
{
    const Settings::EffectParams& effect;
    ImU32 colL, colR, highlight, outlineColor;
    float outlineWidth, phase01, strength, textSizeScale, alpha;
    bool fastOutlines;
};

namespace
{
static void ApplyNone(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::AddTextOutline4(
        dl, font, sz, pos, text, a.colL, a.outlineColor, a.outlineWidth, a.fastOutlines);
}

static void ApplyGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextHorizontalGradient>(
        dl, font, sz, pos, text, a.outlineColor, a.outlineWidth, a.fastOutlines, a.colL, a.colR);
}

static void ApplyVerticalGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextVerticalGradient>(
        dl, font, sz, pos, text, a.outlineColor, a.outlineWidth, a.fastOutlines, a.colL, a.colR);
}

static void ApplyDiagonalGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextDiagonalGradient>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.colL,
        a.colR,
        ImVec2(a.effect.param1, a.effect.param2));
}

static void ApplyRadialGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextRadialGradient>(dl,
                                                                 font,
                                                                 sz,
                                                                 pos,
                                                                 text,
                                                                 a.outlineColor,
                                                                 a.outlineWidth,
                                                                 a.fastOutlines,
                                                                 a.colL,
                                                                 a.colR,
                                                                 a.effect.param1,
                                                                 nullptr);
}

static void ApplyShimmer(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextShimmer>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.colL,
        a.colR,
        a.highlight,
        a.phase01,
        a.effect.param1,
        ParamOr(a.effect.param2, 1.0f) * a.strength);
}

static void ApplyChromaticShimmer(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::AddTextOutline4ChromaticShimmer(dl,
                                                 font,
                                                 sz,
                                                 pos,
                                                 text,
                                                 a.colL,
                                                 a.colR,
                                                 a.highlight,
                                                 a.outlineColor,
                                                 a.outlineWidth,
                                                 a.phase01,
                                                 a.effect.param1,
                                                 a.effect.param2 * a.strength,
                                                 a.effect.param3 * a.textSizeScale,
                                                 a.effect.param4);
}

static void ApplyPulseGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    float time = (float)ImGui::GetTime();
    TextEffects::WithOutline<TextEffects::AddTextPulseGradient>(dl,
                                                                font,
                                                                sz,
                                                                pos,
                                                                text,
                                                                a.outlineColor,
                                                                a.outlineWidth,
                                                                a.fastOutlines,
                                                                a.colL,
                                                                a.colR,
                                                                time,
                                                                a.effect.param1,
                                                                a.effect.param2 * a.strength);
}

static void ApplyRainbowWave(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::AddTextOutline4RainbowWave(dl,
                                            font,
                                            sz,
                                            pos,
                                            text,
                                            a.effect.param1,
                                            a.effect.param2,
                                            a.effect.param3,
                                            a.effect.param4,
                                            a.effect.param5,
                                            a.alpha,
                                            a.outlineColor,
                                            a.outlineWidth,
                                            a.fastOutlines,
                                            a.effect.useWhiteBase);
}

static void ApplyConicRainbow(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::AddTextOutline4ConicRainbow(dl,
                                             font,
                                             sz,
                                             pos,
                                             text,
                                             a.effect.param1,
                                             a.effect.param2,
                                             a.effect.param3,
                                             a.effect.param4,
                                             a.alpha,
                                             a.outlineColor,
                                             a.outlineWidth,
                                             a.fastOutlines,
                                             a.effect.useWhiteBase);
}

static void ApplyAurora(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextAurora>(dl,
                                                         font,
                                                         sz,
                                                         pos,
                                                         text,
                                                         a.outlineColor,
                                                         a.outlineWidth,
                                                         a.fastOutlines,
                                                         a.colL,
                                                         a.colR,
                                                         ParamOr(a.effect.param1, .5f),
                                                         ParamOr(a.effect.param2, 3.0f),
                                                         ParamOr(a.effect.param3, 1.0f),
                                                         ParamOr(a.effect.param4, .3f));
}

static void ApplySparkle(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextSparkle>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.colL,
        a.colR,
        a.highlight,
        ParamOr(a.effect.param1, .3f),
        ParamOr(a.effect.param2, 2.0f),
        ParamOr(a.effect.param3, 1.0f) * a.strength);
}

static void ApplyPlasma(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextPlasma>(dl,
                                                         font,
                                                         sz,
                                                         pos,
                                                         text,
                                                         a.outlineColor,
                                                         a.outlineWidth,
                                                         a.fastOutlines,
                                                         a.colL,
                                                         a.colR,
                                                         ParamOr(a.effect.param1, 2.0f),
                                                         ParamOr(a.effect.param2, 3.0f),
                                                         ParamOr(a.effect.param3, .5f));
}

static void ApplyScanline(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutline<TextEffects::AddTextScanline>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.colL,
        a.colR,
        a.highlight,
        ParamOr(a.effect.param1, .5f),
        ParamOr(a.effect.param2, .15f),
        ParamOr(a.effect.param3, 1.0f) * a.strength);
}
}  // namespace

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
                     bool fastOutlines)
{
    EffectArgs args{effect,
                    colL,
                    colR,
                    highlight,
                    outlineColor,
                    outlineWidth,
                    phase01,
                    strength,
                    textSizeScale,
                    alpha,
                    fastOutlines};

    switch (effect.type)
    {
        case Settings::EffectType::None:
            ApplyNone(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Gradient:
            ApplyGradient(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::VerticalGradient:
            ApplyVerticalGradient(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::DiagonalGradient:
            ApplyDiagonalGradient(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::RadialGradient:
            ApplyRadialGradient(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Shimmer:
            ApplyShimmer(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::ChromaticShimmer:
            ApplyChromaticShimmer(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::PulseGradient:
            ApplyPulseGradient(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::RainbowWave:
            ApplyRainbowWave(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::ConicRainbow:
            ApplyConicRainbow(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Aurora:
            ApplyAurora(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Sparkle:
            ApplySparkle(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Plasma:
            ApplyPlasma(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Scanline:
            ApplyScanline(drawList, font, fontSize, pos, text, args);
            break;
        default:
            break;
    }
}

// Computed particle configuration for a single actor label.
struct ParticleConfig
{
    bool showParticles;
    ImU32 particleColor;
    float spreadX;
    float spreadY;
    int boostedCount;
    float boostedSize;
    float boostedAlpha;
    bool showOrbs;
    bool showWisps;
    bool showRunes;
    bool showSparks;
    bool showStars;
    int enabledStyles;
};

// Compute all particle parameters without drawing.
static ParticleConfig ComputeParticleConfig(const ActorDrawData& d,
                                            const LabelStyle& style,
                                            const LabelLayout& layout,
                                            float lodEffectsFactor,
                                            const RenderSettingsSnapshot& snap)
{
    ParticleConfig cfg{};
    const Settings::TierDefinition& tier = *style.tier;
    const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);

    bool tierHasParticles = !tier.particleTypes.empty() && tier.particleTypes != "None";
    bool globalHasParticles = snap.enableOrbs || snap.enableWisps || snap.enableRunes ||
                              snap.enableSparks || snap.enableStars;
    bool hasAnyParticles = tierHasParticles || globalHasParticles;
    cfg.showParticles =
        ((snap.enableParticleAura && hasAnyParticles && style.tierAllowsParticles) ||
         (style.specialTitle && style.specialTitle->forceParticles)) &&
        lodEffectsFactor > .01f;
    if (!cfg.showParticles)
    {
        return cfg;
    }

    if (style.specialTitle)
    {
        cfg.particleColor = ImGui::ColorConvertFloat4ToU32(ImVec4(style.specialTitle->color.r,
                                                                  style.specialTitle->color.g,
                                                                  style.specialTitle->color.b,
                                                                  1.0f));
    }
    else
    {
        cfg.particleColor = ImGui::ColorConvertFloat4ToU32(
            ImVec4(tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, 1.0f));
    }

    const float pSpacingScale =
        snap.proportionalSpacing ? (layout.nameFontSize / layout.fontName->FontSize) : 1.0f;
    cfg.spreadX = layout.nameplateWidth * .5f + snap.particleSpread * 1.4f * pSpacingScale;
    cfg.spreadY = layout.nameplateHeight * .5f + snap.particleSpread * 1.1f * pSpacingScale;

    int particleCount = (tier.particleCount > 0) ? tier.particleCount : snap.particleCount;
    float tierBoost = .0f;
    if (snap.tiers.size() > 1)
    {
        tierBoost = static_cast<float>(style.tierIdx) / static_cast<float>(snap.tiers.size() - 1);
    }
    float levelBoost = TextEffects::Saturate((static_cast<float>(lv) - 100.0f) / 400.0f);
    float particleBoost = 1.0f + .6f * tierBoost + .6f * levelBoost;
    cfg.boostedCount =
        std::clamp(static_cast<int>(std::round(particleCount * particleBoost)), particleCount, 96);
    cfg.boostedSize =
        snap.particleSize * (1.0f + .4f * tierBoost + .35f * levelBoost) * pSpacingScale;
    cfg.boostedAlpha =
        std::clamp(snap.particleAlpha * style.alpha * (.95f + .35f * tierBoost + .35f * levelBoost),
                   .0f,
                   1.0f);

    if (tierHasParticles)
    {
        cfg.showOrbs = tier.particleTypes.find("Orbs") != std::string::npos;
        cfg.showWisps = tier.particleTypes.find("Wisps") != std::string::npos;
        cfg.showRunes = tier.particleTypes.find("Runes") != std::string::npos;
        cfg.showSparks = tier.particleTypes.find("Sparks") != std::string::npos;
        cfg.showStars = tier.particleTypes.find("Stars") != std::string::npos;
    }
    else
    {
        cfg.showOrbs = snap.enableOrbs;
        cfg.showWisps = snap.enableWisps;
        cfg.showRunes = snap.enableRunes;
        cfg.showSparks = snap.enableSparks;
        cfg.showStars = snap.enableStars;
    }

    cfg.enabledStyles = (int)cfg.showOrbs + (int)cfg.showWisps + (int)cfg.showRunes +
                        (int)cfg.showSparks + (int)cfg.showStars;
    return cfg;
}

// Draw particle aura effects behind the nameplate.
void DrawParticles(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodEffectsFactor,
                   float time,
                   ImDrawListSplitter* splitter,
                   const RenderSettingsSnapshot& snap)
{
    ParticleConfig cfg = ComputeParticleConfig(d, style, layout, lodEffectsFactor, snap);
    if (!cfg.showParticles)
    {
        return;
    }

    splitter->SetCurrentChannel(dl, 0);  // Back layer: particles
    int slot = 0;

    const bool useTextures = snap.useParticleTextures;

    const int blendMode = snap.particleBlendMode;

    if (cfg.showOrbs)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX,
                                       cfg.spreadY,
                                       cfg.particleColor,
                                       cfg.boostedAlpha,
                                       Settings::ParticleStyle::Orbs,
                                       cfg.boostedCount,
                                       cfg.boostedSize,
                                       snap.particleSpeed,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       useTextures,
                                       blendMode});
    }
    if (cfg.showWisps)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX * 1.15f,
                                       cfg.spreadY * 1.15f,
                                       cfg.particleColor,
                                       cfg.boostedAlpha,
                                       Settings::ParticleStyle::Wisps,
                                       cfg.boostedCount,
                                       cfg.boostedSize,
                                       snap.particleSpeed,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       useTextures,
                                       blendMode});
    }
    if (cfg.showRunes)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX * .9f,
                                       cfg.spreadY * .7f,
                                       cfg.particleColor,
                                       cfg.boostedAlpha,
                                       Settings::ParticleStyle::Runes,
                                       std::max(4, cfg.boostedCount / 2),
                                       cfg.boostedSize * 1.2f,
                                       snap.particleSpeed * .6f,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       useTextures,
                                       blendMode});
    }
    if (cfg.showSparks)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX,
                                       cfg.spreadY * .8f,
                                       cfg.particleColor,
                                       cfg.boostedAlpha,
                                       Settings::ParticleStyle::Sparks,
                                       cfg.boostedCount,
                                       cfg.boostedSize * .7f,
                                       snap.particleSpeed * 1.5f,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       useTextures,
                                       blendMode});
    }
    if (cfg.showStars)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX,
                                       cfg.spreadY,
                                       cfg.particleColor,
                                       cfg.boostedAlpha,
                                       Settings::ParticleStyle::Stars,
                                       cfg.boostedCount,
                                       cfg.boostedSize,
                                       snap.particleSpeed,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       useTextures,
                                       blendMode});
    }
}

// Collect drawable ornament characters from a UTF-8 string, filtering out
// replacement characters and control codes, and skipping missing glyphs.
static std::vector<std::string> CollectDrawableOrnaments(const std::string& raw,
                                                         ImFont* ornamentFont)
{
    std::vector<std::string> out;
    const char* p = raw.c_str();
    while (*p)
    {
        unsigned int cp = 0;
        const char* next = Utf8Next(p, cp);
        if (!next || next <= p)
        {
            ++p;
            continue;
        }
        if (cp == 0xFFFD || cp < 0x20)
        {
            p = next;
            continue;
        }

        const ImFontGlyph* glyph = nullptr;
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 18804
        glyph = ornamentFont->FindGlyphNoFallback(static_cast<ImWchar>(cp));
#else
        glyph = ornamentFont->FindGlyph(static_cast<ImWchar>(cp));
#endif
        if (glyph)
        {
            out.emplace_back(p, static_cast<size_t>(next - p));
        }
        p = next;
    }
    return out;
}

// Draw decorative ornament characters beside the nameplate.
void DrawOrnaments(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodEffectsFactor,
                   float time,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap)
{
    const Settings::TierDefinition& tier = *style.tier;

    const std::string& leftOrns = (style.specialTitle && !style.specialTitle->leftOrnaments.empty())
                                      ? style.specialTitle->leftOrnaments
                                      : tier.leftOrnaments;
    const std::string& rightOrns =
        (style.specialTitle && !style.specialTitle->rightOrnaments.empty())
            ? style.specialTitle->rightOrnaments
            : tier.rightOrnaments;
    bool hasOrnaments = !leftOrns.empty() || !rightOrns.empty();
    bool showOrnaments =
        ((d.isPlayer && snap.enableOrnaments && hasOrnaments && style.tierAllowsOrnaments) ||
         (style.specialTitle && style.specialTitle->forceOrnaments && hasOrnaments)) &&
        lodEffectsFactor > .01f;
    auto& ornIo = ImGui::GetIO();
    ImFont* ornamentFont = (ornIo.Fonts->Fonts.Size > RenderConstants::FONT_INDEX_ORNAMENT)
                               ? ornIo.Fonts->Fonts[RenderConstants::FONT_INDEX_ORNAMENT]
                               : nullptr;
    if (!showOrnaments || snap.ornamentFontPath.empty() || !ornamentFont)
    {
        return;
    }

    const auto leftChars = CollectDrawableOrnaments(leftOrns, ornamentFont);
    const auto rightChars = CollectDrawableOrnaments(rightOrns, ornamentFont);
    if (leftChars.empty() && rightChars.empty())
    {
        return;
    }

    const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;

    float ornamentScale = .75f;
    if (snap.tiers.size() > 1)
    {
        ornamentScale = .75f + .3f * (static_cast<float>(style.tierIdx) /
                                      static_cast<float>(snap.tiers.size() - 1));
    }
    float sizeMultiplier = (style.specialTitle != nullptr) ? ornamentScale * 1.3f : ornamentScale;
    float ornamentSize =
        snap.ornamentFontSize * snap.ornamentScale * sizeMultiplier * textSizeScale;

    const float spacingScale = snap.proportionalSpacing ? textSizeScale : 1.0f;
    float extraPadding = ornamentSize * .30f;
    float totalSpacing = snap.ornamentSpacing * 1.35f * spacingScale + extraPadding;
    float ornamentCharGap = std::max(2.0f, ornamentSize * .16f);

    ImU32 ornColL =
        ImGui::ColorConvertFloat4ToU32(ImVec4(style.Lc.x, style.Lc.y, style.Lc.z, style.alpha));
    ImU32 ornColR =
        ImGui::ColorConvertFloat4ToU32(ImVec4(style.Rc.x, style.Rc.y, style.Rc.z, style.alpha));
    ImU32 ornHighlight = ImGui::ColorConvertFloat4ToU32(
        ImVec4(tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, style.alpha));
    ImU32 ornOutline = IM_COL32(0, 0, 0, (int)(style.alpha * 255.0f));
    float ornOutlineWidth = style.outlineWidth * (ornamentSize / layout.nameFontSize);

    ImU32 glowColor =
        ImGui::ColorConvertFloat4ToU32(ImVec4(style.Lc.x, style.Lc.y, style.Lc.z, style.alpha));
    bool showOrnGlow = snap.enableGlow && snap.glowIntensity > .0f && style.tierAllowsGlow;

    auto drawOrnChar = [&](ImVec2 charPos, const char* ch)
    {
        if (showOrnGlow)
        {
            splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
            ParticleTextures::PushAdditiveBlend(dl);
            TextEffects::AddTextGlow(dl,
                                     ornamentFont,
                                     ornamentSize,
                                     charPos,
                                     ch,
                                     glowColor,
                                     snap.glowRadius,
                                     snap.glowIntensity,
                                     snap.glowSamples);
            ParticleTextures::PopBlendState(dl);
        }
        splitter->SetCurrentChannel(dl, 1);  // Front layer: ornament shapes
        ApplyTextEffect(dl,
                        ornamentFont,
                        ornamentSize,
                        charPos,
                        ch,
                        tier.nameEffect,
                        ornColL,
                        ornColR,
                        ornHighlight,
                        ornOutline,
                        ornOutlineWidth,
                        style.phase01,
                        style.strength,
                        textSizeScale,
                        style.alpha,
                        fastOutlines);
    };

    const float ornAnchorY =
        snap.ornamentAnchorToMainLine ? layout.mainLineCenterY : layout.nameplateCenter.y;

    if (!leftChars.empty())
    {
        float cursorX = layout.nameplateCenter.x - layout.nameplateWidth * .5f - totalSpacing;
        for (int i = static_cast<int>(leftChars.size()) - 1; i >= 0; --i)
        {
            const std::string& ch = leftChars[i];
            ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, .0f, ch.c_str());
            cursorX -= charSize.x;
            ImVec2 charPos(cursorX, ornAnchorY - charSize.y * .5f);
            drawOrnChar(charPos, ch.c_str());
            if (i > 0)
            {
                cursorX -= ornamentCharGap;
            }
        }
    }

    if (!rightChars.empty())
    {
        float cursorX = layout.nameplateCenter.x + layout.nameplateWidth * .5f + totalSpacing;
        for (size_t i = 0; i < rightChars.size(); ++i)
        {
            const std::string& ch = rightChars[i];
            ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, .0f, ch.c_str());
            ImVec2 charPos(cursorX, ornAnchorY - charSize.y * .5f);
            drawOrnChar(charPos, ch.c_str());
            cursorX += charSize.x;
            if (i + 1 < rightChars.size())
            {
                cursorX += ornamentCharGap;
            }
        }
    }
}

// Render the title line above the main nameplate line.
void DrawTitleText(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodTitleFactor,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap)
{
    const char* titleDisplayText = layout.titleDisplayStr.c_str();
    if (!titleDisplayText || !*titleDisplayText || lodTitleFactor <= .01f)
    {
        return;
    }

    const float spacingScale =
        snap.proportionalSpacing ? (layout.nameFontSize / layout.fontName->FontSize) : 1.0f;

    float titleOffsetX = (layout.totalWidth - layout.titleSize.x) * .5f;
    ImVec2 titlePos(layout.startPos.x - layout.totalWidth * .5f + titleOffsetX,
                    layout.startPos.y + layout.titleY);

    float lodTitleAlpha = style.alpha * lodTitleFactor;
    ImU32 titleShadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlpha * .5f));

    splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
    if (snap.enableGlow && snap.glowIntensity > .0f && style.tierAllowsGlow)
    {
        ImVec4 glowColorVec =
            style.specialTitle
                ? ImVec4(style.specialGlowColor.x,
                         style.specialGlowColor.y,
                         style.specialGlowColor.z,
                         style.alpha)
                : ImVec4(style.LcTitle.x, style.LcTitle.y, style.LcTitle.z, style.alpha);
        ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowColorVec);
        float glowIntensity = style.specialTitle ? snap.glowIntensity * 1.15f : snap.glowIntensity;
        float glowRadius = style.specialTitle ? snap.glowRadius * 1.1f : snap.glowRadius;
        ParticleTextures::PushAdditiveBlend(dl);
        TextEffects::AddTextGlow(dl,
                                 layout.fontTitle,
                                 layout.titleFontSize,
                                 titlePos,
                                 titleDisplayText,
                                 glowColor,
                                 glowRadius,
                                 glowIntensity,
                                 snap.glowSamples);
        ParticleTextures::PopBlendState(dl);
    }

    splitter->SetCurrentChannel(dl, 1);  // Front layer: shadow + text
    dl->AddText(layout.fontTitle,
                layout.titleFontSize,
                ImVec2(titlePos.x + snap.titleShadowOffsetX * spacingScale,
                       titlePos.y + snap.titleShadowOffsetY * spacingScale),
                titleShadow,
                titleDisplayText);

    float lodTitleAlphaFinal = style.titleAlpha * lodTitleFactor;
    float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    if (d.isPlayer)
    {
        ApplyTextEffect(dl,
                        layout.fontTitle,
                        layout.titleFontSize,
                        titlePos,
                        titleDisplayText,
                        style.tier->titleEffect,
                        style.colLTitle,
                        style.colRTitle,
                        style.highlight,
                        style.outlineColor,
                        style.titleOutlineWidth,
                        style.phase01,
                        style.strength,
                        textSizeScale,
                        lodTitleAlphaFinal,
                        fastOutlines);
    }
    else
    {
        ImVec4 dColV = WashColor(style.dispoCol, snap.colorWashAmount);
        dColV.w = lodTitleAlphaFinal;
        ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
        ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlphaFinal));
        TextEffects::AddTextOutline4(dl,
                                     layout.fontTitle,
                                     layout.titleFontSize,
                                     titlePos,
                                     titleDisplayText,
                                     dCol,
                                     npcOutline,
                                     style.titleOutlineWidth,
                                     fastOutlines);
    }
}

// Render each segment of the main nameplate line.
void DrawMainLineSegments(ImDrawList* dl,
                          const ActorDrawData& d,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap)
{
    const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    const float spacingScale = snap.proportionalSpacing ? textSizeScale : 1.0f;

    ImVec2 currentPos;
    currentPos.x = layout.startPos.x - layout.totalWidth * .5f +
                   (layout.totalWidth - layout.mainLineWidth) * .5f;
    currentPos.y = layout.startPos.y + layout.mainLineY;

    for (const auto& seg : layout.segments)
    {
        if (seg.displayText.empty())
        {
            currentPos.x += seg.size.x + layout.segmentPadding;
            continue;
        }

        float vOffset = (layout.mainLineHeight - seg.size.y) * .5f;
        ImVec2 pos = ImVec2(currentPos.x, currentPos.y + vOffset);

        splitter->SetCurrentChannel(dl, 0);  // Back layer: glow
        if (snap.enableGlow && snap.glowIntensity > .0f && style.tierAllowsGlow)
        {
            ImVec4 glowCol =
                style.specialTitle
                    ? ImVec4(style.specialGlowColor.x,
                             style.specialGlowColor.y,
                             style.specialGlowColor.z,
                             style.alpha)
                    : (seg.isLevel
                           ? ImVec4(style.LcLevel.x, style.LcLevel.y, style.LcLevel.z, style.alpha)
                           : ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, style.alpha));
            ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(glowCol);
            float glowIntensity =
                style.specialTitle ? snap.glowIntensity * 1.15f : snap.glowIntensity;
            float glowRadius = style.specialTitle ? snap.glowRadius * 1.1f : snap.glowRadius;
            ParticleTextures::PushAdditiveBlend(dl);
            TextEffects::AddTextGlow(dl,
                                     seg.font,
                                     seg.fontSize,
                                     pos,
                                     seg.displayText.c_str(),
                                     glowColor,
                                     glowRadius,
                                     glowIntensity,
                                     snap.glowSamples);
            ParticleTextures::PopBlendState(dl);
        }

        splitter->SetCurrentChannel(dl, 1);  // Front layer: shadow + text
        dl->AddText(seg.font,
                    seg.fontSize,
                    ImVec2(pos.x + snap.mainShadowOffsetX * spacingScale,
                           pos.y + snap.mainShadowOffsetY * spacingScale),
                    style.shadowColor,
                    seg.displayText.c_str());

        float segOutlineWidth = seg.isLevel ? style.levelOutlineWidth : style.nameOutlineWidth;

        if (seg.isLevel)
        {
            ApplyTextEffect(dl,
                            seg.font,
                            seg.fontSize,
                            pos,
                            seg.displayText.c_str(),
                            style.tier->levelEffect,
                            style.colLLevel,
                            style.colRLevel,
                            style.highlight,
                            style.outlineColor,
                            segOutlineWidth,
                            style.phase01,
                            style.strength,
                            textSizeScale,
                            style.levelAlpha,
                            fastOutlines);
        }
        else
        {
            if (d.isPlayer)
            {
                ApplyTextEffect(dl,
                                seg.font,
                                seg.fontSize,
                                pos,
                                seg.displayText.c_str(),
                                style.tier->nameEffect,
                                style.colL,
                                style.colR,
                                style.highlight,
                                style.outlineColor,
                                segOutlineWidth,
                                style.phase01,
                                style.strength,
                                textSizeScale,
                                style.alpha,
                                fastOutlines);
            }
            else
            {
                ImVec4 dColV = style.dispoCol;
                dColV.w = style.alpha;
                ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
                ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, style.alpha));
                TextEffects::AddTextOutline4(dl,
                                             seg.font,
                                             seg.fontSize,
                                             pos,
                                             seg.displayText.c_str(),
                                             dCol,
                                             npcOutline,
                                             segOutlineWidth,
                                             fastOutlines);
            }
        }

        currentPos.x += seg.size.x + layout.segmentPadding;
    }
}

}  // namespace Renderer
