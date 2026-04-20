#include "TextEffectsInternal.h"

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

    // Whisper-subtle amplitude: cap peak highlight contribution so the band
    // reads as a gentle gloss instead of a sweeping flashlight.
    static constexpr float kAmplitudeCap = .35f;
    const float bandHalf = (std::max)(bandWidth01 * .65f, .01f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        // Wrap-around distance so the band smoothly exits right and enters left
        const float d =
            (std::min)(std::abs(t - phase01),
                       (std::min)(std::abs(t - phase01 + 1.0f), std::abs(t - phase01 - 1.0f)));

        // Primary shimmer band with soft quintic falloff
        float h = (d < bandHalf) ? 1.0f - SmoothStep(d / bandHalf) : .0f;

        // Subtle vertical bias toward the upper half of each glyph
        float verticalBoost = 1.0f + (1.0f - v) * .12f;
        h = h * strength01 * verticalBoost;

        // Secondary soft glow halo around the band (dialled down)
        float glow = std::exp(-d * d * 6.0f) * .08f * strength01;

        // Tertiary wide ambient glow for luxury feel (dialled down)
        float ambient = std::exp(-d * d * 2.0f) * .03f * strength01;

        h = Saturate((h + glow + ambient) * kAmplitudeCap);

        list->VtxBuffer[i].col = LerpColorU32(base, highlight, h);
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

    // Whisper-subtle amplitude: ember should glow, not strobe.
    static constexpr float kAmplitudeCap = .30f;

    // Warm highlight color (gentler tint than the original golden-orange pop)
    int rA = (colA >> IM_COL32_R_SHIFT) & 0xFF;
    int gA = (colA >> IM_COL32_G_SHIFT) & 0xFF;
    int bA = (colA >> IM_COL32_B_SHIFT) & 0xFF;
    ImU32 warmHighlight =
        IM_COL32((std::min)(255, rA + 40), (std::min)(255, gA + 14), (std::max)(0, bA - 18), 255);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(colA, colB, nx);

        // Multiple noise layers for organic ember flicker
        float n1 = std::sin(nx * 5.0f + time * 1.2f + ny * 3.0f) * .5f + .5f;
        float n2 = std::sin(nx * 8.0f - time * 2.0f + ny * 5.0f) * .5f + .5f;
        float n3 = std::sin(nx * 12.0f + time * 3.5f - ny * 2.0f) * .5f + .5f;

        // Combine with different weights for organic feel
        float ember = n1 * .5f + n2 * .3f + n3 * .2f;

        // Sharpen the ember peaks but less aggressively than before
        ember = ember * ember;

        // Gentler vertical heat: bottom still brighter but not dramatically so
        float heatGrad = .88f + .12f * ny;

        // Per-character phase offset from X for varied flicker (dialled down)
        float charFlicker = std::sin(time * 4.0f + nx * 20.0f) * .5f + .5f;
        charFlicker = charFlicker * charFlicker * .05f;

        float glow = Saturate((ember * heatGrad + charFlicker) * intensity) * kAmplitudeCap;

        // Blend base toward warm highlight at bright spots
        list->VtxBuffer[i].col = LerpColorU32(base, warmHighlight, glow);
    }
}

// Golden-ratio scrambling hash for deterministic [0, 1) values from an integer seed.
// Used by Mote and Wander to pick stable positions / phase offsets without the
// FBM / Hash helpers from TextEffectsComplex.cpp.
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

    // Uniform sinusoidal brightness modulation across the whole text.
    // At default amplitude = 0.06 and speed = 0.25 Hz this produces a +/-6%
    // swell over a four-second period.
    const float time = (float)ImGui::GetTime();
    const float mod = 1.0f + amplitude * std::sin(TWO_PI * time * speed);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        list->VtxBuffer[i].col = ScaleRGB(base, mod);
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

    // One mote per period. periodIdx selects a stable position; phase drives
    // a bell-curve envelope peaking mid-period.
    const float time = (float)ImGui::GetTime();
    const float safePeriod = (std::max)(period, .1f);
    const float periodIdx = std::floor(time / safePeriod);
    const float phase = Saturate((time / safePeriod) - periodIdx);

    // Bell envelope: 4 * phase * (1 - phase) peaks at 1.0 when phase = 0.5.
    // Squared for a sharper fade-in / fade-out that spends less time at peak.
    float envelope = 4.0f * phase * (1.0f - phase);
    envelope = envelope * envelope;

    // Stable pseudo-random position within the bounding box.
    const size_t seed = static_cast<size_t>(periodIdx);
    const float normX = SubtleHash01(seed * 0x1f1f1f1fULL + 1ULL);
    const float normY = SubtleHash01(seed * 0x27d4eb2dULL + 2ULL);
    const float moteX = s.bbMin.x + normX * s.width();
    const float moteY = s.bbMin.y + normY * s.height();

    // Mote radius scaled to text height so the glint covers roughly one glyph.
    const float moteRadius = (std::max)(s.height() * .35f, 1.0f);
    const float radius2 = moteRadius * moteRadius;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float t = s.normalizedX(p.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const float dx = p.x - moteX;
        const float dy = p.y - moteY;
        const float d2 = dx * dx + dy * dy;
        // Gaussian falloff; exp(-(d^2) / (r^2 / 3)) keeps the mote tight.
        const float falloff = std::exp(-d2 * 3.0f / radius2);

        const float intensity = Saturate(peakAlpha * envelope * falloff);
        list->VtxBuffer[i].col = LerpColorU32(base, moteColor, intensity);
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

    // Per-character breathing: each glyph has its own phase so the text
    // feels alive without any single point pulling focus. Four vertices per
    // glyph is the ImGui glyph-quad convention.
    const float time = (float)ImGui::GetTime();
    const float clampedSpread = (std::max)(spread, .0f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const int charIdx = (i - s.vtxStart) / 4;
        const float phaseOffset =
            SubtleHash01(static_cast<size_t>(charIdx) + 1ULL) * TWO_PI * clampedSpread;

        const float mod = 1.0f + amplitude * std::sin(TWO_PI * time * speed + phaseOffset);
        list->VtxBuffer[i].col = ScaleRGB(base, mod);
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

    // Extract shine color RGB, we'll modulate alpha per-vertex
    const int sr = (shineColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int sg = (shineColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int sb = (shineColor >> IM_COL32_B_SHIFT) & 0xFF;
    const float shapedFalloff = (std::max)(0.5f, falloff) * 3.4f;
    const float intensityScale = .12f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);

        // Keep the shine tight to the top edge so it reads as a highlight
        // instead of washing over the entire glyph face.
        const float topBand = Saturate(1.0f - v * 2.2f);
        float shine = std::pow(topBand, shapedFalloff) * intensity * intensityScale;

        // Slight horizontal variation for natural feel
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
