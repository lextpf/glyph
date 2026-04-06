#include "RendererInternal.h"

#include "ParticleTextures.h"
#include "TextPostProcess.h"

namespace Renderer
{
// Common parameters passed to each per-effect helper.
struct EffectArgs
{
    const Settings::EffectParams& effect;
    ImU32 colL, colR, highlight, outlineColor;
    float outlineWidth, phase01, strength, textSizeScale, alpha;
    bool fastOutlines;
    TextEffects::OutlineGlowParams outlineGlow;
    TextEffects::DualOutlineParams dualOutline;
};

namespace
{
static void ApplyNone(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::AddTextOutline4(dl,
                                 font,
                                 sz,
                                 pos,
                                 text,
                                 a.colL,
                                 a.outlineColor,
                                 a.outlineWidth,
                                 a.fastOutlines,
                                 a.outlineGlow.enabled ? &a.outlineGlow : nullptr);
}

static void ApplyGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextHorizontalGradient>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR);
}

static void ApplyVerticalGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextVerticalGradient>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR);
}

static void ApplyDiagonalGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextDiagonalGradient>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        ImVec2(a.effect.param1, a.effect.param2));
}

static void ApplyRadialGradient(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextRadialGradient>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        a.effect.param1,
        nullptr);
}

static void ApplyShimmer(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextShimmer>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        a.highlight,
        a.phase01,
        ParamOr(a.effect.param1, .12f),
        ParamOr(a.effect.param2, 1.0f) * a.strength);
}

static void ApplyChromaticShimmer(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    if (a.outlineGlow.enabled)
    {
        TextEffects::DrawOutlineGlow(dl,
                                     font,
                                     sz,
                                     pos,
                                     text,
                                     a.outlineGlow.color,
                                     a.outlineWidth,
                                     a.outlineGlow.scale,
                                     a.outlineGlow.alpha,
                                     a.outlineGlow.rings,
                                     a.fastOutlines);
    }
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
                                                 ParamOr(a.effect.param1, .1f),
                                                 ParamOr(a.effect.param2, .8f) * a.strength,
                                                 ParamOr(a.effect.param3, 1.5f) * a.textSizeScale,
                                                 ParamOr(a.effect.param4, .35f));
}

static void ApplyEmber(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextEmber>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        ParamOr(a.effect.param1, .5f),
        ParamOr(a.effect.param2, .8f) * a.strength);
}

static void ApplyRainbowWave(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    if (a.outlineGlow.enabled)
    {
        TextEffects::DrawOutlineGlow(dl,
                                     font,
                                     sz,
                                     pos,
                                     text,
                                     a.outlineGlow.color,
                                     a.outlineWidth,
                                     a.outlineGlow.scale,
                                     a.outlineGlow.alpha,
                                     a.outlineGlow.rings,
                                     a.fastOutlines);
    }
    TextEffects::AddTextOutline4RainbowWave(dl,
                                            font,
                                            sz,
                                            pos,
                                            text,
                                            a.effect.param1,
                                            ParamOr(a.effect.param2, 1.0f),
                                            ParamOr(a.effect.param3, .5f),
                                            ParamOr(a.effect.param4, .85f),
                                            ParamOr(a.effect.param5, .95f),
                                            a.alpha,
                                            a.outlineColor,
                                            a.outlineWidth,
                                            a.fastOutlines,
                                            a.effect.useWhiteBase);
}

static void ApplyConicRainbow(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    if (a.outlineGlow.enabled)
    {
        TextEffects::DrawOutlineGlow(dl,
                                     font,
                                     sz,
                                     pos,
                                     text,
                                     a.outlineGlow.color,
                                     a.outlineWidth,
                                     a.outlineGlow.scale,
                                     a.outlineGlow.alpha,
                                     a.outlineGlow.rings,
                                     a.fastOutlines);
    }
    TextEffects::AddTextOutline4ConicRainbow(dl,
                                             font,
                                             sz,
                                             pos,
                                             text,
                                             a.effect.param1,
                                             ParamOr(a.effect.param2, .4f),
                                             ParamOr(a.effect.param3, .85f),
                                             ParamOr(a.effect.param4, .95f),
                                             a.alpha,
                                             a.outlineColor,
                                             a.outlineWidth,
                                             a.fastOutlines,
                                             a.effect.useWhiteBase);
}

static void ApplyAurora(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextAurora>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
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
    TextEffects::WithOutlineGlow<TextEffects::AddTextSparkle>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
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
    TextEffects::WithOutlineGlow<TextEffects::AddTextPlasma>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        ParamOr(a.effect.param1, 2.0f),
        ParamOr(a.effect.param2, 3.0f),
        ParamOr(a.effect.param3, .5f));
}

static void ApplyScanline(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextScanline>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        a.highlight,
        ParamOr(a.effect.param1, .5f),
        ParamOr(a.effect.param2, .15f),
        ParamOr(a.effect.param3, 1.0f) * a.strength);
}

static void ApplyEnchant(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextEnchant>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        ParamOr(a.effect.param1, .3f),
        ParamOr(a.effect.param2, 2.0f),
        ParamOr(a.effect.param3, 1.0f));
}

static void ApplyFrost(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextFrost>(
        dl,
        font,
        sz,
        pos,
        text,
        a.outlineColor,
        a.outlineWidth,
        a.fastOutlines,
        a.outlineGlow.enabled ? &a.outlineGlow : nullptr,
        a.colL,
        a.colR,
        ParamOr(a.effect.param1, .4f),
        ParamOr(a.effect.param2, .8f),
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
                     bool fastOutlines,
                     const TextEffects::OutlineGlowParams* outlineGlow,
                     const TextEffects::DualOutlineParams* dualOutline,
                     const TextEffects::WaveParams* wave,
                     const TextEffects::ShineParams* shine)
{
    // Capture vertex count before rendering for wave displacement
    const int vtxBefore = drawList->VtxBuffer.Size;

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
                    fastOutlines,
                    outlineGlow ? *outlineGlow : TextEffects::OutlineGlowParams{},
                    dualOutline ? *dualOutline : TextEffects::DualOutlineParams{}};

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
        case Settings::EffectType::Ember:
            ApplyEmber(drawList, font, fontSize, pos, text, args);
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
        case Settings::EffectType::Enchant:
            ApplyEnchant(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Frost:
            ApplyFrost(drawList, font, fontSize, pos, text, args);
            break;
        default:
            break;
    }

    // Draw inner outline on top (sits between outer outline and text visually
    // due to smaller width and semi-transparent tier-tinted color)
    if (args.dualOutline.enabled)
    {
        TextEffects::DrawDirectionalInnerOutline(drawList,
                                                 font,
                                                 fontSize,
                                                 pos,
                                                 text,
                                                 args.outlineColor,
                                                 args.dualOutline.tierColor,
                                                 args.outlineWidth,
                                                 args.dualOutline.innerScale,
                                                 args.dualOutline.tintFactor,
                                                 args.dualOutline.alphaFactor,
                                                 args.dualOutline.lightAngle,
                                                 args.dualOutline.lightBias,
                                                 args.fastOutlines);
    }

    // Translucent glowing text: boost brightness then reduce alpha so text
    // looks like a self-luminous light source, not dim transparent glass.
    // Dark outlines (luminance < 0.15) keep full alpha and original color.
    if (shine && shine->textGlowAlpha > .0f)
    {
        const int vtxTextEnd = drawList->VtxBuffer.Size;
        const float alphaKeep = 1.0f - shine->textGlowAlpha;
        const float brightnessBoost = 1.0f + shine->textGlowAlpha * 2.0f;
        for (int i = vtxBefore; i < vtxTextEnd; ++i)
        {
            ImU32 c = drawList->VtxBuffer[i].col;
            int cr = (c >> IM_COL32_R_SHIFT) & 0xFF;
            int cg = (c >> IM_COL32_G_SHIFT) & 0xFF;
            int cb = (c >> IM_COL32_B_SHIFT) & 0xFF;
            int ca = (c >> IM_COL32_A_SHIFT) & 0xFF;

            float lum = (cr * .299f + cg * .587f + cb * .114f) / 255.0f;
            if (lum < .15f)
            {
                continue;
            }

            cr = (int)std::min(cr * brightnessBoost, 255.0f);
            cg = (int)std::min(cg * brightnessBoost, 255.0f);
            cb = (int)std::min(cb * brightnessBoost, 255.0f);
            ca = (int)std::clamp(ca * alphaKeep, .0f, 255.0f);
            drawList->VtxBuffer[i].col = IM_COL32(cr, cg, cb, ca);
        }
    }

    // Additive shine overlay (static top-edge highlight)
    if (shine && shine->enabled && shine->intensity > .0f)
    {
        ParticleTextures::PushAdditiveBlend(drawList);
        TextEffects::AddTextShineOverlay(
            drawList, font, fontSize, pos, text, shine->intensity, shine->falloff, IM_COL32_WHITE);
        ParticleTextures::PopBlendState(drawList);
    }

    // Apply per-glyph wave displacement to all vertices added during this call
    if (wave && wave->enabled)
    {
        const int vtxAfter = drawList->VtxBuffer.Size;
        if (vtxAfter > vtxBefore)
        {
            // Compute bounding box of all added vertices
            float bbMinX = FLT_MAX, bbMaxX = -FLT_MAX;
            for (int i = vtxBefore; i < vtxAfter; ++i)
            {
                float x = drawList->VtxBuffer[i].pos.x;
                if (x < bbMinX)
                    bbMinX = x;
                if (x > bbMaxX)
                    bbMaxX = x;
            }
            float bbWidth = bbMaxX - bbMinX;
            TextEffects::ApplyWaveDisplacement(drawList,
                                               vtxBefore,
                                               vtxAfter,
                                               bbMinX,
                                               bbWidth,
                                               wave->amplitude,
                                               wave->frequency,
                                               wave->speed,
                                               wave->time);
        }
    }
}

// Build outline glow params from snapshot and tier color.
static TextEffects::OutlineGlowParams BuildOutlineGlow(const RenderSettingsSnapshot& snap,
                                                       const LabelStyle& style)
{
    TextEffects::OutlineGlowParams glow;
    glow.enabled = snap.enableOutlineGlow;
    if (!glow.enabled)
    {
        return glow;
    }
    glow.scale = snap.outlineGlowScale;
    glow.alpha = snap.outlineGlowAlpha;
    glow.rings = snap.outlineGlowRings;

    // Base glow color from settings (default white)
    float gr = snap.outlineGlowR;
    float gg = snap.outlineGlowG;
    float gb = snap.outlineGlowB;

    // Optionally tint toward tier color
    if (snap.outlineGlowTierTint)
    {
        float t = .35f;  // blend 35% tier color into the glow
        gr = gr + (style.Lc.x - gr) * t;
        gg = gg + (style.Lc.y - gg) * t;
        gb = gb + (style.Lc.z - gb) * t;
    }

    int a = std::clamp((int)(style.alpha * 255.0f + .5f), 0, 255);
    glow.color = IM_COL32((int)(gr * 255.0f), (int)(gg * 255.0f), (int)(gb * 255.0f), a);
    return glow;
}

// Build dual-tone directional outline params from snapshot and tier color.
static TextEffects::DualOutlineParams BuildDualOutline(const RenderSettingsSnapshot& snap,
                                                       const LabelStyle& style)
{
    TextEffects::DualOutlineParams dual;
    dual.enabled = snap.dualOutlineEnabled;
    if (!dual.enabled)
    {
        return dual;
    }
    // Use the left tier color as the tint target
    int a = std::clamp((int)(style.alpha * 255.0f + .5f), 0, 255);
    dual.tierColor = IM_COL32(
        (int)(style.Lc.x * 255.0f), (int)(style.Lc.y * 255.0f), (int)(style.Lc.z * 255.0f), a);
    dual.innerScale = snap.innerOutlineScale;
    dual.tintFactor = snap.innerOutlineTint;
    dual.alphaFactor = snap.innerOutlineAlpha;
    dual.lightAngle = snap.directionalLightAngle;
    dual.lightBias = snap.directionalLightBias;
    return dual;
}

// Build wave displacement params from snapshot and style.
static TextEffects::WaveParams BuildWaveParams(const RenderSettingsSnapshot& snap,
                                               const LabelStyle& style,
                                               float time)
{
    TextEffects::WaveParams wave;
    wave.enabled = snap.visual.EnableWave && style.tierIdx >= snap.visual.WaveMinTier;
    if (!wave.enabled)
    {
        return wave;
    }
    wave.amplitude = snap.visual.WaveAmplitude;
    wave.frequency = snap.visual.WaveFrequency;
    wave.speed = snap.visual.WaveSpeed;
    wave.time = time;
    return wave;
}

static TextEffects::ShineParams BuildShineParams(const RenderSettingsSnapshot& snap)
{
    TextEffects::ShineParams shine;
    shine.enabled = snap.enableShine || snap.textGlowAlpha > .0f;
    shine.textGlowAlpha = snap.textGlowAlpha;
    if (!shine.enabled)
    {
        return shine;
    }
    shine.intensity = snap.shineIntensity;
    shine.falloff = snap.shineFalloff;
    return shine;
}

// Computed particle configuration for a single actor label.
struct ParticleConfig
{
    bool showParticles;
    ImU32 particleColor;
    ImU32 particleColorSecondary;
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
    bool showCrystals;
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
                              snap.enableSparks || snap.enableStars || snap.enableCrystals;
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
        cfg.particleColorSecondary = 0;  // single color for special titles
    }
    else
    {
        const Settings::Color3& pc = tier.particleColor.value_or(tier.highlightColor);
        cfg.particleColor = ImGui::ColorConvertFloat4ToU32(ImVec4(pc.r, pc.g, pc.b, 1.0f));
        // Secondary color from the tier's right gradient for a gradient particle cloud
        cfg.particleColorSecondary =
            ImGui::ColorConvertFloat4ToU32(ImVec4(style.Rc.x, style.Rc.y, style.Rc.z, 1.0f));
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
    float particleBoost = 1.0f + .3f * tierBoost + .3f * levelBoost;
    cfg.boostedCount =
        std::clamp(static_cast<int>(std::round(particleCount * particleBoost)), particleCount, 96);
    cfg.boostedSize =
        snap.particleSize * (1.0f + .2f * tierBoost + .2f * levelBoost) * pSpacingScale;
    cfg.boostedAlpha =
        std::clamp(snap.particleAlpha * style.alpha * (.95f + .15f * tierBoost + .15f * levelBoost),
                   .0f,
                   1.0f);

    if (tierHasParticles)
    {
        cfg.showOrbs = tier.particleTypes.find("Orbs") != std::string::npos;
        cfg.showWisps = tier.particleTypes.find("Wisps") != std::string::npos;
        cfg.showRunes = tier.particleTypes.find("Runes") != std::string::npos;
        cfg.showSparks = tier.particleTypes.find("Sparks") != std::string::npos;
        cfg.showStars = tier.particleTypes.find("Stars") != std::string::npos;
        cfg.showCrystals = tier.particleTypes.find("Crystals") != std::string::npos;
    }
    else
    {
        cfg.showOrbs = snap.enableOrbs;
        cfg.showWisps = snap.enableWisps;
        cfg.showRunes = snap.enableRunes;
        cfg.showSparks = snap.enableSparks;
        cfg.showStars = snap.enableStars;
        cfg.showCrystals = snap.enableCrystals;
    }

    cfg.enabledStyles = (int)cfg.showOrbs + (int)cfg.showWisps + (int)cfg.showRunes +
                        (int)cfg.showSparks + (int)cfg.showStars + (int)cfg.showCrystals;
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

    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    splitter->SetCurrentChannel(dl, gpuGlow ? 1 : 0);  // Back layer: particles
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
                                       blendMode,
                                       cfg.particleColorSecondary});
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
                                       blendMode,
                                       cfg.particleColorSecondary});
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
                                       blendMode,
                                       cfg.particleColorSecondary});
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
                                       blendMode,
                                       cfg.particleColorSecondary});
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
                                       blendMode,
                                       cfg.particleColorSecondary});
    }
    if (cfg.showCrystals)
    {
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX * .85f,
                                       cfg.spreadY * .85f,
                                       cfg.particleColor,
                                       cfg.boostedAlpha,
                                       Settings::ParticleStyle::Crystals,
                                       (std::max)(4, cfg.boostedCount / 2),
                                       cfg.boostedSize * 1.1f,
                                       snap.particleSpeed * .3f,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       useTextures,
                                       blendMode,
                                       cfg.particleColorSecondary});
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
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int chBack = gpuGlow ? 1 : 0;
    const int chFront = gpuGlow ? 2 : 1;

    auto ornGlow = BuildOutlineGlow(snap, style);
    auto ornDual = BuildDualOutline(snap, style);
    auto ornShine = BuildShineParams(snap);

    auto drawOrnChar = [&](ImVec2 charPos, const char* ch)
    {
        if (showOrnGlow)
        {
            if (gpuGlow)
            {
                // GPU path: single AddText to glow capture channel
                splitter->SetCurrentChannel(dl, 0);
                dl->AddText(ornamentFont, ornamentSize, charPos, glowColor, ch);
            }
            else
            {
                // CPU fallback: multi-copy glow
                splitter->SetCurrentChannel(dl, chBack);
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
        }
        splitter->SetCurrentChannel(dl, chFront);  // Front layer: ornament shapes
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
                        fastOutlines,
                        ornGlow.enabled ? &ornGlow : nullptr,
                        ornDual.enabled ? &ornDual : nullptr,
                        nullptr,
                        ornShine.enabled ? &ornShine : nullptr);
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

    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int chFront = gpuGlow ? 2 : 1;

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

        if (gpuGlow)
        {
            // GPU path: single AddText to glow capture channel
            splitter->SetCurrentChannel(dl, 0);
            dl->AddText(
                layout.fontTitle, layout.titleFontSize, titlePos, glowColor, titleDisplayText);
        }
        else
        {
            // CPU fallback: multi-copy glow
            float glowIntensity =
                style.specialTitle ? snap.glowIntensity * 1.15f : snap.glowIntensity;
            float glowRadius = style.specialTitle ? snap.glowRadius * 1.1f : snap.glowRadius;
            splitter->SetCurrentChannel(dl, 0);
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
    }

    splitter->SetCurrentChannel(dl, chFront);  // Front layer: shadow + text
    dl->AddText(layout.fontTitle,
                layout.titleFontSize,
                ImVec2(titlePos.x + snap.titleShadowOffsetX * spacingScale,
                       titlePos.y + snap.titleShadowOffsetY * spacingScale),
                titleShadow,
                titleDisplayText);

    float lodTitleAlphaFinal = style.titleAlpha * lodTitleFactor;
    float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    auto titleGlow = BuildOutlineGlow(snap, style);
    auto titleDual = BuildDualOutline(snap, style);
    auto titleWave = BuildWaveParams(snap, style, (float)ImGui::GetTime());
    auto titleShine = BuildShineParams(snap);

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
                        fastOutlines,
                        titleGlow.enabled ? &titleGlow : nullptr,
                        titleDual.enabled ? &titleDual : nullptr,
                        titleWave.enabled ? &titleWave : nullptr,
                        titleShine.enabled ? &titleShine : nullptr);
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
                                     fastOutlines,
                                     titleGlow.enabled ? &titleGlow : nullptr);
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
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int chFront = gpuGlow ? 2 : 1;
    auto mainGlow = BuildOutlineGlow(snap, style);
    auto mainDual = BuildDualOutline(snap, style);
    auto mainWave = BuildWaveParams(snap, style, (float)ImGui::GetTime());
    auto mainShine = BuildShineParams(snap);

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

            if (gpuGlow)
            {
                // GPU path: single AddText to glow capture channel
                splitter->SetCurrentChannel(dl, 0);
                dl->AddText(seg.font, seg.fontSize, pos, glowColor, seg.displayText.c_str());
            }
            else
            {
                // CPU fallback: multi-copy glow
                float glowIntensity =
                    style.specialTitle ? snap.glowIntensity * 1.15f : snap.glowIntensity;
                float glowRadius = style.specialTitle ? snap.glowRadius * 1.1f : snap.glowRadius;
                splitter->SetCurrentChannel(dl, 0);
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
        }

        splitter->SetCurrentChannel(dl, chFront);  // Front layer: shadow + text
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
                            fastOutlines,
                            mainGlow.enabled ? &mainGlow : nullptr,
                            mainDual.enabled ? &mainDual : nullptr,
                            mainWave.enabled ? &mainWave : nullptr,
                            mainShine.enabled ? &mainShine : nullptr);
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
                                fastOutlines,
                                mainGlow.enabled ? &mainGlow : nullptr,
                                mainDual.enabled ? &mainDual : nullptr,
                                mainWave.enabled ? &mainWave : nullptr,
                                mainShine.enabled ? &mainShine : nullptr);
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
                                             fastOutlines,
                                             mainGlow.enabled ? &mainGlow : nullptr);
            }
        }

        currentPos.x += seg.size.x + layout.segmentPadding;
    }
}

}  // namespace Renderer
