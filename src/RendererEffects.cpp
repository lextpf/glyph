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
static void ApplyTextTransparency(ImDrawList* drawList,
                                  int vtxStart,
                                  float innerTextAlpha,
                                  float textGlowAlpha)
{
    if (!drawList)
    {
        return;
    }

    const int vtxEnd = drawList->VtxBuffer.Size;
    if (vtxStart < 0 || vtxStart >= vtxEnd)
    {
        return;
    }

    const bool reduceAlpha = innerTextAlpha < 1.0f;
    const bool glowText = textGlowAlpha > .0f;
    if (!reduceAlpha && !glowText)
    {
        return;
    }

    int maxBatchAlpha = 0;
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        ImU32 c = drawList->VtxBuffer[i].col;
        maxBatchAlpha = std::max(maxBatchAlpha, (int)((c >> IM_COL32_A_SHIFT) & 0xFF));
    }
    if (maxBatchAlpha <= 0)
    {
        return;
    }

    // Main text fill is the brightest pass that also carries the highest alpha in
    // the batch. Lower-alpha support layers such as inner outlines, glows, and
    // shimmer accents should not be made more transparent a second time.
    const int bodyAlphaThreshold = std::max(8, (maxBatchAlpha * 5 + 5) / 6);
    const float alphaKeep = 1.0f - textGlowAlpha * .20f;
    const float brightnessBoost = 1.0f + textGlowAlpha * .30f;

    // Only the bright text body should become translucent. Dark outline/shadow
    // pixels stay solid so readability does not collapse when text glow is on.
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        ImU32 c = drawList->VtxBuffer[i].col;
        int cr = (c >> IM_COL32_R_SHIFT) & 0xFF;
        int cg = (c >> IM_COL32_G_SHIFT) & 0xFF;
        int cb = (c >> IM_COL32_B_SHIFT) & 0xFF;
        int ca = (c >> IM_COL32_A_SHIFT) & 0xFF;
        const float lum = (cr * .299f + cg * .587f + cb * .114f) / 255.0f;
        const int maxCh = std::max({cr, cg, cb});
        const bool isTextFill = lum >= .22f && maxCh >= 80 && ca >= bodyAlphaThreshold;

        if (reduceAlpha && isTextFill)
        {
            ca = (int)std::clamp(ca * innerTextAlpha, .0f, 255.0f);
        }

        if (glowText && isTextFill)
        {
            // Scale all channels uniformly so the brightest channel just
            // reaches 255 - this preserves hue instead of clipping individual
            // channels to white independently.
            float scale = (maxCh > 0) ? std::min(brightnessBoost, 255.0f / (float)maxCh) : 1.0f;
            cr = (int)(cr * scale);
            cg = (int)(cg * scale);
            cb = (int)(cb * scale);
            ca = (int)std::clamp(ca * alphaKeep, .0f, 255.0f);
        }

        drawList->VtxBuffer[i].col = IM_COL32(cr, cg, cb, ca);
    }
}

static ImVec4 MixVec4(const ImVec4& a, const ImVec4& b, float t)
{
    t = std::clamp(t, .0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
}

static void BoostSaturation(ImVec4& c, float amount)
{
    float gray = c.x * .299f + c.y * .587f + c.z * .114f;
    c.x = std::clamp(gray + (c.x - gray) * amount, .0f, 1.0f);
    c.y = std::clamp(gray + (c.y - gray) * amount, .0f, 1.0f);
    c.z = std::clamp(gray + (c.z - gray) * amount, .0f, 1.0f);
}

static ImVec4 DeriveSupportTint(const ImVec4& left,
                                const ImVec4& right,
                                const Settings::Color3& highlight,
                                float highlightMix,
                                float saturationBoost)
{
    ImVec4 support = MixVec4(left, right, .5f);
    support = MixVec4(support, ImVec4(highlight.r, highlight.g, highlight.b, 1.0f), highlightMix);
    BoostSaturation(support, saturationBoost);
    support.w = 1.0f;
    return support;
}

static ImU32 PackSupportTint(const ImVec4& tint, float tintFactor, float alpha)
{
    tintFactor = std::clamp(tintFactor, .0f, 1.0f);
    alpha = TextEffects::Saturate(alpha);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(tint.x * tintFactor, .0f, 1.0f),
                                                 std::clamp(tint.y * tintFactor, .0f, 1.0f),
                                                 std::clamp(tint.z * tintFactor, .0f, 1.0f),
                                                 alpha));
}

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

static void ApplyBreathe(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextBreathe>(
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
        ParamOr(a.effect.param1, .25f),
        ParamOr(a.effect.param2, .06f) * a.strength);
}

static void ApplyDrift(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextDrift>(
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
        ParamOr(a.effect.param1, .08f),
        ParamOr(a.effect.param2, 8.0f) * a.strength);
}

static void ApplyMote(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextMote>(
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
        ParamOr(a.effect.param1, 2.5f),
        ParamOr(a.effect.param2, .4f) * a.strength);
}

static void ApplyWander(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextWander>(
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
        ParamOr(a.effect.param2, .05f) * a.strength,
        ParamOr(a.effect.param3, 1.0f));
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
        case Settings::EffectType::Ember:
            ApplyEmber(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Aurora:
            ApplyAurora(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Sparkle:
            ApplySparkle(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Enchant:
            ApplyEnchant(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Frost:
            ApplyFrost(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Breathe:
            ApplyBreathe(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Drift:
            ApplyDrift(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Mote:
            ApplyMote(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Wander:
            ApplyWander(drawList, font, fontSize, pos, text, args);
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

    // Apply text-body alpha shaping after all fill/outline vertices have been emitted.
    if (shine)
    {
        ApplyTextTransparency(drawList, vtxBefore, shine->innerTextAlpha, shine->textGlowAlpha);
    }

    // Top-edge shine overlay
    if (shine && shine->enabled && shine->intensity > .0f)
    {
        TextEffects::AddTextShineOverlay(
            drawList, font, fontSize, pos, text, shine->intensity, shine->falloff, IM_COL32_WHITE);
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
                                                       const ImVec4& supportTint,
                                                       float alpha)
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
        gr = gr + (supportTint.x - gr) * t;
        gg = gg + (supportTint.y - gg) * t;
        gb = gb + (supportTint.z - gb) * t;
    }

    int a = std::clamp((int)(alpha * 255.0f + .5f), 0, 255);
    glow.color = IM_COL32((int)(gr * 255.0f), (int)(gg * 255.0f), (int)(gb * 255.0f), a);
    return glow;
}

// Build dual-tone directional outline params from snapshot and tier color.
static TextEffects::DualOutlineParams BuildDualOutline(const RenderSettingsSnapshot& snap,
                                                       const ImVec4& supportTint,
                                                       float alpha)
{
    TextEffects::DualOutlineParams dual;
    dual.enabled = snap.dualOutlineEnabled;
    if (!dual.enabled)
    {
        return dual;
    }
    int a = std::clamp((int)(alpha * 255.0f + .5f), 0, 255);
    dual.tierColor = IM_COL32((int)(supportTint.x * 255.0f),
                              (int)(supportTint.y * 255.0f),
                              (int)(supportTint.z * 255.0f),
                              a);
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
    shine.innerTextAlpha = snap.innerTextAlpha;
    shine.textGlowAlpha = snap.textGlowAlpha;
    shine.enabled = snap.enableShine;
    shine.intensity = snap.shineIntensity;
    shine.falloff = snap.shineFalloff;
    return shine;
}

static ImVec4 PrepareMistTint(ImVec4 tint, float brightnessScale, float saturationBoost)
{
    BoostSaturation(tint, saturationBoost);
    tint.x = std::clamp(tint.x * brightnessScale, .0f, 1.0f);
    tint.y = std::clamp(tint.y * brightnessScale, .0f, 1.0f);
    tint.z = std::clamp(tint.z * brightnessScale, .0f, 1.0f);
    tint.w = 1.0f;
    return tint;
}

static ImU32 PackGlowTint(const ImVec4& tint, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(tint.x, .0f, 1.0f),
                                                 std::clamp(tint.y, .0f, 1.0f),
                                                 std::clamp(tint.z, .0f, 1.0f),
                                                 std::clamp(alpha, .0f, 1.0f)));
}

static void DrawMistVeil(ImDrawList* dl,
                         const ImVec2& min,
                         const ImVec2& max,
                         const ImVec4& leftTint,
                         const ImVec4& centerTint,
                         const ImVec4& rightTint,
                         float baseAlpha,
                         float expandX,
                         float expandY,
                         bool gpuGlow)
{
    if (!dl || baseAlpha <= .001f || max.x <= min.x || max.y <= min.y)
    {
        return;
    }

    struct MistLobe
    {
        float offsetX;
        float offsetY;
        float radiusMul;
        float alphaMul;
        float sideMix;
    };

    static constexpr MistLobe kGpuLobes[] = {
        {-0.56f, .05f, 1.08f, .14f, -.94f},
        {-0.34f, -.17f, .92f, .20f, -.62f},
        {-0.10f, .14f, 1.00f, .23f, -.18f},
        {.14f, -.11f, .96f, .24f, .18f},
        {.38f, .11f, .90f, .20f, .62f},
        {.58f, -.01f, 1.05f, .14f, .94f},
        {-0.22f, .30f, .72f, .11f, -.18f},
        {.24f, .29f, .76f, .11f, .18f},
    };
    static constexpr MistLobe kCpuLobes[] = {
        {-0.52f, .04f, 1.02f, .12f, -.88f},
        {-0.31f, -.15f, .88f, .17f, -.56f},
        {-0.08f, .13f, .94f, .20f, -.16f},
        {.12f, -.10f, .92f, .21f, .16f},
        {.35f, .10f, .86f, .17f, .56f},
        {.54f, .00f, .98f, .12f, .88f},
        {-0.20f, .26f, .68f, .09f, -.15f},
        {.22f, .25f, .72f, .09f, .15f},
    };

    const float width = (max.x - min.x) + expandX * 2.0f;
    const float height = (max.y - min.y) + expandY * 2.0f;
    const ImVec2 center((min.x + max.x) * .5f, (min.y + max.y) * .5f);
    const float left = center.x - width * .5f;
    const float right = center.x + width * .5f;
    const float top = center.y - height * .5f;
    const float bottom = center.y + height * .5f;
    const float round = std::max(10.0f, height * .74f);

    // Broad wash that ties the lobes together into a single fog bank.
    dl->AddRectFilled(ImVec2(left - expandX * .10f, top - expandY * .12f),
                      ImVec2(right + expandX * .10f, bottom + expandY * .12f),
                      PackGlowTint(centerTint, baseAlpha * (gpuGlow ? .08f : .06f)),
                      round);

    const float ribbonHalfHeight = height * .22f;
    dl->AddRectFilled(ImVec2(center.x - width * .30f, center.y - ribbonHalfHeight),
                      ImVec2(center.x + width * .30f, center.y + ribbonHalfHeight),
                      PackGlowTint(centerTint, baseAlpha * (gpuGlow ? .07f : .05f)),
                      ribbonHalfHeight);

    const float coreRadius = std::max(height * .64f, std::min(width * .21f, height * 1.34f));
    const float coreOffsets[] = {-0.24f, 0.0f, 0.24f};
    const float coreAlphas[] = {.09f, .15f, .09f};
    for (int i = 0; i < 3; ++i)
    {
        const float mixT = (coreOffsets[i] + .24f) / .48f;
        const ImVec4 tint = MixVec4(leftTint, rightTint, mixT);
        dl->AddCircleFilled(
            ImVec2(center.x + coreOffsets[i] * width, center.y),
            coreRadius,
            PackGlowTint(MixVec4(centerTint, tint, .58f),
                         baseAlpha * (gpuGlow ? coreAlphas[i] : coreAlphas[i] * .82f)),
            gpuGlow ? 30 : 24);
    }

    const MistLobe* lobes = gpuGlow ? kGpuLobes : kCpuLobes;
    const int lobeCount =
        gpuGlow ? static_cast<int>(std::size(kGpuLobes)) : static_cast<int>(std::size(kCpuLobes));
    const float lobeRadius = std::max(height * .84f, std::min(width * .24f, height * 1.54f));

    for (int i = 0; i < lobeCount; ++i)
    {
        const MistLobe& lobe = lobes[i];
        ImVec4 tint = centerTint;
        if (lobe.sideMix < .0f)
        {
            tint = MixVec4(centerTint, leftTint, -lobe.sideMix);
        }
        else if (lobe.sideMix > .0f)
        {
            tint = MixVec4(centerTint, rightTint, lobe.sideMix);
        }

        const ImVec2 lobeCenter(center.x + lobe.offsetX * width * .5f,
                                center.y + lobe.offsetY * height * .6f);
        dl->AddCircleFilled(lobeCenter,
                            lobeRadius * lobe.radiusMul,
                            PackGlowTint(tint, baseAlpha * lobe.alphaMul),
                            gpuGlow ? 30 : 24);
    }
}

void DrawBackgroundGlow(ImDrawList* dl,
                        const LabelStyle& style,
                        const LabelLayout& layout,
                        float lodTitleFactor,
                        ImDrawListSplitter* splitter,
                        const RenderSettingsSnapshot& snap)
{
    if (!dl || !splitter || !snap.enableGlow || snap.glowIntensity <= .0f || !style.tierAllowsGlow)
    {
        return;
    }

    const bool gpuGlow = TextPostProcess::IsInitialized();
    const int glowChannel = 0;
    splitter->SetCurrentChannel(dl, glowChannel);

    if (!gpuGlow)
    {
        ParticleTextures::PushAdditiveBlend(dl);
    }

    const float intensityScale = .58f + snap.glowIntensity * .56f;
    const float specialBoost = style.specialTitle ? 1.18f : 1.0f;

    // Background mist should read as colored fog from the actual name text,
    // not a blended white-ish support plate from title/level accents.
    const ImVec4 nameMidTint = MixVec4(style.LcName, style.RcName, .5f);
    const ImVec4 mistCenterTint = PrepareMistTint(nameMidTint, 1.10f, 1.10f);
    const ImVec4 mistLeftTint = PrepareMistTint(style.LcName, 1.14f, 1.12f);
    const ImVec4 mistRightTint = PrepareMistTint(style.RcName, 1.14f, 1.12f);

    const float fullPadX = std::max(14.0f, layout.nameplateWidth * .12f + snap.glowRadius * .96f);
    const float fullPadY = std::max(11.0f, layout.nameplateHeight * .20f + snap.glowRadius * .84f);
    const float fullAlpha = style.alpha * intensityScale * specialBoost * (gpuGlow ? .32f : .18f);

    DrawMistVeil(dl,
                 ImVec2(layout.nameplateLeft, layout.nameplateTop),
                 ImVec2(layout.nameplateRight, layout.nameplateBottom),
                 mistLeftTint,
                 mistCenterTint,
                 mistRightTint,
                 fullAlpha,
                 fullPadX,
                 fullPadY,
                 gpuGlow);

    if (!layout.titleDisplayStr.empty() && lodTitleFactor > .01f && layout.titleSize.x > .0f)
    {
        const float titleOffsetX = (layout.totalWidth - layout.titleSize.x) * .5f;
        const ImVec2 titleMin(layout.startPos.x - layout.totalWidth * .5f + titleOffsetX,
                              layout.startPos.y + layout.titleY);
        const ImVec2 titleMax(titleMin.x + layout.titleSize.x, titleMin.y + layout.titleSize.y);
        const float titlePadX = std::max(11.0f, layout.titleSize.x * .13f + snap.glowRadius * .66f);
        const float titlePadY = std::max(8.0f, layout.titleSize.y * .28f + snap.glowRadius * .54f);
        const float titleAlpha = style.titleAlpha * lodTitleFactor * intensityScale * specialBoost *
                                 (gpuGlow ? .16f : .10f);
        DrawMistVeil(dl,
                     titleMin,
                     titleMax,
                     mistLeftTint,
                     mistCenterTint,
                     mistRightTint,
                     titleAlpha,
                     titlePadX,
                     titlePadY,
                     gpuGlow);
    }

    if (layout.mainLineWidth > .0f && layout.mainLineHeight > .0f)
    {
        const ImVec2 mainMin(layout.startPos.x - layout.mainLineWidth * .5f,
                             layout.startPos.y + layout.mainLineY);
        const ImVec2 mainMax(mainMin.x + layout.mainLineWidth, mainMin.y + layout.mainLineHeight);
        const float mainPadX =
            std::max(12.0f, layout.mainLineWidth * .10f + snap.glowRadius * .74f);
        const float mainPadY =
            std::max(9.0f, layout.mainLineHeight * .34f + snap.glowRadius * .60f);
        const float mainAlpha = std::max(style.alpha, style.levelAlpha) * intensityScale *
                                specialBoost * (gpuGlow ? .22f : .14f);
        DrawMistVeil(dl,
                     mainMin,
                     mainMax,
                     mistLeftTint,
                     mistCenterTint,
                     mistRightTint,
                     mainAlpha,
                     mainPadX,
                     mainPadY,
                     gpuGlow);
    }

    if (!gpuGlow)
    {
        ParticleTextures::PopBlendState(dl);
    }
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
    cfg.spreadX = layout.nameplateWidth * .5f + snap.particleSpread * 1.55f * pSpacingScale;
    cfg.spreadY = layout.nameplateHeight * .5f + snap.particleSpread * 1.22f * pSpacingScale;

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
        snap.particleSize * (1.12f + .24f * tierBoost + .24f * levelBoost) * pSpacingScale;
    cfg.boostedAlpha = std::clamp(
        snap.particleAlpha * style.alpha * (1.02f + .18f * tierBoost + .18f * levelBoost),
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
        const bool soloWisps = cfg.enabledStyles == 1;
        const float wispSpreadScale = soloWisps ? 1.22f : 1.15f;
        const float wispAlpha = cfg.boostedAlpha * (soloWisps ? .62f : .74f);
        const float wispSize = cfg.boostedSize * (soloWisps ? .78f : .86f);
        const float wispSpeed = snap.particleSpeed * (soloWisps ? .82f : .90f);

        // Keep wisps on the procedural renderer; textured wisp variants still
        // read as thin sticks/rods in motion.
        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX * wispSpreadScale,
                                       cfg.spreadY * wispSpreadScale,
                                       cfg.particleColor,
                                       wispAlpha,
                                       Settings::ParticleStyle::Wisps,
                                       cfg.boostedCount,
                                       wispSize,
                                       wispSpeed,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       false,
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
                                       cfg.boostedSize * .95f,
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

    // Per-tier ornament color overrides bypass pastelization for punchier ornaments.
    // Special titles keep their dedicated color (already stored in style.Lc/Rc).
    ImVec4 ornLv, ornRv;
    auto MixColors = [](const ImVec4& a, const ImVec4& b, float t)
    {
        t = std::clamp(t, .0f, 1.0f);
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
    };
    auto BoostSaturation = [](ImVec4& c, float amount)
    {
        float gray = c.x * .299f + c.y * .587f + c.z * .114f;
        c.x = std::clamp(gray + (c.x - gray) * amount, .0f, 1.0f);
        c.y = std::clamp(gray + (c.y - gray) * amount, .0f, 1.0f);
        c.z = std::clamp(gray + (c.z - gray) * amount, .0f, 1.0f);
    };
    auto DeriveOrnamentAccent =
        [&](const ImVec4& titleCol, const ImVec4& nameCol, float highlightMix) -> ImVec4
    {
        ImVec4 highlight(tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, 1.0f);
        ImVec4 accent = MixColors(titleCol, highlight, highlightMix);
        accent = MixColors(accent, nameCol, .15f);
        BoostSaturation(accent, 1.25f);
        accent.w = style.alpha;
        return accent;
    };
    if (style.specialTitle)
    {
        ornLv = ImVec4(style.Lc.x, style.Lc.y, style.Lc.z, style.alpha);
        ornRv = ImVec4(style.Rc.x, style.Rc.y, style.Rc.z, style.alpha);
    }
    else
    {
        if (tier.ornamentLeftColor)
        {
            const auto& oL = *tier.ornamentLeftColor;
            ornLv = ImVec4(oL.r, oL.g, oL.b, style.alpha);
        }
        else
        {
            ornLv = DeriveOrnamentAccent(
                ImVec4(style.LcTitle.x, style.LcTitle.y, style.LcTitle.z, 1.0f),
                ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, 1.0f),
                .32f);
        }

        if (tier.ornamentRightColor)
        {
            const auto& oR = *tier.ornamentRightColor;
            ornRv = ImVec4(oR.r, oR.g, oR.b, style.alpha);
        }
        else
        {
            ornRv = DeriveOrnamentAccent(
                ImVec4(style.RcTitle.x, style.RcTitle.y, style.RcTitle.z, 1.0f),
                ImVec4(style.RcName.x, style.RcName.y, style.RcName.z, 1.0f),
                .42f);
        }
    }
    ImU32 ornColL = ImGui::ColorConvertFloat4ToU32(ornLv);
    ImU32 ornColR = ImGui::ColorConvertFloat4ToU32(ornRv);
    ImU32 ornHighlight = ImGui::ColorConvertFloat4ToU32(
        ImVec4(tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, style.alpha));
    ImVec4 ornSupportTint = DeriveSupportTint(ornLv, ornRv, tier.highlightColor, .18f, 1.15f);
    ImU32 ornOutline =
        PackSupportTint(ornSupportTint, snap.outlineColorTint, style.alpha * snap.outlineAlpha);
    float ornOutlineWidth = style.outlineWidth * (ornamentSize / layout.nameFontSize);

    ImU32 glowColor = ImGui::ColorConvertFloat4ToU32(
        ImVec4(ornSupportTint.x, ornSupportTint.y, ornSupportTint.z, style.alpha));
    bool showOrnGlow = snap.enableGlow && snap.glowIntensity > .0f && style.tierAllowsGlow;
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int chBack = gpuGlow ? 1 : 0;
    const int chFront = gpuGlow ? 2 : 1;

    auto ornGlow = BuildOutlineGlow(snap, ornSupportTint, style.alpha);
    auto ornDual = BuildDualOutline(snap, ornSupportTint, style.alpha);
    auto ornShine = BuildShineParams(snap);
    const bool ornNeedsTextAdjust =
        snap.enableShine || snap.textGlowAlpha > .0f || snap.innerTextAlpha < 1.0f;

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
                        ornNeedsTextAdjust ? &ornShine : nullptr);
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
    float lodTitleAlphaFinal = style.titleAlpha * lodTitleFactor;
    ImU32 titleShadow =
        d.isPlayer
            ? PackSupportTint(style.supportTitle, snap.shadowColorTint, lodTitleAlphaFinal * .5f)
            : ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlpha * .5f));

    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int chFront = gpuGlow ? 2 : 1;

    if (snap.enableGlow && snap.glowIntensity > .0f && style.tierAllowsGlow)
    {
        ImVec4 glowColorVec = style.specialTitle ? ImVec4(style.specialGlowColor.x,
                                                          style.specialGlowColor.y,
                                                          style.specialGlowColor.z,
                                                          lodTitleAlphaFinal)
                                                 : ImVec4(style.supportTitle.x,
                                                          style.supportTitle.y,
                                                          style.supportTitle.z,
                                                          lodTitleAlphaFinal);
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

    float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    ImU32 titleOutline = PackSupportTint(
        style.supportTitle, snap.outlineColorTint, lodTitleAlphaFinal * snap.outlineAlpha);
    auto titleGlow = BuildOutlineGlow(snap, style.supportTitle, lodTitleAlphaFinal);
    auto titleDual = BuildDualOutline(snap, style.supportTitle, lodTitleAlphaFinal);
    auto titleWave = BuildWaveParams(snap, style, (float)ImGui::GetTime());
    auto titleShine = BuildShineParams(snap);
    const bool titleNeedsTextAdjust =
        snap.enableShine || snap.textGlowAlpha > .0f || snap.innerTextAlpha < 1.0f;

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
                        titleOutline,
                        style.titleOutlineWidth,
                        style.phase01,
                        style.strength,
                        textSizeScale,
                        lodTitleAlphaFinal,
                        fastOutlines,
                        titleGlow.enabled ? &titleGlow : nullptr,
                        titleDual.enabled ? &titleDual : nullptr,
                        titleWave.enabled ? &titleWave : nullptr,
                        titleNeedsTextAdjust ? &titleShine : nullptr);
    }
    else
    {
        ImVec4 dColV = style.dispoCol;
        dColV.w = lodTitleAlphaFinal;
        ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
        ImU32 npcOutline =
            ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, lodTitleAlphaFinal * snap.outlineAlpha));
        const int vtxBefore = dl->VtxBuffer.Size;
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
        ApplyTextTransparency(dl, vtxBefore, snap.innerTextAlpha, snap.textGlowAlpha);
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
    auto nameGlow = BuildOutlineGlow(snap, style.supportName, style.alpha);
    auto levelGlow = BuildOutlineGlow(snap, style.supportLevel, style.levelAlpha);
    auto nameDual = BuildDualOutline(snap, style.supportName, style.alpha);
    auto levelDual = BuildDualOutline(snap, style.supportLevel, style.levelAlpha);
    ImU32 nameOutline =
        PackSupportTint(style.supportName, snap.outlineColorTint, style.alpha * snap.outlineAlpha);
    ImU32 levelOutline = PackSupportTint(
        style.supportLevel, snap.outlineColorTint, style.levelAlpha * snap.outlineAlpha);
    ImU32 nameShadow = PackSupportTint(
        style.supportName, snap.shadowColorTint, style.alpha * .75f * snap.outlineAlpha);
    ImU32 levelShadow = PackSupportTint(
        style.supportLevel, snap.shadowColorTint, style.levelAlpha * .75f * snap.outlineAlpha);
    auto mainWave = BuildWaveParams(snap, style, (float)ImGui::GetTime());
    auto mainShine = BuildShineParams(snap);
    const bool mainNeedsTextAdjust =
        snap.enableShine || snap.textGlowAlpha > .0f || snap.innerTextAlpha < 1.0f;

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
            ImVec4 glowCol = style.specialTitle ? ImVec4(style.specialGlowColor.x,
                                                         style.specialGlowColor.y,
                                                         style.specialGlowColor.z,
                                                         style.alpha)
                                                : (seg.isLevel ? ImVec4(style.supportLevel.x,
                                                                        style.supportLevel.y,
                                                                        style.supportLevel.z,
                                                                        style.levelAlpha)
                                                               : ImVec4(style.supportName.x,
                                                                        style.supportName.y,
                                                                        style.supportName.z,
                                                                        style.alpha));
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
                    seg.isLevel ? levelShadow : nameShadow,
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
                            levelOutline,
                            segOutlineWidth,
                            style.phase01,
                            style.strength,
                            textSizeScale,
                            style.levelAlpha,
                            fastOutlines,
                            levelGlow.enabled ? &levelGlow : nullptr,
                            levelDual.enabled ? &levelDual : nullptr,
                            mainWave.enabled ? &mainWave : nullptr,
                            mainNeedsTextAdjust ? &mainShine : nullptr);
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
                                nameOutline,
                                segOutlineWidth,
                                style.phase01,
                                style.strength,
                                textSizeScale,
                                style.alpha,
                                fastOutlines,
                                nameGlow.enabled ? &nameGlow : nullptr,
                                nameDual.enabled ? &nameDual : nullptr,
                                mainWave.enabled ? &mainWave : nullptr,
                                mainNeedsTextAdjust ? &mainShine : nullptr);
            }
            else
            {
                ImVec4 dColV = style.dispoCol;
                dColV.w = style.alpha;
                ImU32 dCol = ImGui::ColorConvertFloat4ToU32(dColV);
                ImU32 npcOutline = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(0, 0, 0, style.alpha * snap.outlineAlpha));
                const int vtxBefore = dl->VtxBuffer.Size;
                TextEffects::AddTextOutline4(dl,
                                             seg.font,
                                             seg.fontSize,
                                             pos,
                                             seg.displayText.c_str(),
                                             dCol,
                                             npcOutline,
                                             segOutlineWidth,
                                             fastOutlines,
                                             seg.isLevel
                                                 ? (levelGlow.enabled ? &levelGlow : nullptr)
                                                 : (nameGlow.enabled ? &nameGlow : nullptr));
                ApplyTextTransparency(dl, vtxBefore, snap.innerTextAlpha, snap.textGlowAlpha);
            }
        }

        currentPos.x += seg.size.x + layout.segmentPadding;
    }
}

}  // namespace Renderer
