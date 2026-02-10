#include "TextEffectsInternal.h"

namespace TextEffects
{

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
void DrawOutline4Internal(ImDrawList* list,
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
void DrawOutline8Internal(ImDrawList* list,
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

// Draw outline using fastOutlines flag to pick 4-dir or 8-dir
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

    // Extract RGB from glow color, we'll modulate alpha per ring
    const int gr = (glowColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int gg = (glowColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int gb = (glowColor >> IM_COL32_B_SHIFT) & 0xFF;
    const int ga = (glowColor >> IM_COL32_A_SHIFT) & 0xFF;

    // Draw concentric rings from outermost (faintest) to innermost (brightest)
    for (int ring = rings; ring >= 1; --ring)
    {
        // Ring offset scales: ring 1 = 1.6x, ring 2 = 2.2x, ring 3 = 2.8x
        float ringScale = glowScale + (ring - 1) * .6f;
        float ringOffset = outlineWidth * ringScale;

        // Alpha falls off per ring: innermost is peak, outermost is faintest
        float ringAlphaFactor = 1.0f / (float)ring;
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
    // Apply alpha factor
    int ba = (baseColor >> IM_COL32_A_SHIFT) & 0xFF;
    ba = std::clamp((int)(ba * alphaFactor + .5f), 0, 255);
    baseColor = (baseColor & ~(0xFFu << IM_COL32_A_SHIFT)) | ((ImU32)ba << IM_COL32_A_SHIFT);

    float innerW = outerWidth * innerScale;
    if (innerW < .5f)
    {
        return;
    }

    if (lightBias <= .001f || fastOutlines)
    {
        // Uniform inner outline (no directionality)
        DrawOutlineInternal(list, font, size, pos, text, baseColor, innerW, fastOutlines);
        return;
    }

    // Directional: 8 offsets with per-direction width scaling
    float lightRad = lightAngleDeg * (3.14159265f / 180.0f);
    float lx = std::cos(lightRad);
    float ly = std::sin(lightRad);

    // 8 directions: E, W, N, S, NE, SE, NW, SW
    constexpr float dirs[][2] = {{1, 0},
                                 {-1, 0},
                                 {0, -1},
                                 {0, 1},
                                 {.70710678f, -.70710678f},
                                 {.70710678f, .70710678f},
                                 {-.70710678f, -.70710678f},
                                 {-.70710678f, .70710678f}};

    for (const auto& dir : dirs)
    {
        // Dot product with light direction: directions opposing light get thicker
        float dot = dir[0] * lx + dir[1] * ly;
        float scale = 1.0f - dot * lightBias;  // opposing = thicker
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

    // Draw outline glow behind everything if enabled
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

    // Draw 4-dir or 8-dir outline based on fastOutlines
    DrawOutlineInternal(list, font, size, pos, text, outline, w, fastOutlines);

    // Draw main text on top
    list->AddText(font, size, pos, col, text);
}

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

ImVec4 HSVtoRGB(float h, float s, float v, float a)
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

    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        auto& vtx = list->VtxBuffer[i];
        float nx = (vtx.pos.x - bbMinX) / bbWidth;
        float wave = std::sin(nx * frequency * TWO_PI + time * speed * TWO_PI) * amplitude;
        vtx.pos.y += wave;
    }
}

}  // namespace TextEffects
