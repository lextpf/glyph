// Render-thread draw passes consume snapshots only. Draw supplies three channels
// with GPU glow (snap.enableGlow && TextPostProcess::IsInitialized), two otherwise:
//
//   gpuGlow                                 no gpuGlow
//     0  glow capture: the mist veil,         0  back: the mist veil, the particle
//        plus one flat AddText copy of           aura, and the multi-copy CPU glow
//        every glowing string                 1  front: shadow, text, badges, emblem
//     1  back: the particle aura
//     2  front: shadow, text, badges, emblem
//
// Channels merge in index order. passes select their channel without restoring it.
//
// Draw order for one live plate (Renderer.cpp, DrawLabel):
//
//   DrawBackgroundGlow -> DrawParticlesAndOrnaments -> DrawTitleText ->
//   DrawMainLineSegments -> DrawInfoLineSegments -> DrawBadges -> DrawTierEmblem

#include "RendererInternal.hpp"

#include "ParticleTextures.hpp"
#include "RenderSampling.hpp"
#include "TextPostProcess.hpp"

#include <cctype>
#include <cstring>

namespace Renderer
{
/**
 * @struct EffectArgs
 * @brief Borrowed effect options shared by text-effect dispatch adapters.
 * @author Alex (<https://github.com/lextpf>)
 */
struct EffectArgs
{
    const Settings::EffectParams& effect;
    ImU32 colL, colR, highlight, outlineColor;
    float outlineWidth, phase01, strength;
    bool fastOutlines;
    TextEffects::OutlineGlowParams outlineGlow;
    TextEffects::DualOutlineParams dualOutline;
};

namespace
{
/**
 * @fn static void ApplyTextTransparency(ImDrawList* drawList, int vtxStart, float innerTextAlpha,
 *     float textGlowAlpha)
 * @brief Shape alpha and brightness on vertices emitted by the text effect.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Reshape only vertices added by this effect; skip the scan when both knobs are off.
 */
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

    // Body alpha threshold is ~83% of the batch maximum, floored at 8.
    // Do not fade lower-alpha support passes twice.
    const int bodyAlphaThreshold = std::max(8, (maxBatchAlpha * 5 + 5) / 6);
    const float alphaKeep = 1.0f - textGlowAlpha * .20f;
    const float brightnessBoost = 1.0f + textGlowAlpha * .30f;

    // Keep dark outlines/shadows solid when making the bright body translucent.
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
            // Scale RGB uniformly to preserve hue at channel saturation.
            float scale = (maxCh > 0) ? std::min(brightnessBoost, 255.0f / (float)maxCh) : 1.0f;
            cr = (int)(cr * scale);
            cg = (int)(cg * scale);
            cb = (int)(cb * scale);
            ca = (int)std::clamp(ca * alphaKeep, .0f, 255.0f);
        }

        drawList->VtxBuffer[i].col = IM_COL32(cr, cg, cb, ca);
    }
}

/**
 * @fn static ImVec4 MixVec4(const ImVec4& a, const ImVec4& b, float t)
 * @brief Blend RGB channels and return an opaque color.
 * @author Alex (<https://github.com/lextpf>)
 */
static ImVec4 MixVec4(const ImVec4& a, const ImVec4& b, float t)
{
    t = std::clamp(t, .0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
}

/**
 * @fn static void BoostSaturation(ImVec4& c, float amount)
 * @brief Scale chroma around Rec.601 luminance and clamp the color channels.
 * @author Alex (<https://github.com/lextpf>)
 */
static void BoostSaturation(ImVec4& c, float amount)
{
    float gray = c.x * .299f + c.y * .587f + c.z * .114f;
    c.x = std::clamp(gray + (c.x - gray) * amount, .0f, 1.0f);
    c.y = std::clamp(gray + (c.y - gray) * amount, .0f, 1.0f);
    c.z = std::clamp(gray + (c.z - gray) * amount, .0f, 1.0f);
}

/**
 * @fn static ImVec4 DeriveSupportTint(const ImVec4& left, const ImVec4& right, const
 *     Settings::Color3& highlight, float highlightMix, float saturationBoost)
 * @brief Resolve an opaque support tint from the gradient and tier highlight.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Support layers share the gradient midpoint pulled toward the tier highlight.
 */
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

/**
 * @fn static ImU32 PackSupportTint(const ImVec4& tint, float tintFactor, float alpha)
 * @brief Pack a darkened support tint with the resolved draw alpha.
 * @author Alex (<https://github.com/lextpf>)
 *
 * tintFactor scales RGB toward black; alpha is already faded.
 */
static ImU32 PackSupportTint(const ImVec4& tint, float tintFactor, float alpha)
{
    tintFactor = std::clamp(tintFactor, .0f, 1.0f);
    alpha = TextEffects::Saturate(alpha);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(tint.x * tintFactor, .0f, 1.0f),
                                                 std::clamp(tint.y * tintFactor, .0f, 1.0f),
                                                 std::clamp(tint.z * tintFactor, .0f, 1.0f),
                                                 alpha));
}

/**
 * @fn static void ApplyNone( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text,
 *     const EffectArgs& a)
 * @brief Draw the configured plain outlined fill with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Strength scales amplitude/intensity, never rate. Gradients, Aurora and Enchant
 * use colors and INI parameters without a strength multiplier.
 */
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

/**
 * @fn static void ApplyGradient( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured horizontal color gradient with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyVerticalGradient( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const
 *     char* text, const EffectArgs& a)
 * @brief Draw the configured vertical color gradient with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyDiagonalGradient( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const
 *     char* text, const EffectArgs& a)
 * @brief Draw the configured diagonal color gradient with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyRadialGradient( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const
 *     char* text, const EffectArgs& a)
 * @brief Draw the configured radial color gradient with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyShimmer( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured moving shimmer with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyEmber( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text,
 *     const EffectArgs& a)
 * @brief Draw the configured ember glow with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyAurora( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured aurora color bands with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplySparkle( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured sparkle highlights with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyEnchant( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured enchantment glow with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyFrost( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text,
 *     const EffectArgs& a)
 * @brief Draw the configured frost highlights with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static void ApplyBreathe( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured breathing opacity with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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
        (std::max)(.10f, ParamOr(a.effect.param2, .16f) * a.strength));
}

/**
 * @fn static void ApplyDrift( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text,
 *     const EffectArgs& a)
 * @brief Draw the configured drifting highlights with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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
        ParamOr(a.effect.param2, 26.0f) * a.strength);
}

/**
 * @fn static void ApplyMote( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text,
 *     const EffectArgs& a)
 * @brief Draw the configured moving mote highlights with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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
        ParamOr(a.effect.param1, 3.5f),
        (std::max)(.50f, ParamOr(a.effect.param2, .60f) * a.strength));
}

/**
 * @fn static void ApplyWander( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured wandering color bands with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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
        (std::max)(.10f, ParamOr(a.effect.param2, .14f) * a.strength),
        ParamOr(a.effect.param3, 1.0f));
}

/**
 * @fn static void ApplyEclipse( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured eclipse shading with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
static void ApplyEclipse(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextEclipse>(
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
        ParamOr(a.effect.param1, .16f),
        ParamOr(a.effect.param2, 1.0f) * a.strength);
}

/**
 * @fn static void ApplyPulse( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text,
 *     const EffectArgs& a)
 * @brief Draw the configured pulsing highlights with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
static void ApplyPulse(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextPulse>(
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
        ParamOr(a.effect.param1, .45f),
        ParamOr(a.effect.param2, .55f) * a.strength);
}

/**
 * @fn static void ApplyElectric( ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char*
 *     text, const EffectArgs& a)
 * @brief Draw the configured electric highlights with shared outline parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
static void ApplyElectric(
    ImDrawList* dl, ImFont* font, float sz, ImVec2 pos, const char* text, const EffectArgs& a)
{
    TextEffects::WithOutlineGlow<TextEffects::AddTextElectric>(
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
        ParamOr(a.effect.param1, .18f),
        ParamOr(a.effect.param2, .85f) * a.strength);
}
}  // namespace

// Five passes, in this order, all into the caller's current splitter channel:
//
//   1. Inner directional outline (dualOutline), under the fill
//   2. The selected effect, which also draws the outer outline and its glow
//   3. Text-body alpha and brightness shaping (shine->innerTextAlpha/textGlowAlpha)
//   4. The top-edge shine overlay (shine->enabled)
//   5. Per-glyph wave displacement of every vertex this call added (wave->enabled)
//
// Null optional pointers skip their passes. textSizeScale and alpha are unused:
// callers pre-fade colors and each effect sizes geometry from fontSize.
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
    const int vtxBefore = drawList->VtxBuffer.Size;

    EffectArgs args{effect,
                    colL,
                    colR,
                    highlight,
                    outlineColor,
                    outlineWidth,
                    phase01,
                    strength,
                    fastOutlines,
                    outlineGlow ? *outlineGlow : TextEffects::OutlineGlowParams{},
                    dualOutline ? *dualOutline : TextEffects::DualOutlineParams{}};

    // Draw inner stamps below fill; overlaid stamps would halve effect contrast.
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
        case Settings::EffectType::Eclipse:
            ApplyEclipse(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Pulse:
            ApplyPulse(drawList, font, fontSize, pos, text, args);
            break;
        case Settings::EffectType::Electric:
            ApplyElectric(drawList, font, fontSize, pos, text, args);
            break;
        default:
            break;
    }

    if (shine)
    {
        ApplyTextTransparency(drawList, vtxBefore, shine->innerTextAlpha, shine->textGlowAlpha);
    }

    if (shine && shine->enabled && shine->intensity > .0f)
    {
        TextEffects::AddTextShineOverlay(
            drawList, font, fontSize, pos, text, shine->intensity, shine->falloff, IM_COL32_WHITE);
    }

    if (wave && wave->enabled)
    {
        const int vtxAfter = drawList->VtxBuffer.Size;
        if (vtxAfter > vtxBefore)
        {
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

/**
 * @fn static TextEffects::OutlineGlowParams BuildOutlineGlow(const RenderSettingsSnapshot& snap,
 *     const ImVec4& supportTint, float alpha, bool allowed)
 * @brief Resolve optional outline-glow parameters for one text role.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Disabled glow leaves other fields at defaults; check enabled before use.
 */
static TextEffects::OutlineGlowParams BuildOutlineGlow(const RenderSettingsSnapshot& snap,
                                                       const ImVec4& supportTint,
                                                       float alpha,
                                                       bool allowed)
{
    TextEffects::OutlineGlowParams glow;
    glow.enabled = allowed && snap.enableOutlineGlow;
    if (!glow.enabled)
    {
        return glow;
    }
    glow.scale = snap.outlineGlowScale;
    glow.alpha = snap.outlineGlowAlpha;
    glow.rings = snap.outlineGlowRings;

    float gr = snap.outlineGlowR;
    float gg = snap.outlineGlowG;
    float gb = snap.outlineGlowB;

    // 85% tier tint avoids stacking excessive white with bloom and shine.
    if (snap.outlineGlowTierTint)
    {
        float t = .85f;  // Blend 85% tier color into the glow
        gr = gr + (supportTint.x - gr) * t;
        gg = gg + (supportTint.y - gg) * t;
        gb = gb + (supportTint.z - gb) * t;
    }

    int a = std::clamp((int)(alpha * 255.0f + .5f), 0, 255);
    glow.color = IM_COL32((int)(gr * 255.0f), (int)(gg * 255.0f), (int)(gb * 255.0f), a);
    return glow;
}

/**
 * @fn static TextEffects::DualOutlineParams BuildDualOutline(const RenderSettingsSnapshot& snap,
 *     const ImVec4& supportTint, float alpha)
 * @brief Resolve the inner outline and its directional lighting parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn static TextEffects::WaveParams BuildWaveParams(const RenderSettingsSnapshot& snap, const
 *     LabelStyle& style, float time)
 * @brief Resolve wave displacement for player or special-title text.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Wave is a player/special-title decoration.
 */
static TextEffects::WaveParams BuildWaveParams(const RenderSettingsSnapshot& snap,
                                               const LabelStyle& style,
                                               float time)
{
    TextEffects::WaveParams wave;
    wave.enabled =
        snap.visual.EnableWave && style.tierIdx >= snap.visual.WaveMinTier && style.usesTierVisuals;
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

/**
 * @fn static TextEffects::ShineParams BuildShineParams(const RenderSettingsSnapshot& snap, const
 *     LabelStyle& style)
 * @brief Resolve top-edge shine and text-body transparency parameters.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Shine is decorative; transparency shaping applies to every actor.
 */
static TextEffects::ShineParams BuildShineParams(const RenderSettingsSnapshot& snap,
                                                 const LabelStyle& style)
{
    TextEffects::ShineParams shine;
    shine.innerTextAlpha = snap.innerTextAlpha;
    shine.textGlowAlpha = snap.textGlowAlpha;
    shine.enabled = snap.enableShine && style.usesTierVisuals;
    shine.intensity = snap.shineIntensity;
    shine.falloff = snap.shineFalloff;
    return shine;
}

/**
 * @fn static ImVec4 PrepareMistTint(ImVec4 tint, float brightnessScale, float saturationBoost)
 * @brief Saturate and brighten a support color for the mist veil.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Saturate before brightening to preserve tint in near-grey tiers.
 */
static ImVec4 PrepareMistTint(ImVec4 tint, float brightnessScale, float saturationBoost)
{
    BoostSaturation(tint, saturationBoost);
    tint.x = std::clamp(tint.x * brightnessScale, .0f, 1.0f);
    tint.y = std::clamp(tint.y * brightnessScale, .0f, 1.0f);
    tint.z = std::clamp(tint.z * brightnessScale, .0f, 1.0f);
    tint.w = 1.0f;
    return tint;
}

/**
 * @fn static ImU32 PackGlowTint(const ImVec4& tint, float alpha)
 * @brief Pack an untapered glow tint with bounded alpha.
 * @author Alex (<https://github.com/lextpf>)
 */
static ImU32 PackGlowTint(const ImVec4& tint, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(tint.x, .0f, 1.0f),
                                                 std::clamp(tint.y, .0f, 1.0f),
                                                 std::clamp(tint.z, .0f, 1.0f),
                                                 std::clamp(alpha, .0f, 1.0f)));
}

/**
 * @fn static void DrawMistVeil(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const ImVec4&
 *     leftTint, const ImVec4& centerTint, const ImVec4& rightTint, float baseAlpha, float expandX,
 *     float expandY, bool gpuGlow)
 * @brief Draw mist lobes sized for the active CPU or GPU glow path.
 * @author Alex (<https://github.com/lextpf>)
 *
 * GPU glow uses broader/brighter lobes to survive blur; no scene sampling occurs.
 */
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

    // Lobe geometry is relative to the expanded veil box.
    struct MistLobe
    {
        float offsetX;    // Center offset from box center, in half-widths
        float offsetY;    // Center offset from box center, in units of 0.6 * height
        float radiusMul;  // Multiple of lobeRadius
        float alphaMul;   // Fraction of baseAlpha
        float sideMix;    // -1 = full leftTint, 0 = centerTint, +1 = full rightTint
    };

    // CPU lobes and core alpha stay smaller/dimmer to avoid visible hard discs.
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

    // Blend lobes into a continuous field.
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
            gpuGlow ? 48 : 32);
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
                            gpuGlow ? 48 : 32);
    }
}

// Whole-plate, title and main-row veils use channel 0 on both glow paths.
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

    // Use name colors; support accents would wash the veil toward white.
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

/**
 * @fn static std::vector<std::string> CollectDrawableOrnaments(const std::string& raw, ImFont*
 *     ornamentFont)
 * @brief Keep UTF-8 ornament characters that have drawable font glyphs.
 * @author Alex (<https://github.com/lextpf>)
 */
static std::vector<std::string> CollectDrawableOrnaments(const std::string& raw,
                                                         ImFont* ornamentFont);

/**
 * @struct OrnamentMetrics
 * @brief Measured ornament geometry shared by drawing and particle placement.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Shared ornament bounds; hidden ornaments leave zero/null fields.
 */
struct OrnamentMetrics
{
    bool shown = false;
    std::vector<std::string> leftChars;
    std::vector<std::string> rightChars;
    ImFont* ornamentFont = nullptr;
    float ornamentSize = .0f;
    float totalSpacing = .0f;
    float ornamentCharGap = .0f;
    float leftExtent = .0f;   // Px occupied left of nameplate (spacing + chars + gaps)
    float rightExtent = .0f;  // Px occupied right of nameplate
};

/**
 * @fn static OrnamentMetrics ComputeOrnamentMetrics(const ActorDrawData& d, const LabelStyle&
 *     style, const LabelLayout& layout, float lodEffectsFactor, const RenderSettingsSnapshot& snap)
 * @brief Measure shared geometry for ornament draws and particle placement.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Measure once for both ornament draws and aura sizing. Only players or forced
 * special titles qualify. Missing font or all-missing glyphs omit ornaments.
 */
static OrnamentMetrics ComputeOrnamentMetrics(const ActorDrawData& d,
                                              const LabelStyle& style,
                                              const LabelLayout& layout,
                                              float lodEffectsFactor,
                                              const RenderSettingsSnapshot& snap)
{
    OrnamentMetrics m{};
    const Settings::TierDefinition& tier = *style.tier;

    const std::string& leftOrns = (style.specialTitle && !style.specialTitle->leftOrnaments.empty())
                                      ? style.specialTitle->leftOrnaments
                                      : tier.leftOrnaments;
    const std::string& rightOrns =
        (style.specialTitle && !style.specialTitle->rightOrnaments.empty())
            ? style.specialTitle->rightOrnaments
            : tier.rightOrnaments;
    const bool hasOrnaments = !leftOrns.empty() || !rightOrns.empty();
    const bool visible =
        ((d.isPlayer && snap.enableOrnaments && hasOrnaments && style.tierAllowsOrnaments) ||
         (style.specialTitle && style.specialTitle->forceOrnaments && hasOrnaments)) &&
        lodEffectsFactor > .01f;

    ImFont* ornamentFont = (ImGui::GetIO().Fonts->Fonts.Size > RenderConstants::FONT_INDEX_ORNAMENT)
                               ? ImGui::GetIO().Fonts->Fonts[RenderConstants::FONT_INDEX_ORNAMENT]
                               : nullptr;
    if (!visible || snap.ornamentFontPath.empty() || !ornamentFont)
    {
        return m;
    }

    m.leftChars = CollectDrawableOrnaments(leftOrns, ornamentFont);
    m.rightChars = CollectDrawableOrnaments(rightOrns, ornamentFont);
    if (m.leftChars.empty() && m.rightChars.empty())
    {
        return m;
    }

    const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    float ornamentScale = .75f;
    if (snap.tiers.size() > 1)
    {
        ornamentScale = .75f + .3f * (static_cast<float>(style.tierIdx) /
                                      static_cast<float>(snap.tiers.size() - 1));
    }
    const float sizeMultiplier =
        (style.specialTitle != nullptr) ? ornamentScale * 1.3f : ornamentScale;
    m.ornamentFont = ornamentFont;
    m.ornamentSize = snap.ornamentFontSize * snap.ornamentScale * sizeMultiplier * textSizeScale;

    const float spacingScale = textSizeScale;
    const float extraPadding = m.ornamentSize * .18f;
    m.totalSpacing = snap.ornamentSpacing * 1.1f * spacingScale + extraPadding;
    m.ornamentCharGap = std::max(1.0f, m.ornamentSize * .08f);

    auto sideExtent = [&](const std::vector<std::string>& chars) -> float
    {
        if (chars.empty())
        {
            return .0f;
        }
        float w = m.totalSpacing;
        for (size_t i = 0; i < chars.size(); ++i)
        {
            w += ornamentFont->CalcTextSizeA(m.ornamentSize, FLT_MAX, .0f, chars[i].c_str()).x;
            if (i + 1 < chars.size())
            {
                w += m.ornamentCharGap;
            }
        }
        return w;
    };
    m.leftExtent = sideExtent(m.leftChars);
    m.rightExtent = sideExtent(m.rightChars);
    m.shown = true;
    return m;
}

/**
 * @struct ParticleTypeSpec
 * @brief Per-style emission tuning matched by the canonical settings token.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Per-style tuning adjusts emission; TextEffectsParticle owns motion/layout.
 * Style identifies the enum and token matches ParticleTypes, independent of row order.
 */
struct ParticleTypeSpec
{
    Settings::ParticleStyle style;
    const char* token;  // Must be lowercase: only the INI side is lowercased at match time
    float spreadXScale;
    float spreadYScale;
    float sizeScale;
    float speedScale;
    float countScale;   // Multiplies boostedCount (with tier weight; clamped per type)
    int minCount = 4;   // Per-type count floor (scaled down by weights below 1)
    int maxCount = 96;  // Per-type count cap - single-accent types (moon/planet) set 2
};

static constexpr int kParticleTypeCount = Settings::kParticleStyleCount;
static constexpr ParticleTypeSpec kParticleTypes[kParticleTypeCount] = {
    {Settings::ParticleStyle::Firefly, "firefly", 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
    {Settings::ParticleStyle::Snow, "snow", 1.00f, 1.05f, 0.90f, 0.70f, 1.00f},
    {Settings::ParticleStyle::Smoke, "smoke", 0.90f, 1.05f, 1.10f, 0.60f, 0.80f},
    {Settings::ParticleStyle::Spark, "spark", 1.00f, 0.80f, 0.70f, 1.50f, 1.00f},
    {Settings::ParticleStyle::Wisp, "wisp", 1.15f, 1.15f, 0.86f, 0.90f, 1.00f},
    {Settings::ParticleStyle::Leaf, "leaf", 1.00f, 1.05f, 1.00f, 0.70f, 0.80f},
    {Settings::ParticleStyle::Aurora, "aurora", 1.10f, 0.90f, 1.00f, 0.60f, 0.80f},
    {Settings::ParticleStyle::CherryBlossom, "cherryblossom", 1.00f, 1.05f, 1.00f, 0.70f, 0.90f},
    {Settings::ParticleStyle::Dust, "dust", 1.00f, 1.00f, 0.80f, 0.40f, 1.00f},
    {Settings::ParticleStyle::Mote, "mote", 1.00f, 1.00f, 1.00f, 0.50f, 0.90f},
    {Settings::ParticleStyle::Arcane, "arcane", 1.05f, 1.00f, 1.05f, 0.85f, 0.70f},
    {Settings::ParticleStyle::Ash, "ash", 1.00f, 1.05f, 0.90f, 0.80f, 1.00f},
    {Settings::ParticleStyle::Bat, "bat", 1.15f, 1.00f, 1.15f, 1.10f, 0.60f, 3},
    {Settings::ParticleStyle::Bubble, "bubble", 0.95f, 1.05f, 1.00f, 0.90f, 0.80f},
    {Settings::ParticleStyle::Butterfly, "butterfly", 1.10f, 1.05f, 1.10f, 0.80f, 0.60f, 3},
    {Settings::ParticleStyle::Coin, "coin", 1.00f, 1.05f, 1.05f, 1.20f, 0.80f},
    {Settings::ParticleStyle::Confetti, "confetti", 1.00f, 1.05f, 0.95f, 1.10f, 1.00f},
    {Settings::ParticleStyle::Constellation, "constellation", 1.10f, 0.95f, 1.10f, 0.50f, 0.70f},
    {Settings::ParticleStyle::Curse, "curse", 1.05f, 1.00f, 1.10f, 0.70f, 0.60f, 3},
    {Settings::ParticleStyle::Enchant, "enchant", 1.05f, 1.05f, 0.95f, 0.90f, 1.00f},
    {Settings::ParticleStyle::Fairy, "fairy", 1.10f, 1.10f, 1.00f, 1.00f, 0.70f, 3},
    {Settings::ParticleStyle::Fog, "fog", 1.15f, 0.95f, 1.60f, 0.50f, 0.60f},
    {Settings::ParticleStyle::Gem, "gem", 1.00f, 0.95f, 1.00f, 0.75f, 0.70f},
    {Settings::ParticleStyle::Glitter, "glitter", 1.00f, 1.00f, 0.85f, 0.90f, 1.20f, 6},
    {Settings::ParticleStyle::Heart, "heart", 0.95f, 1.05f, 1.05f, 0.85f, 0.70f},
    {Settings::ParticleStyle::Hex, "hex", 1.05f, 1.00f, 1.05f, 0.75f, 0.70f},
    {Settings::ParticleStyle::Ink, "ink", 0.95f, 1.05f, 0.95f, 1.30f, 0.80f},
    {Settings::ParticleStyle::Moon, "moon", 1.10f, 0.95f, 1.45f, 0.45f, 0.10f, 1, 2},
    {Settings::ParticleStyle::Planet, "planet", 1.15f, 1.00f, 1.50f, 0.50f, 0.10f, 1, 2},
    {Settings::ParticleStyle::Pollen, "pollen", 1.05f, 1.05f, 0.85f, 0.70f, 1.10f, 5},
    {Settings::ParticleStyle::Soul, "soul", 1.00f, 1.05f, 1.15f, 0.70f, 0.70f, 3},
    {Settings::ParticleStyle::Steam, "steam", 0.90f, 1.05f, 1.00f, 1.00f, 0.80f},
    {Settings::ParticleStyle::Void, "void", 1.05f, 1.00f, 1.20f, 0.70f, 0.60f, 3},
    {Settings::ParticleStyle::Vortex, "vortex", 1.05f, 1.00f, 1.10f, 1.10f, 0.70f},
    {Settings::ParticleStyle::Wind, "wind", 1.15f, 0.95f, 1.05f, 1.40f, 0.80f},
    {Settings::ParticleStyle::Zap, "zap", 1.05f, 1.00f, 1.10f, 1.20f, 0.70f},
    {Settings::ParticleStyle::Zzz, "zzz", 1.00f, 1.05f, 1.00f, 0.70f, 0.60f, 3},
    {Settings::ParticleStyle::Ember, "ember", 1.00f, 1.05f, 0.90f, 0.85f, 0.90f},
    {Settings::ParticleStyle::Pixiedust, "pixiedust", 1.05f, 1.05f, 0.85f, 0.75f, 1.10f, 5},
    {Settings::ParticleStyle::Runes, "runes", 1.05f, 1.00f, 1.05f, 0.80f, 0.70f},
    {Settings::ParticleStyle::Sand, "sand", 1.15f, 0.95f, 0.90f, 1.00f, 0.90f},
};
// Require one canonical Settings token per style.
static_assert(
    []() consteval
    {
        auto eq = [](const char* a, const char* b)
        {
            while (*a && *b && *a == *b)
            {
                ++a;
                ++b;
            }
            return *a == *b;
        };
        bool seen[kParticleTypeCount] = {};
        for (const auto& row : kParticleTypes)
        {
            const int idx = static_cast<int>(row.style);
            if (idx < 0 || idx >= kParticleTypeCount || seen[idx] ||
                !eq(row.token, Settings::kParticleStyleTokens[idx].token))
            {
                return false;
            }
            seen[idx] = true;
        }
        return true;
    }(),
    "kParticleTypes must cover every ParticleStyle once, with canonical tokens");

/**
 * @fn static float ParticleTypeWeight(const std::string& particleTypes, const char* token)
 * @brief Read the bounded weight of one enabled particle style token.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Parse whole, trimmed, case-insensitive type[:weight] tokens. Absent types yield 0;
 * bad weights use 1; valid weights clamp to [kMinTypeWeight,kMaxTypeWeight].
 * ComputeParticleConfig mean-normalizes ratios to preserve total density.
 */
static float ParticleTypeWeight(const std::string& particleTypes, const char* token)
{
    constexpr float kMinTypeWeight = 0.1f;
    constexpr float kMaxTypeWeight = 10.0f;
    const size_t tokenLen = std::strlen(token);
    size_t start = 0;
    while (start <= particleTypes.size())
    {
        size_t comma = particleTypes.find(',', start);
        size_t end = (comma == std::string::npos) ? particleTypes.size() : comma;
        size_t colon = particleTypes.find(':', start);
        size_t typeEnd = (colon != std::string::npos && colon < end) ? colon : end;
        size_t a = start, b = typeEnd;
        while (a < b && std::isspace(static_cast<unsigned char>(particleTypes[a])))
        {
            ++a;
        }
        while (b > a && std::isspace(static_cast<unsigned char>(particleTypes[b - 1])))
        {
            --b;
        }
        const size_t len = b - a;
        bool match = (tokenLen == len);
        for (size_t i = 0; match && i < len; ++i)
        {
            match = std::tolower(static_cast<unsigned char>(particleTypes[a + i])) ==
                    static_cast<unsigned char>(token[i]);
        }
        if (match)
        {
            float weight = 1.0f;
            if (colon != std::string::npos && colon < end)
            {
                try
                {
                    weight = std::stof(particleTypes.substr(colon + 1, end - (colon + 1)));
                }
                catch (...)
                {
                    weight = 1.0f;
                }
            }
            return std::clamp(weight, kMinTypeWeight, kMaxTypeWeight);
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return .0f;
}

/**
 * @struct ParticleConfig
 * @brief Resolved particle styles and normalized emission values for a plate.
 * @author Alex (<https://github.com/lextpf>)
 */
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
    bool enabled[kParticleTypeCount];
    float weight[kParticleTypeCount];  // Mean-normalized per-type ratio weight
    int enabledStyles;
};

/**
 * @fn static ParticleConfig ComputeParticleConfig(const ActorDrawData& d, const LabelStyle& style,
 *     const LabelLayout& layout, float lodEffectsFactor, const RenderSettingsSnapshot& snap, const
 *     OrnamentMetrics& ornMetrics)
 * @brief Resolve enabled particle styles and normalize their relative density.
 * @author Alex (<https://github.com/lextpf>)
 */
static ParticleConfig ComputeParticleConfig(const ActorDrawData& d,
                                            const LabelStyle& style,
                                            const LabelLayout& layout,
                                            float lodEffectsFactor,
                                            const RenderSettingsSnapshot& snap,
                                            const OrnamentMetrics& ornMetrics)
{
    ParticleConfig cfg{};
    const Settings::TierDefinition& tier = *style.tier;
    const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);

    // ParticleTypes alone selects styles. ForceParticles bypasses actor/master/tier
    // gates but cannot add types to an empty or none list. Ordinary auras are player-only.
    const bool tierHasParticles = !tier.particleTypes.empty() && tier.particleTypes != "None";
    cfg.showParticles =
        ((d.isPlayer && snap.enableParticleAura && tierHasParticles && style.tierAllowsParticles) ||
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
        cfg.particleColorSecondary = 0;  // Single color for special titles
    }
    else
    {
        const Settings::Color3& pc = tier.particleColor.value_or(tier.highlightColor);
        cfg.particleColor = ImGui::ColorConvertFloat4ToU32(ImVec4(pc.r, pc.g, pc.b, 1.0f));
        cfg.particleColorSecondary = ImGui::ColorConvertFloat4ToU32(
            ImVec4(style.RcName.x, style.RcName.y, style.RcName.z, 1.0f));
    }

    const float pSpacingScale = layout.nameFontSize / layout.fontName->FontSize;

    // Inflate symmetrically around ornaments to keep the aura centered on the actor.
    const float ornHalfInflate = std::max(ornMetrics.leftExtent, ornMetrics.rightExtent);

    cfg.spreadX =
        layout.nameplateWidth * .5f + ornHalfInflate + snap.particleSpread * 1.55f * pSpacingScale;

    // Retain vertical coverage for enlarged ornaments even at low particleSpread.
    const float baseSpreadY = snap.particleSpread * 1.22f * pSpacingScale;
    const float ornVerticalPad = ornMetrics.shown ? ornMetrics.ornamentSize * .5f + 2.0f : .0f;
    cfg.spreadY = layout.nameplateHeight * .5f + std::max(baseSpreadY, ornVerticalPad);

    int particleCount = (tier.particleCount > 0) ? tier.particleCount : snap.particleCount;
    float tierBoost = .0f;
    if (snap.tiers.size() > 1)
    {
        tierBoost = static_cast<float>(style.tierIdx) / static_cast<float>(snap.tiers.size() - 1);
    }
    // Tier position and the level 100-500 ramp contribute equally. Count spans 1-1.6,
    // size 1.12-1.60, alpha 1.02-1.38. Count never falls below particleCount.
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

    cfg.enabledStyles = 0;
    float sumWeights = .0f;
    for (int i = 0; i < kParticleTypeCount; ++i)
    {
        cfg.weight[i] = ParticleTypeWeight(tier.particleTypes, kParticleTypes[i].token);
        cfg.enabled[i] = cfg.weight[i] > .0f;
        if (cfg.enabled[i])
        {
            ++cfg.enabledStyles;
            sumWeights += cfg.weight[i];
        }
    }
    // Mean-normalize weights to redistribute density without changing the total budget.
    if (cfg.enabledStyles > 0 && sumWeights > .0f)
    {
        const float norm = static_cast<float>(cfg.enabledStyles) / sumWeights;
        for (int i = 0; i < kParticleTypeCount; ++i)
        {
            cfg.weight[i] *= norm;
        }
    }
    return cfg;
}

/**
 * @fn void DrawParticles(ImDrawList*, const ActorDrawData&, const LabelStyle&, const LabelLayout&,
 *     float, float, ImDrawListSplitter*, const RenderSettingsSnapshot&, const OrnamentMetrics&)
 * @brief Draw the configured particle styles around the measured plate.
 * @author Alex (<https://github.com/lextpf>)
 */
void DrawParticles(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodEffectsFactor,
                   float time,
                   ImDrawListSplitter* splitter,
                   const RenderSettingsSnapshot& snap,
                   const OrnamentMetrics& ornMetrics)
{
    ParticleConfig cfg =
        ComputeParticleConfig(d, style, layout, lodEffectsFactor, snap, ornMetrics);
    if (!cfg.showParticles)
    {
        return;
    }

    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    splitter->SetCurrentChannel(dl, gpuGlow ? 1 : 0);  // Back layer: particles
    int slot = 0;

    const bool useTextures = snap.useParticleTextures;

    const int blendMode = snap.particleBlendMode;

    for (int i = 0; i < kParticleTypeCount; ++i)
    {
        if (!cfg.enabled[i])
        {
            continue;
        }
        const ParticleTypeSpec& spec = kParticleTypes[i];
        // Scale floors below weight 1 so small ratios survive; caps retain accent counts.
        const int lo = (std::max)(1,
                                  static_cast<int>(static_cast<float>(spec.minCount) *
                                                   std::min(1.0f, cfg.weight[i])));
        const int count =
            std::clamp(static_cast<int>(cfg.boostedCount * spec.countScale * cfg.weight[i]),
                       lo,
                       spec.maxCount);

        TextEffects::DrawParticleAura({dl,
                                       layout.nameplateCenter,
                                       cfg.spreadX * spec.spreadXScale,
                                       cfg.spreadY * spec.spreadYScale,
                                       cfg.particleColor,
                                       cfg.boostedAlpha,
                                       spec.style,
                                       count,
                                       cfg.boostedSize * spec.sizeScale,
                                       snap.particleSpeed * spec.speedScale,
                                       time,
                                       slot++,
                                       cfg.enabledStyles,
                                       useTextures,
                                       blendMode,
                                       cfg.particleColorSecondary,
                                       snap.particleDepthStrength,
                                       snap.particleColorWarmth,
                                       snap.particleGlowStrength,
                                       snap.particleGlowSize,
                                       snap.particleShineThreshold});
    }
}

// Exclude replacement/control characters and missing glyphs.
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

/**
 * @fn void DrawOrnaments(ImDrawList*, const ActorDrawData&, const LabelStyle&, const LabelLayout&,
 *     float, float, ImDrawListSplitter*, bool, const RenderSettingsSnapshot&, const
 *     OrnamentMetrics&)
 * @brief Draw paired ornament glyphs using the shared measurement result.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Metrics must come from this label ComputeOrnamentMetrics call. Its gates already
 * consumed d and lodEffectsFactor; phase comes from style, not time.
 */
void DrawOrnaments(ImDrawList* dl,
                   const ActorDrawData& d,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodEffectsFactor,
                   float time,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap,
                   const OrnamentMetrics& metrics)
{
    const Settings::TierDefinition& tier = *style.tier;

    if (!metrics.shown)
    {
        return;
    }
    const auto& leftChars = metrics.leftChars;
    const auto& rightChars = metrics.rightChars;
    ImFont* const ornamentFont = metrics.ornamentFont;
    const float ornamentSize = metrics.ornamentSize;
    const float totalSpacing = metrics.totalSpacing;
    const float ornamentCharGap = metrics.ornamentCharGap;
    const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;

    // Tier ornament colors may differ from text; special titles retain their own color.
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
        ornLv = ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, style.alpha);
        ornRv = ImVec4(style.RcName.x, style.RcName.y, style.RcName.z, style.alpha);
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

    auto ornGlow = BuildOutlineGlow(snap, ornSupportTint, style.alpha, style.outlineGlowAllowed);
    auto ornDual = BuildDualOutline(snap, ornSupportTint, style.alpha);
    auto ornShine = BuildShineParams(snap, style);
    const bool ornNeedsTextAdjust =
        snap.enableShine || snap.textGlowAlpha > .0f || snap.innerTextAlpha < 1.0f;

    auto drawOrnChar = [&](ImVec2 charPos, const char* ch)
    {
        if (showOrnGlow)
        {
            if (gpuGlow)
            {
                splitter->SetCurrentChannel(dl, 0);
                dl->AddText(ornamentFont, ornamentSize, charPos, glowColor, ch);
            }
            else
            {
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

    // Vertical nudge scales with the plate; negative moves upward.
    const float ornAnchorY =
        (snap.ornamentAnchorToMainLine ? layout.mainLineCenterY : layout.nameplateCenter.y) +
        snap.ornamentOffsetY * textSizeScale;

    // Center ornament ink, not the em box, on the name cap-band anchor.
    // One offset preserves a shared baseline across both sides.
    float ornInkTop = +FLT_MAX;
    float ornInkBottom = -FLT_MAX;
    auto accumulateInk = [&](const std::vector<std::string>& chars)
    {
        for (const std::string& ch : chars)
        {
            float top, bottom;
            CalcTightYBoundsFromTop(ornamentFont, ornamentSize, ch.c_str(), top, bottom);
            if (bottom > top)
            {
                ornInkTop = std::min(ornInkTop, top);
                ornInkBottom = std::max(ornInkBottom, bottom);
            }
        }
    };
    accumulateInk(leftChars);
    accumulateInk(rightChars);
    const float ornYOffset =
        (ornInkBottom > ornInkTop) ? (ornInkTop + ornInkBottom) * .5f : ornamentSize * .5f;

    if (!leftChars.empty())
    {
        float cursorX = layout.nameplateCenter.x - layout.nameplateWidth * .5f - totalSpacing;
        for (int i = static_cast<int>(leftChars.size()) - 1; i >= 0; --i)
        {
            const std::string& ch = leftChars[i];
            ImVec2 charSize = ornamentFont->CalcTextSizeA(ornamentSize, FLT_MAX, .0f, ch.c_str());
            cursorX -= charSize.x;
            ImVec2 charPos(cursorX, ornAnchorY - ornYOffset);
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
            ImVec2 charPos(cursorX, ornAnchorY - ornYOffset);
            drawOrnChar(charPos, ch.c_str());
            cursorX += charSize.x;
            if (i + 1 < rightChars.size())
            {
                cursorX += ornamentCharGap;
            }
        }
    }
}

void DrawParticlesAndOrnaments(ImDrawList* dl,
                               const ActorDrawData& d,
                               const LabelStyle& style,
                               const LabelLayout& layout,
                               float lodEffectsFactor,
                               float time,
                               ImDrawListSplitter* splitter,
                               bool fastOutlines,
                               const RenderSettingsSnapshot& snap)
{
    const OrnamentMetrics ornMetrics =
        ComputeOrnamentMetrics(d, style, layout, lodEffectsFactor, snap);
    DrawParticles(dl, d, style, layout, lodEffectsFactor, time, splitter, snap, ornMetrics);
    DrawOrnaments(
        dl, d, style, layout, lodEffectsFactor, time, splitter, fastOutlines, snap, ornMetrics);
}

void DrawTitleText(ImDrawList* dl,
                   const LabelStyle& style,
                   const LabelLayout& layout,
                   float lodTitleFactor,
                   ImDrawListSplitter* splitter,
                   bool fastOutlines,
                   const RenderSettingsSnapshot& snap)
{
    const char* titleDisplayText = layout.titleDisplayStr.c_str();
    if (!*titleDisplayText || lodTitleFactor <= .01f)
    {
        return;
    }

    const float spacingScale = layout.nameFontSize / layout.fontName->FontSize;

    float titleOffsetX = (layout.totalWidth - layout.titleSize.x) * .5f;
    ImVec2 titlePos(layout.startPos.x - layout.totalWidth * .5f + titleOffsetX,
                    layout.startPos.y + layout.titleY);

    float lodTitleAlphaFinal = style.titleAlpha * lodTitleFactor;
    ImU32 titleShadow =
        PackSupportTint(style.supportTitle, snap.shadowColorTint, lodTitleAlphaFinal * .5f);

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
            splitter->SetCurrentChannel(dl, 0);
            dl->AddText(
                layout.fontTitle, layout.titleFontSize, titlePos, glowColor, titleDisplayText);
        }
        else
        {
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
    if (snap.softShadowEnabled)
    {
        const float ang = snap.softShadowAngle * 0.01745329252f;  // Deg -> rad
        TextEffects::AddTextSoftShadow(dl,
                                       layout.fontTitle,
                                       layout.titleFontSize,
                                       titlePos,
                                       titleDisplayText,
                                       titleShadow,
                                       std::cos(ang),
                                       std::sin(ang),
                                       snap.softShadowDistance * spacingScale,
                                       snap.softShadowSoftness * spacingScale,
                                       snap.softShadowOpacity,
                                       snap.softShadowSamples);
    }
    else
    {
        dl->AddText(layout.fontTitle,
                    layout.titleFontSize,
                    ImVec2(titlePos.x + snap.titleShadowOffsetX * spacingScale,
                           titlePos.y + snap.titleShadowOffsetY * spacingScale),
                    titleShadow,
                    titleDisplayText);
    }

    float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    ImU32 titleOutline = PackSupportTint(
        style.supportTitle, snap.outlineColorTint, lodTitleAlphaFinal * snap.outlineAlpha);
    auto titleGlow =
        BuildOutlineGlow(snap, style.supportTitle, lodTitleAlphaFinal, style.outlineGlowAllowed);
    auto titleDual = BuildDualOutline(snap, style.supportTitle, lodTitleAlphaFinal);
    auto titleWave = BuildWaveParams(snap, style, (float)ImGui::GetTime());
    auto titleShine = BuildShineParams(snap, style);
    const bool titleNeedsTextAdjust =
        snap.enableShine || snap.textGlowAlpha > .0f || snap.innerTextAlpha < 1.0f;

    ApplyTextEffect(dl,
                    layout.fontTitle,
                    layout.titleFontSize,
                    titlePos,
                    titleDisplayText,
                    *style.titleEffect,
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

/**
 * @fn static void DrawSegmentRow(ImDrawList*, const LabelStyle&, const LabelLayout&, const
 *     std::vector<RenderSeg>&, float, float, float, ImDrawListSplitter*, bool, const
 *     RenderSettingsSnapshot&)
 * @brief Draw one formatted row with shared font, outline, and effect rules.
 * @author Alex (<https://github.com/lextpf>)
 *
 * main/info rows share effects and differ only in segment and line geometry.
 */
static void DrawSegmentRow(ImDrawList* dl,
                           const LabelStyle& style,
                           const LabelLayout& layout,
                           const std::vector<RenderSeg>& segments,
                           float lineWidth,
                           float lineHeight,
                           float lineY,
                           ImDrawListSplitter* splitter,
                           bool fastOutlines,
                           const RenderSettingsSnapshot& snap)
{
    const float textSizeScale = layout.nameFontSize / layout.fontName->FontSize;
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int chFront = gpuGlow ? 2 : 1;
    auto nameGlow =
        BuildOutlineGlow(snap, style.supportName, style.alpha, style.outlineGlowAllowed);
    auto levelGlow =
        BuildOutlineGlow(snap, style.supportLevel, style.levelAlpha, style.outlineGlowAllowed);
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
    auto mainShine = BuildShineParams(snap, style);
    const bool mainNeedsTextAdjust =
        snap.enableShine || snap.textGlowAlpha > .0f || snap.innerTextAlpha < 1.0f;

    ImVec2 currentPos;
    currentPos.x =
        layout.startPos.x - layout.totalWidth * .5f + (layout.totalWidth - lineWidth) * .5f;
    currentPos.y = layout.startPos.y + lineY;

    for (const auto& seg : segments)
    {
        if (seg.displayText.empty())
        {
            currentPos.x += seg.size.x + layout.segmentPadding;
            continue;
        }

        float vOffset = (lineHeight - seg.size.y) * .5f;
        ImVec2 pos = ImVec2(currentPos.x, currentPos.y + vOffset);
        const int segmentVertexStart = dl->VtxBuffer.Size;
        const float segmentSpacingScale = seg.font && seg.font->FontSize > .0f
                                              ? seg.fontSize / seg.font->FontSize
                                              : textSizeScale;

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
                splitter->SetCurrentChannel(dl, 0);
                dl->AddText(seg.font, seg.fontSize, pos, glowColor, seg.displayText.c_str());
            }
            else
            {
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
        const ImU32 segShadow = seg.isLevel ? levelShadow : nameShadow;
        if (snap.softShadowEnabled)
        {
            const float ang = snap.softShadowAngle * 0.01745329252f;  // Deg -> rad
            TextEffects::AddTextSoftShadow(dl,
                                           seg.font,
                                           seg.fontSize,
                                           pos,
                                           seg.displayText.c_str(),
                                           segShadow,
                                           std::cos(ang),
                                           std::sin(ang),
                                           snap.softShadowDistance * segmentSpacingScale,
                                           snap.softShadowSoftness * segmentSpacingScale,
                                           snap.softShadowOpacity,
                                           snap.softShadowSamples);
        }
        else
        {
            dl->AddText(seg.font,
                        seg.fontSize,
                        ImVec2(pos.x + snap.mainShadowOffsetX * segmentSpacingScale,
                               pos.y + snap.mainShadowOffsetY * segmentSpacingScale),
                        segShadow,
                        seg.displayText.c_str());
        }

        const float segOutlineWidth = style.CalcOutlineWidth(seg.fontSize, snap);

        if (seg.isLevel)
        {
            ApplyTextEffect(dl,
                            seg.font,
                            seg.fontSize,
                            pos,
                            seg.displayText.c_str(),
                            *style.levelEffect,
                            style.colLLevel,
                            style.colRLevel,
                            style.highlight,
                            levelOutline,
                            segOutlineWidth,
                            style.phase01,
                            style.strength,
                            segmentSpacingScale,
                            style.levelAlpha,
                            fastOutlines,
                            levelGlow.enabled ? &levelGlow : nullptr,
                            levelDual.enabled ? &levelDual : nullptr,
                            mainWave.enabled ? &mainWave : nullptr,
                            mainNeedsTextAdjust ? &mainShine : nullptr);
        }
        else
        {
            ApplyTextEffect(dl,
                            seg.font,
                            seg.fontSize,
                            pos,
                            seg.displayText.c_str(),
                            *style.nameEffect,
                            style.colL,
                            style.colR,
                            style.highlight,
                            nameOutline,
                            segOutlineWidth,
                            style.phase01,
                            style.strength,
                            segmentSpacingScale,
                            style.alpha,
                            fastOutlines,
                            nameGlow.enabled ? &nameGlow : nullptr,
                            nameDual.enabled ? &nameDual : nullptr,
                            mainWave.enabled ? &mainWave : nullptr,
                            mainNeedsTextAdjust ? &mainShine : nullptr);
        }

        ScaleNewVerticesX(dl, segmentVertexStart, pos.x, seg.horizontalScale);
        currentPos.x += seg.size.x + layout.segmentPadding;
    }
}

void DrawMainLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap)
{
    DrawSegmentRow(dl,
                   style,
                   layout,
                   layout.segments,
                   layout.mainLineWidth,
                   layout.mainLineHeight,
                   layout.mainLineY,
                   splitter,
                   fastOutlines,
                   snap);
}

void DrawInfoLineSegments(ImDrawList* dl,
                          const LabelStyle& style,
                          const LabelLayout& layout,
                          ImDrawListSplitter* splitter,
                          bool fastOutlines,
                          const RenderSettingsSnapshot& snap)
{
    if (layout.infoSegments.empty())
    {
        return;
    }
    if (style.infoAlphaMul < .01f)
    {
        return;
    }

    // Apply info alpha to a copy; other rows still need the original style.
    LabelStyle infoStyle = style;
    infoStyle.alpha *= style.infoAlphaMul;
    infoStyle.titleAlpha *= style.infoAlphaMul;
    infoStyle.levelAlpha *= style.infoAlphaMul;

    DrawSegmentRow(dl,
                   infoStyle,
                   layout,
                   layout.infoSegments,
                   layout.infoLineWidth,
                   layout.infoLineHeight,
                   layout.infoLineY,
                   splitter,
                   fastOutlines,
                   snap);
}

// Badge alpha is style.alpha * badgeAlphaMul, independent of infoAlphaMul.
// Lit icons add shadow/glow; muted icons remain flat except player rim lighting.
// Texture alpha carries duotone opacities.
void DrawBadges(ImDrawList* dl,
                const LabelStyle& style,
                const LabelLayout& layout,
                ImDrawListSplitter* splitter,
                bool,
                const RenderSettingsSnapshot& snap,
                bool restrainedWorld)
{
    if (layout.badges.empty())
    {
        return;
    }

    // restrainedWorld still uses the active front channel.
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    splitter->SetCurrentChannel(dl, gpuGlow ? 2 : 1);
    RenderSampling::PushBadgeSampler(dl);

    const float spacingScale = layout.nameFontSize / layout.fontName->FontSize;
    const ImVec2 shadowOff(snap.mainShadowOffsetX * spacingScale * .5f,
                           snap.mainShadowOffsetY * spacingScale * .5f);

    // Player-only breathing discs light the strip without changing muted icons.
    // Missing discs skip the bed; badgeAlphaMul folds it with the strip.
    if (!restrainedWorld && layout.isPlayer && snap.icons.playerStripBedEnabled &&
        !layout.badges.empty() && ParticleTextures::HasSoftGlow())
    {
        float minL = FLT_MAX, maxR = -FLT_MAX, rowH = .0f, cy = .0f;
        for (const auto& b : layout.badges)
        {
            minL = std::min(minL, b.pos.x);
            maxR = std::max(maxR, b.pos.x + b.size.x);
            rowH = std::max(rowH, b.size.y);
            cy = b.pos.y + b.size.y * .5f;
        }
        const float breathe =
            .85f + .15f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.2831853f *
                                   snap.icons.playerStripBedBreatheHz);
        const float bedA =
            style.alpha * style.badgeAlphaMul * snap.icons.playerStripBedAlpha * breathe;
        if (bedA > .002f)
        {
            ImVec4 acc;
            if (snap.icons.playerStripBedColor.has_value())
            {
                const auto& col = *snap.icons.playerStripBedColor;
                acc = ImVec4(col.r, col.g, col.b, 1.0f);
            }
            else
            {
                const ImVec4& base = style.LcName;
                const float luma = .299f * base.x + .587f * base.y + .114f * base.z;
                const float k = .65f;  // Near-neutral
                acc = ImVec4(base.x + (luma - base.x) * k,
                             base.y + (luma - base.y) * k,
                             base.z + (luma - base.z) * k,
                             1.0f);
            }
            const ImU32 bedCol = IM_COL32(std::clamp((int)(acc.x * 255.0f), 0, 255),
                                          std::clamp((int)(acc.y * 255.0f), 0, 255),
                                          std::clamp((int)(acc.z * 255.0f), 0, 255),
                                          std::clamp((int)(bedA * 255.0f), 0, 255));
            const float stripW = maxR - minL;
            const float edge = rowH * snap.icons.playerStripBedSize;
            // Disc centers one visible radius apart keep the light continuous.
            const float spacing = std::max(1.0f, edge / 3.0f);
            const int count = std::max(2, static_cast<int>(std::ceil(stripW / spacing)) + 1);
            for (int i = 0; i < count; ++i)
            {
                const float t =
                    (count > 1) ? static_cast<float>(i) / static_cast<float>(count - 1) : .5f;
                ParticleTextures::DrawSoftGlow(dl, ImVec2(minL + t * stripW, cy), edge, bedCol);
            }
        }
    }

    for (const auto& b : layout.badges)
    {
        float a = style.alpha * style.badgeAlphaMul;
        if (b.pulse && snap.icons.deadlyPulse)
        {
            // 1.2 rad/s (~0.19 hz) gives a slow warning pulse.
            a *= .75f + .25f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.2f);
        }

        // IconOpacity applies to every badge; mutedAlpha adds a resting multiplier.
        a = (std::min)(1.0f, a * snap.icons.opacity);

        Settings::Color3 c = b.color;
        if (b.muted)
        {
            a *= snap.icons.mutedAlpha;
            const float luma = .299f * c.r + .587f * c.g + .114f * c.b;
            const float k = snap.icons.mutedDesat;
            c.r += (luma - c.r) * k;
            c.g += (luma - c.g) * k;
            c.b += (luma - c.b) * k;
        }
        if (a < .01f)
        {
            continue;
        }
        const ImVec2 pMax(b.pos.x + b.size.x, b.pos.y + b.size.y);
        // White multiply preserves authored emblem RGB.
        const ImU32 tint = b.fullColor ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, a))
                                       : ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, a));
        if (!b.muted)
        {
            const ImU32 shadow =
                ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, a * .75f * snap.outlineAlpha));
            dl->AddImage(b.tex,
                         ImVec2(b.pos.x + shadowOff.x, b.pos.y + shadowOff.y),
                         ImVec2(pMax.x + shadowOff.x, pMax.y + shadowOff.y),
                         ImVec2(0, 0),
                         ImVec2(1, 1),
                         shadow);

            // Use a featureless semantic-color halo to avoid duplicating icon shapes.
            // Missing discs fall back to two enlarged icon copies.
            if (!restrainedWorld && !b.fullColor)
            {
                const ImVec2 gc((b.pos.x + pMax.x) * .5f, (b.pos.y + pMax.y) * .5f);
                if (ParticleTextures::HasSoftGlow())
                {
                    // Three widening gaussian discs form one soft halo.
                    const int gr = std::clamp(static_cast<int>(c.r * 255.0f), 0, 255);
                    const int gg = std::clamp(static_cast<int>(c.g * 255.0f), 0, 255);
                    const int gb = std::clamp(static_cast<int>(c.b * 255.0f), 0, 255);
                    constexpr float kGScale[3] = {1.0f, .64f, .4f};
                    constexpr float kGWeight[3] = {.34f, .40f, .44f};
                    for (int gi = 0; gi < 3; ++gi)
                    {
                        const int ga =
                            std::clamp(static_cast<int>(a * .5f * kGWeight[gi] * 255.0f), 0, 255);
                        if (ga > 2)
                        {
                            ParticleTextures::DrawSoftGlow(
                                dl, gc, b.size.x * 3.0f * kGScale[gi], IM_COL32(gr, gg, gb, ga));
                        }
                    }
                }
                else
                {
                    const auto glow = [&](float scale, float ga)
                    {
                        const ImVec2 gh(b.size.x * .5f * scale, b.size.y * .5f * scale);
                        dl->AddImage(b.tex,
                                     ImVec2(gc.x - gh.x, gc.y - gh.y),
                                     ImVec2(gc.x + gh.x, gc.y + gh.y),
                                     ImVec2(0, 0),
                                     ImVec2(1, 1),
                                     ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, ga)));
                    };
                    glow(1.65f, a * .22f);
                    glow(1.32f, a * .34f);
                }
            }
        }
        dl->AddImage(b.tex, b.pos, pMax, ImVec2(0, 0), ImVec2(1, 1), tint);

        // Resting player icons alone get a warm upper rim and carved lower shadow.
        if (!restrainedWorld && b.muted && layout.isPlayer && snap.icons.playerRimLightEnabled)
        {
            const float off = (std::max)(.5f, snap.icons.playerRimOffset * spacingScale);
            ImVec4 rim;
            if (snap.icons.playerRimColor.has_value())
            {
                const auto& rc = *snap.icons.playerRimColor;
                rim = ImVec4(rc.r, rc.g, rc.b, 1.0f);
            }
            else
            {
                const ImVec4& base = style.LcName;  // Warm-white lift
                rim = ImVec4((std::min)(1.0f, base.x * .3f + .72f),
                             (std::min)(1.0f, base.y * .3f + .70f),
                             (std::min)(1.0f, base.z * .3f + .63f),
                             1.0f);
            }
            const int rimA =
                std::clamp(static_cast<int>(a * snap.icons.playerRimAlpha * 255.0f), 0, 255);
            const int carveA =
                std::clamp(static_cast<int>(a * snap.icons.playerCarveAlpha * 255.0f), 0, 255);
            if (rimA > 2)
            {
                dl->AddImage(b.tex,
                             ImVec2(b.pos.x, b.pos.y - off),
                             ImVec2(pMax.x, pMax.y - off),
                             ImVec2(0, 0),
                             ImVec2(1, 1),
                             IM_COL32(static_cast<int>(rim.x * 255.0f),
                                      static_cast<int>(rim.y * 255.0f),
                                      static_cast<int>(rim.z * 255.0f),
                                      rimA));
            }
            if (carveA > 2)
            {
                dl->AddImage(b.tex,
                             ImVec2(b.pos.x, b.pos.y + off),
                             ImVec2(pMax.x, pMax.y + off),
                             ImVec2(0, 0),
                             ImVec2(1, 1),
                             IM_COL32(12, 12, 16, carveA));
            }
        }
    }
    RenderSampling::PopSampler(dl);
}

// Screen-space emblems use a near-neutral backlight; restrained world planes
// retain only the crisp mark.
void DrawTierEmblem(ImDrawList* dl,
                    const LabelStyle& style,
                    const LabelLayout& layout,
                    float time,
                    ImDrawListSplitter* splitter,
                    const RenderSettingsSnapshot& snap,
                    bool restrainedWorld)
{
    if (!layout.tierEmblemShown || layout.tierEmblemTex == 0)
    {
        return;
    }

    // Share strip alpha so pans fold the entire block above the name.
    float a = style.alpha * style.badgeAlphaMul;
    // IconOpacity matches the status-icon row; see DrawBadges.
    a = (std::min)(1.0f, a * snap.icons.opacity);
    if (a < .01f)
    {
        return;
    }

    // Keep crisp Graffito rank in the projected text front channel.
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    splitter->SetCurrentChannel(dl, gpuGlow ? 2 : 1);
    RenderSampling::PushBadgeSampler(dl);

    const ImVec2 c(layout.tierEmblemPos.x + layout.tierEmblemSize.x * .5f,
                   layout.tierEmblemPos.y + layout.tierEmblemSize.y * .5f);

    // White multiply preserves RGB and applies fade alpha only.
    const auto drawScaled = [&](float scale, float alpha)
    {
        if (alpha < .004f)
        {
            return;
        }
        const ImVec2 half(layout.tierEmblemSize.x * .5f * scale,
                          layout.tierEmblemSize.y * .5f * scale);
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));
        dl->AddImage(layout.tierEmblemTex,
                     ImVec2(c.x - half.x, c.y - half.y),
                     ImVec2(c.x + half.x, c.y + half.y),
                     ImVec2(0, 0),
                     ImVec2(1, 1),
                     col);
    };

    if (restrainedWorld)
    {
        drawScaled(1.0f, a * snap.icons.emblemCrispAlpha);
        RenderSampling::PopSampler(dl);
        return;
    }

    // Explicit backlight color wins; otherwise desaturate tier name color.
    const auto neutralAccent = [&]() -> ImVec4
    {
        if (snap.icons.emblemBacklightColor.has_value())
        {
            const auto& col = *snap.icons.emblemBacklightColor;
            return ImVec4(col.r, col.g, col.b, 1.0f);
        }
        const ImVec4& base = style.LcName;
        const float luma = .299f * base.x + .587f * base.y + .114f * base.z;
        const float k = .65f;  // Pull most of the way to grey -> near-neutral
        return ImVec4(base.x + (luma - base.x) * k,
                      base.y + (luma - base.y) * k,
                      base.z + (luma - base.z) * k,
                      1.0f);
    };

    if (snap.icons.emblemBacklightEnabled && ParticleTextures::HasSoftGlow())
    {
        // DrawSoftGlow takes quad edge length; visible radius is roughly edge/3.
        const float breathe =
            .78f + .22f * std::sin(time * 6.2831853f * snap.icons.emblemBacklightBreatheHz);
        const ImVec4 acc = neutralAccent();
        const int ar = std::clamp((int)(acc.x * 255.0f), 0, 255);
        const int ag = std::clamp((int)(acc.y * 255.0f), 0, 255);
        const int ab = std::clamp((int)(acc.z * 255.0f), 0, 255);
        const float edge = layout.tierEmblemSize.x * snap.icons.emblemBacklightSize;

        // Disc weights total 1.18, so EmblemBacklightAlpha is not the composite peak.
        const float peak = a * snap.icons.emblemBacklightAlpha * breathe;
        constexpr float kBScale[3] = {1.0f, .64f, .4f};
        constexpr float kBWeight[3] = {.34f, .40f, .44f};
        for (int gi = 0; gi < 3; ++gi)
        {
            const int ga = std::clamp(static_cast<int>(peak * kBWeight[gi] * 255.0f), 0, 255);
            if (ga > 2)
            {
                ParticleTextures::DrawSoftGlow(dl, c, edge * kBScale[gi], IM_COL32(ar, ag, ab, ga));
            }
        }
        if (snap.icons.emblemKeyFillEnabled)
        {
            // Warm key above, cooler fill below; breathe slightly out of phase.
            const float kfBreathe =
                .82f +
                .18f * std::sin(time * 6.2831853f * snap.icons.emblemBacklightBreatheHz + 1.3f);
            ImVec4 key;
            if (snap.icons.emblemKeyColor.has_value())
            {
                const auto& kc = *snap.icons.emblemKeyColor;
                key = ImVec4(kc.r, kc.g, kc.b, 1.0f);
            }
            else
            {
                const ImVec4& base = style.LcName;  // Warm
                key = ImVec4((std::min)(1.0f, base.x * .4f + .55f),
                             (std::min)(1.0f, base.y * .4f + .50f),
                             (std::min)(1.0f, base.z * .4f + .40f),
                             1.0f);
            }
            ImVec4 fill;
            if (snap.icons.emblemFillColor.has_value())
            {
                const auto& fc = *snap.icons.emblemFillColor;
                fill = ImVec4(fc.r, fc.g, fc.b, 1.0f);
            }
            else
            {
                const ImVec4& base = style.LcName;  // Cool
                fill = ImVec4((std::min)(1.0f, base.x * .4f + .35f),
                              (std::min)(1.0f, base.y * .4f + .42f),
                              (std::min)(1.0f, base.z * .4f + .50f),
                              1.0f);
            }
            const float keyY = c.y - layout.tierEmblemSize.x * snap.icons.emblemKeyRise;
            const float fillY = c.y + layout.tierEmblemSize.x * snap.icons.emblemFillDrop;
            const int keyA = std::clamp(
                static_cast<int>(a * snap.icons.emblemKeyAlpha * kfBreathe * 255.0f), 0, 255);
            const int fillA = std::clamp(
                static_cast<int>(a * snap.icons.emblemFillAlpha * kfBreathe * 255.0f), 0, 255);
            if (keyA > 2)
            {
                ParticleTextures::DrawSoftGlow(dl,
                                               ImVec2(c.x, keyY),
                                               edge * .8f,
                                               IM_COL32(static_cast<int>(key.x * 255.0f),
                                                        static_cast<int>(key.y * 255.0f),
                                                        static_cast<int>(key.z * 255.0f),
                                                        keyA));
            }
            if (fillA > 2)
            {
                ParticleTextures::DrawSoftGlow(dl,
                                               ImVec2(c.x, fillY),
                                               edge * .7f,
                                               IM_COL32(static_cast<int>(fill.x * 255.0f),
                                                        static_cast<int>(fill.y * 255.0f),
                                                        static_cast<int>(fill.z * 255.0f),
                                                        fillA));
            }
        }
        drawScaled(1.0f, a * snap.icons.emblemCrispAlpha);
    }
    else
    {
        // Without a glow disc or enabled backlight, enlarge three art copies behind
        //  the crisp mark.
        const float breathe = .78f + .22f * std::sin(time * 1.05f);
        drawScaled(1.62f, a * .14f * breathe);
        drawScaled(1.34f, a * .24f * breathe);
        drawScaled(1.16f, a * .32f * breathe);
        drawScaled(1.0f, a * snap.icons.emblemCrispAlpha);
    }
    RenderSampling::PopSampler(dl);
}

}  // namespace Renderer
