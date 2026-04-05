#include "TextEffectsInternal.h"

namespace TextEffects
{

void AddTextRainbowWave(ImDrawList* list,
                        ImFont* font,
                        float size,
                        const ImVec2& pos,
                        const char* text,
                        float baseHue,
                        float hueSpread,
                        float speed,
                        float saturation,
                        float value,
                        float alpha)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);

        // Calculate hue with smooth wave motion
        const float hue = baseHue + t * hueSpread + time * speed * .4f;

        // Add subtle vertical brightness gradient
        float vertBrightness = 1.0f + (1.0f - v) * .12f;

        // Add gentle shimmer wave that travels across text
        float shimmerPhase = t * 3.0f - time * speed * .8f;
        float shimmer = std::sin(shimmerPhase) * .5f + .5f;
        shimmer = shimmer * shimmer * .08f;  // Very subtle shimmer

        // Gentle saturation variation for depth
        float satVar = saturation * (.97f + std::sin(t * 2.0f + time * .15f) * .03f);

        // Combine brightness modifiers
        float finalValue = value * vertBrightness + shimmer;
        finalValue = std::min(finalValue, 1.0f);

        const ImVec4 rgb = HSVtoRGB(hue, satVar, finalValue, alpha);
        list->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(rgb);
    }
}

void AddTextShimmer(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    ImU32 highlight,
                    float phase01,
                    float bandWidth01,
                    float strength01)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float bandHalf = (std::max)(bandWidth01 * .5f, .01f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        // Wrap-around distance so the band smoothly exits right and enters left
        const float d =
            (std::min)(std::abs(t - phase01),
                       (std::min)(std::abs(t - phase01 + 1.0f), std::abs(t - phase01 - 1.0f)));

        // Primary shimmer band with soft quintic falloff
        float h = (d < bandHalf) ? 1.0f - SmoothStep(d / bandHalf) : .0f;

        // Add vertical gradient to shimmer
        float verticalBoost = 1.0f + (1.0f - v) * .3f;
        h = h * strength01 * verticalBoost;

        // Secondary soft glow halo around the band
        float glow = std::exp(-d * d * 6.0f) * .2f * strength01;

        // Tertiary wide ambient glow for luxury feel
        float ambient = std::exp(-d * d * 2.0f) * .08f * strength01;

        // Edge highlight, subtle brightness at text edges
        float edgeDist = std::min(v, 1.0f - v) * 2.0f;  // 0 at edges, 1 at center
        float edgeGlow = (1.0f - edgeDist) * .1f * strength01 * (1.0f - d * .5f);

        h = Saturate(h + glow + ambient + edgeGlow);

        list->VtxBuffer[i].col = LerpColorU32(base, highlight, h);
    }
}

static void AddTextGradientShimmer(ImDrawList* list,
                                   ImFont* font,
                                   float size,
                                   const ImVec2& pos,
                                   const char* text,
                                   ImU32 baseL,
                                   ImU32 baseR,
                                   ImU32 highlight,
                                   float phase01,
                                   float bandWidth01,
                                   float strength01)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float sigma = (std::max)(bandWidth01, 1e-3f);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const float d = t - phase01;
        float h = Saturate(std::exp(-(d * d) * inv2s2) * strength01);

        list->VtxBuffer[i].col = LerpColorU32(base, highlight, h);
    }
}

static void AddTextSolidShimmer(ImDrawList* list,
                                ImFont* font,
                                float size,
                                const ImVec2& pos,
                                const char* text,
                                ImU32 base,
                                ImU32 highlight,
                                float phase01,
                                float bandWidth01,
                                float strength01)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float sigma = (std::max)(bandWidth01, 1e-3f);
    const float inv2s2 = 1.0f / (2.0f * sigma * sigma);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float d = t - phase01;
        float h = Saturate(std::exp(-(d * d) * inv2s2) * strength01);
        list->VtxBuffer[i].col = LerpColorU32(base, highlight, h);
    }
}

// Perf: renders 2 ghost layers + outline (8-dir) + main = ~11 text draws.
void AddTextOutline4ChromaticShimmer(ImDrawList* list,
                                     ImFont* font,
                                     float size,
                                     const ImVec2& pos,
                                     const char* text,
                                     ImU32 baseL,
                                     ImU32 baseR,
                                     ImU32 highlight,
                                     ImU32 outline,
                                     float outlineW,
                                     float phase01,
                                     float bandWidth01,
                                     float strength01,
                                     float splitPx,
                                     float ghostAlphaMul)
{
    // Extract alpha from base color so ghosts fade with distance
    const float baseA = (float)GetA(baseL) / 255.0f;
    const float gMul = ghostAlphaMul;

    // Create tinted ghost colors
    // These simulate chromatic aberration
    ImU32 ghostR =
        IM_COL32(255, 80, 80, (int)std::clamp(255.0f * baseA * gMul, .0f, 255.0f));  // Red ghost
    ImU32 ghostB =
        IM_COL32(80, 160, 255, (int)std::clamp(255.0f * baseA * gMul, .0f, 255.0f));  // Blue ghost

    // Highlight for ghosts
    ImU32 hiGhost = WithAlpha(highlight, gMul);

    // Layer 1: Draw ghost layers behind main text
    // Red ghost slightly to the left
    AddTextSolidShimmer(list,
                        font,
                        size,
                        ImVec2(pos.x - splitPx, pos.y),
                        text,
                        ghostR,
                        hiGhost,
                        Frac(phase01 + .02f),
                        bandWidth01,
                        strength01);

    // Blue ghost slightly to the right
    AddTextSolidShimmer(list,
                        font,
                        size,
                        ImVec2(pos.x + splitPx, pos.y),
                        text,
                        ghostB,
                        hiGhost,
                        Frac(phase01 + .07f),
                        bandWidth01,
                        strength01);

    // Layer 2: Draw 8-directional outline on main text
    DrawOutline8Internal(list, font, size, pos, text, outline, outlineW);

    // Layer 3: Draw main text with gradient and shimmer
    AddTextGradientShimmer(
        list, font, size, pos, text, baseL, baseR, highlight, phase01, bandWidth01, strength01);
}

void AddTextEmber(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float speed,
                  float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;

    // Warm highlight color (golden-orange tint of the brighter input color)
    int rA = (colA >> IM_COL32_R_SHIFT) & 0xFF;
    int gA = (colA >> IM_COL32_G_SHIFT) & 0xFF;
    int bA = (colA >> IM_COL32_B_SHIFT) & 0xFF;
    ImU32 warmHighlight =
        IM_COL32((std::min)(255, rA + 60), (std::min)(255, gA + 20), (std::max)(0, bA - 30), 255);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(colA, colB, nx);

        // Multiple noise layers for organic ember flicker
        float n1 = std::sin(nx * 5.0f + time * 1.2f + ny * 3.0f) * .5f + .5f;
        float n2 = std::sin(nx * 8.0f - time * 2.0f + ny * 5.0f) * .5f + .5f;
        float n3 = std::sin(nx * 12.0f + time * 3.5f - ny * 2.0f) * .5f + .5f;

        // Combine with different weights for organic feel
        float ember = n1 * .5f + n2 * .3f + n3 * .2f;

        // Sharpen the ember peaks for more dramatic flicker
        ember = ember * ember;

        // Vertical heat: hotter (brighter) at bottom
        float heatGrad = .7f + .3f * ny;

        // Per-character phase offset from X for varied flicker
        float charFlicker = std::sin(time * 4.0f + nx * 20.0f) * .5f + .5f;
        charFlicker = charFlicker * charFlicker * .15f;

        float glow = Saturate((ember * heatGrad + charFlicker) * intensity);

        // Blend base toward warm highlight at bright spots
        list->VtxBuffer[i].col = LerpColorU32(base, warmHighlight, glow);
    }
}

void AddTextConicRainbow(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         float baseHue,
                         float speed,
                         float saturation,
                         float value,
                         float alpha)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const ImVec2 c = s.center();
    const float time = (float)ImGui::GetTime();

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float ang = std::atan2(p.y - c.y, p.x - c.x);
        const float u = (ang + PI) * INV_TWO_PI;
        // Slower, more gradual rotation (0.3x speed)
        const float hue = baseHue + u + time * speed * .3f;
        list->VtxBuffer[i].col =
            ImGui::ColorConvertFloat4ToU32(HSVtoRGB(hue, saturation, value, alpha));
    }
}

void AddTextOutline4RainbowWave(ImDrawList* list,
                                ImFont* font,
                                float size,
                                const ImVec2& pos,
                                const char* text,
                                float baseHue,
                                float hueSpread,
                                float speed,
                                float saturation,
                                float value,
                                float alpha,
                                ImU32 outline,
                                float w,
                                bool fastOutlines,
                                bool useWhiteBase)
{
    // Always draw outline first
    DrawOutlineInternal(list, font, size, pos, text, outline, w, fastOutlines);

    // Optionally draw white base layer
    if (useWhiteBase)
    {
        ImU32 whiteBase = IM_COL32(255, 255, 255, (int)(alpha * 255.0f));
        list->AddText(font, size, pos, whiteBase, text);
        // Draw rainbow overlay with reduced alpha
        AddTextRainbowWave(list,
                           font,
                           size,
                           pos,
                           text,
                           baseHue,
                           hueSpread,
                           speed,
                           saturation,
                           value,
                           alpha * .35f);
    }
    else
    {
        // Draw rainbow directly
        AddTextRainbowWave(
            list, font, size, pos, text, baseHue, hueSpread, speed, saturation, value, alpha);
    }
}

void AddTextOutline4ConicRainbow(ImDrawList* list,
                                 ImFont* font,
                                 float size,
                                 const ImVec2& pos,
                                 const char* text,
                                 float baseHue,
                                 float speed,
                                 float saturation,
                                 float value,
                                 float alpha,
                                 ImU32 outline,
                                 float w,
                                 bool fastOutlines,
                                 bool useWhiteBase)
{
    // Always draw outline first
    DrawOutlineInternal(list, font, size, pos, text, outline, w, fastOutlines);

    // Optionally draw white base layer
    if (useWhiteBase)
    {
        ImU32 whiteBase = IM_COL32(255, 255, 255, (int)(alpha * 255.0f));
        list->AddText(font, size, pos, whiteBase, text);
        // Draw rainbow overlay with reduced alpha
        AddTextConicRainbow(
            list, font, size, pos, text, baseHue, speed, saturation, value, alpha * .35f);
    }
    else
    {
        // Draw rainbow directly
        AddTextConicRainbow(list, font, size, pos, text, baseHue, speed, saturation, value, alpha);
    }
}

}  // namespace TextEffects
