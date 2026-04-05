#pragma once

#include "PCH.h"

#include "ParticleTextures.h"
#include "Settings.h"
#include "TextEffects.h"
#include "Utf8Utils.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace TextEffects
{
using Utf8Utils::Utf8ToChars;

static constexpr float PI = std::numbers::pi_v<float>;  ///< pi
static constexpr float TWO_PI = 2.0f * PI;              ///< 2*pi
static constexpr float INV_TWO_PI =
    std::numbers::inv_pi_v<float> *
    0.5f;  ///< 1/(2*pi), used to normalize angles from radians to [0,1]

/// Return the fractional part of @p x.
/// For negative input the result is still in [0,1) because floor() is used.
inline float Frac(float x)
{
    return x - std::floor(x);
}

/// Convert HSV color to RGBA.
/// @param h  Hue in [0,1], wraps around.
/// @param s  Saturation [0,1].
/// @param v  Value/brightness [0,1].
/// @param a  Alpha [0,1].
/// @return RGBA color as ImVec4.
ImVec4 HSVtoRGB(float h, float s, float v, float a);

/// Extract alpha channel [0-255] from a packed ImU32 color.
inline int GetA(ImU32 c)
{
    return (c >> IM_COL32_A_SHIFT) & 0xFF;
}

/// Create a new color with scaled alpha (preserves RGB).
inline ImU32 WithAlpha(ImU32 c, float mul)
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

/// Scale RGB channels by a multiplier, preserving alpha.
inline ImU32 ScaleRGB(ImU32 c, float mul)
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

/// Captures ImGui vertex buffer state after rendering white text, enabling
/// callers to recolor vertices per-effect.
///
/// Members:
/// - @c list       Draw list pointer (must remain valid for the recolor pass).
/// - @c vtxStart / @c vtxEnd  Vertex index range written by the text call.
/// - @c bbMin / @c bbMax      Bounding box of the text in screen pixels.
///
/// width() / height() return clamped dimensions (minimum 1e-3f to avoid
/// division by zero).  normalizedX() / normalizedY() return [0,1] within
/// the bounding box.
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

    /// Render text in white (IM_COL32_WHITE) so the vertex buffer is
    /// populated, then record the vertex range and bounding box.  Callers
    /// recolor the vertices afterward based on the desired effect.
    /// @return true if vertices were added.
    static bool Begin(TextVertexSetup& out,
                      ImDrawList* list,
                      ImFont* font,
                      float size,
                      const ImVec2& pos,
                      const char* text);
};

/// Draw concentric glow rings behind the text outline.
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
                     bool fastOutlines);

/// Draw text outline using 4 cardinal directions (N/S/E/W).
/// Faster but less smooth than 8-dir.
void DrawOutline4Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w);

/// Draw text outline using 8 directions (cardinals + diagonals).
/// Smoother than 4-dir.
void DrawOutline8Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w);

/// Draw text outline, delegating to 4-dir or 8-dir based on @p fastOutlines.
void DrawOutlineInternal(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         ImU32 outline,
                         float w,
                         bool fastOutlines);

}  // namespace TextEffects
