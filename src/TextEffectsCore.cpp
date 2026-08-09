

#include "TextEffectsInternal.hpp"

namespace TextEffects
{

ImU32 LerpColorU32(ImU32 a, ImU32 b, float t)
{
    t = Saturate(t);

    const int ar = (a >> IM_COL32_R_SHIFT) & 0xFF;
    const int ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
    const int ab = (a >> IM_COL32_B_SHIFT) & 0xFF;
    const int aa = (a >> IM_COL32_A_SHIFT) & 0xFF;

    const int br = (b >> IM_COL32_R_SHIFT) & 0xFF;
    const int bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
    const int bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    const int ba = (b >> IM_COL32_A_SHIFT) & 0xFF;

    // A + (b - a) * t per channel; +0.5f rounds to the nearest integer.
    const int rr = (int)(ar + (br - ar) * t + .5f);
    const int rg = (int)(ag + (bg - ag) * t + .5f);
    const int rb = (int)(ab + (bb - ab) * t + .5f);
    const int ra = (int)(aa + (ba - aa) * t + .5f);

    return IM_COL32(rr, rg, rb, ra);
}

// Return exactly 1 at completion so callers can compare against the endpoint.
float EaseOutExpo(float t)
{
    t = Saturate(t);
    return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

void DrawOutline4Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w)
{
    list->AddText(font, size, ImVec2(pos.x - w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x + w, pos.y), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y - w), outline, text);
    list->AddText(font, size, ImVec2(pos.x, pos.y + w), outline, text);
}

// Scale taps with circumference (~1 stamp / 2 px) to avoid star-shaped outlines.
// The half-step phase avoids cardinal axes; the 8..24 clamp bounds draw cost.
void DrawOutline8Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w)
{
    const int n = std::clamp(static_cast<int>(std::ceil(TWO_PI * w * .5f)), 8, 24);
    const float step = TWO_PI / static_cast<float>(n);
    for (int i = 0; i < n; ++i)
    {
        const float a = (static_cast<float>(i) + .5f) * step;
        list->AddText(
            font, size, ImVec2(pos.x + std::cos(a) * w, pos.y + std::sin(a) * w), outline, text);
    }
}

void DrawOutlineInternal(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         ImU32 outline,
                         float w,
                         bool fastOutlines)
{
    if (fastOutlines)
    {
        DrawOutline4Internal(list, font, size, pos, text, outline, w);
    }
    else
    {
        DrawOutline8Internal(list, font, size, pos, text, outline, w);
    }
}

void DrawOutlineGlow(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 glowColor,
                     float outlineWidth,
                     float glowScale,
                     float glowAlpha,
                     int rings,
                     bool fastOutlines)
{
    if (!list || !font || !text || !text[0] || glowAlpha <= .0f || rings <= 0)
    {
        return;
    }

    const int gr = (glowColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int gg = (glowColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int gb = (glowColor >> IM_COL32_B_SHIFT) & 0xFF;
    const int ga = (glowColor >> IM_COL32_A_SHIFT) & 0xFF;

    for (int ring = rings; ring >= 1; --ring)
    {
        // Ring position: 0 = innermost, 1 = outermost.
        float ringT = (float)(ring - 1) / (float)(std::max)(rings - 1, 1);

        // Overlap the rings into one halo.
        float ringScale = glowScale * (1.0f + ringT * .6f);
        float ringOffset = outlineWidth * ringScale;

        float ringAlphaFactor = std::exp(-2.5f * ringT * ringT);
        float finalAlpha = glowAlpha * ringAlphaFactor;
        int ringAlpha = std::clamp((int)(ga * finalAlpha + .5f), 0, 255);
        if (ringAlpha <= 0)
        {
            continue;
        }

        ImU32 ringColor = IM_COL32(gr, gg, gb, ringAlpha);
        DrawOutlineInternal(list, font, size, pos, text, ringColor, ringOffset, fastOutlines);
    }
}

void DrawOutline(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 outline,
                 float w,
                 bool fastOutlines)
{
    DrawOutlineInternal(list, font, size, pos, text, outline, w, fastOutlines);
}

void DrawDirectionalInnerOutline(ImDrawList* list,
                                 ImFont* font,
                                 float size,
                                 const ImVec2& pos,
                                 const char* text,
                                 ImU32 outerColor,
                                 ImU32 tierColor,
                                 float outerWidth,
                                 float innerScale,
                                 float tintFactor,
                                 float alphaFactor,
                                 float lightAngleDeg,
                                 float lightBias,
                                 bool fastOutlines)
{
    if (!list || !font || !text || !text[0] || alphaFactor <= .0f)
    {
        return;
    }

    ImU32 baseColor = LerpColorU32(outerColor, tierColor, tintFactor);
    int ba = (baseColor >> IM_COL32_A_SHIFT) & 0xFF;
    ba = std::clamp((int)(ba * alphaFactor + .5f), 0, 255);
    baseColor = (baseColor & ~(0xFFu << IM_COL32_A_SHIFT)) | ((ImU32)ba << IM_COL32_A_SHIFT);

    float innerW = outerWidth * innerScale;
    if (innerW < .5f)
    {
        // Below half a pixel, the rim covers the fill instead of extending past it.
        return;
    }

    if (lightBias <= .001f || fastOutlines)
    {
        DrawOutlineInternal(list, font, size, pos, text, baseColor, innerW, fastOutlines);
        return;
    }

    float lightRad = lightAngleDeg * (3.14159265f / 180.0f);
    float lx = std::cos(lightRad);
    float ly = std::sin(lightRad);

    // E, W, N, S, NE, SE, NW, SW.
    constexpr float dirs[][2] = {{1, 0},
                                 {-1, 0},
                                 {0, -1},
                                 {0, 1},
                                 {.70710678f, -.70710678f},
                                 {.70710678f, .70710678f},
                                 {-.70710678f, -.70710678f},
                                 {-.70710678f, .70710678f}};

    // Each direction needs a separate width.
    for (const auto& dir : dirs)
    {
        float dot = dir[0] * lx + dir[1] * ly;
        float scale = 1.0f - dot * lightBias;  // Opposing the light = thicker
        float w = innerW * scale;
        float ox = dir[0] * w;
        float oy = dir[1] * w;
        list->AddText(font, size, ImVec2(pos.x + ox, pos.y + oy), baseColor, text);
    }
}

void AddTextOutline4(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 col,
                     ImU32 outline,
                     float w,
                     bool fastOutlines,
                     const OutlineGlowParams* glow)
{
    if (!list || !font || !text || !text[0])
    {
        return;
    }

    // Draw order: glow rings -> outline -> fill.
    if (glow && glow->enabled)
    {
        DrawOutlineGlow(list,
                        font,
                        size,
                        pos,
                        text,
                        glow->color,
                        w,
                        glow->scale,
                        glow->alpha,
                        glow->rings,
                        fastOutlines);
    }

    DrawOutlineInternal(list, font, size, pos, text, outline, w, fastOutlines);

    list->AddText(font, size, pos, col, text);
}

// Measure emitted quads; CalcTextSize does not describe the captured vertex extent.
bool TextVertexSetup::Begin(TextVertexSetup& out,
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

// tests/test_utils.cpp re-implements this function; mirror any change there.
ImVec4 HSVtoRGB(float h, float s, float v, float a)
{
    h = Frac(h);  // Wrap hue to [0, 1)

    const float c = v * s;  // Chroma
    const float x = c * (1.0f - std::fabs(Frac(h * 6.0f) * 2.0f - 1.0f));
    const float m = v - c;  // Match value

    float r = 0, g = 0, b = 0;

    const int i = (int)std::floor(h * 6.0f);
    switch (i % 6)
    {
        case 0:
            r = c;
            g = x;
            b = 0;
            break;  // Red to yellow
        case 1:
            r = x;
            g = c;
            b = 0;
            break;  // Yellow to green
        case 2:
            r = 0;
            g = c;
            b = x;
            break;  // Green to cyan
        case 3:
            r = 0;
            g = x;
            b = c;
            break;  // Cyan to blue
        case 4:
            r = x;
            g = 0;
            b = c;
            break;  // Blue to magenta
        case 5:
            r = c;
            g = 0;
            b = x;
            break;  // Magenta to red
    }

    return ImVec4(r + m, g + m, b + m, a);
}

void ApplyWaveDisplacement(ImDrawList* list,
                           int vtxStart,
                           int vtxEnd,
                           float bbMinX,
                           float bbWidth,
                           float amplitude,
                           float frequency,
                           float speed,
                           float time)
{
    if (!list || vtxEnd <= vtxStart || bbWidth < 1e-3f || amplitude < .01f)
    {
        return;
    }

    constexpr float TWO_PI = 6.28318530718f;

    // Displace each vertex from its own X; unchanged UVs make the glyph shear vertically.
    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        auto& vtx = list->VtxBuffer[i];
        float nx = (vtx.pos.x - bbMinX) / bbWidth;
        float wave = std::sin(nx * frequency * TWO_PI + time * speed * TWO_PI) * amplitude;
        vtx.pos.y += wave;
    }
}

}  // namespace TextEffects
