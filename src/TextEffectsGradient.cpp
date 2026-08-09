// Ramps span the whole string bounds and are interpolated from glyph-quad corners.

#include "TextEffectsInternal.hpp"

namespace TextEffects
{

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

    const int vtxStart = list->VtxBuffer.Size;
    list->AddText(font, size, pos, IM_COL32_WHITE, text);
    const int vtxEnd = list->VtxBuffer.Size;

    if (vtxEnd <= vtxStart)
    {
        return;  // No vertices added
    }

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
        for (int i = vtxStart; i < vtxEnd; ++i)
        {
            list->VtxBuffer[i].col = colLeft;
        }
        return;
    }

    for (int i = vtxStart; i < vtxEnd; ++i)
    {
        const float x = list->VtxBuffer[i].pos.x;
        const float t = (x - minX) / denom;
        list->VtxBuffer[i].col = LerpColorU32(colLeft, colRight, t);
    }
}

void AddTextVerticalGradient(ImDrawList* list,
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

void AddTextDiagonalGradient(ImDrawList* list,
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

    // Screen Y grows downward; (1, 1) runs down-right.
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

    // An external center leaves no vertex at t = 0, so pure colCenter is absent.
    const ImVec2 center = overrideCenter ? *overrideCenter : s.center();

    // T = 1 at the bounding-box corner furthest from the center.
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
        // Gamma is not validated. RadialGradient without an INI argument gives gamma = 0,
        // so every vertex receives colEdge.
        if (gamma != 1.0f)
        {
            t = std::pow(t, gamma);
        }
        list->VtxBuffer[i].col = LerpColorU32(colCenter, colEdge, t);
    }
}

}  // namespace TextEffects
