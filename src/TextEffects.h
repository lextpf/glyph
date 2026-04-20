#pragma once

#include "PCH.h"
#include "Settings.h"

#include <algorithm>

/**
 * @namespace TextEffects
 * @brief Collection of text rendering effects for ImGui.
 * @author Alex (https://github.com/lextpf)
 * @ingroup TextEffects
 *
 * Provides functions for rendering text with various visual effects using ImGui's
 * draw list API. Effects are achieved by manipulating per-vertex colors after
 * text is rendered to the draw list.
 *
 * ## :material-palette-swatch-variant: Effect Categories
 *
 * - **Gradients**: Horizontal, Vertical, Diagonal, Radial
 * - **Animated**: Shimmer, Ember, Aurora, Breathe, Mote, Wander
 * - **Complex**: Sparkle, Enchant, Frost, Drift
 * - **Utility**: Outline, Glow
 *
 * ## :material-sort-variant: Rendering Order
 *
 * 1. Glow (if enabled) - soft bloom behind text
 * 2. Shadow - offset dark copy
 * 3. Outline - 4-directional (FastOutlines) or 8-directional border
 * 4. Main text - with gradient/effect colors
 *
 * @see Settings::EffectType, Settings::EffectParams
 */
namespace TextEffects
{
/**
 * Clamp value to [0, 1] range.
 *
 * @param x Input value to clamp.
 *
 * @return Value clamped to [0.0, 1.0].
 */
constexpr float Saturate(float x)
{
    return std::clamp(x, .0f, 1.0f);
}

/**
 * Quintic smoothstep (smootherstep).
 *
 * Applies Ken Perlin's improved smoothstep with C2 continuity
 * (smooth first and second derivatives at boundaries).
 *
 * $$\text{smoothstep}(t) = 6t^5 - 15t^4 + 10t^3$$
 *
 * @param t Normalized input value (clamped to [0, 1]).
 *
 * @return Smoothly interpolated value in [0, 1].
 */
constexpr float SmoothStep(float t)
{
    t = Saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/**
 * Linearly interpolate between two packed colors.
 *
 * Interpolates each RGBA channel independently using linear interpolation:
 * $$C_{out} = C_a + (C_b - C_a) \cdot t = C_a(1-t) + C_b \cdot t$$
 *
 * Where $t \in [0, 1]$, $t=0$ returns color $a$, $t=1$ returns color $b$.
 *
 * @param a First color (ImU32 packed ABGR format).
 * @param b Second color (ImU32 packed ABGR format).
 * @param t Interpolation factor [0, 1]. Values outside range are clamped.
 *
 * @return Interpolated color as ImU32.
 *
 * @see Saturate
 */
ImU32 LerpColorU32(ImU32 a, ImU32 b, float t);

/// Parameters for the white outline glow (back halo behind outlines).
struct OutlineGlowParams
{
    bool enabled = false;  ///< Whether to draw glow rings
    ImU32 color = 0;       ///< Glow color (typically white with alpha)
    float scale = 1.6f;    ///< Glow radius as multiplier of outline width
    float alpha = .20f;    ///< Peak glow ring opacity
    int rings = 2;         ///< Number of concentric rings (1-3)
};

/**
 * Draw outline around text position (4-dir or 8-dir based on fastOutlines).
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param outline Outline color.
 * @param w Outline width in pixels.
 * @param fastOutlines If true, uses 4 cardinal directions; otherwise 8 directions.
 */
void DrawOutline(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 outline,
                 float w,
                 bool fastOutlines);

/// Parameters for shine overlay effect (static top-edge highlight).
struct ShineParams
{
    bool enabled = false;
    float intensity = .35f;       ///< Peak brightness at top edge
    float falloff = 2.0f;         ///< Vertical falloff exponent
    float textGlowAlpha = .0f;    ///< Text body translucency 0-1 (0=opaque, 1=fully see-through)
    float innerTextAlpha = 1.0f;  ///< Text body alpha multiplier 0-1 (applied after effects)
};

/// Parameters for wave displacement effect.
struct WaveParams
{
    bool enabled = false;
    float amplitude = 1.5f;
    float frequency = 3.0f;
    float speed = 1.0f;
    float time = .0f;
};

/**
 * Apply sine-wave Y displacement to vertices in the given range.
 * Call after text has been rendered and vertex-colored by an effect.
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
 * Draw directional inner outline tinted with tier color.
 * Sits between the outer outline and the main text for depth.
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
 * Draw concentric glow rings behind text outline.
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
 * Generic outline wrapper: draws outline then delegates to any effect function.
 *
 * Replaces the boilerplate `AddTextOutline4<Effect>` wrappers. Usage:
 * @code
 * WithOutline<AddTextHorizontalGradient>(list, font, size, pos, text,
 *                                        outline, w, fastOutlines, colLeft, colRight);
 * @endcode
 *
 * @tparam EffectFn Pointer to the non-outline effect function.
 * @tparam Args     Additional effect-specific argument types.
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

/// Parameters for dual-tone directional inner outline (forward declaration for template).
struct DualOutlineParams
{
    bool enabled = false;
    ImU32 tierColor = 0;       ///< Tier color to blend toward
    float innerScale = .5f;    ///< Inner outline width as fraction of outer
    float tintFactor = .3f;    ///< Blend toward tier color (0=outline, 1=tier)
    float alphaFactor = .5f;   ///< Inner outline opacity
    float lightAngle = 315.f;  ///< Light direction in degrees
    float lightBias = .15f;    ///< Directional width variation
};

/// WithOutline variant that also draws outline glow behind the outline.
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
 * Draw text with outline.
 *
 * Despite the `4` in the name (a historical artifact), this function supports
 * both 4-direction and 8-direction outlines based on the `fastOutlines` parameter.
 *
 * Renders text with a solid color and surrounding outline for readability.
 * When `fastOutlines` is true, uses 4 cardinal directions only;
 * otherwise draws in 8 directions (cardinal + diagonal).
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param col Main text color (ImU32).
 * @param outline Outline color (typically black for contrast).
 * @param w Outline width in pixels.
 * @param fastOutlines If true, uses 4-direction outlines; otherwise 8-direction.
 * @param glow Optional outline glow parameters (nullptr = no glow).
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
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
 * Draw text with horizontal gradient (no outline).
 *
 * Colors transition from left to right across the entire text width.
 * Each vertex is colored based on its X position within the text bounds.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colLeft Color at left edge of text.
 * @param colRight Color at right edge of text.
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
 */
void AddTextHorizontalGradient(ImDrawList* list,
                               ImFont* font,
                               float size,
                               const ImVec2& pos,
                               const char* text,
                               ImU32 colLeft,
                               ImU32 colRight);

/**
 * Draw text with vertical gradient (no outline).
 *
 * Colors transition from top to bottom across the text height.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param top Color at top of text.
 * @param bottom Color at bottom of text.
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
 */
void AddTextVerticalGradient(ImDrawList* list,
                             ImFont* font,
                             float size,
                             const ImVec2& pos,
                             const char* text,
                             ImU32 top,
                             ImU32 bottom);

/**
 * Draw text with diagonal gradient.
 *
 * Colors transition along an arbitrary direction vector.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param a Start color of gradient.
 * @param b End color of gradient.
 * @param dir Gradient direction vector (should be normalized).
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
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
 * Draw text with radial gradient (center to edge).
 *
 * Colors radiate outward from the center of the text bounds.
 * The interpolation factor is computed from normalized distance:
 * $$t = \left(\frac{d}{r_{max}}\right)^\gamma$$
 *
 * Where $d$ is distance from center, $r_{max}$ is the maximum radius.
 * - $\gamma < 1$: faster falloff near center (more center color visible)
 * - $\gamma > 1$: slower falloff near center (more edge color visible)
 * - $\gamma = 1$: linear falloff
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colCenter Center color (at center point).
 * @param colEdge Edge color (at maximum radius).
 * @param gamma Gamma exponent for falloff curve (1.0 = linear).
 * @param overrideCenter Optional custom center point (nullptr = auto).
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
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
 * Draw text with warm ember/fire flickering effect.
 *
 * Creates an organic heat glow using multiple overlapping sine-noise layers
 * with per-character phase variation.  Bright spots use a warm highlight
 * derived from the first color, and a vertical heat gradient makes the
 * bottom of the text appear hotter.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA First gradient color (warm base).
 * @param colB Second gradient color (cool base).
 * @param speed Flicker animation speed multiplier.
 * @param intensity Brightness of the warm highlights [0, 1].
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
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
 * Draw text with shimmer effect (moving highlight band).
 *
 * A bright highlight band sweeps across the text horizontally.
 * Band intensity uses a Gaussian-like profile via quintic smoothstep:
 * $$I(x) = \text{smoothstep}\left(1 - \frac{|x - x_{band}|}{w/2}\right) \cdot strength$$
 *
 * Where $x_{band} = phase \cdot width_{text}$ is the band center position.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param highlight Highlight color for the shimmer band.
 * @param phase01 Animation phase [0, 1] controlling band position.
 * @param bandWidth01 Width of band as fraction of text width [0, 1].
 * @param strength01 Highlight intensity [0, 1].
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
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
 * Draw text with animated aurora/northern lights effect.
 *
 * Creates flowing, wave-like color transitions reminiscent of aurora borealis.
 * Multiple sine waves combine to create organic, curtain-like movement.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA First aurora color.
 * @param colB Second aurora color.
 * @param speed Animation speed multiplier.
 * @param waves Number of wave cycles across text width.
 * @param intensity Color blend intensity.
 * @param sway Horizontal sway amount for curtain effect.
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
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
 * Draw text with sparkle/glitter effect.
 *
 * Creates twinkling star highlights across the text surface.
 * Ideal for ice/frost, magical, or precious metal effects.
 *
 * Uses hash-based pseudo-random sparkle positions with sine-wave
 * brightness modulation at different phases per sparkle.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param sparkleColor Sparkle highlight color.
 * @param density Sparkle density [0, 1] (higher = more sparkles).
 * @param speed Twinkle animation speed.
 * @param intensity Sparkle brightness multiplier.
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 *
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
 * Draw text with flowing magical energy effect.
 *
 * Uses Fractal Brownian Motion (FBM) noise to create smooth, organic
 * energy patterns that flow across the text surface.  Two offset noise
 * layers combine for a richer, non-repeating flow.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA First energy color.
 * @param colB Second energy color.
 * @param speed Animation speed multiplier.
 * @param scale Noise scale (higher = finer detail).
 * @param intensity Color blend intensity [0, 1].
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
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
 * Draw text with crystalline frost pattern and sparkle flashes.
 *
 * Combines a creeping frost overlay (hash-based crystalline noise)
 * with twinkling sparkle highlights for an icy appearance.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param colA First frost color.
 * @param colB Second frost color.
 * @param density Frost coverage [0, 1] (higher = more ice).
 * @param speed Animation speed multiplier.
 * @param sparkleIntensity Brightness of sparkle flashes [0, 1].
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
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
 * Draw text with a slow uniform brightness pulse (Breathe).
 *
 * Multiplies every vertex's RGB by a shared sinusoidal modulation so the text
 * gently brightens and dims together. The effect is intentionally subtle:
 * default amplitude of 0.06 yields a +/-6% swell, and default speed of 0.25 Hz
 * completes one cycle every four seconds.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Pulse frequency in Hz.
 * @param amplitude Brightness swing in [0, 1] (0.06 = +/-6%).
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
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
 * Draw text with a slow uniform hue wander (Drift).
 *
 * Converts each vertex's color to HSV, shifts the hue by a shared
 * sinusoidal offset, and converts back. Saturation and value are preserved so
 * the only motion is in color family -- text drifts through neighbouring hues
 * and returns. Default speed 0.08 Hz completes one cycle over ~12 seconds.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Drift frequency in Hz.
 * @param hueRangeDeg Peak hue deviation in degrees (default 8 = +/-4 degrees).
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
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
 * Draw text with a single rare twinkling mote.
 *
 * Once per `period` seconds a deterministic position within the text bounds is
 * chosen by hashing the period index; a soft bell-curve envelope fades the
 * mote in and out within that period, and a Gaussian radial falloff keeps the
 * bright spot roughly one glyph wide. Exactly one mote is visible at a time.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param moteColor Color of the twinkle itself (usually highlight).
 * @param period Seconds between twinkles.
 * @param peakAlpha Peak intensity of the mote blend in [0, 1].
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
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
 * Draw text with per-character asynchronous breathing (Wander).
 *
 * Like Breathe, but each glyph carries its own phase offset hashed from its
 * character index. The result is a subtle "choir" effect where every glyph
 * pulses gently at its own rhythm; no single point commands attention.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param baseL Left base gradient color.
 * @param baseR Right base gradient color.
 * @param speed Pulse frequency in Hz.
 * @param amplitude Brightness swing per glyph in [0, 1].
 * @param spread Phase desync in [0, 1] (0 = all glyphs in phase, 1 = full random).
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
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

/// Draw a static top-edge shine overlay on top of already-rendered text.
/// Simulates overhead lighting by brightening vertices near the top of
/// each glyph. Intended to be rendered with additive blending.
/// @param intensity Peak brightness at top edge [0, 1].
/// @param falloff   Vertical falloff exponent (higher = sharper highlight edge).
/// @param shineColor Highlight tint color (typically white).
void AddTextShineOverlay(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         float intensity,
                         float falloff,
                         ImU32 shineColor);

/**
 * Draw soft glow/bloom effect behind text.
 *
 * Creates a multi-layer soft bloom by drawing offset copies of the text
 * at multiple radii with per-layer alpha multipliers. Three concentric
 * layers (outer, middle, inner) produce a smooth falloff:
 *
 * - **Outer** (1.5x radius, lowest alpha): wide ambient glow
 * - **Middle** (1.0x radius, medium alpha): primary bloom
 * - **Inner** (0.6x radius, highest alpha): bright core halo
 *
 * Each layer draws `samples` copies around a circle at its radius.
 *
 * @param list ImGui draw list to render to.
 * @param font Font to use for rendering.
 * @param size Font size in pixels.
 * @param pos Top-left position for text.
 * @param text Null-terminated UTF-8 string to render.
 * @param glowColor Glow color (alpha will be modulated).
 * @param radius Glow spread radius in pixels.
 * @param intensity Glow brightness [0, 1].
 * @param samples Number of blur samples (8-16 recommended).
 *
 * @pre list != nullptr
 * @pre font != nullptr
 * @pre text != nullptr
 * @pre Should be called **before** drawing the main text.
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

/// Parameters for DrawParticleAura.
struct ParticleAuraParams
{
    ImDrawList* list;                 ///< ImGui draw list to render to
    ImVec2 center;                    ///< Center of particle region
    float radiusX;                    ///< Horizontal spread radius in pixels
    float radiusY;                    ///< Vertical spread radius in pixels
    ImU32 color;                      ///< Particle base color
    float alpha;                      ///< Maximum particle opacity [0, 1]
    Settings::ParticleStyle style;    ///< Particle visual style
    int particleCount;                ///< Number of particles to draw
    float particleSize;               ///< Base particle size in pixels
    float speed;                      ///< Animation speed multiplier
    float time;                       ///< Current time for animation
    int styleIndex = 0;               ///< Index among enabled styles (for spatial offset)
    int enabledStyleCount = 1;        ///< Total number of enabled particle styles
    bool useParticleTextures = true;  ///< Use texture sprites instead of procedural shapes
    int blendMode = 0;                ///< 0=Additive, 1=Screen, 2=Alpha
    ImU32 colorSecondary = 0;         ///< Optional second gradient color (0 = use primary only)
};

/**
 * Draw floating particle aura around a text region.
 *
 * Creates an aura of animated particles around the nameplate.
 * Multiple visual styles are available (see ParticleAuraParams).
 *
 * @param params Particle aura parameters.
 *
 * @pre params.list != nullptr
 *
 * @see ParticleAuraParams, Settings::ParticleStyle, Settings::Particle
 */
void DrawParticleAura(const ParticleAuraParams& params);
}  // namespace TextEffects
