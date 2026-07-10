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

    // The band creates its own contrast: the resting face sits a shade below
    // the base so the sweep reads as light catching a jewel, and the band
    // core blows through the highlight toward white-hot. Amplitude is real
    // now -- the old .30 cap stacked with the strength band into invisibility.
    static constexpr float kAmplitudeCap = .85f;
    static constexpr float kFaceShade = .22f;  ///< resting dip below base
    // .85 (was .65): with shipped bandWidths of .12-.15 the band covered only
    // ~8% of the text -- most frames showed no sweep at all.
    const float bandHalf = (std::max)(bandWidth01 * .85f, .01f);

    const ImU32 deepL = DeepShade(baseL, .60f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .60f, 1.25f);
    // Adopt the fill's alpha: the raw highlight carries the low effectAlpha,
    // which would make the band transparent where it brightens.
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .85f), baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Wrap-around distance so the band smoothly exits right and enters left
        const float d =
            (std::min)(std::abs(t - phase01),
                       (std::min)(std::abs(t - phase01 + 1.0f), std::abs(t - phase01 - 1.0f)));

        // Primary shimmer band with soft quintic falloff
        float h = (d < bandHalf) ? 1.0f - SmoothStep(d / bandHalf) : .0f;

        // Subtle vertical bias toward the upper half of each glyph
        float verticalBoost = 1.0f + (1.0f - v) * .12f;
        h = h * strength01 * verticalBoost;

        // Secondary soft glow halo around the band
        float glow = std::exp(-d * d * 6.0f) * .18f * strength01;

        // Tertiary wide ambient glow for luxury feel
        float ambient = std::exp(-d * d * 2.0f) * .06f * strength01;

        h = Saturate((h + glow + ambient) * kAmplitudeCap);

        // Resting face: a gentle dip toward the deep shade, released as the
        // band approaches so the sweep carries a real luminance swing.
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

    // Living fire needs both poles: charcoal troughs that cool DOWN from the
    // base and molten peaks that blow past it. The old +40/255 warm tint at a
    // .24 cap moved pixels ~4% -- invisible.
    static constexpr float kAmplitudeCap = .70f;

    // Charcoal pole: deep, warm-shifted shade of the base pair.
    const ImU32 charcoalL = DeepShade(colA, .45f, 1.45f);
    const ImU32 charcoalR = DeepShade(colB, .45f, 1.45f);
    // Molten pole: hot amber-white regardless of tier hue -- fire is fire.
    const int aA = (colA >> IM_COL32_A_SHIFT) & 0xFF;
    const ImU32 molten = IM_COL32(255, 214, 140, aA);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(colA, colB, nx);
        ImU32 charcoal = LerpColorU32(charcoalL, charcoalR, nx);

        // Multiple noise layers for organic ember flicker
        float n1 = std::sin(nx * 5.0f + time * 1.2f + ny * 3.0f) * .5f + .5f;
        float n2 = std::sin(nx * 8.0f - time * 2.0f + ny * 5.0f) * .5f + .5f;
        float n3 = std::sin(nx * 12.0f + time * 3.5f - ny * 2.0f) * .5f + .5f;

        // Combine with different weights for organic feel
        float ember = n1 * .5f + n2 * .3f + n3 * .2f;

        // Vertical heat: the glyph base runs hotter, like coals under flame.
        float heatGrad = .80f + .20f * ny;

        // Per-character licking flicker (1.5x, was 4x -- the one fast term in
        // an otherwise slow burn; at shipped speeds it crossed 0.3 Hz)
        float charFlicker = std::sin(time * 1.5f + nx * 20.0f) * .5f + .5f;
        charFlicker = charFlicker * charFlicker * .12f;

        // Signed heat field: below the midline the glyph cools toward
        // charcoal, above it glows toward molten. Both directions visible.
        // Midline at .32 (was .45): at the low end of the strength band the
        // heat field never crossed .45, so Ember spent whole tiers with zero
        // molten phase -- fire that never glowed.
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

    // Uniform swell across the whole text. Bright bases have no upward
    // headroom, so the cycle dips INTO a deep shade and lifts back through
    // the base to a slight overshoot -- an asymmetric breath (2/3 of the
    // swing is the dip) that reads on any palette.
    const float time = (float)ImGui::GetTime();
    const float wave = std::sin(TWO_PI * time * speed);

    const ImU32 deepL = DeepShade(baseL, .55f, 1.30f);
    const ImU32 deepR = DeepShade(baseR, .55f, 1.30f);
    const ImU32 hotL = HotHighlight(baseL, .55f);
    const ImU32 hotR = HotHighlight(baseR, .55f);
    // amplitude [0,1] maps to the fraction of the full deep<->hot range used.
    // Near-balanced gains (was 2.2 dip / 1.1 lift): a darkening-only breath
    // reads as "nothing happening"; the lift toward the hot pole is what the
    // eye actually registers as a swell.
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

    // One mote per period. periodIdx selects a stable GLYPH; phase drives a
    // single bell envelope. The old version hashed a position over the whole
    // bounding box -- spaces, gaps, and the empty band above the x-height --
    // so many periods recolored nothing at all.
    const float time = (float)ImGui::GetTime();
    const float safePeriod = (std::max)(period, .1f);
    const float periodIdx = std::floor(time / safePeriod);
    const float phase = Saturate((time / safePeriod) - periodIdx);

    // Single bell: peaks at 1.0 mid-period, a soft ~1s glint, never a pop.
    const float envelope = 4.0f * phase * (1.0f - phase);

    // Land on ink: pick a glyph, center the mote on its quad (4 verts/glyph).
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

    // Mote radius scaled to text height so the glint covers roughly one glyph
    // and change (wide enough to actually catch the eye when it fires).
    const float moteRadius = (std::max)(s.height() * .55f, 1.0f);
    const float radius2 = moteRadius * moteRadius;

    // The glint must stay opaque: adopt the fill's alpha, not the highlight's
    // low effectAlpha (which would dim the glyph as it "brightens").
    const ImU32 glint = WithAlphaFrom(moteColor, baseL);

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

    // Per-character breathing: each glyph has its own phase so the text
    // feels alive without any single point pulling focus. Four vertices per
    // glyph is the ImGui glyph-quad convention. Each glyph swings around its
    // base: DOWN into a deep shade on one half of its cycle and UP toward a
    // hot lift on the other -- candlelight both shadows and catches letters.
    // (The old dip-only version never brightened, and a darkening-only choir
    // read as flat text.)
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

        // Signed swing per glyph; amplitude scales how far it travels.
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

    // Inverse shimmer: a soft SHADOW crosses the text with a hot rim on its
    // leading edge -- an eclipse passing over a jewel. Darkening always has
    // headroom on bright bases, so this reads on every tier palette.
    const float bandHalf = (std::max)(bandWidth01 * .8f, .01f);
    const ImU32 deepL = DeepShade(baseL, .40f, 1.40f);
    const ImU32 deepR = DeepShade(baseR, .40f, 1.40f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .90f), baseL);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Signed wrap-around offset from the band center in [-.5, .5).
        float d = t - phase01;
        d -= std::floor(d + .5f);

        // Shadow body: smooth dark bell over the band.
        float shadow = std::exp(-(d * d) / (bandHalf * bandHalf * .5f));

        // Hot rim on the LEADING edge only (just ahead of the shadow).
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

    // Weighty two-beat heartbeat: lub, softer dub, long rest. The glow blooms
    // from the center of the text outward and the rest state sits slightly
    // shaded, so both beats carry a real swing.
    const float time = (float)ImGui::GetTime();
    const float safeRate = (std::max)(rateHz, .05f);
    const float ph = Frac(time * safeRate);

    auto beat = [](float ph, float center, float width)
    {
        const float d = (ph - center) / width;
        return std::exp(-d * d);
    };
    // Beat widths .09/.10 (were .045/.05): at shipped rates the old ~105 ms
    // gaussian transients read as staccato blinking; twice the width keeps
    // the two-beat cadence but each beat swells and releases.
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

        // Radial falloff from the text center so the beat blooms outward.
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

    // Rare crackling arc: every cycle a sharp bright front sweeps the text
    // left-to-right in the first quarter of the period, jittered by a
    // per-cycle hash so no two strikes trace the same path; the rest of the
    // cycle holds a calm, faintly-shaded face with whisper static. Fully
    // stateless -- same discipline as the particle zap.
    const float time = (float)ImGui::GetTime();
    const float safeRate = (std::max)(rateHz, .02f);
    const float cyc = time * safeRate;
    const float cycIdx = std::floor(cyc);
    const float ph = cyc - cycIdx;

    const ImU32 deepL = DeepShade(baseL, .60f, 1.25f);
    const ImU32 deepR = DeepShade(baseR, .60f, 1.25f);
    const ImU32 hot = WithAlphaFrom(HotHighlight(highlight, .95f), baseL);

    // The strike occupies the first 38% of the cycle (was 28% -- a slower,
    // more deliberate sweep for the calm register).
    const bool striking = ph < .38f;
    const float frontX = striking ? (ph / .38f) * 1.2f - .1f : -1.0f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        const float v = s.normalizedY(list->VtxBuffer[i].pos.y);
        ImU32 base = LerpColorU32(baseL, baseR, t);
        ImU32 deep = LerpColorU32(deepL, deepR, t);

        // Calm face with faint per-glyph static between strikes.
        const int charIdx = (i - s.vtxStart) / 4;
        const float staticFlick = SubtleHash01(static_cast<size_t>(charIdx) * 31ULL +
                                               static_cast<size_t>(cycIdx) * 131ULL) *
                                  .10f;
        ImU32 col = LerpColorU32(base, deep, .10f + staticFlick);

        if (striking)
        {
            // Jittered vertical path: the front is not a straight bar.
            const float jitter = (SubtleHash01(static_cast<size_t>(cycIdx) * 977ULL +
                                               static_cast<size_t>(v * 7.0f) * 53ULL) -
                                  .5f) *
                                 .08f;
            const float d = t - (frontX + jitter);
            // White-hot core with a warm afterglow trailing behind. Core width
            // 300 (was 900): each glyph now glows for a beat as the front
            // passes instead of a harsh ~50 ms flash.
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

    // Extract shine color RGB, we'll modulate alpha per-vertex
    const int sr = (shineColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int sg = (shineColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int sb = (shineColor >> IM_COL32_B_SHIFT) & 0xFF;
    const float shapedFalloff = (std::max)(0.5f, falloff) * 3.4f;
    // .10 rendered the shine at ~0.5/255 alpha with the shipped INI -- zero
    // after rounding. .40 puts a real lacquer rim on the cap height.
    const float intensityScale = .40f;

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
