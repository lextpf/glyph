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
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup TextEffects
 *
 * Render-thread helpers for TextEffects.hpp; no game-state access. TextVertexSetup captures
 * white text vertices and string bounds before an effect rewrites their colors.
 *
 * @code{.cpp}
 * TextVertexSetup vs;
 * if (!TextVertexSetup::Begin(vs, list, font, size, pos, text)) return;
 * for (int i = vs.vtxStart; i < vs.vtxEnd; ++i)
 * {
 *     auto& v = list->VtxBuffer[i];
 *     v.col = ScaleRGB(v.col, brightness);  // or HSV shift, gradient, ...
 * }
 * @endcode
 *
 * | Helper               | Stamps         | Use when                                 |
 * |----------------------|----------------|------------------------------------------|
 * | DrawOutline4Internal | 4 cardinal     | FastOutlines = true (lower draw cost)    |
 * | DrawOutline8Internal | 8-24 ring taps | Smoother edges, default                  |
 * | DrawOutlineInternal  | per argument   | Caller passes `fastOutlines` as argument |
 *
 * @see TextEffects::DrawOutline (public wrapper)
 */
namespace TextEffects
{
using Utf8Utils::Utf8ToChars;

static constexpr float PI = std::numbers::pi_v<float>;  ///< Pi
static constexpr float TWO_PI = 2.0f * PI;              ///< 2*pi
static constexpr float INV_TWO_PI =
    std::numbers::inv_pi_v<float> *
    0.5f;  ///< 1/(2*pi); scales an angle in radians to one turn from 0 to 1

/**
 * @fn float Frac(float x)
 * @brief Compute the non-negative fractional part of a value.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The expression x - floor(x) stays in [0, 1) for negative inputs.
 *
 * @param x  Input value.
 * @return   The fractional part of `x`.
 */
inline float Frac(float x)
{
    return x - std::floor(x);
}

/**
 * @fn float NoiseHash(float x, float y)
 * @brief Compute the integer-grid hash for value noise.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param x  Grid X coordinate.
 * @param y  Grid Y coordinate.
 * @return   A value in [0, 1).
 */
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

/**
 * @fn float ValueNoise(float x, float y)
 * @brief Compute two-dimensional value noise with quintic interpolation.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param x  Sample X coordinate.
 * @param y  Sample Y coordinate.
 * @return   A value in [0, 1).
 */
inline float ValueNoise(float x, float y)
{
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    // Quintic interpolation curve
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

/**
 * @fn float FBMNoise(float x, float y, int octaves, float persistence = .5f)
 * @brief Compute fractal Brownian motion from value noise.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param x Sample X coordinate.
 * @param y            Sample Y coordinate.
 * @param octaves      Positive number of noise octaves, capped at eight.
 * @param persistence  Non-negative amplitude multiplier for each successive octave.
 * @return             The normalized weighted noise, normally in [0, 1).
 * @pre Coordinates and persistence are finite; octaves is at least one.
 */
inline float FBMNoise(float x, float y, int octaves, float persistence = .5f)
{
    // Capped at 8 octaves. Each octave costs one ValueNoise sample and doubles the
    // sample frequency; at the default persistence the 8th already carries 1/128 of
    // the first octave's weight, so further octaves only cost time.
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

/**
 * @fn ImVec4 HSVtoRGB(float h, float s, float v, float a)
 * @brief Convert an HSV color to RGBA.
 * @author Alex (<https://github.com/lextpf>)
 *
 * tests/test_utils.cpp has a separate implementation; keep it in sync.
 *
 * @param h  Hue in [0, 1]. The value wraps.
 * @param s  Saturation in [0, 1].
 * @param v  Brightness in [0, 1].
 * @param a  Alpha in [0, 1].
 * @return   The RGBA color.
 */
ImVec4 HSVtoRGB(float h, float s, float v, float a);

/**
 * @fn int GetA(ImU32 c)
 * @brief Extract the alpha channel from a packed ImU32 color.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param c  Packed color.
 * @return   The alpha value in [0, 255].
 */
inline int GetA(ImU32 c)
{
    return (c >> IM_COL32_A_SHIFT) & 0xFF;
}

/**
 * @fn ImU32 WithAlpha(ImU32 c, float mul)
 * @brief Scale the alpha channel and preserve the RGB channels.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param c    Packed color.
 * @param mul  Alpha multiplier.
 * @return The color with its alpha clamped to [0, 255].
 */
inline ImU32 WithAlpha(ImU32 c, float mul)
{
    const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;

    const int na = (int)std::clamp(a * mul, .0f, 255.0f);

    return IM_COL32(r, g, b, na);
}

/**
 * @fn ImU32 ScaleRGB(ImU32 c, float mul)
 * @brief Scale the RGB channels and preserve the alpha channel.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param c    Packed color.
 * @param mul Non-negative RGB multiplier. Negative values are clamped to zero.
 * @return     The scaled color.
 */
inline ImU32 ScaleRGB(ImU32 c, float mul)
{
    mul = std::max(.0f, mul);  // Prevent negative colors

    const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;

    const int nr = (int)std::clamp(r * mul, .0f, 255.0f);
    const int ng = (int)std::clamp(g * mul, .0f, 255.0f);
    const int nb = (int)std::clamp(b * mul, .0f, 255.0f);

    return IM_COL32(nr, ng, nb, a);
}

/**
 * @struct TextVertexSetup
 * @brief Vertex state captured after a white text draw.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Members are valid only after Begin returns true. Bounds cover emitted glyph quads,
 * excluding spaces and clipped glyphs. Width and height are floored at 1e-3 pixels.
 * Normalized coordinates are not clamped; positions outside the bounds can exceed [0, 1].
 */
struct TextVertexSetup
{
    ImDrawList* list;  ///< Draw list; must stay valid for the recolor pass
    int vtxStart;      ///< First vertex index written by the text call
    int vtxEnd;        ///< One past the last vertex index written by the text call
    ImVec2 bbMin;      ///< Top-left of the text bounding box, in screen pixels
    ImVec2 bbMax;      ///< Bottom-right of the text bounding box, in screen pixels

    /**
     * @fn float width() const
     * @brief Measure the captured vertex extent along screen X.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return The width in pixels, floored at 1e-3.
     */
    float width() const { return (std::max)(bbMax.x - bbMin.x, 1e-3f); }
    /**
     * @fn float height() const
     * @brief Measure the captured vertex extent along screen Y.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return The height in pixels, floored at 1e-3.
     */
    float height() const { return (std::max)(bbMax.y - bbMin.y, 1e-3f); }
    /**
     * @fn float normalizedX(float x) const
     * @brief Normalize a screen X coordinate against the captured bounds.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return The relative coordinate without clamping.
     */
    float normalizedX(float x) const { return (x - bbMin.x) / width(); }
    /**
     * @fn float normalizedY(float y) const
     * @brief Normalize a screen Y coordinate against the captured bounds.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return The relative coordinate without clamping.
     */
    float normalizedY(float y) const { return (y - bbMin.y) / height(); }
    /**
     * @fn ImVec2 center() const
     * @brief Locate the midpoint of the emitted glyph bounds.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @return The center in screen pixels.
     */
    ImVec2 center() const { return ImVec2((bbMin.x + bbMax.x) * .5f, (bbMin.y + bbMax.y) * .5f); }

    /**
     * @fn bool Begin(TextVertexSetup& out, ImDrawList* list, ImFont* font, float size, const
     *     ImVec2& pos, const char* text)
     * @brief Draw white text and capture its emitted vertex range.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @param out   Receives the draw list, vertex range, and bounding box on success.
     * @return      True when the draw adds vertices. False for a null input, empty text, or a draw
     * that adds no vertices. Do not read `out` after a false return.
     */
    static bool Begin(TextVertexSetup& out,
                      ImDrawList* list,
                      ImFont* font,
                      float size,
                      const ImVec2& pos,
                      const char* text);
};

// DrawOutlineGlow is documented at its canonical declaration in TextEffects.hpp.
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

/**
 * @fn void DrawOutline4Internal(ImDrawList* list, ImFont* font, float size, const ImVec2& pos,
 *     const char* text, ImU32 outline, float w)
 * @brief Draw a text outline with four cardinal stamps.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param outline  Outline color.
 * @param w        Outline width, in pixels.
 * @pre `list`, `font`, and `text` are not null.
 */
void DrawOutline4Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w);

/**
 * @fn void DrawOutline8Internal(ImDrawList* list, ImFont* font, float size, const ImVec2& pos,
 *     const char* text, ImU32 outline, float w)
 * @brief Draw a circular text outline with eight through 24 stamps.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Tap count = clamp(ceil(pi * w), 8, 24), about one stamp per two circumference pixels.
 * A half-step offset avoids cardinal axes.
 *
 * @param outline  Outline color.
 * @param w        Outline radius, in pixels.
 * @pre `list`, `font`, and `text` are not null.
 */
void DrawOutline8Internal(ImDrawList* list,
                          ImFont* font,
                          float size,
                          const ImVec2& pos,
                          const char* text,
                          ImU32 outline,
                          float w);

/**
 * @fn void DrawOutlineInternal(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 outline, float w, bool fastOutlines)
 * @brief Select and draw one internal text-outline variant.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param outline       Outline color.
 * @param w             Outline width, in pixels.
 * @param fastOutlines  Whether to use the four-stamp path.
 * @pre `list`, `font`, and `text` are not null.
 */
void DrawOutlineInternal(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         ImU32 outline,
                         float w,
                         bool fastOutlines);

/**
 * @fn ImU32 ThreeColorGradient(ImU32 colA, ImU32 colMid, ImU32 colB, float t)
 * @brief Blend three colors with smoothstep transitions.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param colA Color at `t = 0`.
 * @param colMid  Color at `t = 0.5`.
 * @param colB    Color at `t = 1`.
 * @param t       Interpolation value in [0, 1].
 * @return        The blended packed color.
 */
inline ImU32 ThreeColorGradient(ImU32 colA, ImU32 colMid, ImU32 colB, float t)
{
    t = Saturate(t);
    float s1 = SmoothStep(Saturate(t * 2.0f));
    float s2 = SmoothStep(Saturate(t * 2.0f - 1.0f));
    return LerpColorU32(LerpColorU32(colA, colMid, s1), colB, s2);
}

/**
 * @fn ImU32 DeepShade(ImU32 c, float valMul = .55f, float satMul = 1.35f)
 * @brief Make a color darker and more saturated while preserving alpha.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Use with HotHighlight to give bright, similar tier colors visible animation contrast.
 *
 * @param c       Packed source color.
 * @param valMul  Brightness multiplier.
 * @param satMul  Saturation multiplier.
 * @return The adjusted packed color.
 */
inline ImU32 DeepShade(ImU32 c, float valMul = .55f, float satMul = 1.35f)
{
    const int r = (c >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (c >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (c >> IM_COL32_B_SHIFT) & 0xFF;
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    float h = .0f, s = .0f, v = .0f;
    ImGui::ColorConvertRGBtoHSV(r / 255.0f, g / 255.0f, b / 255.0f, h, s, v);
    v = Saturate(v * valMul);
    s = Saturate(s * satMul + .10f);  // Desaturated near-whites gain a hue too
    float nr = .0f, ng = .0f, nb = .0f;
    ImGui::ColorConvertHSVtoRGB(h, s, v, nr, ng, nb);
    return IM_COL32(static_cast<int>(nr * 255.0f + .5f),
                    static_cast<int>(ng * 255.0f + .5f),
                    static_cast<int>(nb * 255.0f + .5f),
                    a);
}

/**
 * @fn ImU32 HotHighlight(ImU32 c, float whiteness = .75f)
 * @brief Move a color toward white while preserving its alpha.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param c          Packed source color.
 * @param whiteness  Blend factor toward white.
 * @return           The adjusted packed color.
 */
inline ImU32 HotHighlight(ImU32 c, float whiteness = .75f)
{
    const int a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    ImU32 white = (IM_COL32(255, 255, 255, 0)) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    return LerpColorU32(c, white, Saturate(whiteness));
}

/**
 * @fn ImU32 WithAlphaFrom(ImU32 rgbSrc, ImU32 alphaSrc)
 * @brief Combine the RGB channels of one color with the alpha of another.
 * @author Alex (<https://github.com/lextpf>)
 *
 * LerpColorU32 interpolates alpha too. Highlights must keep fill alpha to avoid
 * transparent bright regions.
 *
 * @param rgbSrc    Color that supplies the RGB channels.
 * @param alphaSrc  Color that supplies the alpha channel.
 * @return The repacked color.
 */
inline ImU32 WithAlphaFrom(ImU32 rgbSrc, ImU32 alphaSrc)
{
    constexpr ImU32 kAlphaMask = static_cast<ImU32>(0xFF) << IM_COL32_A_SHIFT;
    return (rgbSrc & ~kAlphaMask) | (alphaSrc & kAlphaMask);
}

}  // namespace TextEffects
