// Fields span the whole string bounds. Blank and clipped characters emit no quads,
// so (i - vtxStart) / 4 indexes drawn glyphs, not string positions.

#include "TextEffectsInternal.hpp"

namespace TextEffects
{

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

    // Shade the resting face to keep the sweep visible on bright palettes.
    static constexpr float kAmplitudeCap = .85f;
    static constexpr float kFaceShade = .22f;  // Resting dip below base
    // core width = 2 * .85 * bandWidth01; .01 keeps a degenerate half-width visible.
    const float bandHalf = (std::max)(bandWidth01 * .85f, .01f);

    const ImU32 deepL = DeepShade(baseL, .60f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .60f, 1.25f);
    // Keep fill alpha; highlight alpha would make bright regions transparent.
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .85f), baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Wrap the band from right to left.
        const float d =
            (std::min)(std::abs(t - phase01),
                       (std::min)(std::abs(t - phase01 + 1.0f), std::abs(t - phase01 - 1.0f)));

        float h = (d < bandHalf) ? 1.0f - SmoothStep(d / bandHalf) : .0f;

        // V spans the whole string height.
        float verticalBoost = 1.0f + (1.0f - v) * .12f;
        h = h * strength01 * verticalBoost;

        float glow = std::exp(-d * d * 6.0f) * .18f * strength01;

        float ambient = std::exp(-d * d * 2.0f) * .06f * strength01;

        h = Saturate((h + glow + ambient) * kAmplitudeCap);

        ImU32 face = LerpColorU32(base, deep, kFaceShade * strength01 * (1.0f - h));
        list->VtxBuffer[i].col = LerpColorU32(face, hot, h);
    }
}

void AddTextEmber(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float speed,
                  float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;

    static constexpr float kAmplitudeCap = .70f;

    const ImU32 charcoalL = DeepShade(colA, .45f, 1.45f);
    const ImU32 charcoalR = DeepShade(colB, .45f, 1.45f);
    // Fixed amber-white keeps the fire hue independent of the tier.
    const int aA = (colA >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 molten = IM_COL32(255, 214, 140, aA);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(colA, colB, nx);
        ImU32 charcoal = LerpColorU32(charcoalL, charcoalR, nx);

        float n1 = std::sin(nx * 5.0f + time * 1.2f + ny * 3.0f) * .5f + .5f;
        float n2 = std::sin(nx * 8.0f - time * 2.0f + ny * 5.0f) * .5f + .5f;
        float n3 = std::sin(nx * 12.0f + time * 3.5f - ny * 2.0f) * .5f + .5f;

        float ember = n1 * .5f + n2 * .3f + n3 * .2f;

        float heatGrad = .80f + .20f * ny;

        float charFlicker = std::sin(time * 1.5f + nx * 20.0f) * .5f + .5f;
        charFlicker = charFlicker * charFlicker * .12f;

        // The .32 midline lets low-strength tiers reach the molten pole.
        float heat = Saturate(ember * heatGrad + charFlicker) * intensity;
        float signedHeat = (heat - .32f) * 2.0f * kAmplitudeCap;

        if (signedHeat >= .0f)
        {
            float up = SmoothStep(Saturate(signedHeat));
            list->VtxBuffer[i].col = LerpColorU32(base, molten, up);
        }
        else
        {
            float down = SmoothStep(Saturate(-signedHeat));
            list->VtxBuffer[i].col = LerpColorU32(base, charcoal, down * .8f);
        }
    }
}

/**
 * @fn float SubtleHash01(size_t seed)
 * @brief Generate stable variation for glyph selection and animation phase.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return A deterministic value in [0, 1).
 */
static inline float SubtleHash01(size_t seed)
{
    seed ^= seed >> 16;
    seed *= 0x9e3779b97f4a7c15ULL;
    seed ^= seed >> 13;
    seed *= 0xc2b2ae35ULL;
    seed ^= seed >> 16;
    return static_cast<float>(seed & 0xFFFFFF) / 16777216.0f;
}

void AddTextBreathe(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 baseL,
                    ImU32 baseR,
                    float speed,
                    float amplitude)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    const float wave = std::sin(TWO_PI * time * speed);

    const ImU32 deepL = DeepShade(baseL, .55f, 1.30f);
    const ImU32 deepR = DeepShade(baseR, .55f, 1.30f);
    const ImU32 hotL = HotHighlight(baseL, .55f);
    const ImU32 hotR = HotHighlight(baseR, .55f);
    // Amplitude [0, 1] selects a fraction of the deep-to-hot range.
    const float dip = Saturate(amplitude * 2.0f) * Saturate(-wave);
    const float lift = Saturate(amplitude * 1.7f) * Saturate(wave);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        if (dip > .0f)
        {
            ImU32 deep = LerpColorU32(deepL, deepR, t);
            list->VtxBuffer[i].col = LerpColorU32(base, deep, SmoothStep(dip));
        }
        else
        {
            ImU32 hot = LerpColorU32(hotL, hotR, t);
            list->VtxBuffer[i].col = LerpColorU32(base, hot, SmoothStep(lift));
        }
    }
}

void AddTextMote(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 baseL,
                 ImU32 baseR,
                 ImU32 moteColor,
                 float period,
                 float peakAlpha)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Choose a drawn glyph; a point in the whole string bounds can land outside ink.
    const float time = (float)ImGui::GetTime();
    const float safePeriod = (std::max)(period, .1f);
    const float periodIdx = std::floor(time / safePeriod);
    const float phase = Saturate((time / safePeriod) - periodIdx);

    const float envelope = 4.0f * phase * (1.0f - phase);

    const int charCount = (s.vtxEnd - s.vtxStart) / 4;
    if (charCount <= 0)
    {
        return;
    }
    const size_t seed = static_cast<size_t>(periodIdx);
    const int litChar =
        static_cast<int>(SubtleHash01(seed * 0x1f1f1f1fULL + 1ULL) * (float)charCount) % charCount;
    float moteX = .0f, moteY = .0f;
    for (int k = 0; k < 4; ++k)
    {
        const ImVec2& q = list->VtxBuffer[s.vtxStart + litChar * 4 + k].pos;
        moteX += q.x * .25f;
        moteY += q.y * .25f;
    }

    const float moteRadius = (std::max)(s.height() * .55f, 1.0f);
    const float radius2 = moteRadius * moteRadius;

    const ImU32 glint = WithAlphaFrom(moteColor, baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float t = s.normalizedX(p.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const float dx = p.x - moteX;
        const float dy = p.y - moteY;
        const float d2 = dx * dx + dy * dy;
        // Gaussian falloff: exp(-(d^2) / (r^2 / 3)).
        const float falloff = std::exp(-d2 * 3.0f / radius2);

        const float intensity = Saturate(peakAlpha * envelope * falloff);
        list->VtxBuffer[i].col = LerpColorU32(base, glint, intensity);
    }
}

void AddTextWander(ImDrawList* list,
                   ImFont* font,
                   float size,
                   const ImVec2& pos,
                   const char* text,
                   ImU32 baseL,
                   ImU32 baseR,
                   float speed,
                   float amplitude,
                   float spread)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Four vertices form one drawn glyph. Spread = 0 synchronizes all glyphs; 1 spans a turn.
    const float time = (float)ImGui::GetTime();
    const float clampedSpread = (std::max)(spread, .0f);

    const ImU32 deepL = DeepShade(baseL, .58f, 1.30f);
    const ImU32 deepR = DeepShade(baseR, .58f, 1.30f);
    const ImU32 hotL = HotHighlight(baseL, .50f);
    const ImU32 hotR = HotHighlight(baseR, .50f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const int charIdx = (i - s.vtxStart) / 4;
        const float phaseOffset =
            SubtleHash01(static_cast<size_t>(charIdx) + 1ULL) * TWO_PI * clampedSpread;

        const float wave = std::sin(TWO_PI * time * speed + phaseOffset);
        if (wave >= .0f)
        {
            ImU32 deep = LerpColorU32(deepL, deepR, t);
            const float dip = wave * Saturate(amplitude * 2.2f);
            list->VtxBuffer[i].col = LerpColorU32(base, deep, SmoothStep(dip));
        }
        else
        {
            ImU32 hot = LerpColorU32(hotL, hotR, t);
            const float lift = -wave * Saturate(amplitude * 1.8f);
            list->VtxBuffer[i].col = LerpColorU32(base, hot, SmoothStep(lift));
        }
    }
}

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
                    float strength01)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float bandHalf = (std::max)(bandWidth01 * .8f, .01f);
    const ImU32 deepL = DeepShade(baseL, .40f, 1.40f);
    const ImU32 deepR = DeepShade(baseR, .40f, 1.40f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .90f), baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Signed wrapped offset in [-.5, .5).
        float d = t - phase01;
        d -= std::floor(d + .5f);

        float shadow = std::exp(-(d * d) / (bandHalf * bandHalf * .5f));

        // D > 0 is ahead of the shadow as phase01 advances.
        float rimD = d - bandHalf * 1.1f;
        float rim = std::exp(-(rimD * rimD) / (bandHalf * bandHalf * .06f));

        ImU32 shadowed = LerpColorU32(base, deep, Saturate(shadow * .70f * strength01));
        list->VtxBuffer[i].col = LerpColorU32(shadowed, hot, Saturate(rim * .85f * strength01));
    }
}

void AddTextPulse(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 baseL,
                  ImU32 baseR,
                  ImU32 highlight,
                  float rateHz,
                  float amplitude)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Two-beat cycle: one beat, a softer second, then a long rest. The glow
    // blooms from the center of the text outward and the rest state sits
    // slightly shaded, so both beats carry a real swing.
    //
    // Envelope over one cycle, which lasts 1 / rateHz seconds:
    //
    //   Phase   0     .12       .34                                1
    //           |______|_________|_________________________________|
    //                beat 1   beat 2 (55%)      rest
    //
    // The envelope is above ~0.1 only up to phase .47, so about half of every
    // cycle is rest. That gap is what makes the pair of beats readable.
    const float time = (float)ImGui::GetTime();
    const float safeRate = (std::max)(rateHz, .05f);
    const float ph = Frac(time * safeRate);

    auto beat = [](float ph, float center, float width)
    {
        const float d = (ph - center) / width;
        return std::exp(-d * d);
    };
    // Beat widths are cycle fractions, so duration scales with rateHz.
    const float envelope = Saturate(beat(ph, .12f, .09f) + .55f * beat(ph, .34f, .10f));

    const ImU32 deepL = DeepShade(baseL, .62f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .62f, 1.25f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .70f), baseL);
    const float amp = Saturate(amplitude);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        const float centerDist = std::abs(t - .5f) * 2.0f;
        const float bloom = 1.0f - centerDist * .45f;

        ImU32 rest = LerpColorU32(base, deep, .18f * amp * (1.0f - envelope));
        list->VtxBuffer[i].col = LerpColorU32(rest, hot, Saturate(envelope * bloom * amp));
    }
}

void AddTextElectric(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 baseL,
                     ImU32 baseR,
                     ImU32 highlight,
                     float rateHz,
                     float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    const float safeRate = (std::max)(rateHz, .02f);
    const float cyc = time * safeRate;
    const float cycIdx = std::floor(cyc);
    const float ph = cyc - cycIdx;

    const ImU32 deepL = DeepShade(baseL, .60f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .60f, 1.25f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .95f), baseL);

    const bool striking = ph < .38f;
    const float frontX = striking ? (ph / .38f) * 1.2f - .1f : -1.0f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Reseed static once per cycle so the face holds still between strikes.
        const int charIdx = (i - s.vtxStart) / 4;
        const float staticFlick = SubtleHash01(static_cast<size_t>(charIdx) * 31ULL +
                                               static_cast<size_t>(cycIdx) * 131ULL) *
                                  .10f;
        ImU32 col = LerpColorU32(base, deep, .10f + staticFlick);

        if (striking)
        {
            const float jitter = (SubtleHash01(static_cast<size_t>(cycIdx) * 977ULL +
                                               static_cast<size_t>(v * 7.0f) * 53ULL) -
                                  .5f) *
                                 .08f;
            const float d = t - (frontX + jitter);
            // Core half-width = 1/sqrt(300), about 6% of the string.
            const float core = std::exp(-d * d * 300.0f);
            const float tail = (d < .0f) ? std::exp(d * 9.0f) * .35f : .0f;
            col = LerpColorU32(col, hot, Saturate((core + tail) * intensity));
        }

        list->VtxBuffer[i].col = col;
    }
}

void AddTextShineOverlay(ImDrawList* list,
                         ImFont* font,
                         float size,
                         const ImVec2& pos,
                         const char* text,
                         float intensity,
                         float falloff,
                         ImU32 shineColor)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Draw a separate alpha-over copy after the fill; only vertex alpha varies.
    const int sr = (shineColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int sg = (shineColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int sb = (shineColor >> IM_COL32_B_SHIFT) & 0xFF;
    const float shapedFalloff = (std::max)(0.5f, falloff) * 3.4f;
    // The .40 scale keeps rim alpha above integer rounding at low INI strengths.
    const float intensityScale = .40f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);

        // topBand reaches 0 at v = 1/2.2; short glyphs can sit below the shine band.
        const float topBand = Saturate(1.0f - v * 2.2f);
        float shine = std::pow(topBand, shapedFalloff) * intensity * intensityScale;

        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        float h = 2.0f * t - 1.0f;
        float hMod = 1.0f - .45f * h * h;
        shine *= hMod;
        shine = std::pow(Saturate(shine), 1.35f);

        const int a = (int)std::clamp(shine * 255.0f, .0f, 255.0f);
        list->VtxBuffer[i].col = IM_COL32(sr, sg, sb, a);
    }
}

}  // namespace TextEffects
