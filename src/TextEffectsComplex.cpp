#include "TextEffectsInternal.hpp"

namespace TextEffects
{

// Integer hash for pseudo-random noise [0, 1)
static inline float Hash(float x, float y)
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

// ValueNoise and FBMNoise are shared with the particle drift; they now live
// inline in TextEffectsInternal.hpp. The file-local Hash above is kept because
// the crystalline/frost shaders below sample it directly on the integer grid.

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
                   float sway)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;

    // The curtain spans a real luminance range: deep dusk shade -> base pair
    // -> white-hot crest. The tier's own L/R colors are a same-family pair
    // ~15% apart, so a curtain confined to them is invisible by construction.
    ImU32 colDeep = DeepShade(LerpColorU32(colA, colB, .5f), .50f, 1.40f);
    ImU32 colMid = LerpColorU32(colA, colB, .5f);
    ImU32 colBright = HotHighlight(colB, .70f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        // Multiple flowing wave layers for organic aurora movement
        float wave1 = std::sin(nx * waves * TWO_PI + time * 1.2f + ny * 2.0f);
        float wave2 = std::sin(nx * waves * .7f * TWO_PI - time * .8f + ny * 1.5f) * .6f;
        float wave3 = std::sin(nx * waves * 1.3f * TWO_PI + time * .5f - ny * 1.0f) * .4f;

        // Vertical curtain effect
        float curtain = std::sin(ny * TWO_PI * 2.0f + time * .7f + nx * sway * 3.0f);
        curtain = curtain * .5f + .5f;  // Normalize to [0, 1]

        // Combine waves
        float combined = (wave1 + wave2 + wave3) / 2.0f;  // Range roughly [-1, 1]
        combined = combined * .5f + .5f;                  // Normalize to [0, 1]

        // Add subtle shimmer (1.5x, was 4x -- the one fast term in an
        // otherwise slow curtain; at shipped speeds it crossed 0.3 Hz)
        float shimmer = std::sin(time * 1.5f + nx * 12.0f + ny * 8.0f) * .5f + .5f;
        shimmer = shimmer * shimmer * .15f;  // Subtle sparkle

        // Horizontal sway effect
        float swayOffset = std::sin(ny * 3.0f + time * 1.5f) * sway;
        float swayedX = nx + swayOffset;
        float swayFactor = std::sin(swayedX * TWO_PI * waves + time) * .5f + .5f;

        // Full-range curtain: t sweeps deep dusk -> palette -> white crest.
        float t =
            Saturate((combined * .6f + curtain * .25f + swayFactor * .15f) * intensity + shimmer);

        // Continuous gradient across the widened range with smooth handoffs:
        // deep (0) -> A (1/3) -> B (2/3) -> bright crest (1).
        ImU32 low = LerpColorU32(colDeep, colA, SmoothStep(Saturate(t * 3.0f)));
        ImU32 mid = LerpColorU32(low, colB, SmoothStep(Saturate(t * 3.0f - 1.0f)));
        ImU32 finalColor = LerpColorU32(mid, colBright, SmoothStep(Saturate(t * 3.0f - 2.0f)));

        list->VtxBuffer[i].col = finalColor;
    }
}

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
                    float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();

    // Create color variations for richer sparkle. Glint targets adopt the
    // fill's alpha -- the raw highlight carries the low effectAlpha, which
    // would make glyphs transparent exactly where they glint.
    ImU32 sparkleWhite = WithAlphaFrom(IM_COL32(255, 255, 255, 255), baseL);
    ImU32 sparkleBase = WithAlphaFrom(sparkleColor, baseL);
    ImU32 sparkleTint = LerpColorU32(sparkleBase, sparkleWhite, .3f);
    // Resting face endpoints, slightly shaded (see kFaceShade below).
    const ImU32 faceL = LerpColorU32(baseL, DeepShade(baseL, .70f, 1.20f), .12f);
    const ImU32 faceR = LerpColorU32(baseR, DeepShade(baseR, .70f, 1.20f), .12f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);
        float totalSparkle = .0f;
        float colorShift = .0f;  // For varying sparkle color

        // Layer 1: Large slow-twinkling stars (soft threshold)
        float seed1 = Hash(std::floor(p.x * .06f), std::floor(p.y * .06f));
        {
            float thresh1 = 1.0f - density * .4f;
            float mask1 = SmoothStep(Saturate((seed1 - thresh1) / (density * .15f + .01f)));
            float phase1 = seed1 * TWO_PI;
            float sparkleTime1 = time * speed * (.6f + seed1 * .4f);
            float sparkle1 = std::sin(sparkleTime1 + phase1);
            sparkle1 = std::max(.0f, sparkle1);
            sparkle1 = std::pow(sparkle1, 3.0f);

            // Star burst pattern
            float gridX = Frac(p.x * .06f);
            float gridY = Frac(p.y * .06f);
            float distFromCenter =
                std::sqrt((gridX - .5f) * (gridX - .5f) + (gridY - .5f) * (gridY - .5f));
            float starPattern = std::max(.0f, 1.0f - distFromCenter * 3.0f);

            totalSparkle += sparkle1 * starPattern * .9f * mask1;
            colorShift += sparkle1 * .3f * mask1;
        }

        // Layer 2: Medium fast-twinkling sparkles (soft threshold)
        float seed2 = Hash(std::floor(p.x * .12f) + 50.0f, std::floor(p.y * .12f) + 50.0f);
        {
            float thresh2 = 1.0f - density * .7f;
            float mask2 = SmoothStep(Saturate((seed2 - thresh2) / (density * .15f + .01f)));
            float phase2 = seed2 * TWO_PI;
            // .8x layer rate + gentler pow (was 1.8x / pow 5): the medium layer
            // was the main "rapid blinking" read -- glints now swell and fade.
            float sparkleTime2 = time * speed * .8f * (.8f + seed2 * .4f);
            float sparkle2 = std::sin(sparkleTime2 + phase2);
            sparkle2 = std::max(.0f, sparkle2);
            sparkle2 = std::pow(sparkle2, 3.0f);
            totalSparkle += sparkle2 * .6f * mask2;
        }

        // Layer 3: Fine shimmer dust (soft threshold)
        float seed3 = Hash(std::floor(p.x * .2f) + 100.0f, std::floor(p.y * .2f) + 100.0f);
        {
            float thresh3 = 1.0f - density * .9f;
            float mask3 = SmoothStep(Saturate((seed3 - thresh3) / (density * .15f + .01f)));
            float phase3 = seed3 * TWO_PI;
            // 1.0x layer rate + pow 4 (was 2.5x / pow 8): the dust layer's
            // needle flashes exceeded 0.5 Hz at every shipped speed.
            float sparkle3 = std::sin(time * speed * 1.0f + phase3);
            sparkle3 = std::max(.0f, sparkle3);
            sparkle3 = std::pow(sparkle3, 4.0f);
            totalSparkle += sparkle3 * .35f * mask3;
        }

        // Layer 4: Rare brilliant flares (soft threshold)
        float seed4 = Hash(std::floor(p.x * .04f) + 200.0f, std::floor(p.y * .04f) + 200.0f);
        {
            float mask4 = SmoothStep(Saturate((seed4 - .88f) / .1f));
            float phase4 = seed4 * TWO_PI;
            float flare = std::sin(time * speed * .4f + phase4);
            flare = std::max(.0f, flare);
            flare = std::pow(flare, 2.0f);
            totalSparkle += flare * 1.5f * mask4;
            colorShift += flare * .6f * mask4;
        }

        // Glints are spatially tiny, so they can afford to flash near-full --
        // that contrast IS the effect. The face rests slightly shaded (12%
        // toward the deep pole, precomputed above) so each glint has
        // something to flash against.
        static constexpr float kAmplitudeCap = .95f;
        totalSparkle = Saturate(totalSparkle * intensity) * kAmplitudeCap;
        colorShift = Saturate(colorShift);

        ImU32 face = LerpColorU32(faceL, faceR, nx);
        // Blend sparkle color with white based on intensity for brighter sparkles
        ImU32 finalSparkle = LerpColorU32(sparkleBase, sparkleTint, colorShift);
        list->VtxBuffer[i].col = LerpColorU32(face, finalSparkle, totalSparkle);
    }
}

void AddTextEnchant(ImDrawList* list,
                    ImFont* font,
                    float size,
                    const ImVec2& pos,
                    const char* text,
                    ImU32 colA,
                    ImU32 colB,
                    float speed,
                    float scale,
                    float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;
    // Magical fabric with a real luminance weave: deep arcane shade in the
    // folds, palette mid-tones in the body, white-hot filaments at the energy
    // peaks. (The old .26 cap over the tier's ~15%-apart pair was invisible.)
    ImU32 colDeep = DeepShade(LerpColorU32(colA, colB, .5f), .48f, 1.45f);
    ImU32 colMid = LerpColorU32(colA, colB, .5f);
    ImU32 colBright = HotHighlight(colB, .75f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        // FBM noise for smooth, flowing magical energy
        float energy = FBMNoise(nx * scale + time * .3f, ny * scale + time * .15f, 4, .6f);

        // Second offset layer for richer flow
        float energy2 =
            FBMNoise(nx * scale * .7f - time * .2f, ny * scale * 1.3f + time * .25f, 3, .5f);

        // Combine for organic, flowing pattern
        float combined = (energy * .6f + energy2 * .4f);

        // Bright filaments at the energy peaks. Threshold .52 (was .58): the
        // FBM field rarely crested .58 at low strength, so lower tiers showed
        // the weave but never a filament.
        float highlight = SmoothStep(Saturate((combined - .52f) * 3.0f)) * intensity;

        // Full-range weave: deep folds -> palette -> mid.
        float t = Saturate(combined * intensity);
        ImU32 low = LerpColorU32(colDeep, colA, SmoothStep(Saturate(t * 2.4f)));
        ImU32 finalColor = LerpColorU32(low, colMid, SmoothStep(Saturate(t * 2.4f - 1.2f)));

        // White-hot filament pulse on top.
        if (highlight > .0f)
        {
            finalColor = LerpColorU32(finalColor, colBright, Saturate(highlight * .9f));
        }

        list->VtxBuffer[i].col = finalColor;
    }
}

void AddTextFrost(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 colA,
                  ImU32 colB,
                  float density,
                  float speed,
                  float sparkleIntensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    // Icy glaze target adopts the fill's alpha (see WithAlphaFrom).
    const ImU32 iceWhite = WithAlphaFrom(IM_COL32(220, 235, 255, 255), colA);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(colA, colB, nx);

        // Crystalline frost pattern: sharp, high-frequency noise. Scroll rates
        // .1/.07 (were .3/.2): each hash-cell boundary crossing is an instant
        // recolor pop, so the scroll is kept slow enough that pops are rare.
        float frost1 = Hash(std::floor(p.x * .08f + time * speed * .1f), std::floor(p.y * .08f));
        float frost2 = Hash(std::floor(p.x * .15f - time * speed * .07f) + 50.0f,
                            std::floor(p.y * .15f) + 50.0f);

        // Creeping frost animation
        float creep = std::sin(nx * 4.0f + time * speed * .5f) * .5f + .5f;
        float frostPattern = (frost1 * .6f + frost2 * .4f) * creep;

        // Apply density threshold for crystalline look
        float frostThreshold = 1.0f - density;
        float frostMask = SmoothStep(Saturate((frostPattern - frostThreshold + .1f) * 2.0f));

        // Frost reads as a real glaze now: a visible icy film with hard
        // crystalline glints. (The old .20/.40 caps left a ~5% tint.)
        static constexpr float kFrostTintCap = .50f;
        static constexpr float kSparkleCap = .90f;

        // Tint base color toward icy blue-white at frosted areas (gentler)
        ImU32 frosted = LerpColorU32(base, iceWhite, frostMask * kFrostTintCap);

        // Sparkle flashes: bright white points that twinkle
        float sparkle = .0f;

        // Layer 1: Medium sparkles (soft threshold)
        float seed1 = Hash(std::floor(p.x * .1f) + 100.0f, std::floor(p.y * .1f) + 100.0f);
        {
            float fThresh1 = 1.0f - density * .5f;
            float fMask1 = SmoothStep(Saturate((seed1 - fThresh1) / (density * .15f + .01f)));
            float phase = seed1 * TWO_PI;
            // 1.2x + pow 4 (was 2x / pow 6): swell-and-fade, not a blink.
            float s1 = std::sin(time * speed * 1.2f + phase);
            s1 = (std::max)(.0f, s1);
            sparkle += std::pow(s1, 4.0f) * .7f * fMask1;
        }

        // Layer 2: Rare brilliant flash (soft threshold)
        float seed2 = Hash(std::floor(p.x * .05f) + 200.0f, std::floor(p.y * .05f) + 200.0f);
        {
            float fMask2 = SmoothStep(Saturate((seed2 - .85f) / .1f));
            float phase = seed2 * TWO_PI;
            float s2 = std::sin(time * speed * .8f + phase);
            s2 = (std::max)(.0f, s2);
            sparkle += std::pow(s2, 3.0f) * 1.2f * fMask2;
        }

        sparkle = Saturate(sparkle * sparkleIntensity) * kSparkleCap;

        // Blend: frosted base + sparkle highlights
        list->VtxBuffer[i].col = LerpColorU32(frosted, iceWhite, sparkle);
    }
}

void AddTextDrift(ImDrawList* list,
                  ImFont* font,
                  float size,
                  const ImVec2& pos,
                  const char* text,
                  ImU32 baseL,
                  ImU32 baseR,
                  float speed,
                  float hueRangeDeg)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    // Uniform hue wander: the whole text drifts by the same hue offset per
    // frame, so color slowly breathes between the two flanks of the base pair
    // without pattern, band, or character variation. A gentle saturation and
    // value swell rides the same cycle (offset so the peaks interleave):
    // hue-only drift on the low-chroma tier palettes was imperceptible.
    const float time = (float)ImGui::GetTime();
    const float cycle = TWO_PI * time * speed;
    const float hueShift = (hueRangeDeg / 360.0f) * std::sin(cycle);
    const float satSwell = 1.0f + .18f * std::sin(cycle + TWO_PI * .25f);
    const float valSwell = 1.0f + .07f * std::sin(cycle + TWO_PI * .60f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const float t = s.normalizedX(list->VtxBuffer[i].pos.x);
        ImU32 base = LerpColorU32(baseL, baseR, t);

        const int rI = (base >> IM_COL32_R_SHIFT) & 0xFF;
        const int gI = (base >> IM_COL32_G_SHIFT) & 0xFF;
        const int bI = (base >> IM_COL32_B_SHIFT) & 0xFF;
        const int aI = (base >> IM_COL32_A_SHIFT) & 0xFF;

        float hue = .0f, sat = .0f, val = .0f;
        ImGui::ColorConvertRGBtoHSV(rI / 255.0f, gI / 255.0f, bI / 255.0f, hue, sat, val);
        hue = Frac(hue + hueShift);
        sat = Saturate(sat * satSwell);
        val = Saturate(val * valSwell);

        float nr = .0f, ng = .0f, nb = .0f;
        ImGui::ColorConvertHSVtoRGB(hue, sat, val, nr, ng, nb);

        list->VtxBuffer[i].col = IM_COL32(static_cast<int>(std::clamp(nr * 255.0f, .0f, 255.0f)),
                                          static_cast<int>(std::clamp(ng * 255.0f, .0f, 255.0f)),
                                          static_cast<int>(std::clamp(nb * 255.0f, .0f, 255.0f)),
                                          aI);
    }
}

// Perf: draws up to 3 layers x 8 samples = 24 AddText calls per string.
// Use GlowSamples <= 4 for a cheaper single-layer fallback.
void AddTextGlow(ImDrawList* list,
                 ImFont* font,
                 float size,
                 const ImVec2& pos,
                 const char* text,
                 ImU32 glowColor,
                 float radius,
                 float intensity,
                 int samples)
{
    if (!list || !font || !text || !text[0] || radius <= .0f || intensity <= .01f)
    {
        return;
    }

    const int baseAlpha = (glowColor >> IM_COL32_A_SHIFT) & 0xFF;
    if (baseAlpha < 5)
    {
        return;
    }

    const int r = (glowColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (glowColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (glowColor >> IM_COL32_B_SHIFT) & 0xFF;

    // Multi-layer soft bloom for premium glow effect
    // Layer 1: Wide soft outer glow (largest, dimmest)
    // Layer 2: Medium glow
    // Layer 3: Tight inner glow (smallest, brightest)

    struct GlowLayer
    {
        float radiusMul;
        float alphaMul;
    };

    const GlowLayer layers[] = {
        {1.5f, .15f},  // Outer - wide and soft
        {1.0f, .25f},  // Middle
        {.6f, .35f},   // Inner - tight and bright
    };

    const int numLayers = (samples > 8) ? 3 : (samples > 4) ? 2 : 1;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        float layerRadius = radius * layers[layer].radiusMul;
        // Per-copy alpha solved so N overlapping stamps SUM to the layer's
        // target peak: the call sites render this under additive blending
        // (PushAdditiveBlend), where copies add linearly instead of
        // saturating like alpha-over. The old alpha-over solve
        // (1-(1-p)^(1/N)) overshot the peak by up to ~20% and clipped the
        // stacked layers toward white.
        int numOffsets = (samples > 4) ? 8 : 4;
        const float peak =
            std::clamp(baseAlpha / 255.0f * intensity * layers[layer].alphaMul, .0f, 1.0f);
        const float perCopy = peak / (float)numOffsets;
        int layerAlpha = std::clamp((int)(perCopy * 255.0f + .5f), 0, 255);
        if (layerAlpha < 1)
        {
            continue;
        }

        ImU32 col = IM_COL32(r, g, b, layerAlpha);

        // 8-directional samples for smooth circular glow
        const float offsets[8][2] = {
            {layerRadius, .0f},
            {-layerRadius, .0f},
            {.0f, layerRadius},
            {.0f, -layerRadius},
            {layerRadius * .707f, layerRadius * .707f},
            {-layerRadius * .707f, layerRadius * .707f},
            {layerRadius * .707f, -layerRadius * .707f},
            {-layerRadius * .707f, -layerRadius * .707f},
        };

        for (int i = 0; i < numOffsets; ++i)
        {
            list->AddText(
                font, size, ImVec2(pos.x + offsets[i][0], pos.y + offsets[i][1]), col, text);
        }
    }
}

// Perf: draws 1 + (samples-1) AddText calls per string (<= 24). Skips far/low
// alpha labels at the call site. Feathered disc, not a hollow ring.
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
                       int samples)
{
    if (!list || !font || !text || !text[0] || opacity <= .01f)
    {
        return;
    }

    const int baseA = (shadowColor >> IM_COL32_A_SHIFT) & 0xFF;
    if (baseA < 4)
    {
        return;
    }
    const int r = (shadowColor >> IM_COL32_R_SHIFT) & 0xFF;
    const int g = (shadowColor >> IM_COL32_G_SHIFT) & 0xFF;
    const int b = (shadowColor >> IM_COL32_B_SHIFT) & 0xFF;

    const float cx = pos.x + dirX * distance;
    const float cy = pos.y + dirY * distance;

    samples = std::clamp(samples, 1, 24);

    // Target peak opacity at full sample overlap. Preserves the per-frame fade
    // carried in the shadow color's alpha, scaled by the master opacity.
    const float peak = Saturate((baseA / 255.0f) * opacity);

    // Degenerate cases collapse to a single crisp offset copy.
    if (samples <= 1 || softness <= .01f)
    {
        const int a = std::clamp((int)(peak * 255.0f + .5f), 0, 255);
        if (a >= 3)
        {
            list->AddText(font, size, ImVec2(cx, cy), IM_COL32(r, g, b, a), text);
        }
        return;
    }

    // Per-sample alpha solved so N fully-overlapping samples accumulate to the
    // target peak under alpha-over compositing: a = 1 - (1 - peak)^(1/N).
    const float perSample = 1.0f - std::pow(1.0f - peak, 1.0f / (float)samples);
    const int a = std::clamp((int)(perSample * 255.0f + .5f), 0, 255);
    if (a < 1)
    {
        return;
    }
    const ImU32 col = IM_COL32(r, g, b, a);

    // Central tap plus a sunflower (golden-angle) disc so coverage is even for
    // any sample count -- a filled soft shadow rather than a hollow ring.
    list->AddText(font, size, ImVec2(cx, cy), col, text);
    const int ringSamples = samples - 1;
    for (int i = 0; i < ringSamples; ++i)
    {
        const float frac = ((float)i + .5f) / (float)ringSamples;
        const float rad = softness * std::sqrt(frac);
        const float ang = (float)i * 2.3999632f;  // golden angle (radians)
        list->AddText(
            font, size, ImVec2(cx + std::cos(ang) * rad, cy + std::sin(ang) * rad), col, text);
    }
}

}  // namespace TextEffects
