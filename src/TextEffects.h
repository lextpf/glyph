#pragma once

#include "PCH.h"
#include "Settings.h"

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
 * - **Animated**: Shimmer, Pulse, RainbowWave, ConicRainbow
 * - **Complex**: Aurora, Sparkle, Plasma, Scanline
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
    float Saturate(float x);

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
    float SmoothStep(float t);

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

    /**
     * Draw text with outline.
     *
     * Renders text with a solid color and surrounding outline for readability.
     * When `Settings::FastOutlines` is true, uses 4 cardinal directions only;
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
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 col, ImU32 outline, float w);

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
     * @see AddTextOutline4Gradient
     */
    void AddTextHorizontalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colLeft, ImU32 colRight);

    /**
     * Draw text with horizontal gradient and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param colLeft Left gradient color.
     * @param colRight Right gradient color.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextHorizontalGradient, AddTextOutline4
     */
    void AddTextOutline4Gradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colLeft, ImU32 colRight, ImU32 outline, float w);

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
     * @see AddTextOutline4VerticalGradient
     */
    void AddTextVerticalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 top, ImU32 bottom);

    /**
     * Draw text with vertical gradient and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param top Top gradient color.
     * @param bottom Bottom gradient color.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4VerticalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 top, ImU32 bottom, ImU32 outline, float w);

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
     * @see AddTextOutline4DiagonalGradient
     */
    void AddTextDiagonalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, ImVec2 dir);

    /**
     * Draw text with diagonal gradient and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param a Start gradient color.
     * @param b End gradient color.
     * @param dir Gradient direction vector (normalized).
     * @param outline Outline color.
     * @param w Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4DiagonalGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, ImVec2 dir, ImU32 outline, float w);

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
     * @see AddTextOutline4RadialGradient
     */
    void AddTextRadialGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colCenter, ImU32 colEdge,
        float gamma = 1.0f, ImVec2* overrideCenter = nullptr);

    /**
     * Draw text with radial gradient and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param colCenter Center color.
     * @param colEdge Edge color.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param gamma Gamma correction (1.0 = linear).
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4RadialGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 colCenter, ImU32 colEdge, ImU32 outline, float w,
        float gamma = 1.0f);

    /**
     * Draw text with pulsing brightness.
     *
     * Gradient colors oscillate in brightness over time using sine wave modulation.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param a First gradient color.
     * @param b Second gradient color.
     * @param time Current time in seconds (typically from ImGui::GetTime()).
     * @param freqHz Pulse frequency in Hz (cycles per second).
     * @param amp Pulse amplitude [0, 1] (0 = no pulse, 1 = full range).
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4PulseGradient
     */
    void AddTextPulseGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, float time, float freqHz, float amp);

    /**
     * Draw text with pulsing gradient and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param a First gradient color.
     * @param b Second gradient color.
     * @param time Current time in seconds.
     * @param freqHz Pulse frequency in Hz.
     * @param amp Pulse amplitude [0, 1].
     * @param outline Outline color.
     * @param w Outline width in pixels.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4PulseGradient(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 a, ImU32 b, float time, float freqHz, float amp,
        ImU32 outline, float w);

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
     * @see AddTextOutline4Shimmer, AddTextOutline4ChromaticShimmer
     */
    void AddTextShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight,
        float phase01, float bandWidth01, float strength01 = 1.0f);

    /**
     * Draw text with shimmer effect and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseL Left base gradient color.
     * @param baseR Right base gradient color.
     * @param highlight Highlight color for shimmer band.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param phase01 Animation phase [0, 1].
     * @param bandWidth01 Band width as fraction [0, 1].
     * @param strength01 Highlight intensity [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Shimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight, ImU32 outline, float w,
        float phase01, float bandWidth01, float strength01 = 1.0f);

    /**
     * Draw text with gradient base and shimmer (internal helper).
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseL Left base gradient color.
     * @param baseR Right base gradient color.
     * @param highlight Shimmer highlight color.
     * @param phase01 Animation phase [0, 1].
     * @param bandWidth01 Band width fraction [0, 1].
     * @param strength01 Highlight strength [0, 1].
     */
    void AddTextGradientShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight,
        float phase01, float bandWidth01, float strength01);

    /**
     * Draw text with solid base and shimmer (internal helper).
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param base Solid base color.
     * @param highlight Shimmer highlight color.
     * @param phase01 Animation phase [0, 1].
     * @param bandWidth01 Band width fraction [0, 1].
     * @param strength01 Highlight strength [0, 1].
     */
    void AddTextSolidShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 base, ImU32 highlight,
        float phase01, float bandWidth01, float strength01);

    /**
     * Draw text with chromatic aberration shimmer.
     *
     * High-quality shimmer with RGB channel separation creating a "ghosting" effect.
     * Renders multiple offset layers with color tinting for a premium look.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseL Left base color.
     * @param baseR Right base color.
     * @param highlight Highlight color for shimmer band.
     * @param outline Outline color.
     * @param outlineW Outline width in pixels.
     * @param phase01 Animation phase [0, 1].
     * @param bandWidth01 Highlight band width [0, 1].
     * @param strength01 Effect strength [0, 1].
     * @param splitPx Chromatic separation distance in pixels.
     * @param ghostAlphaMul Alpha multiplier for ghost layers [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Shimmer
     */
    void AddTextOutline4ChromaticShimmer(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 highlight,
        ImU32 outline, float outlineW,
        float phase01, float bandWidth01, float strength01,
        float splitPx, float ghostAlphaMul);

    /**
     * Draw text with animated rainbow wave (no outline).
     *
     * Colors cycle through the hue spectrum with a wave pattern.
     * Uses HSV color space for smooth transitions.
     *
     * Per-vertex hue is calculated as:
     * $$H = H_{base} + \frac{x - x_{min}}{x_{max} - x_{min}} \cdot H_{spread} + t \cdot speed$$
     *
     * HSV to RGB conversion follows standard formulas with $H \in [0,1]$
     * mapped to $[0, 360]$.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseHue Starting hue offset [0, 1] (0 = red, 0.33 = green, 0.66 = blue).
     * @param hueSpread Hue variation across text width [0, 1].
     * @param speed Animation speed multiplier (hue shift per second).
     * @param saturation Color saturation [0, 1] (0 = grayscale, 1 = vivid).
     * @param value Color brightness [0, 1] (0 = black, 1 = bright).
     * @param alpha Transparency [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4RainbowWave
     */
    void AddTextRainbowWave(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float hueSpread, float speed, float saturation, float value, float alpha);

    /**
     * Draw text with rainbow wave effect and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseHue Starting hue [0, 1].
     * @param hueSpread Hue variation [0, 1].
     * @param speed Animation speed multiplier.
     * @param saturation Color saturation [0, 1].
     * @param value Color brightness [0, 1].
     * @param alpha Transparency [0, 1].
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param useWhiteBase If true, draws white base layer for brightness boost.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4RainbowWave(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float hueSpread, float speed, float saturation, float value,
        float alpha, ImU32 outline, float w, bool useWhiteBase = false);

    /**
     * Draw text with conic rainbow (circular hue rotation).
     *
     * Hue rotates around a center point, creating a circular rainbow pattern.
     * Per-vertex hue is computed from the angle to center:
     * $$H = \frac{\text{atan2}(y - c_y, x - c_x)}{2\pi} + H_{base} + t \cdot speed$$
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseHue Starting hue offset [0, 1].
     * @param speed Rotation speed (hue shift per second).
     * @param saturation Color saturation [0, 1].
     * @param value Color brightness [0, 1].
     * @param alpha Transparency [0, 1].
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4ConicRainbow
     */
    void AddTextConicRainbow(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float speed, float saturation, float value, float alpha);

    /**
     * Draw text with conic rainbow and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseHue Starting hue [0, 1].
     * @param speed Rotation speed.
     * @param saturation Color saturation [0, 1].
     * @param value Color brightness [0, 1].
     * @param alpha Transparency [0, 1].
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param useWhiteBase If true, draws white base for brightness.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4ConicRainbow(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        float baseHue, float speed, float saturation, float value, float alpha,
        ImU32 outline, float w, bool useWhiteBase = false);

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
     * @see AddTextOutline4Aurora
     */
    void AddTextAurora(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB,
        float speed, float waves, float intensity, float sway);

    /**
     * Draw text with aurora effect and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param colA First aurora color.
     * @param colB Second aurora color.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param speed Animation speed multiplier.
     * @param waves Number of wave cycles.
     * @param intensity Color blend intensity.
     * @param sway Horizontal sway amount.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Aurora(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB, ImU32 outline, float w,
        float speed, float waves, float intensity, float sway);

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
     * @see AddTextOutline4Sparkle
     */
    void AddTextSparkle(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 sparkleColor,
        float density, float speed, float intensity);

    /**
     * Draw text with sparkle effect and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseL Left base gradient color.
     * @param baseR Right base gradient color.
     * @param sparkleColor Sparkle highlight color.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param density Sparkle density [0, 1].
     * @param speed Twinkle animation speed.
     * @param intensity Sparkle brightness multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Sparkle(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 sparkleColor, ImU32 outline, float w,
        float density, float speed, float intensity);

    /**
     * Draw text with classic plasma effect.
     *
     * Creates demoscene-style plasma using overlapping sine waves.
     * Two wave patterns create interference for organic movement:
     * $$t = \frac{\sin(x \cdot f_1 + time \cdot speed) + \sin(y \cdot f_2 + time \cdot speed)}{2} \cdot 0.5 + 0.5$$
     *
     * The normalized value $t$ drives interpolation between the two colors.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param colA First plasma color.
     * @param colB Second plasma color.
     * @param freq1 First sine wave frequency.
     * @param freq2 Second sine wave frequency.
     * @param speed Animation speed multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Plasma
     */
    void AddTextPlasma(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB,
        float freq1, float freq2, float speed);

    /**
     * Draw text with plasma effect and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param colA First plasma color.
     * @param colB Second plasma color.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param freq1 First sine wave frequency.
     * @param freq2 Second sine wave frequency.
     * @param speed Animation speed multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Plasma(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 colA, ImU32 colB, ImU32 outline, float w,
        float freq1, float freq2, float speed);

    /**
     * Draw text with horizontal scanline effect.
     *
     * Creates a sweeping horizontal highlight bar like a scanner
     * or cyberpunk/Dwemer-style effect.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseL Left base gradient color.
     * @param baseR Right base gradient color.
     * @param scanColor Scanline highlight color.
     * @param speed Scan speed (cycles per second).
     * @param width Scanline width [0, 1] as fraction of text height.
     * @param intensity Scanline brightness multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     *
     * @see AddTextOutline4Scanline
     */
    void AddTextScanline(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 scanColor,
        float speed, float width, float intensity);

    /**
     * Draw text with scanline effect and outline.
     *
     * @param list ImGui draw list to render to.
     * @param font Font to use for rendering.
     * @param size Font size in pixels.
     * @param pos Top-left position for text.
     * @param text Null-terminated UTF-8 string to render.
     * @param baseL Left base gradient color.
     * @param baseR Right base gradient color.
     * @param scanColor Scanline highlight color.
     * @param outline Outline color.
     * @param w Outline width in pixels.
     * @param speed Scan speed (cycles per second).
     * @param width Scanline width [0, 1].
     * @param intensity Scanline brightness multiplier.
     *
     * @pre list != nullptr
     * @pre font != nullptr
     * @pre text != nullptr
     */
    void AddTextOutline4Scanline(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text,
        ImU32 baseL, ImU32 baseR, ImU32 scanColor, ImU32 outline, float w,
        float speed, float width, float intensity);

    /**
     * Draw soft glow/bloom effect behind text.
     *
     * Creates a blurred glow by drawing multiple offset copies of the text
     * in a circular pattern with decreasing alpha.
     *
     * Draws `samples` copies of text at positions around a circle of
     * radius `radius`, with alpha scaled by `intensity / samples`.
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
    void AddTextGlow(ImDrawList* list, ImFont* font, float size,
        const ImVec2& pos, const char* text, ImU32 glowColor,
        float radius, float intensity, int samples);

    /**
     * Draw decorative ornaments on sides of a text region.
     *
     * Renders ornamental characters from the ornament font extending
     * from both sides, suitable for high-tier character names.
     *
     * @param list ImGui draw list to render to.
     * @param center Center point of the text being decorated.
     * @param textWidth Width of the text in pixels.
     * @param textHeight Height of the text in pixels.
     * @param color Ornament color (ImU32).
     * @param alpha Opacity [0, 1].
     * @param scale Size multiplier for ornament dimensions.
     * @param spacing Distance from text edges in pixels.
     * @param animated If true, applies subtle pulse animation.
     * @param time Current time for animation (from ImGui::GetTime()).
     * @param outlineWidth Outline thickness matching text outline.
     * @param enableGlow Enable glow effect matching text glow.
     * @param glowRadius Glow spread radius in pixels.
     * @param glowIntensity Glow brightness [0, 1].
     * @param glowSamples Number of glow samples.
     * @param leftOrnaments Characters to draw on the left side.
     * @param rightOrnaments Characters to draw on the right side.
     * @param ornamentScale Additional scale multiplier (default: 1.0).
     * @param isSpecialTitle If true, uses special title styling.
     *
     * @pre list != nullptr
     *
     * @see Settings::EnableOrnaments
     */
    void DrawSideOrnaments(ImDrawList* list, const ImVec2& center,
        float textWidth, float textHeight, ImU32 color, float alpha,
        float scale, float spacing, bool animated, float time,
        float outlineWidth, bool enableGlow, float glowRadius, float glowIntensity, int glowSamples,
        const std::string& leftOrnaments, const std::string& rightOrnaments,
        float ornamentScale = 1.0f, bool isSpecialTitle = false);

    /**
     * Draw floating particle aura around a text region.
     *
     * Creates an aura of animated particles around the nameplate.
     * Multiple visual styles are available:
     *
     * | Style | Description |
     * |-------|-------------|
     * | Stars | Twinkling star points (classic) |
     * | Sparks | Fast, erratic fire-like sparks |
     * | Wisps | Slow, flowing ethereal wisps |
     * | Runes | Small magical rune symbols |
     * | Orbs | Soft glowing orbs |
     *
     * @param list ImGui draw list to render to.
     * @param center Center of the particle region.
     * @param radiusX Horizontal spread radius in pixels.
     * @param radiusY Vertical spread radius in pixels.
     * @param color Particle base color.
     * @param alpha Maximum particle opacity [0, 1].
     * @param style Particle visual style from Settings::ParticleStyle.
     * @param particleCount Number of particles to draw.
     * @param particleSize Base particle size in pixels.
     * @param speed Animation speed multiplier.
     * @param time Current time for animation.
     * @param styleIndex Index of this style among enabled styles (for spatial offset).
     * @param enabledStyleCount Total number of enabled particle styles.
     *
     * @pre list != nullptr
     *
     * @see Settings::ParticleStyle, Settings::EnableParticleAura
     */
    void DrawParticleAura(ImDrawList* list, const ImVec2& center,
        float radiusX, float radiusY, ImU32 color, float alpha,
        Settings::ParticleStyle style, int particleCount, float particleSize,
        float speed, float time, int styleIndex = 0, int enabledStyleCount = 1);
}
