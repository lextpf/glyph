#include "TextEffects.h"
#include "ParticleTextures.h"
#include "Settings.h"
#include "Utf8Utils.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

static constexpr float PI = std::numbers::pi_v<float>;
static constexpr float TWO_PI = 2.0f * PI;
static constexpr float INV_TWO_PI = std::numbers::inv_pi_v<float> * 0.5f;

namespace TextEffects
{
using Utf8Utils::Utf8ToChars;

ImU32 LerpColorU32(ImU32 a, ImU32 b, float t)
{
    // Linear interpolation between two packed RGBA colors
    t = Saturate(t);  // Ensure t is in [0, 1]

    // Extract color components from first color (a)
    // ImGui uses ABGR format on little-endian systems
    const int ar = (a >> IM_COL32_R_SHIFT) & 0xFF;
    const int ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
    const int ab = (a >> IM_COL32_B_SHIFT) & 0xFF;
    const int aa = (a >> IM_COL32_A_SHIFT) & 0xFF;

    // Extract components from second color (b)
    const int br = (b >> IM_COL32_R_SHIFT) & 0xFF;
    const int bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
    const int bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    const int ba = (b >> IM_COL32_A_SHIFT) & 0xFF;

    // Interpolate each component: a + (b - a) * t
    // Add 0.5f for proper rounding when converting to int
    const int rr = (int)(ar + (br - ar) * t + .5f);
    const int rg = (int)(ag + (bg - ag) * t + .5f);
    const int rb = (int)(ab + (bb - ab) * t + .5f);
    const int ra = (int)(aa + (ba - aa) * t + .5f);

    // Pack back into ImU32
    return IM_COL32(rr, rg, rb, ra);
}

// Fast 4-directional outline (4 draw calls)
static inline void DrawOutline4Internal(ImDrawList* list,
                                        ImFont* font,
                                        float size,
                                        const ImVec2& pos,
                                        const char* text,
                                        ImU32 outline,
                                        float w)
{
    // Cardinal directions only - faster, slightly less smooth
    list->AddText(font, size, ImVec2(pos.x - w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x + w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y - w), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y + w), outline, text);
}

// 8-directional outline (smoother, 8 draw calls)
static inline void DrawOutline8Internal(ImDrawList* list,
                                        ImFont* font,
                                        float size,
                                        const ImVec2& pos,
                                        const char* text,
                                        ImU32 outline,
                                        float w)
{
    const float d = w * .70710678118f;  // Diagonal offset (w / sqrt(2))
    // Cardinal directions
    list->AddText(font, size, ImVec2(pos.x - w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x + w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y - w), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y + w), outline, text);
    // Diagonal directions for smoother appearance
    list->AddText(font, size, ImVec2(pos.x - d, pos.y - d), outline, text);
    list->AddText(font, size, ImVec2(pos.x + d, pos.y - d), outline, text);
    list->AddText(font, size, ImVec2(pos.x - d, pos.y + d), outline, text);
    list->AddText(font, size, ImVec2(pos.x + d, pos.y + d), outline, text);
}

// Draw outline using Settings::FastOutlines to pick 4-dir or 8-dir
static inline void DrawOutlineInternal(ImDrawList* list,
                                       ImFont* font,
                                       float size,
                                       const ImVec2& pos,
                                       const char* text,
                                       ImU32 outline,
                                       float w)
{
    if (Settings::FastOutlines)
    {
        DrawOutline4Internal(list, font, size, pos, text, outline, w);
    }
    else
    {
        DrawOutline8Internal(list, font, size, pos, text, outline, w);
    }
}

void DrawOutline(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 outline,
                 float w)
{
    DrawOutlineInternal(list, font, size, pos, text, outline, w);
}

void AddTextOutline4(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 col,
                     ImU32 outline,
                     float w)
{
    if (!list || !font || !text || !text[0])
    {
        return;
    }

    // Draw 8-directional outline for smoother edges
    DrawOutlineInternal(list, font, size, pos, text, outline, w);

    // Draw main text on top
    list->AddText(font, size, pos, col, text);
}

void AddTextHorizontalGradient(ImDrawList* list,
                               ImFont* font,
                               float size,
                               const ImVec2& pos,
                               const char* text,
                               ImU32 colLeft,
                               ImU32 colRight)
{
    if (!list || !font || !text || !text[0])
    {
        return;
    }

    // First, add the text normally to get vertices in the buffer
    const int vtxStart = list->VtxBuffer.Size;
    list->AddText(font, size, pos, IM_COL32_WHITE, text);
    const int vtxEnd = list->VtxBuffer.Size;

    if (vtxEnd <= vtxStart)
    {
        return;  // No vertices added
    }

    // Find the horizontal bounds of the text
    float minX = FLT_MAX;
    float maxX = -FLT_MAX;
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        const float x = list->VtxBuffer[i].pos.x;
        minX = (std::min)(minX, x);
        maxX = (std::max)(maxX, x);
    }

    const float denom = (maxX - minX);
    if (denom < 1e-3f)
    {
        // Text too narrow, just use left color
        for (int i = vtxStart; i < vtxEnd; ++i)
        {
            list->VtxBuffer[i].col = colLeft;
        }
        return;
    }

    // Recolor each vertex based on its X position
    // Left edge gets colLeft, right edge gets colRight, interpolated in between
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        const float x = list->VtxBuffer[i].pos.x;
        const float t = (x - minX) / denom;  // Normalize to [0, 1]
        list->VtxBuffer[i].col = LerpColorU32(colLeft, colRight, t);
    }
}

// Helper struct for text vertex manipulation
struct TextVertexSetup
{
    ImDrawList* list;
    int vtxStart;
    int vtxEnd;
    ImVec2 bbMin;
    ImVec2 bbMax;

    float width() const { return (std::max)(bbMax.x - bbMin.x, 1e-3f); }
    float height() const { return (std::max)(bbMax.y - bbMin.y, 1e-3f); }
    float normalizedX(float x) const { return (x - bbMin.x) / width(); }
    float normalizedY(float y) const { return (y - bbMin.y) / height(); }
    ImVec2 center() const { return ImVec2((bbMin.x + bbMax.x) * .5f, (bbMin.y + bbMax.y) * .5f); }

    // Validates params, adds text, computes bounds. Returns true if vertices were added.
    static bool Begin(TextVertexSetup& out,
                      ImDrawList* list,
                      ImFont* font,
                      float size,
                      const ImVec2& pos,
                      const char* text)
    {
        if (!list || !font || !text || !text[0])
        {
            return false;
        }

        out.list = list;
        out.vtxStart = list->VtxBuffer.Size;
        list->AddText(font, size, pos, IM_COL32_WHITE, text);
        out.vtxEnd = list->VtxBuffer.Size;

        if (out.vtxEnd <= out.vtxStart)
        {
            return false;
        }

        // Compute bounding box
        out.bbMin = ImVec2(FLT_MAX, FLT_MAX);
        out.bbMax = ImVec2(-FLT_MAX, -FLT_MAX);
        for (int i = out.vtxStart; i < out.vtxEnd; ++i)
        {
            const ImVec2 p = list->VtxBuffer[i].pos;
            out.bbMin.x = (std::min)(out.bbMin.x, p.x);
            out.bbMin.y = (std::min)(out.bbMin.y, p.y);
            out.bbMax.x = (std::max)(out.bbMax.x, p.x);
            out.bbMax.y = (std::max)(out.bbMax.y, p.y);
        }
        return true;
    }
};

// Get fractional part of float
static inline float Frac(float x)
{
    return x - std::floor(x);
}

static inline ImVec4 HSVtoRGB(float h, float s, float v, float a)
{
    // Convert HSV (Hue, Saturation, Value) to RGB
    // h = hue [0, 1], wraps around
    // s = saturation [0, 1], 0 = grayscale, 1 = full color
    // v = value [0, 1], 0 = black, 1 = bright
    // a = alpha [0, 1]

    h = Frac(h);  // Wrap hue to [0, 1]

    // HSV to RGB conversion using standard algorithm
    const float c = v * s;  // Chroma
    const float x = c * (1.0f - std::fabs(Frac(h * 6.0f) * 2.0f - 1.0f));
    const float m = v - c;  // Match value

    float r = 0, g = 0, b = 0;

    // Determine which of the 6 hue sextants we're in
    const int i = (int)std::floor(h * 6.0f);
    switch (i % 6)
    {
        case 0:
            r = c;
            g = x;
            b = 0;
            break;  // Red to Yellow
        case 1:
            r = x;
            g = c;
            b = 0;
            break;  // Yellow to Green
        case 2:
            r = 0;
            g = c;
            b = x;
            break;  // Green to Cyan
        case 3:
            r = 0;
            g = x;
            b = c;
            break;  // Cyan to Blue
        case 4:
            r = x;
            g = 0;
            b = c;
            break;  // Blue to Magenta
        case 5:
            r = c;
            g = 0;
            b = x;
            break;  // Magenta to Red
    }

    // Add match value to bring up to desired brightness
    return ImVec4(r + m, g + m, b + m, a);
}

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

        const float d = std::abs(t - phase01);

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

void AddTextRadialGradient(ImDrawList* list,
                           ImFont* font,
                           float size,
                           const ImVec2& pos,
                           const char* text,
                           ImU32 colCenter,
                           ImU32 colEdge,
                           float gamma,
                           ImVec2* overrideCenter)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const ImVec2 center = overrideCenter ? *overrideCenter : s.center();

    // Calculate maximum radius to furthest corner
    auto dist2 = [&](const ImVec2& p)
    {
        const float dx = p.x - center.x, dy = p.y - center.y;
        return dx * dx + dy * dy;
    };
    const float r2 = (std::max)({dist2(s.bbMin),
                                 dist2(ImVec2(s.bbMax.x, s.bbMin.y)),
                                 dist2(ImVec2(s.bbMin.x, s.bbMax.y)),
                                 dist2(s.bbMax)});
    const float invR = 1.0f / std::sqrt((std::max)(r2, 1e-6f));

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        float t = Saturate(
            std::sqrt((p.x - center.x) * (p.x - center.x) + (p.y - center.y) * (p.y - center.y)) *
            invR);
        if (gamma != 1.0f)
        {
            t = std::pow(t, gamma);
        }
        list->VtxBuffer[i].col = LerpColorU32(colCenter, colEdge, t);
    }
}

// Extract alpha channel [0-255] from packed color
static inline int GetA(ImU32 c)
{
    return (c >> IM_COL32_A_SHIFT) & 0xFF;
}

// Create new color with scaled alpha (preserves RGB)
static inline ImU32 WithAlpha(ImU32 c, float mul)
{
    // Extract RGBA components
    const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;

    // Scale alpha and clamp to valid range
    const int na = (int)std::clamp(a * mul, .0f, 255.0f);

    // Repack color with new alpha
    return IM_COL32(r, g, b, na);
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
                                     float ghostAlphaMul = .35f)
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

void TextEffects::AddTextVerticalGradient(ImDrawList* list,
                                          ImFont* font,
                                          float size,
                                          const ImVec2& pos,
                                          const char* text,
                                          ImU32 colTop,
                                          ImU32 colBottom)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedY(list->VtxBuffer[i].pos.y);
        list->VtxBuffer[i].col = LerpColorU32(colTop, colBottom, t);
    }
}

// Scale RGB channels by multiplier
static inline ImU32 ScaleRGB(ImU32 c, float mul)
{
    mul = std::max(.0f, mul);  // Prevent negative colors

    // Extract channels
    const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;

    // Scale RGB and clamp to valid range
    const int nr = (int)std::clamp(r * mul, .0f, 255.0f);
    const int ng = (int)std::clamp(g * mul, .0f, 255.0f);
    const int nb = (int)std::clamp(b * mul, .0f, 255.0f);

    // Repack with original alpha
    return IM_COL32(nr, ng, nb, a);
}

void TextEffects::AddTextDiagonalGradient(ImDrawList* list,
                                          ImFont* font,
                                          float size,
                                          const ImVec2& pos,
                                          const char* text,
                                          ImU32 a,
                                          ImU32 b,
                                          ImVec2 dir)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Normalize direction vector
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len < 1e-3f)
    {
        dir = ImVec2(1, 0);
    }
    else
    {
        dir.x /= len;
        dir.y /= len;
    }

    // Project all vertices onto direction to find extent
    float minP = FLT_MAX, maxP = -FLT_MAX;
    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float proj = p.x * dir.x + p.y * dir.y;
        minP = (std::min)(minP, proj);
        maxP = (std::max)(maxP, proj);
    }

    const float denom = (std::max)(maxP - minP, 1e-3f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float t = (p.x * dir.x + p.y * dir.y - minP) / denom;
        list->VtxBuffer[i].col = LerpColorU32(a, b, t);
    }
}

void TextEffects::AddTextPulseGradient(ImDrawList* list,
                                       ImFont* font,
                                       float size,
                                       const ImVec2& pos,
                                       const char* text,
                                       ImU32 a,
                                       ImU32 b,
                                       float time,
                                       float freqHz,
                                       float amp)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float pulse = 1.0f + amp * std::sin(time * TWO_PI * freqHz);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(a, b, t);
        list->VtxBuffer[i].col = ScaleRGB(base, pulse);
    }
}

void TextEffects::AddTextConicRainbow(ImDrawList* list,
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

void TextEffects::AddTextOutline4RainbowWave(ImDrawList* list,
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
                                             bool useWhiteBase)
{
    // Always draw outline first
    DrawOutlineInternal(list, font, size, pos, text, outline, w);

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

void TextEffects::AddTextOutline4ConicRainbow(ImDrawList* list,
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
                                              bool useWhiteBase)
{
    // Always draw outline first
    DrawOutlineInternal(list, font, size, pos, text, outline, w);

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

// Integer hash for pseudo-random noise [0, 1)
static inline float Hash(float x, float y)
{
    size_t hash = static_cast<size_t>(static_cast<int>(x));
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    // Mix Y through its own scramble before combining to avoid
    // (1,100)/(100,1) collisions from a simple XOR-multiply.
    size_t yHash = static_cast<size_t>(static_cast<int>(y));
    yHash ^= yHash >> 16;
    yHash *= 0x9e3779b97f4a7c15ULL;
    yHash ^= yHash >> 13;
    hash ^= yHash;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    return static_cast<float>(hash & 0xFFFFFF) / 16777216.0f;  // [0, 1)
}

// 2D value noise with quintic interpolation
static inline float ValueNoise(float x, float y)
{
    // Get integer and fractional parts
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    // Quintic interpolation curve for smoother results
    fx = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    fy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

    // Sample four corners and interpolate
    float a = Hash(ix, iy);
    float b = Hash(ix + 1.0f, iy);
    float c = Hash(ix, iy + 1.0f);
    float d = Hash(ix + 1.0f, iy + 1.0f);

    // Bilinear interpolation
    float ab = a + (b - a) * fx;
    float cd = c + (d - c) * fx;
    return ab + (cd - ab) * fy;
}

// Fractal Brownian Motion
static inline float FBMNoise(float x, float y, int octaves, float persistence = .5f)
{
    // Cap octaves to prevent excessive computation and float overflow in frequency
    octaves = std::min(octaves, 8);

    float total = .0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxValue = .0f;

    for (int i = 0; i < octaves; i++)
    {
        total += ValueNoise(x * frequency, y * frequency) * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= 2.0f;
    }

    return total / maxValue;
}

void TextEffects::AddTextAurora(ImDrawList* list,
                                ImFont* font,
                                float size,
                                const ImVec2& pos,
                                const char* text,
                                ImU32 colA,
                                ImU32 colB,
                                float speed,
                                float waves,
                                float intensity,
                                float sway)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;

    // Create intermediate colors for richer aurora palette
    ImU32 colMid = LerpColorU32(colA, colB, .5f);
    // Add subtle brightness variation
    ImU32 colBright = LerpColorU32(colA, IM_COL32(255, 255, 255, 255), .25f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        // Multiple flowing wave layers for organic aurora movement
        float wave1 = std::sin(nx * waves * TWO_PI + time * 1.2f + ny * 2.0f);
        float wave2 = std::sin(nx * waves * .7f * TWO_PI - time * .8f + ny * 1.5f) * .6f;
        float wave3 = std::sin(nx * waves * 1.3f * TWO_PI + time * .5f - ny * 1.0f) * .4f;

        // Vertical curtain effect
        float curtain = std::sin(ny * TWO_PI * 2.0f + time * .7f + nx * sway * 3.0f);
        curtain = curtain * .5f + .5f;  // Normalize to [0, 1]

        // Combine waves
        float combined = (wave1 + wave2 + wave3) / 2.0f;  // Range roughly [-1, 1]
        combined = combined * .5f + .5f;                  // Normalize to [0, 1]

        // Add subtle shimmer
        float shimmer = std::sin(time * 4.0f + nx * 12.0f + ny * 8.0f) * .5f + .5f;
        shimmer = shimmer * shimmer * .15f;  // Subtle sparkle

        // Horizontal sway effect
        float swayOffset = std::sin(ny * 3.0f + time * 1.5f) * sway;
        float swayedX = nx + swayOffset;
        float swayFactor = std::sin(swayedX * TWO_PI * waves + time) * .5f + .5f;

        // Blend all factors
        float t =
            Saturate((combined * .6f + curtain * .25f + swayFactor * .15f) * intensity + shimmer);

        // Three-color gradient for rich aurora appearance
        ImU32 finalColor;
        if (t < .4f)
        {
            finalColor = LerpColorU32(colA, colMid, t * 2.5f);
        }
        else if (t < .7f)
        {
            finalColor = LerpColorU32(colMid, colB, (t - .4f) * 3.33f);
        }
        else
        {
            finalColor = LerpColorU32(colB, colBright, (t - .7f) * 3.33f);
        }

        list->VtxBuffer[i].col = finalColor;
    }
}

void TextEffects::AddTextSparkle(ImDrawList* list,
                                 ImFont* font,
                                 float size,
                                 const ImVec2& pos,
                                 const char* text,
                                 ImU32 baseL,
                                 ImU32 baseR,
                                 ImU32 sparkleColor,
                                 float density,
                                 float speed,
                                 float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();

    // Create color variations for richer sparkle
    ImU32 sparkleWhite = IM_COL32(255, 255, 255, 255);
    ImU32 sparkleTint = LerpColorU32(sparkleColor, sparkleWhite, .3f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(baseL, baseR, nx);
        float totalSparkle = .0f;
        float colorShift = .0f;  // For varying sparkle color

        // Layer 1: Large slow-twinkling stars
        float seed1 = Hash(std::floor(p.x * .06f), std::floor(p.y * .06f));
        if (seed1 > (1.0f - density * .4f))
        {
            float phase1 = seed1 * TWO_PI;
            float sparkleTime1 = time * speed * (.6f + seed1 * .4f);
            float sparkle1 = std::sin(sparkleTime1 + phase1);
            sparkle1 = std::max(.0f, sparkle1);
            sparkle1 = std::pow(sparkle1, 3.0f);

            // Star burst pattern
            float gridX = Frac(p.x * .06f);
            float gridY = Frac(p.y * .06f);
            float distFromCenter =
                std::sqrt((gridX - .5f) * (gridX - .5f) + (gridY - .5f) * (gridY - .5f));
            float starPattern = std::max(.0f, 1.0f - distFromCenter * 3.0f);

            totalSparkle += sparkle1 * starPattern * .9f;
            colorShift += sparkle1 * .3f;
        }

        // Layer 2: Medium fast-twinkling sparkles
        float seed2 = Hash(std::floor(p.x * .12f) + 50.0f, std::floor(p.y * .12f) + 50.0f);
        if (seed2 > (1.0f - density * .7f))
        {
            float phase2 = seed2 * TWO_PI;
            float sparkleTime2 = time * speed * 1.8f * (.8f + seed2 * .4f);
            float sparkle2 = std::sin(sparkleTime2 + phase2);
            sparkle2 = std::max(.0f, sparkle2);
            sparkle2 = std::pow(sparkle2, 5.0f);
            totalSparkle += sparkle2 * .6f;
        }

        // Layer 3: Fine shimmer dust
        float seed3 = Hash(std::floor(p.x * .2f) + 100.0f, std::floor(p.y * .2f) + 100.0f);
        if (seed3 > (1.0f - density * .9f))
        {
            float phase3 = seed3 * TWO_PI;
            float sparkle3 = std::sin(time * speed * 2.5f + phase3);
            sparkle3 = std::max(.0f, sparkle3);
            sparkle3 = std::pow(sparkle3, 8.0f);
            totalSparkle += sparkle3 * .35f;
        }

        // Layer 4: Rare brilliant flares
        float seed4 = Hash(std::floor(p.x * .04f) + 200.0f, std::floor(p.y * .04f) + 200.0f);
        if (seed4 > .93f)
        {
            float phase4 = seed4 * TWO_PI;
            float flare = std::sin(time * speed * .4f + phase4);
            flare = std::max(.0f, flare);
            flare = std::pow(flare, 2.0f);
            totalSparkle += flare * 1.5f;
            colorShift += flare * .6f;  // Flares shift toward white
        }

        totalSparkle = Saturate(totalSparkle * intensity);
        colorShift = Saturate(colorShift);

        // Blend sparkle color with white based on intensity for brighter sparkles
        ImU32 finalSparkle = LerpColorU32(sparkleColor, sparkleTint, colorShift);
        list->VtxBuffer[i].col = LerpColorU32(base, finalSparkle, totalSparkle);
    }
}

void TextEffects::AddTextPlasma(ImDrawList* list,
                                ImFont* font,
                                float size,
                                const ImVec2& pos,
                                const char* text,
                                ImU32 colA,
                                ImU32 colB,
                                float freq1,
                                float freq2,
                                float speed)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;
    ImU32 colMid = LerpColorU32(colA, colB, .5f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        // Enhanced plasma with more organic patterns
        float plasma = .0f;

        // Primary waves with varied phases
        plasma += std::sin(nx * freq1 * TWO_PI + time);
        plasma += std::sin(ny * freq2 * TWO_PI + time * .7f);

        // Diagonal waves
        plasma += std::sin((nx + ny) * (freq1 + freq2) * .5f * TWO_PI + time * 1.3f);
        plasma += std::sin((nx - ny) * freq1 * TWO_PI + time * .9f) * .5f;

        // Radial waves from offset centers for more organic look
        float cx1 = nx - .3f - std::sin(time * .3f) * .2f;
        float cy1 = ny - .5f - std::cos(time * .4f) * .15f;
        float dist1 = std::sqrt(cx1 * cx1 + cy1 * cy1);
        plasma += std::sin(dist1 * freq1 * TWO_PI * 2.0f - time * 1.2f);

        float cx2 = nx - .7f + std::cos(time * .35f) * .15f;
        float cy2 = ny - .5f + std::sin(time * .45f) * .2f;
        float dist2 = std::sqrt(cx2 * cx2 + cy2 * cy2);
        plasma += std::sin(dist2 * freq2 * TWO_PI * 1.5f + time * .8f) * .7f;

        // Normalize to [0, 1] with smoother transition
        plasma = (plasma + 5.2f) / 10.4f;
        plasma = SmoothStep(plasma);  // Use quintic smoothing

        // Three-color gradient for richer appearance
        ImU32 finalColor;
        if (plasma < .5f)
        {
            finalColor = LerpColorU32(colA, colMid, plasma * 2.0f);
        }
        else
        {
            finalColor = LerpColorU32(colMid, colB, (plasma - .5f) * 2.0f);
        }

        list->VtxBuffer[i].col = finalColor;
    }
}

void TextEffects::AddTextScanline(ImDrawList* list,
                                  ImFont* font,
                                  float size,
                                  const ImVec2& pos,
                                  const char* text,
                                  ImU32 baseL,
                                  ImU32 baseR,
                                  ImU32 scanColor,
                                  float speed,
                                  float scanWidth,
                                  float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    float phase1 = std::sin(time * speed * PI) * .5f + .5f;
    float phase2 = std::sin(time * speed * PI + 2.0f) * .5f + .5f;

    const float bandWidth = (std::max)(scanWidth, .05f);
    const float bandHalf = bandWidth * .5f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        // Base gradient color
        ImU32 base = LerpColorU32(baseL, baseR, nx);

        // Primary scanline with smooth quintic falloff
        float d1 = std::abs(ny - phase1);
        float scan1 = .0f;
        if (d1 < bandHalf)
        {
            scan1 = 1.0f - SmoothStep(d1 / bandHalf);
        }

        // Secondary scanline
        float d2 = std::abs(ny - phase2);
        float scan2 = .0f;
        if (d2 < bandHalf * .7f)
        {
            scan2 = (1.0f - SmoothStep(d2 / (bandHalf * .7f))) * .4f;
        }

        // Subtle horizontal scan lines
        float crtLines = std::sin(ny * s.height() * .5f) * .5f + .5f;
        crtLines = crtLines * .08f;  // Very subtle

        // Combine effects
        float totalScan = Saturate((scan1 + scan2) * intensity + crtLines);

        // Add slight glow around the main scanline
        float glow = std::exp(-d1 * d1 * 20.0f) * .15f * intensity;
        totalScan = Saturate(totalScan + glow);

        list->VtxBuffer[i].col = LerpColorU32(base, scanColor, totalScan);
    }
}

// Perf: draws up to 3 layers x 8 samples = 24 AddText calls per string.
// Use GlowSamples <= 4 for a cheaper single-layer fallback.
void TextEffects::AddTextGlow(ImDrawList* list,
                              ImFont* font,
                              float size,
                              const ImVec2& pos,
                              const char* text,
                              ImU32 glowColor,
                              float radius,
                              float intensity,
                              int samples)
{
    if (!list || !font || !text || !text[0] || radius <= .0f || intensity <= .01f)
    {
        return;
    }

    const int baseAlpha = (glowColor >> IM_COL32_A_SHIFT) & 0xFF;
    if (baseAlpha < 5)
    {
        return;
    }

    const int r = (glowColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (glowColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (glowColor >> IM_COL32_B_SHIFT) & 0xFF;

    // Multi-layer soft bloom for premium glow effect
    // Layer 1: Wide soft outer glow (largest, dimmest)
    // Layer 2: Medium glow
    // Layer 3: Tight inner glow (smallest, brightest)

    struct GlowLayer
    {
        float radiusMul;
        float alphaMul;
    };

    const GlowLayer layers[] = {
        {1.5f, .15f},  // Outer - wide and soft
        {1.0f, .25f},  // Middle
        {.6f, .35f},   // Inner - tight and bright
    };

    const int numLayers = (samples > 8) ? 3 : (samples > 4) ? 2 : 1;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        float layerRadius = radius * layers[layer].radiusMul;
        int layerAlpha = (int)(baseAlpha * intensity * layers[layer].alphaMul);
        layerAlpha = std::clamp(layerAlpha, 0, 255);
        if (layerAlpha < 3)
        {
            continue;
        }

        ImU32 col = IM_COL32(r, g, b, layerAlpha);

        // 8-directional samples for smooth circular glow
        const float offsets[8][2] = {
            {layerRadius, .0f},
            {-layerRadius, .0f},
            {.0f, layerRadius},
            {.0f, -layerRadius},
            {layerRadius * .707f, layerRadius * .707f},
            {-layerRadius * .707f, layerRadius * .707f},
            {layerRadius * .707f, -layerRadius * .707f},
            {-layerRadius * .707f, -layerRadius * .707f},
        };

        int numOffsets = (samples > 4) ? 8 : 4;
        for (int i = 0; i < numOffsets; ++i)
        {
            list->AddText(
                font, size, ImVec2(pos.x + offsets[i][0], pos.y + offsets[i][1]), col, text);
        }
    }
}

// Draw 4-pointed star with glow
static void DrawStar4(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      ImU32 color,
                      ImU32 glowColor,
                      float rotation = .0f)
{
    const float innerRatio = .35f;  // Inner radius as fraction of outer
    const float outerR = size;
    const float innerR = size * innerRatio;

    // 4 points = 8 vertices alternating outer/inner
    ImVec2 points[8];
    for (int i = 0; i < 8; ++i)
    {
        float angle = rotation + (float)i * .785398f;  // 45 degrees = pi/4
        float radius = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
    }

    // Draw soft glow behind
    list->AddCircleFilled(pos, size * 1.8f, glowColor, 16);

    // Draw star shape
    list->AddConvexPolyFilled(points, 8, color);
}

// Draw 6-pointed star with glow
static void DrawStar6(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      ImU32 color,
                      ImU32 glowColor,
                      float rotation = .0f)
{
    const float innerRatio = .45f;
    const float outerR = size;
    const float innerR = size * innerRatio;

    // 6 points = 12 vertices
    ImVec2 points[12];
    for (int i = 0; i < 12; ++i)
    {
        float angle = rotation + (float)i * .5235988f;  // 30 degrees = pi/6
        float radius = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
    }

    // Draw soft glow layers
    list->AddCircleFilled(pos, size * 2.2f, glowColor, 16);
    list->AddCircleFilled(pos, size * 1.5f, glowColor, 16);

    // Draw star shape
    list->AddConvexPolyFilled(points, 12, color);
}

// Draw soft glowing orb with gradient layers
static void DrawSoftOrb(
    ImDrawList* list, const ImVec2& pos, float size, int r, int g, int b, int baseAlpha)
{
    // Multiple layers for smooth gradient falloff
    const int layers = 5;
    for (int i = layers - 1; i >= 0; --i)
    {
        float t = (float)i / (float)(layers - 1);
        float radius = size * (.4f + .6f * t);
        int layerAlpha = (int)(baseAlpha * (1.0f - t * .7f) * (1.0f - t * .3f));
        layerAlpha = std::clamp(layerAlpha, 0, 255);
        list->AddCircleFilled(pos, radius, IM_COL32(r, g, b, layerAlpha), 16);
    }
    // Bright core
    list->AddCircleFilled(pos, size * .25f, IM_COL32(255, 255, 255, baseAlpha / 2), 12);
}

// Draw ethereal wisp with flowing trail
static void DrawWisp(ImDrawList* list,
                     const ImVec2& pos,
                     float size,
                     float angle,
                     int r,
                     int g,
                     int b,
                     int baseAlpha,
                     float trailLength)
{
    // Trail direction
    float trailAngle = angle + PI;
    float dx = std::cos(trailAngle);
    float dy = std::sin(trailAngle);

    // Draw trail segments with decreasing alpha
    const int trailSegments = 6;
    for (int i = trailSegments - 1; i >= 0; --i)
    {
        float t = (float)i / (float)trailSegments;
        float segX = pos.x + dx * size * trailLength * t;
        float segY = pos.y + dy * size * trailLength * t;
        float segSize = size * (1.0f - t * .6f);
        int segAlpha = (int)(baseAlpha * (1.0f - t * .8f));
        segAlpha = std::clamp(segAlpha, 0, 255);
        list->AddCircleFilled(ImVec2(segX, segY), segSize, IM_COL32(r, g, b, segAlpha), 12);
    }

    // Main wisp body with glow
    list->AddCircleFilled(pos, size * 1.6f, IM_COL32(r, g, b, baseAlpha / 4), 14);
    list->AddCircleFilled(pos, size, IM_COL32(r, g, b, baseAlpha), 12);
    list->AddCircleFilled(pos, size * .4f, IM_COL32(255, 255, 255, baseAlpha / 2), 8);
}

// Draw magical rune symbol with glow
static void DrawRune(ImDrawList* list,
                     const ImVec2& pos,
                     float size,
                     int r,
                     int g,
                     int b,
                     int baseAlpha,
                     int runeType)
{
    ImU32 glowCol = IM_COL32(r, g, b, baseAlpha / 3);
    ImU32 mainCol = IM_COL32(r, g, b, baseAlpha);
    ImU32 brightCol =
        IM_COL32(std::min(255, r + 50), std::min(255, g + 50), std::min(255, b + 50), baseAlpha);
    float thickness = size * .15f;

    // Draw glow behind
    list->AddCircleFilled(pos, size * 1.8f, IM_COL32(r, g, b, baseAlpha / 5), 16);

    // Different rune patterns based on type
    switch (runeType % 4)
    {
        case 0:
        {
            // Diamond with cross
            float s = size;
            // Outer diamond
            list->AddQuad(ImVec2(pos.x, pos.y - s),
                          ImVec2(pos.x + s * .7f, pos.y),
                          ImVec2(pos.x, pos.y + s),
                          ImVec2(pos.x - s * .7f, pos.y),
                          mainCol,
                          thickness);
            // Inner cross
            list->AddLine(ImVec2(pos.x, pos.y - s * .5f),
                          ImVec2(pos.x, pos.y + s * .5f),
                          brightCol,
                          thickness * .8f);
            list->AddLine(ImVec2(pos.x - s * .35f, pos.y),
                          ImVec2(pos.x + s * .35f, pos.y),
                          brightCol,
                          thickness * .8f);
            // Corner dots
            list->AddCircleFilled(ImVec2(pos.x, pos.y - s), size * .12f, brightCol, 8);
            list->AddCircleFilled(ImVec2(pos.x, pos.y + s), size * .12f, brightCol, 8);
            break;
        }
        case 1:
        {
            // Triangle with eye
            float s = size * .9f;
            ImVec2 p1(pos.x, pos.y - s);
            ImVec2 p2(pos.x - s * .866f, pos.y + s * .5f);
            ImVec2 p3(pos.x + s * .866f, pos.y + s * .5f);
            list->AddTriangle(p1, p2, p3, mainCol, thickness);
            // Inner circle (eye)
            list->AddCircle(pos, size * .35f, brightCol, 12, thickness * .7f);
            list->AddCircleFilled(pos, size * .15f, brightCol, 8);
            break;
        }
        case 2:
        {
            // Asterisk/star burst
            float s = size;
            for (int i = 0; i < 6; ++i)
            {
                float angle = (float)i * .5236f;  // 30 degrees
                ImVec2 outer(pos.x + std::cos(angle) * s, pos.y + std::sin(angle) * s);
                list->AddLine(pos, outer, mainCol, thickness);
            }
            list->AddCircleFilled(pos, size * .2f, brightCol, 10);
            break;
        }
        case 3:
        {
            // Concentric circles with dots
            list->AddCircle(pos, size * .9f, mainCol, 14, thickness * .7f);
            list->AddCircle(pos, size * .5f, mainCol, 12, thickness * .6f);
            list->AddCircleFilled(pos, size * .2f, brightCol, 8);
            // Cardinal dots
            for (int i = 0; i < 4; ++i)
            {
                float angle = (float)i * 1.5708f;  // 90 degrees
                ImVec2 dotPos(pos.x + std::cos(angle) * size * .7f,
                              pos.y + std::sin(angle) * size * .7f);
                list->AddCircleFilled(dotPos, size * .1f, brightCol, 6);
            }
            break;
        }
    }
}

// Draw a spark with motion trail
static void DrawSpark(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      float angle,
                      int r,
                      int g,
                      int b,
                      int baseAlpha,
                      float life)
{
    // Spark gets more orange/yellow as it's "hotter"
    float heat = 1.0f - life;
    int sr = std::min(255, r + (int)(100 * heat));
    int sg = std::min(255, g + (int)(50 * heat));
    int sb = std::max(0, b - (int)(30 * heat));

    // Trail behind spark
    float trailAngle = angle + PI;
    const int trailSegs = 4;
    for (int i = trailSegs - 1; i >= 0; --i)
    {
        float t = (float)(i + 1) / (float)(trailSegs + 1);
        float trailX = pos.x + std::cos(trailAngle) * size * 3.0f * t;
        float trailY = pos.y + std::sin(trailAngle) * size * 3.0f * t;
        float segSize = size * (1.0f - t * .7f);
        int segAlpha = (int)(baseAlpha * (1.0f - t) * .6f);
        list->AddCircleFilled(ImVec2(trailX, trailY), segSize, IM_COL32(sr, sg, sb, segAlpha), 8);
    }

    // Main spark with glow
    list->AddCircleFilled(pos, size * 2.0f, IM_COL32(sr, sg, sb, baseAlpha / 4), 12);
    list->AddCircleFilled(pos, size, IM_COL32(sr, sg, sb, baseAlpha), 10);
    list->AddCircleFilled(pos, size * .4f, IM_COL32(255, 255, 220, baseAlpha), 8);
}

void DrawParticleAura(const ParticleAuraParams& params)
{
    ImDrawList* list = params.list;
    ImVec2 center = params.center;
    float radiusX = params.radiusX;
    float radiusY = params.radiusY;
    ImU32 color = params.color;
    float alpha = params.alpha;
    Settings::ParticleStyle style = params.style;
    int particleCount = params.particleCount;
    float particleSize = params.particleSize;
    float speed = params.speed;
    float time = params.time;
    int styleIndex = params.styleIndex;
    int enabledStyleCount = params.enabledStyleCount;

    if (!list || alpha <= .05f || particleCount <= 0)
    {
        return;
    }

    // Check if we should use textured particles from folders
    int texStyleId = static_cast<int>(style);
    bool useTextures = Settings::UseParticleTextures && ParticleTextures::IsInitialized();
    int texCount = useTextures ? ParticleTextures::GetTextureCount(texStyleId) : 0;
    bool hasTextures = (texCount > 0);

    // Gentle reduction when multiple styles overlap to prevent brightness pileup
    if (enabledStyleCount > 1)
    {
        alpha /= (1.0f + .15f * (enabledStyleCount - 1));
    }

    int baseR = (color >> 0) & 0xFF;
    int baseG = (color >> 8) & 0xFF;
    int baseB = (color >> 16) & 0xFF;

    float timeScaled = time * speed;

    for (int i = 0; i < particleCount; ++i)
    {
        // Use golden angle spacing with per-particle hash jitter to break regularity
        float golden = (float)(i + styleIndex * 97) * 2.399963f;  // Golden angle, offset per style
        float hashJitter = std::fmod((float)(i * 7 + styleIndex * 13) * .6180339887f, 1.0f);
        float phase = golden + hashJitter * 1.2f;  // Irregular angular distribution

        // Spread styles into radial bands to avoid center clumping when many styles overlap.
        float styleBandT = (enabledStyleCount > 0) ? (static_cast<float>(styleIndex) + .5f) /
                                                         static_cast<float>(enabledStyleCount)
                                                   : .5f;
        float minRadius = std::clamp(.58f + .20f * styleBandT, .58f, .88f);
        float radialSeed =
            std::fmod(static_cast<float>(i) * .6180339887f + styleBandT * .31f, 1.0f);
        float radialAnchor = std::sqrt(radialSeed);

        // Per-particle position jitter to break circular orbit patterns
        float jitterAngle =
            std::fmod((float)(i * 17 + styleIndex * 31) * .3819660113f, 1.0f) * TWO_PI;
        float jitterDist = std::fmod((float)(i * 23 + styleIndex * 7) * .6180339887f, 1.0f) * .25f;

        // Per-particle alpha variation (0.6 to 1.0 range)
        float alphaVariation = .6f + .4f * (.5f + .5f * std::sin(golden * 1.7f + timeScaled * .3f));

        int r = baseR, g = baseG, b = baseB;
        if (!hasTextures)
        {
            // Per-particle hue/saturation variation is only used by procedural fallback rendering.
            float hueShift = std::sin(golden * 2.3f + timeScaled * .25f) * .4f;
            float satMod = 1.1f + .2f * std::sin(golden * 1.5f);

            float hueAngle = hueShift * TWO_PI;
            float cosH = std::cos(hueAngle);
            float sinH = std::sin(hueAngle);

            // Simplified hue rotation matrix
            float newR = baseR * (.213f + .787f * cosH - .213f * sinH) +
                         baseG * (.213f - .213f * cosH + .143f * sinH) +
                         baseB * (.213f - .213f * cosH - .928f * sinH);
            float newG = baseR * (.715f - .715f * cosH - .715f * sinH) +
                         baseG * (.715f + .285f * cosH + .140f * sinH) +
                         baseB * (.715f - .715f * cosH + .283f * sinH);
            float newB = baseR * (.072f - .072f * cosH + .928f * sinH) +
                         baseG * (.072f - .072f * cosH - .283f * sinH) +
                         baseB * (.072f + .928f * cosH + .072f * sinH);

            float gray = .299f * newR + .587f * newG + .114f * newB;
            r = std::clamp((int)(gray + (newR - gray) * satMod), 0, 255);
            g = std::clamp((int)(gray + (newG - gray) * satMod), 0, 255);
            b = std::clamp((int)(gray + (newB - gray) * satMod), 0, 255);
        }

        float x, y, finalAlpha, finalSize;

        switch (style)
        {
            case Settings::ParticleStyle::Stars:
            default:
            {
                // Twinkling stars
                float orbit = phase + timeScaled * .5f;
                float radiusWave = .5f + .5f * std::sin(golden);
                float radiusMod =
                    minRadius + (1.0f - minRadius) * (.72f * radialAnchor + .28f * radiusWave);
                x = center.x + std::cos(orbit) * radiusX * radiusMod +
                    std::cos(jitterAngle) * radiusX * jitterDist;
                y = center.y + std::sin(orbit) * radiusY * radiusMod +
                    std::sin(jitterAngle) * radiusY * jitterDist;

                // Multi-frequency twinkle for more natural look
                float twinkle1 = std::sin(timeScaled * 3.0f + golden * 3.0f);
                float twinkle2 = std::sin(timeScaled * 5.0f + golden * 2.0f) * .3f;
                float twinkle = .5f + .5f * (twinkle1 + twinkle2) / 1.3f;

                if (twinkle < .1f)
                {
                    continue;
                }

                finalAlpha = alpha * (.3f + .7f * twinkle) * alphaVariation;
                finalSize = particleSize;

                int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
                int glowA = std::clamp((int)(finalAlpha * 60.0f), 0, 255);

                // Blue star colors - vary from deep blue to bright cyan-white
                // Brighter stars are more cyan/white, dimmer ones are deeper blue
                float brightness = twinkle * .6f + .4f;
                int sr = std::clamp((int)(80 + 175 * brightness * brightness),
                                    0,
                                    255);  // Low red, increases with brightness
                int sg = std::clamp((int)(120 + 135 * brightness), 0, 255);  // Medium green
                int sb = std::clamp((int)(180 + 75 * brightness), 0, 255);   // High blue base

                // Rotation based on time for sparkle effect
                float rotation = timeScaled * .5f + golden;

                // Use textured sprite if available
                if (hasTextures)
                {
                    // Faint afterimage at previous rotation for motion feel
                    float prevRotation = rotation - .15f;
                    int trailA = std::clamp(static_cast<int>(a * .25f), 0, 255);
                    ParticleTextures::DrawSpriteWithIndex(list,
                                                          ImVec2(x, y),
                                                          finalSize * 6.0f,
                                                          texStyleId,
                                                          i,
                                                          IM_COL32(255, 255, 255, trailA),
                                                          ParticleTextures::BlendMode::Additive,
                                                          prevRotation);
                    // Crisp core sprite, original texture colors
                    ParticleTextures::DrawSpriteWithIndex(list,
                                                          ImVec2(x, y),
                                                          finalSize * 6.0f,
                                                          texStyleId,
                                                          i,
                                                          IM_COL32(255, 255, 255, a),
                                                          ParticleTextures::BlendMode::Additive,
                                                          rotation);
                }
                else
                {
                    // Fallback to shape-based rendering
                    // Alternate between 4-point and 6-point stars
                    if (i % 3 == 0)
                    {
                        DrawStar6(list,
                                  ImVec2(x, y),
                                  finalSize,
                                  IM_COL32(sr, sg, sb, a),
                                  IM_COL32(sr, sg, sb, glowA),
                                  rotation);
                    }
                    else
                    {
                        DrawStar4(list,
                                  ImVec2(x, y),
                                  finalSize * .9f,
                                  IM_COL32(sr, sg, sb, a),
                                  IM_COL32(sr, sg, sb, glowA),
                                  rotation);
                    }

                    // Extra bright white center flash at peak twinkle
                    if (twinkle > .85f)
                    {
                        float flashSize = finalSize * .3f * (twinkle - .85f) / .15f;
                        list->AddCircleFilled(
                            ImVec2(x, y), flashSize, IM_COL32(220, 240, 255, a / 2), 8);
                    }
                }
                break;
            }

            case Settings::ParticleStyle::Sparks:
            {
                // Yellowish sparks with trails - like fire sparks
                float sparkTime = timeScaled * 2.0f + golden;
                float sparkPhase = std::fmod(sparkTime, TWO_PI);
                float life = sparkPhase / TWO_PI;

                // Sparks shoot outward with slight curve
                float sparkMinDist = std::clamp(minRadius - .05f, .52f, .85f);
                float dist = sparkMinDist + life * (1.0f - sparkMinDist);
                float baseAngle = phase + std::sin(golden * 2.0f) * .5f;
                float curveAngle = baseAngle + life * .3f * std::sin(golden);

                x = center.x + std::cos(curveAngle) * radiusX * dist +
                    std::cos(jitterAngle) * radiusX * jitterDist * .5f;
                y = center.y + std::sin(curveAngle) * radiusY * dist - life * radiusY * .4f +
                    std::sin(jitterAngle) * radiusY * jitterDist * .5f;

                // Fade out with life, but also flicker
                float flicker = .8f + .2f * std::sin(timeScaled * 15.0f + golden * 5.0f);
                finalAlpha = alpha * (1.0f - life * life) * flicker * alphaVariation;
                if (finalAlpha < .05f)
                {
                    continue;
                }

                finalSize = particleSize;

                int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                // Yellowish spark colors - bright yellow to orange as they fade
                float heatFade = 1.0f - life * .5f;                    // Cooler as they travel
                int sr = std::clamp((int)(255 * heatFade), 180, 255);  // High red
                int sg = std::clamp(
                    (int)(220 * heatFade - life * 80), 120, 220);    // Medium-high green, fades
                int sb = std::clamp((int)(80 - life * 60), 20, 80);  // Low blue

                // Use textured sprite if available
                if (hasTextures)
                {
                    // Ember trail ghosts - 2 fading crisp echoes behind movement
                    float trailDx = -std::cos(curveAngle);
                    float trailDy = -std::sin(curveAngle);
                    float trailSpacing = finalSize * 3.6f;
                    for (int t = 2; t >= 1; --t)
                    {
                        float tf = static_cast<float>(t) / 3.0f;
                        float tx = x + trailDx * trailSpacing * tf;
                        float ty = y + trailDy * trailSpacing * tf;
                        int trailA = std::clamp(static_cast<int>(a * (.3f - .1f * t)), 0, 255);
                        ParticleTextures::DrawSpriteWithIndex(list,
                                                              ImVec2(tx, ty),
                                                              finalSize * 6.0f,
                                                              texStyleId,
                                                              i,
                                                              IM_COL32(255, 255, 255, trailA),
                                                              ParticleTextures::BlendMode::Additive,
                                                              curveAngle);
                    }
                    // Crisp core sprite - original texture colors
                    ParticleTextures::DrawSpriteWithIndex(list,
                                                          ImVec2(x, y),
                                                          finalSize * 6.0f,
                                                          texStyleId,
                                                          i,
                                                          IM_COL32(255, 255, 255, a),
                                                          ParticleTextures::BlendMode::Additive,
                                                          curveAngle);
                }
                else
                {
                    DrawSpark(list, ImVec2(x, y), finalSize, curveAngle, sr, sg, sb, a, life);
                }
                break;
            }

            case Settings::ParticleStyle::Wisps:
            {
                // Ethereal flowing wisps with pale/blue tint
                float wispTime = timeScaled * .3f;
                float wave1 = std::sin(wispTime + golden) * .3f;
                float wave2 = std::sin(wispTime * 1.7f + golden * 1.3f) * .15f;
                float orbit = phase + wispTime + wave1 + wave2;

                float radiusWave = .5f + .5f * std::sin(golden + wispTime * .5f);
                float radiusMod =
                    minRadius + (1.0f - minRadius) * (.78f * radialAnchor + .22f * radiusWave);
                x = center.x + std::cos(orbit) * radiusX * radiusMod +
                    std::cos(jitterAngle) * radiusX * jitterDist;
                y = center.y + std::sin(orbit * .7f) * radiusY * radiusMod +
                    std::sin(jitterAngle) * radiusY * jitterDist;

                // Gentle pulsing (alpha only, not size - keep sprite consistent)
                float pulse = .6f + .4f * std::sin(wispTime * 2.0f + golden * 2.0f);
                finalAlpha = alpha * pulse * alphaVariation;
                finalSize = particleSize;  // Constant size for clean edges

                int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                // Ethereal pale/blue tint - shift base color toward white/blue
                int wr = std::min(255, r + 40);
                int wg = std::min(255, g + 50);
                int wb = std::min(255, b + 60);

                // Movement angle for trail direction
                float moveAngle = orbit + wave1 * 2.0f;
                float trailLength = 1.5f + .5f * std::sin(golden);

                // Use textured sprite if available
                if (hasTextures)
                {
                    // Trailing echo - faint crisp sprite behind movement
                    float echoDist = finalSize * 5.0f;
                    float ex = x - std::cos(moveAngle) * echoDist;
                    float ey = y - std::sin(moveAngle) * echoDist;
                    int echoA = std::clamp(static_cast<int>(a * .3f), 0, 255);
                    ParticleTextures::DrawSpriteWithIndex(list,
                                                          ImVec2(ex, ey),
                                                          finalSize * 5.0f,
                                                          texStyleId,
                                                          i,
                                                          IM_COL32(255, 255, 255, echoA),
                                                          ParticleTextures::BlendMode::Additive,
                                                          moveAngle);
                    // Crisp core sprite - original texture colors
                    ParticleTextures::DrawSpriteWithIndex(list,
                                                          ImVec2(x, y),
                                                          finalSize * 6.0f,
                                                          texStyleId,
                                                          i,
                                                          IM_COL32(255, 255, 255, a),
                                                          ParticleTextures::BlendMode::Additive,
                                                          moveAngle);
                }
                else
                {
                    DrawWisp(list, ImVec2(x, y), finalSize, moveAngle, wr, wg, wb, a, trailLength);
                }
                break;
            }

            case Settings::ParticleStyle::Runes:
            {
                // Orbiting magical rune symbols
                float runeOrbit = phase + timeScaled * .4f;
                float wobble = std::sin(timeScaled + golden) * .1f;
                float floatY = std::sin(timeScaled * 1.5f + golden * 2.0f) * radiusY * .08f;

                x = center.x + std::cos(runeOrbit + wobble) * radiusX * .9f +
                    std::cos(jitterAngle) * radiusX * jitterDist;
                y = center.y + std::sin(runeOrbit + wobble) * radiusY * .65f + floatY +
                    std::sin(jitterAngle) * radiusY * jitterDist;

                // Pulsing glow
                float pulse = .7f + .3f * std::sin(timeScaled * 2.0f + golden);
                finalAlpha = alpha * pulse * alphaVariation;
                finalSize = particleSize;

                int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                // Use textured rune sprites when available; fallback to procedural runes.
                if (hasTextures)
                {
                    // Periodic mystical surge - staggered per particle (~3s cycle)
                    float surgeCycle = std::sin(timeScaled * .35f + golden * 2.5f);
                    float surgeT =
                        std::clamp((surgeCycle - .7f) / .3f, .0f, 1.0f);  // Active top 30%
                    int surgedA = std::clamp(static_cast<int>(a * (1.0f + .4f * surgeT)), 0, 255);
                    float surgedSize = finalSize;

                    // Crisp core sprite with surge intensity - original texture colors
                    ParticleTextures::DrawSpriteWithIndex(list,
                                                          ImVec2(x, y),
                                                          surgedSize * 6.0f,
                                                          texStyleId,
                                                          i,
                                                          IM_COL32(255, 255, 255, surgedA),
                                                          ParticleTextures::BlendMode::Additive,
                                                          runeOrbit + wobble);
                }
                else
                {
                    DrawRune(list, ImVec2(x, y), finalSize, r, g, b, a, i);
                }
                break;
            }

            case Settings::ParticleStyle::Orbs:
            {
                // Soft glowing orbs with smooth gradients
                float orbTime = timeScaled * .4f;
                float orbit = phase + orbTime;

                // Breathing/floating motion
                float breathe = .85f + .15f * std::sin(orbTime * 1.5f + golden);
                float floatY = std::sin(orbTime * 2.0f + golden * 1.5f) * radiusY * .1f;

                float radiusWave = .5f + .5f * std::sin(golden);
                float radiusMod =
                    (minRadius + (1.0f - minRadius) * (.74f * radialAnchor + .26f * radiusWave)) *
                    breathe;
                x = center.x + std::cos(orbit) * radiusX * radiusMod +
                    std::cos(jitterAngle) * radiusX * jitterDist;
                y = center.y + std::sin(orbit * .8f) * radiusY * radiusMod + floatY +
                    std::sin(jitterAngle) * radiusY * jitterDist;

                // Stronger breathing pulse for clearer visual rhythm
                float glow = .5f + .5f * std::sin(orbTime * 2.0f + golden * 2.0f);
                finalAlpha = alpha * glow * alphaVariation;
                finalSize = particleSize;  // Constant size for clean edges

                int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

                // Use textured sprite if available
                if (hasTextures)
                {
                    // Crisp core sprite - original texture colors
                    ParticleTextures::DrawSpriteWithIndex(list,
                                                          ImVec2(x, y),
                                                          finalSize * 6.0f,
                                                          texStyleId,
                                                          i,
                                                          IM_COL32(255, 255, 255, a),
                                                          ParticleTextures::BlendMode::Additive,
                                                          .0f);
                }
                else
                {
                    DrawSoftOrb(list, ImVec2(x, y), finalSize, r, g, b, a);
                }
                break;
            }
        }
    }
}
}  // namespace TextEffects
