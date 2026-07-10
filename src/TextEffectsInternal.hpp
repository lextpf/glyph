#pragma once

#include "PCH.hpp"

#include "ParticleTextures.hpp"
#include "Settings.hpp"
#include "TextEffects.hpp"
#include "Utf8Utils.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

/**
 * @namespace TextEffects
 * @brief Internal helpers shared by the TextEffects implementation files.
 * @author Alex (https://github.com/lextpf)
 * @ingroup TextEffects
 *
 * Companion to TextEffects.hpp. Public effect functions live in the main
 * header; this file declares the building blocks they share -- color math,
 * vertex-capture state, and internal outline variants.
 *
 * ## :material-palette-outline: Vertex-Recolor Pattern
 *
 * Most animated effects work by rendering text in pure white, capturing
 * the resulting vertex range from the ImDrawList, and then rewriting
 * those vertices' colors per effect. TextVertexSetup::Begin() drives the
 * capture step and records the vertex range plus the bounding box; the
 * caller iterates `[vtxStart, vtxEnd)` and modifies colors directly.
 *
 * ```cpp
 * TextVertexSetup vs;
 * if (!TextVertexSetup::Begin(vs, list, font, size, pos, text)) return;
 * for (int i = vs.vtxStart; i < vs.vtxEnd; ++i)
 * {
 *     auto& v = list->VtxBuffer[i];
 *     v.col = ScaleRGB(v.col, brightness);  // or HSV shift, gradient, ...
 * }
 * ```
 *
 * ## :material-vector-square: Outline Variants
 *
 * |               Helper | Directions | Use when                                 |
 * |----------------------|------------|------------------------------------------|
 * | DrawOutline4Internal |          4 | FastOutlines = true (lower draw cost)    |
 * | DrawOutline8Internal |          8 | Smoother edges, default                  |
 * |  DrawOutlineInternal |    4 or 8  | Caller passes `fastOutlines` as argument |
 *
 * @see TextEffects::DrawOutline (public wrapper)
 */
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

/// Integer-grid value-noise hash in [0,1). Shared by the text energy effects
/// and the particle drift so both draw from the same quality noise field.
inline float NoiseHash(float x, float y)
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

/// 2D value noise with quintic interpolation, range [0,1).
inline float ValueNoise(float x, float y)
{
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    // Quintic interpolation curve for smoother results
    fx = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    fy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

    float a = NoiseHash(ix, iy);
    float b = NoiseHash(ix + 1.0f, iy);
    float c = NoiseHash(ix, iy + 1.0f);
    float d = NoiseHash(ix + 1.0f, iy + 1.0f);

    float ab = a + (b - a) * fx;
    float cd = c + (d - c) * fx;
    return ab + (cd - ab) * fy;
}

/// Fractal Brownian Motion built from ValueNoise, range [0,1).
/// Shared by Enchant/Frost text shading and the M1 falling-particle drift.
inline float FBMNoise(float x, float y, int octaves, float persistence = .5f)
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

/// Blend three colors continuously using smoothstep transitions.
/// Produces a seamless gradient A->Mid->B with no visible breakpoints.
/// @param t  Interpolation value [0, 1].
inline ImU32 ThreeColorGradient(ImU32 colA, ImU32 colMid, ImU32 colB, float t)
{
    t = Saturate(t);
    float s1 = SmoothStep(Saturate(t * 2.0f));
    float s2 = SmoothStep(Saturate(t * 2.0f - 1.0f));
    return LerpColorU32(LerpColorU32(colA, colMid, s1), colB, s2);
}

/// Deep jewel shade of a color: value down, saturation up, alpha preserved.
///
/// Animated effects sweep between this shade and a hot highlight so they
/// carry their OWN contrast. Tier palettes are same-family pairs only ~15%
/// apart on bright bases -- far too narrow to animate visibly, which is why
/// the pre-overhaul effects read as invisible.
inline ImU32 DeepShade(ImU32 c, float valMul = .55f, float satMul = 1.35f)
{
    const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    float h = .0f, s = .0f, v = .0f;
    ImGui::ColorConvertRGBtoHSV(r / 255.0f, g / 255.0f, b / 255.0f, h, s, v);
    v = Saturate(v * valMul);
    s = Saturate(s * satMul + .10f);  // desaturated near-whites gain a hue too
    float nr = .0f, ng = .0f, nb = .0f;
    ImGui::ColorConvertHSVtoRGB(h, s, v, nr, ng, nb);
    return IM_COL32(static_cast<int>(nr * 255.0f + .5f),
                    static_cast<int>(ng * 255.0f + .5f),
                    static_cast<int>(nb * 255.0f + .5f),
                    a);
}

/// Hot highlight: push a color toward pure white, keeping a trace of its
/// hue and its alpha. The bright pole of the DeepShade<->Hot sweep.
inline ImU32 HotHighlight(ImU32 c, float whiteness = .75f)
{
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    ImU32 white = (IM_COL32(255, 255, 255, 0)) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    return LerpColorU32(c, white, Saturate(whiteness));
}

/// Repack a color's RGB with the alpha channel of another color.
///
/// The tier highlight arrives packed with the (low) effectAlpha, and
/// LerpColorU32 lerps all four channels -- so a sweep toward the raw
/// highlight made text MORE TRANSPARENT exactly where it brightened,
/// self-canceling. Every bright sweep target must adopt the fill's alpha.
inline ImU32 WithAlphaFrom(ImU32 rgbSrc, ImU32 alphaSrc)
{
    constexpr ImU32 kAlphaMask = static_cast<ImU32>(0xFF) << IM_COL32_A_SHIFT;
    return (rgbSrc & ~kAlphaMask) | (alphaSrc & kAlphaMask);
}

}  // namespace TextEffects
