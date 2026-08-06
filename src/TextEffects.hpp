#pragma once

#include "PCH.hpp"
#include "Settings.hpp"

#include <algorithm>

/**
 * @namespace TextEffects
 * @brief Collection of text rendering effects for ImGui.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup TextEffects
 *
 * Render-thread only, inside an ImGui frame; no game-state access. Effects recolor emitted
 * vertices, except outline, glow and shadow passes, which stamp extra text copies.
 *
 * Effect clocks use ImGui::GetTime() unless the caller supplies phase01 or time.
 *
 * Text draw functions share list, font, size in pixels, top-left pos and null-terminated
 * UTF-8 text. AddText* functions skip null inputs and empty text. DrawOutline and its
 * template wrappers require non-null inputs.
 *
 * ### :material-sort-variant: Rendering order
 *
 * The caller draws glow and shadow; ApplyTextEffect draws the remaining passes.
 * Optional passes run only when enabled.
 *
 * 1. Glow
 * 2. Shadow
 * 3. Directional inner outline
 * 4. Outline glow rings
 * 5. Outer outline
 * 6. Fill
 * 7. Alpha shaping, top-edge shine, wave displacement
 *
 * @see Settings::EffectType, Settings::EffectParams
 */
namespace TextEffects
{
/**
 * @fn float Saturate(float x)
 * @brief Clamp a value to the 0 to 1 range.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param x Input value.
 * @return Value clamped to [0, 1].
 */
constexpr float Saturate(float x)
{
    return std::clamp(x, .0f, 1.0f);
}

/**
 * @fn float SmoothStep(float t)
 * @brief Quintic smoothstep, with continuous first and second derivatives at the ends.
 * @author Alex (<https://github.com/lextpf>)
 *
 * $$\text{smoothstep}(t) = 6t^5 - 15t^4 + 10t^3$$
 *
 * @param t Input value, clamped to [0, 1].
 * @return Interpolated value in [0, 1].
 */
constexpr float SmoothStep(float t)
{
    t = Saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/**
 * @fn float EaseOutCubic(float t)
 * @brief Cubic ease-out: fast start, gentle deceleration to the target.
 * @author Alex (<https://github.com/lextpf>)
 *
 * $$\text{easeOutCubic}(t) = 1 - (1 - t)^3$$
 *
 * @param t Input value, clamped to [0, 1].
 * @return Eased value in [0, 1].
 */
constexpr float EaseOutCubic(float t)
{
    t = Saturate(t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/**
 * @fn float EaseInCubic(float t)
 * @brief Cubic ease-in: gentle start, accelerating away from the origin.
 * @author Alex (<https://github.com/lextpf>)
 *
 * $$\text{easeInCubic}(t) = t^3$$
 *
 * @param t Input value, clamped to [0, 1].
 * @return Eased value in [0, 1].
 * @see EaseOutCubic
 */
constexpr float EaseInCubic(float t)
{
    t = Saturate(t);
    return t * t * t;
}

/**
 * @fn float EaseOutExpo(float t)
 * @brief Exponential ease-out: very fast start, long flattening tail.
 * @author Alex (<https://github.com/lextpf>)
 *
 * When t >= 1, return exactly 1 for completion checks.
 *
 * $$\text{easeOutExpo}(t) = \begin{cases} 1 & t \ge 1 \\ 1 - 2^{-10t} & t < 1 \end{cases}$$
 *
 * @param t Input value, clamped to [0, 1].
 * @return Eased value in [0, 1].
 * @see EaseOutCubic
 */
float EaseOutExpo(float t);

/**
 * @fn ImU32 LerpColorU32(ImU32 a, ImU32 b, float t)
 * @brief Linearly interpolate two packed colors, channel by channel.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Interpolate alpha with RGB and round each channel to the nearest integer.
 *
 * $$C_{out} = C_a + (C_b - C_a) \cdot t = C_a(1-t) + C_b \cdot t$$
 *
 * @param a First color, ImU32 packed ABGR.
 * @param b Second color, ImU32 packed ABGR.
 * @param t Interpolation factor, clamped to [0, 1]; 0 returns `a`, 1 returns `b`.
 * @return Interpolated color as ImU32.
 * @see Saturate
 */
ImU32 LerpColorU32(ImU32 a, ImU32 b, float t);

/**
 * @struct OutlineGlowParams
 * @brief Parameters for the white halo behind text outlines.
 * @author Alex (<https://github.com/lextpf>)
 */
struct OutlineGlowParams
{
    bool enabled = false;  ///< Master gate; false skips the glow rings
    /// RGB stays fixed; ring falloff scales alpha.
    ImU32 color = 0;
    /// Inner radius / outline width; outer rings reach 1.6 times this radius.
    float scale = 1.6f;
    /// Peak alpha multiplier. Ring falloff = exp(-2.5 * t * t), with t from 0 inside to 1 outside.
    float alpha = .20f;
    int rings = 2;  ///< Number of concentric rings; the INI clamps the value to 1-3
};

/**
 * @fn void DrawOutline(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 outline, float w, bool fastOutlines)
 * @brief Stamp an outline around a text position.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param outline Outline color.
 * @param w Outline width in pixels.
 * @param fastOutlines If true, stamps the 4 cardinal offsets; otherwise stamps a ring of 8 to 24
 * taps, the count scaled to the circumference of `w`.
 * @pre List != nullptr
 * @pre Font != nullptr
 * @pre Text != nullptr
 * @warning The preconditions are mandatory. This call chain has no null guard and dereferences @p
 * list at once, unlike AddTextOutline4 and the AddText* effects, which return on a null argument.
 */
void DrawOutline(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 outline,
                 float w,
                 bool fastOutlines);

/**
 * @struct ShineParams
 * @brief Parameters for the static top-edge shine overlay.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The textGlowAlpha value brightens the text body by up to 30% and fades it by up to 20%.
 * The body pass requires luminance >= .22, max channel >= 80, and the batch alpha threshold.
 */
struct ShineParams
{
    /// Gates shine only; innerTextAlpha and textGlowAlpha apply even when disabled.
    bool enabled = false;
    /// Strength from 0 to 1; scales by .40, fades at horizontal edges, then uses pow(x, 1.35).
    float intensity = .35f;
    /// Vertical exponent = max(.5, falloff) * 3.4.
    float falloff = 2.0f;
    float textGlowAlpha = .0f;    ///< Body glow strength from 0 to 1.
    float innerTextAlpha = 1.0f;  ///< Text body alpha multiplier 0-1 (applied after effects)
};

/**
 * @struct WaveParams
 * @brief Parameters for the wave displacement effect.
 * @author Alex (<https://github.com/lextpf>)
 */
struct WaveParams
{
    bool enabled = false;    ///< Master gate; false skips the displacement pass
    float amplitude = 1.5f;  ///< Peak Y displacement in pixels
    float frequency = 3.0f;  ///< Cycles across the text width
    float speed = 1.0f;      ///< Phase travel in cycles per second
    float time = .0f;        ///< Caller-supplied clock in seconds (ImGui::GetTime())
};

/**
 * @fn void ApplyWaveDisplacement(ImDrawList* list, int vtxStart, int vtxEnd, float bbMinX, float
 *     bbWidth, float amplitude, float frequency, float speed, float time)
 * @brief Apply sine-wave Y displacement to a vertex range.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Apply after coloring. Each vertex moves from its own X; unchanged UVs make glyphs shear.
 *
 * @param vtxStart   First vertex index to displace.
 * @param vtxEnd     One past the last vertex index to displace.
 * @param bbMinX     Left edge of the range's bounding box, in screen pixels.
 * @param bbWidth    Width of that bounding box, in screen pixels.
 * @param amplitude  Peak Y displacement in pixels.
 * @param frequency  Cycles across `bbWidth`.
 * @param speed      Phase travel in cycles per second.
 * @param time       Caller clock in seconds (ImGui::GetTime()).
 * @note No-op when @p list is null, when the vertex range is empty, when @p bbWidth < 1e-3, or when
 * @p amplitude < 0.01.
 */
void ApplyWaveDisplacement(ImDrawList* list,
                           int vtxStart,
                           int vtxEnd,
                           float bbMinX,
                           float bbWidth,
                           float amplitude,
                           float frequency,
                           float speed,
                           float time);

/**
 * @fn void DrawDirectionalInnerOutline(ImDrawList*, ImFont*, float, const ImVec2&, const char*,
 *     ImU32, ImU32, float, float, float, float, float, float, bool)
 * @brief Draw a directional inner outline tinted toward the tier color.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Draw below the fill so only the rim remains. lightBias <= .001 or fastOutlines selects
 * a uniform rim of width outerWidth * innerScale. Otherwise, eight stamp offsets scale by
 * 1 - dot(direction, light) * lightBias.
 *
 * @param outerColor Outer outline color; the rim color starts from it.
 * @param tierColor Color the rim is tinted toward.
 * @param outerWidth Outer outline width in pixels.
 * @param innerScale Inner rim width as a fraction of `outerWidth`.
 * @param tintFactor Blend from @p outerColor to `tierColor`; 0 keeps the outline color, 1 uses the
 * tier color.
 * @param alphaFactor Multiplier applied to the blended color's alpha.
 * @param lightAngleDeg Light direction in degrees, measured with +x right and +y down, so 315
 * points up and to the right.
 * @param lightBias Directional width variation; 0 gives a uniform rim.
 * @param fastOutlines If true, forces the uniform path with the 4-stamp outline.
 * @note Returns without drawing when `list`, @p font or @p text is null, when the string is empty,
 * when @p alphaFactor is 0 or less, or when outerWidth * innerScale is below 0.5 px.
 */
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
                                 bool fastOutlines);

/**
 * @fn void DrawOutlineGlow(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 glowColor, float outlineWidth, float glowScale, float glowAlpha, int rings,
 *     bool fastOutlines)
 * @brief Draw concentric glow rings behind the text outline.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Draw outer rings first; each ring costs one DrawOutline pass. In the formula, k counts
 * from the innermost ring, n is the ring count, tau = k / max(n - 1, 1), w is outlineWidth,
 * and A_glow is the glowColor alpha.
 *
 * $$r_k = w \cdot glowScale \cdot (1 + 0.6\,\tau), \qquad
 *   a_k = A_{glow} \cdot glowAlpha \cdot e^{-2.5\,\tau^2}$$
 *
 *
 * @param glowColor ring color; RGB is used as-is, its alpha is the base for the falloff.
 * @param outlineWidth outline width in pixels; every ring radius is a multiple of it.
 * @param glowScale innermost ring radius, as a multiple of `outlineWidth`. the outermost ring
 * reaches 1.6 times that value.
 * @param glowAlpha peak ring opacity, as a multiplier of the glow color's alpha.
 * @param rings number of concentric rings.
 * @param fastOutlines if true, every ring uses the 4-stamp outline; otherwise the ring of 8 to 24
 * taps.
 * @note unlike DrawOutline, this call is guarded: it returns without drawing when `list`, @p font
 * or @p text is null, when the string is empty, when @p glowAlpha is 0 or less, or when @p rings is
 * 0 or less. A single ring whose solved alpha rounds to 0 is skipped, and the remaining rings still
 * draw.
 */
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
 * @fn void WithOutline(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 outline, float w, bool fastOutlines, Args&&... args)
 * @brief Draw an outline, then delegate the fill to any effect function.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @code
 * WithOutline<AddTextHorizontalGradient>(list, font, size, pos, text,
 *                                        outline, w, fastOutlines, colLeft, colRight);
 * @endcode
 *
 * @tparam EffectFn Pointer to the non-outline effect function.
 * @tparam Args     Additional effect-specific argument types.
 * @param outline       Outline color.
 * @param w             Outline width, in pixels.
 * @param fastOutlines  Whether to use the four-stamp outline path.
 * @param args          Arguments forwarded to the effect function.
 * @warning DrawOutline runs first, so this template inherits its mandatory preconditions:
 */
template <auto EffectFn, typename... Args>
inline void WithOutline(ImDrawList* list,
                        ImFont* font,
                        float size,
                        const ImVec2& pos,
                        const char* text,
                        ImU32 outline,
                        float w,
                        bool fastOutlines,
                        Args&&... args)
{
    static_assert(
        std::is_invocable_v<decltype(EffectFn),
                            ImDrawList*,
                            ImFont*,
                            float,
                            const ImVec2&,
                            const char*,
                            Args...>,
        "EffectFn must accept (ImDrawList*, ImFont*, float, const ImVec2&, const char*, Args...)");
    DrawOutline(list, font, size, pos, text, outline, w, fastOutlines);
    EffectFn(list, font, size, pos, text, std::forward<Args>(args)...);
}

/**
 * @struct DualOutlineParams
 * @brief Parameters for the dual-tone directional inner outline.
 * @author Alex (<https://github.com/lextpf>)
 */
struct DualOutlineParams
{
    bool enabled = false;     ///< Master gate; false skips the inner outline pass
    ImU32 tierColor = 0;      ///< Tier color to blend toward
    float innerScale = .5f;   ///< Inner outline width as fraction of outer
    float tintFactor = .3f;   ///< Blend toward tier color (0=outline, 1=tier)
    float alphaFactor = .5f;  ///< Multiplier on the rim alpha; 0 or less skips the pass
    /// Degrees in downward-Y coordinates; 315 points up-right.
    float lightAngle = 315.f;
    /// Values <= .001 or FastOutlines select a uniform rim.
    float lightBias = .15f;
};

/**
 * @fn void WithOutlineGlow(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 outline, float w, bool fastOutlines, const OutlineGlowParams* glow,
 *     Args&&... args)
 * @brief WithOutline variant that also draws outline glow rings behind the outline.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Null or disabled glow skips the rings.
 *
 * @tparam EffectFn Pointer to the non-outline effect function.
 * @tparam Args     Additional effect-specific argument types.
 * @param outline       Outline color.
 * @param w             Outline width, in pixels.
 * @param fastOutlines  Whether to use the four-stamp outline path.
 * @param glow          Optional outline-glow parameters.
 * @param args          Arguments forwarded to the effect function.
 * @warning DrawOutline runs after the rings, so this template inherits its mandatory
 */
template <auto EffectFn, typename... Args>
inline void WithOutlineGlow(ImDrawList* list,
                            ImFont* font,
                            float size,
                            const ImVec2& pos,
                            const char* text,
                            ImU32 outline,
                            float w,
                            bool fastOutlines,
                            const OutlineGlowParams* glow,
                            Args&&... args)
{
    static_assert(
        std::is_invocable_v<decltype(EffectFn),
                            ImDrawList*,
                            ImFont*,
                            float,
                            const ImVec2&,
                            const char*,
                            Args...>,
        "EffectFn must accept (ImDrawList*, ImFont*, float, const ImVec2&, const char*, Args...)");
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
    DrawOutline(list, font, size, pos, text, outline, w, fastOutlines);
    EffectFn(list, font, size, pos, text, std::forward<Args>(args)...);
}

/**
 * @fn void AddTextOutline4(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 col, ImU32 outline, float w, bool fastOutlines, const OutlineGlowParams*
 *     glow = nullptr)
 * @brief Draw text in a solid color with a surrounding outline for readability.
 * @author Alex (<https://github.com/lextpf>)
 *
 * fastOutlines selects four cardinal stamps or an 8..24-tap ring.
 *
 * @param col Main text color (ImU32).
 * @param outline Outline color (typically black for contrast).
 * @param w Outline width in pixels.
 * @param fastOutlines If true, stamps the 4 cardinal offsets; otherwise stamps a ring of 8 to 24
 * taps, with the tap count scaled to the circumference.
 * @param glow Optional outline glow parameters. A null pointer, or one whose enabled flag is false,
 * draws no glow. The rings are drawn before the outline.
 */
void AddTextOutline4(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 col,
                     ImU32 outline,
                     float w,
                     bool fastOutlines,
                     const OutlineGlowParams* glow = nullptr);

/**
 * @fn void AddTextHorizontalGradient(ImDrawList* list, ImFont* font, float size, const ImVec2& pos,
 *     const char* text, ImU32 colLeft, ImU32 colRight)
 * @brief Draw text with a left-to-right gradient and no outline.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param colLeft Color at left edge of text. A string narrower than 1e-3 px is filled with this
 * color alone, because the interpolation has no range.
 * @param colRight Color at right edge of text.
 */
void AddTextHorizontalGradient(ImDrawList* list,
                               ImFont* font,
                               float size,
                               const ImVec2& pos,
                               const char* text,
                               ImU32 colLeft,
                               ImU32 colRight);

/**
 * @fn void AddTextVerticalGradient(ImDrawList* list, ImFont* font, float size, const ImVec2& pos,
 *     const char* text, ImU32 top, ImU32 bottom)
 * @brief Draw text with a top-to-bottom gradient and no outline.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param top Color at top of text.
 * @param bottom Color at bottom of text.
 */
void AddTextVerticalGradient(ImDrawList* list,
                             ImFont* font,
                             float size,
                             const ImVec2& pos,
                             const char* text,
                             ImU32 top,
                             ImU32 bottom);

/**
 * @fn void AddTextDiagonalGradient(ImDrawList* list, ImFont* font, float size, const ImVec2& pos,
 *     const char* text, ImU32 a, ImU32 b, ImVec2 dir)
 * @brief Draw text with a gradient along an arbitrary direction vector.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param a Start color of gradient.
 * @param b End color of gradient.
 * @param dir Gradient direction; normalized internally. A near-zero vector (|dir| < 1e-3) falls
 * back to (1, 0), which gives a horizontal gradient.
 */
void AddTextDiagonalGradient(ImDrawList* list,
                             ImFont* font,
                             float size,
                             const ImVec2& pos,
                             const char* text,
                             ImU32 a,
                             ImU32 b,
                             ImVec2 dir);

/**
 * @fn void AddTextRadialGradient(ImDrawList* list, ImFont* font, float size, const ImVec2& pos,
 *     const char* text, ImU32 colCenter, ImU32 colEdge, float gamma = 1.0f, ImVec2* overrideCenter
 *     = nullptr)
 * @brief Draw text with a radial gradient from the center of the text bounds outward.
 * @author Alex (<https://github.com/lextpf>)
 *
 * D is distance from the center; r_max reaches the furthest bounding-box corner.
 * A gamma below 1 expands the edge color inward; above 1 expands the center color outward.
 *
 * $$t = \left(\frac{d}{r_{max}}\right)^\gamma$$
 *
 * @param colCenter Center color (at center point).
 * @param colEdge Edge color (at maximum radius).
 * @param gamma Gamma exponent for the falloff curve; exactly 1.0 is linear and skips the pow call.
 * @param overrideCenter Optional center point, in screen pixels. Null uses the center of the text
 * bounding box. The center may sit outside that box; the ratio $d / r_{max}$ is clamped to [0, 1]
 * before the exponent, so the far side saturates at the edge color instead of overshooting.
 */
void AddTextRadialGradient(ImDrawList* list,
                           ImFont* font,
                           float size,
                           const ImVec2& pos,
                           const char* text,
                           ImU32 colCenter,
                           ImU32 colEdge,
                           float gamma = 1.0f,
                           ImVec2* overrideCenter = nullptr);

/**
 * @fn void AddTextEmber(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 colA, ImU32 colB, float speed, float intensity)
 * @brief Draw text with a flickering ember heat effect.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Sine layers and glyph phases drive a bottom-hot heat field between charcoal and
 * amber-white (255, 214, 140). The highlight keeps colA alpha.
 *
 * @param colA Left base gradient color; the molten highlight adopts its alpha.
 * @param colB Right base gradient color.
 * @param speed Flicker animation speed multiplier.
 * @param intensity Scales the heat field, in the range 0 to 1. The field is signed around 0.32, so
 * a low value does not neutralize the effect: it holds the text at the charcoal pole.
 */
void AddTextEmber(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float speed,
                  float intensity);

/**
 * @fn void AddTextShimmer(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 baseL, ImU32 baseR, ImU32 highlight, float phase01, float bandWidth01,
 *     float strength01 = 1.0f)
 * @brief Draw text with a highlight band that sweeps across it horizontally.
 * @author Alex (<https://github.com/lextpf>)
 *
 * T and v are normalized string coordinates; p is phase. The band wraps horizontally.
 * Its core uses half-width max(.85 * bandWidth01, .01), strength01 and top bias
 * 1 + .12 * (1 - v), plus halos .18 * exp(-6d^2) and .06 * exp(-2d^2).
 * The sum scales by .85 and clamps to [0, 1]. The resting face darkens by up to 22%.
 *
 * $$d = \min(|t - p|,\ |t - p + 1|,\ |t - p - 1|)$$
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Highlight color for the shimmer band; it adopts the fill's alpha.
 * @param phase01 Animation phase in [0, 1] controlling band position.
 * @param bandWidth01 Band half-width as a fraction of text width; scaled by 0.85.
 * @param strength01 Highlight intensity [0, 1].
 */
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
                    float strength01 = 1.0f);

/**
 * @fn void AddTextAurora(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 colA, ImU32 colB, float speed, float waves, float intensity, float sway)
 * @brief Draw text with animated aurora color transitions.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The curtain interpolates dusk -> colA at 1/3 -> colB at 2/3 -> a bright crest.
 * This extends contrast beyond the tier color pair.
 *
 * @param colA First aurora color; the 1/3 stop of the ramp.
 * @param colB Second aurora color; the 2/3 stop, and the source of the white crest.
 * @param speed Animation speed multiplier.
 * @param waves Number of wave cycles across text width.
 * @param intensity Color blend intensity. It scales the ramp position, so a low value holds the
 * text near the dusk pole instead of neutralizing the effect.
 * @param sway Horizontal sway amount for curtain effect.
 */
void AddTextAurora(ImDrawList* list,
                   ImFont* font,
                   float size,
                   const ImVec2& pos,
                   const char* text,
                   ImU32 colA,
                   ImU32 colB,
                   float speed,
                   float waves,
                   float intensity,
                   float sway);

/**
 * @fn void AddTextSparkle(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 baseL, ImU32 baseR, ImU32 sparkleColor, float density, float speed, float
 *     intensity)
 * @brief Draw text with twinkling star highlights across its surface.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Hashed grid positions drive four layers: slow stars, sparkles, dust and rare flares.
 * The resting face darkens by 12% to give glints contrast.
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param sparkleColor Sparkle highlight color. It adopts the fill's alpha, so a glint does not make
 * the glyph transparent where it brightens.
 * @param density Sparkle density [0, 1] (higher = more sparkles). It lowers the hash threshold of
 * the first three layers only; the rare flare layer uses a fixed threshold.
 * @param speed Twinkle animation speed.
 * @param intensity Sparkle brightness multiplier; the summed glint is capped at 0.95.
 */
void AddTextSparkle(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    ImU32 sparkleColor,
                    float density,
                    float speed,
                    float intensity);

/**
 * @fn void AddTextEnchant(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 colA, ImU32 colB, float speed, float scale, float intensity)
 * @brief Draw text with a flowing energy pattern driven by fractal Brownian motion.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Two offset noise layers drive deep shade -> colA -> palette midpoint.
 * Above .52, the field adds a bright filament derived from colB.
 *
 * @param colA First energy color; the middle stop of the weave.
 * @param colB Second energy color. The weave never reaches it as a fill; it contributes through the
 * colA/colB midpoint and as the source of the filament.
 * @param speed Animation speed multiplier.
 * @param scale Noise scale (higher = finer detail).
 * @param intensity Color blend intensity [0, 1]. It scales the weave position and the filament
 * together, so 0 holds the text at the deep fold shade.
 */
void AddTextEnchant(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 colA,
                    ImU32 colB,
                    float speed,
                    float scale,
                    float intensity);

/**
 * @fn void AddTextFrost(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 colA, ImU32 colB, float density, float speed, float sparkleIntensity)
 * @brief Draw text with a crystalline frost pattern plus sparkle flashes.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Frost targets icy blue-white (220, 235, 255) with colA alpha. The glaze blends by
 * at most 50%, followed by flashes of at most 90%, retaining the base gradient.
 *
 * @param colA Left end of the base gradient; the ice color adopts its alpha.
 * @param colB Right end of the base gradient.
 * @param density Frost coverage [0, 1] (higher = more ice). It also lowers the threshold of the
 * medium sparkle layer; the rare flash layer uses a fixed threshold.
 * @param speed Animation speed multiplier.
 * @param sparkleIntensity Brightness of sparkle flashes [0, 1].
 */
void AddTextFrost(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float density,
                  float speed,
                  float sparkleIntensity);

/**
 * @fn void AddTextBreathe(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 baseL, ImU32 baseR, float speed, float amplitude)
 * @brief Draw text with a slow uniform brightness pulse.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A shared sine moves between a deep shade and a bright highlight around the base gradient.
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Pulse frequency in Hz.
 * @param amplitude Fraction of the deep-shade to hot-highlight range travelled, with about 2x gain
 * on the dip and 1.7x on the lift (default 0.16, floored at 0.10).
 */
void AddTextBreathe(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    float speed,
                    float amplitude);

/**
 * @fn void AddTextDrift(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 baseL, ImU32 baseR, float speed, float hueRangeDeg)
 * @brief Draw text with a slow uniform hue wander.
 * @author Alex (<https://github.com/lextpf>)
 *
 * HSV hue follows a shared sine. Saturation varies +/-18% and value +/-7%,
 * at offsets of .25 and .60 turns.
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Drift frequency in Hz.
 * @param hueRangeDeg Peak hue deviation in degrees, applied as +/-hueRangeDeg (default 26).
 */
void AddTextDrift(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 baseL,
                  ImU32 baseR,
                  float speed,
                  float hueRangeDeg);

/**
 * @fn void AddTextMote(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 baseL, ImU32 baseR, ImU32 moteColor, float period, float peakAlpha)
 * @brief Draw text with a single rare twinkling mote.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Hash the period index to select one drawn glyph. A bell envelope and gaussian radius
 * keep one mote visible on ink.
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param moteColor Color of the twinkle itself (usually highlight); it adopts the fill's alpha, so
 * the glyph does not thin out where it brightens.
 * @param period Seconds between twinkles; floored at 0.1 s.
 * @param peakAlpha Peak intensity of the mote blend in [0, 1].
 */
void AddTextMote(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 baseL,
                 ImU32 baseR,
                 ImU32 moteColor,
                 float period,
                 float peakAlpha);

/**
 * @fn void AddTextWander(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 baseL, ImU32 baseR, float speed, float amplitude, float spread)
 * @brief Draw text with per-character asynchronous breathing.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Use AddTextBreathe with a phase hashed from each drawn glyph index.
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Pulse frequency in Hz.
 * @param amplitude Brightness swing per glyph in [0, 1].
 * @param spread Phase desync; clamped to >= 0 only (0 = all glyphs in phase, 1 = one full turn of
 * desync; values above 1 wrap).
 * @note Per-glyph grouping assumes ImGui's 4-vertices-per-glyph quad layout, as do AddTextMote and
 * AddTextElectric.
 */
void AddTextWander(ImDrawList* list,
                   ImFont* font,
                   float size,
                   const ImVec2& pos,
                   const char* text,
                   ImU32 baseL,
                   ImU32 baseR,
                   float speed,
                   float amplitude,
                   float spread);

/**
 * @fn void AddTextEclipse(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 baseL, ImU32 baseR, ImU32 highlight, float phase01, float bandWidth01,
 *     float strength01)
 * @brief Draw text with a sweeping shadow band and a hot leading rim.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Rim color (usually tier highlight); it adopts the fill's alpha.
 * @param phase01 Band position in [0, 1) (wraps).
 * @param bandWidth01 Band half-width as a fraction of text width; scaled by 0.8.
 * @param strength01 Overall effect strength in [0, 1].
 */
void AddTextEclipse(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    ImU32 highlight,
                    float phase01,
                    float bandWidth01,
                    float strength01);

/**
 * @fn void AddTextPulse(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 baseL, ImU32 baseR, ImU32 highlight, float rateHz, float amplitude)
 * @brief Draw text with a two-beat heartbeat glow.
 * @author Alex (<https://github.com/lextpf>)
 *
 * One beat -> a softer beat -> rest. Glow falls off from the string center.
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Bright pole of the beat (usually tier highlight); it adopts the fill's alpha.
 * @param rateHz Heartbeat cycle frequency in Hz; floored at 0.05.
 * @param amplitude Swing depth in [0, 1].
 */
void AddTextPulse(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 baseL,
                  ImU32 baseR,
                  ImU32 highlight,
                  float rateHz,
                  float amplitude);

/**
 * @fn void AddTextElectric(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 baseL, ImU32 baseR, ImU32 highlight, float rateHz, float intensity)
 * @brief Draw text with rare crackling arc sweeps.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A hashed front sweeps during the first 38% of each cycle; static holds between strikes.
 *
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Bright pole of the strike (usually tier highlight); it adopts the fill's alpha.
 * @param rateHz Strike cycle frequency in Hz, floored at 0.02 (0.18 = a strike every ~5.5 s, of
 * which the first ~2.1 s is the sweep).
 * @param intensity Strike brightness in [0, 1].
 */
void AddTextElectric(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 baseL,
                     ImU32 baseR,
                     ImU32 highlight,
                     float rateHz,
                     float intensity);

/**
 * @fn void AddTextShineOverlay(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, float intensity, float falloff, ImU32 shineColor)
 * @brief Draw a static top-edge shine over rendered text.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Shine spans the top 45% of the whole string bounds; short glyphs can receive none.
 * Composite the overlay with alpha-over blending.
 *
 * @param intensity   Strength from zero through one. The function scales it by 0.40, attenuates it
 * near the horizontal edges, and shapes it with `pow(x, 1.35)`.
 * @param falloff Vertical falloff control. The exponent is `max(0.5, falloff) * 3.4`.
 * @param shineColor  Highlight tint.
 */
void AddTextShineOverlay(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         float intensity,
                         float falloff,
                         ImU32 shineColor);

/**
 * @fn void AddTextGlow(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const char*
 *     text, ImU32 glowColor, float radius, float intensity, int samples)
 * @brief Draw a soft multi-layer bloom behind text.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Layers use radii 1.5x, 1.0x and .6x, with increasing alpha toward the center.
 * Samples selects the quality level in the table. Per-copy alpha assumes additive
 * blending; alpha-over produces a dimmer result.
 *
 * | Samples   | Layers drawn         | Copies per layer | AddText calls |
 * |-----------|----------------------|------------------|---------------|
 * | 4 or less | outer                | 4                | 4             |
 * | 5 to 8    | outer, middle        | 8                | 16            |
 * | above 8   | outer, middle, inner | 8                | 24            |
 *
 * @param glowColor Glow color (alpha will be modulated).
 * @param radius Glow spread radius in pixels.
 * @param intensity Glow brightness [0, 1].
 * @param samples Quality selector; only the ranges <= 4, 5-8 and > 8 differ.
 * @pre Should be called **before** drawing the main text.
 * @note No-op when `list`, @p font or @p text is null, when the string is empty, when @p radius is
 * 0 or less, when @p intensity is 0.01 or less, or when `glowColor`'s alpha is below 5. A layer
 * whose solved per-copy alpha rounds below 1 is skipped.
 */
void AddTextGlow(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 glowColor,
                 float radius,
                 float intensity,
                 int samples);

/**
 * @fn void AddTextSoftShadow(ImDrawList* list, ImFont* font, float size, const ImVec2& pos, const
 *     char* text, ImU32 shadowColor, float dirX, float dirY, float distance, float softness, float
 *     opacity, int samples)
 * @brief Draw a soft, directional drop-shadow behind text, with no background plate.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Sample a disc at pos + dir * distance with one central tap and a golden-angle spiral.
 * Per-sample alpha assumes alpha-over compositing.
 *
 * @param shadowColor Shadow tint; RGB is used as-is, alpha is the per-frame fade.
 * @param dirX Shadow cast direction X (cos of the angle; +x = right).
 * @param dirY Shadow cast direction Y (sin of the angle; +y = down).
 * @param distance Offset distance along the direction in pixels.
 * @param softness feather/blur radius of the disc in pixels.
 * @param opacity Master opacity multiplier [0, 1] applied to the shadow alpha.
 * @param samples Number of feather samples (1-24, clamped); higher is smoother.
 * @pre Should be called **before** drawing the outline and main text.
 * @note Collapses to a single crisp offset copy when @p samples <= 1 or @p softness <= 0.01; in
 * that collapsed path the copy is dropped as well when its alpha rounds below 3. No-op when @p
 * opacity <= 0.01, when `shadowColor`'s alpha is below 4, or when the feathered path's solved
 * per-sample alpha rounds below 1.
 */
void AddTextSoftShadow(ImDrawList* list,
                       ImFont* font,
                       float size,
                       const ImVec2& pos,
                       const char* text,
                       ImU32 shadowColor,
                       float dirX,
                       float dirY,
                       float distance,
                       float softness,
                       float opacity,
                       int samples);

/**
 * @struct ParticleAuraParams
 * @brief Parameters for DrawParticleAura.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Opacity <= .05 skips drawing. Multiple styles divide opacity by
 * 1 + .05 * (enabledStyleCount - 1) to limit brightness where their bands overlap.
 */
struct ParticleAuraParams
{
    ImDrawList* list;               ///< ImGui draw list to render to
    ImVec2 center;                  ///< Center of particle region
    float radiusX;                  ///< Horizontal spread radius in pixels
    float radiusY;                  ///< Vertical spread radius in pixels
    ImU32 color;                    ///< Particle base color
    float alpha;                    ///< Opacity from 0 to 1, before the multi-style adjustment.
    Settings::ParticleStyle style;  ///< Particle visual style
    int particleCount;              ///< Particle count; 0 or less renders nothing
    float particleSize;             ///< Base particle size in pixels
    float speed;                    ///< Animation speed multiplier applied to time
    float time;                     ///< Animation clock in seconds
    int styleIndex = 0;             ///< Index among enabled styles; picks this style's band
    int enabledStyleCount = 1;      ///< Total enabled particle styles; divides alpha
    /// Missing textures always fall back to procedural shapes.
    bool useParticleTextures = true;
    /// 0=additive, 1=screen, 2=alpha; a motion recipe can override the mode.
    int blendMode = 0;
    ImU32 colorSecondary = 0;     ///< Optional second gradient color (0 = use primary only)
    float depthStrength = .7f;    ///< Scales the 3D depth read (size/alpha/parallax)
    float colorWarmth = .5f;      ///< Scales warm/cool depth temperature mix + apex pulse
    float glowStrength = .35f;    ///< Additive backlight halo alpha multiplier (0 disables)
    float glowSize = 2.2f;        ///< Halo radius as a multiple of the crisp sprite size
    float shineThreshold = .84f;  ///< Sine threshold for the rare specular glint
};

/**
 * @fn void DrawParticleAura(const ParticleAuraParams& params)
 * @brief Draw an aura of animated particles around a text region.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Dedicated renderers and shared motion recipes are listed below. Orbiters use tilted
 * ellipses with depth-scaled size and alpha. Smoke and aurora do not orbit. Snow uses
 * the shared orbit recipe; its dedicated renderer is a fallback. No recipe selects arch::zap.
 *
 * Deterministic hashes keep traits stable across frames. Multiple styles occupy distinct
 * radial bands. Horizontal flipbooks use a stateless, speed-scaled clock and per-particle
 * phase. Frame count is width / height for exact horizontal strips; static sprites use 0.
 *
 * | Style         | Motion                                                     |
 * |---------------|------------------------------------------------------------|
 * | firefly       | Tilted orbit + incommensurate wander, blinking             |
 * | dust          | Slow tilted orbit + tiny drift, twinkling                  |
 * | mote          | Tilted orbit + brownian wander, breathing glow             |
 * | wisp          | Tilted orbit + serpentine weave, tangent echo trail        |
 * | spark         | Tilted orbit + periodic outward ember flare, tangent trail |
 * | leaf          | Tilted orbit + in/out radius drift, fluttering tumble      |
 * | CherryBlossom | Tilted orbit + gentle bob, slow continuous spin            |
 * | snow          | Falling path in the dedicated fallback                     |
 * | smoke         | Rises, expands, dissolves (non-orbiting)                   |
 * | aurora        | Horizontal flowing light curtain (non-orbiting)            |
 *
 * | Archetype | Styles                                                       |
 * |-----------|--------------------------------------------------------------|
 * | orbit     | Arcane, enchant, gem, hex, curse, void, vortex, fairy, runes |
 * | orbit     | Bat (swoops), butterfly (bob), constellation + moon, planet  |
 * | orbit     | Pollen (wide sway), pixiedust (sprinkle), ash (flutter),     |
 * | orbit     | Zap, snow, coin (spinning), ink                              |
 * | rise      | Bubble (pops at top), heart, soul, steam, zzz, ember         |
 * | fall      | Confetti                                                     |
 * | flow      | Wind (mid band), fog + sand (lower band)                     |
 * | twinkle   | Glitter (anchored sparkle field)                             |
 *
 * @param params Particle aura parameters.
 * @note Returns without drawing when params.list is null, when params.alpha is 0.05 or less, or
 * when params.particleCount is 0 or less. A style with neither a dedicated renderer nor a motion
 * recipe falls back to the firefly wanderer.
 * @see ParticleAuraParams, Settings::ParticleStyle, Settings::particle
 */
void DrawParticleAura(const ParticleAuraParams& params);
}  // namespace TextEffects
