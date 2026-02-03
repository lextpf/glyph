#include "TextEffectsInternal.h"

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

// 2D value noise with quintic interpolation
static inline float ValueNoise(float x, float y)
{
    // Get integer and fractional parts
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    // Quintic interpolation curve for smoother results
    fx = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    fy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);

    // Sample four corners and interpolate
    float a = Hash(ix, iy);
    float b = Hash(ix + 1.0f, iy);
    float c = Hash(ix, iy + 1.0f);
    float d = Hash(ix + 1.0f, iy + 1.0f);

    // Bilinear interpolation
    float ab = a + (b - a) * fx;
    float cd = c + (d - c) * fx;
    return ab + (cd - ab) * fy;
}

// Fractal Brownian Motion
static inline float FBMNoise(float x, float y, int octaves, float persistence = .5f)
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

    // Create intermediate colors for richer aurora palette
    ImU32 colMid = LerpColorU32(colA, colB, .5f);
    // Add subtle brightness variation
    ImU32 colBright = LerpColorU32(colA, IM_COL32(255, 255, 255, 255), .25f);

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

        // Add subtle shimmer
        float shimmer = std::sin(time * 4.0f + nx * 12.0f + ny * 8.0f) * .5f + .5f;
        shimmer = shimmer * shimmer * .15f;  // Subtle sparkle

        // Horizontal sway effect
        float swayOffset = std::sin(ny * 3.0f + time * 1.5f) * sway;
        float swayedX = nx + swayOffset;
        float swayFactor = std::sin(swayedX * TWO_PI * waves + time) * .5f + .5f;

        // Blend all factors
        float t =
            Saturate((combined * .6f + curtain * .25f + swayFactor * .15f) * intensity + shimmer);

        // Three-color gradient for rich aurora appearance
        ImU32 finalColor;
        if (t < .4f)
        {
            finalColor = LerpColorU32(colA, colMid, t * 2.5f);
        }
        else if (t < .7f)
        {
            finalColor = LerpColorU32(colMid, colB, (t - .4f) * 3.33f);
        }
        else
        {
            finalColor = LerpColorU32(colB, colBright, (t - .7f) * 3.33f);
        }

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

    // Create color variations for richer sparkle
    ImU32 sparkleWhite = IM_COL32(255, 255, 255, 255);
    ImU32 sparkleTint = LerpColorU32(sparkleColor, sparkleWhite, .3f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        ImU32 base = LerpColorU32(baseL, baseR, nx);
        float totalSparkle = .0f;
        float colorShift = .0f;  // For varying sparkle color

        // Layer 1: Large slow-twinkling stars
        float seed1 = Hash(std::floor(p.x * .06f), std::floor(p.y * .06f));
        if (seed1 > (1.0f - density * .4f))
        {
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

            totalSparkle += sparkle1 * starPattern * .9f;
            colorShift += sparkle1 * .3f;
        }

        // Layer 2: Medium fast-twinkling sparkles
        float seed2 = Hash(std::floor(p.x * .12f) + 50.0f, std::floor(p.y * .12f) + 50.0f);
        if (seed2 > (1.0f - density * .7f))
        {
            float phase2 = seed2 * TWO_PI;
            float sparkleTime2 = time * speed * 1.8f * (.8f + seed2 * .4f);
            float sparkle2 = std::sin(sparkleTime2 + phase2);
            sparkle2 = std::max(.0f, sparkle2);
            sparkle2 = std::pow(sparkle2, 5.0f);
            totalSparkle += sparkle2 * .6f;
        }

        // Layer 3: Fine shimmer dust
        float seed3 = Hash(std::floor(p.x * .2f) + 100.0f, std::floor(p.y * .2f) + 100.0f);
        if (seed3 > (1.0f - density * .9f))
        {
            float phase3 = seed3 * TWO_PI;
            float sparkle3 = std::sin(time * speed * 2.5f + phase3);
            sparkle3 = std::max(.0f, sparkle3);
            sparkle3 = std::pow(sparkle3, 8.0f);
            totalSparkle += sparkle3 * .35f;
        }

        // Layer 4: Rare brilliant flares
        float seed4 = Hash(std::floor(p.x * .04f) + 200.0f, std::floor(p.y * .04f) + 200.0f);
        if (seed4 > .93f)
        {
            float phase4 = seed4 * TWO_PI;
            float flare = std::sin(time * speed * .4f + phase4);
            flare = std::max(.0f, flare);
            flare = std::pow(flare, 2.0f);
            totalSparkle += flare * 1.5f;
            colorShift += flare * .6f;  // Flares shift toward white
        }

        totalSparkle = Saturate(totalSparkle * intensity);
        colorShift = Saturate(colorShift);

        // Blend sparkle color with white based on intensity for brighter sparkles
        ImU32 finalSparkle = LerpColorU32(sparkleColor, sparkleTint, colorShift);
        list->VtxBuffer[i].col = LerpColorU32(base, finalSparkle, totalSparkle);
    }
}

void AddTextPlasma(ImDrawList* list,
                   ImFont* font,
                   float size,
                   const ImVec2& pos,
                   const char* text,
                   ImU32 colA,
                   ImU32 colB,
                   float freq1,
                   float freq2,
                   float speed)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime() * speed;
    ImU32 colMid = LerpColorU32(colA, colB, .5f);

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        // Enhanced plasma with more organic patterns
        float plasma = .0f;

        // Primary waves with varied phases
        plasma += std::sin(nx * freq1 * TWO_PI + time);
        plasma += std::sin(ny * freq2 * TWO_PI + time * .7f);

        // Diagonal waves
        plasma += std::sin((nx + ny) * (freq1 + freq2) * .5f * TWO_PI + time * 1.3f);
        plasma += std::sin((nx - ny) * freq1 * TWO_PI + time * .9f) * .5f;

        // Radial waves from offset centers for more organic look
        float cx1 = nx - .3f - std::sin(time * .3f) * .2f;
        float cy1 = ny - .5f - std::cos(time * .4f) * .15f;
        float dist1 = std::sqrt(cx1 * cx1 + cy1 * cy1);
        plasma += std::sin(dist1 * freq1 * TWO_PI * 2.0f - time * 1.2f);

        float cx2 = nx - .7f + std::cos(time * .35f) * .15f;
        float cy2 = ny - .5f + std::sin(time * .45f) * .2f;
        float dist2 = std::sqrt(cx2 * cx2 + cy2 * cy2);
        plasma += std::sin(dist2 * freq2 * TWO_PI * 1.5f + time * .8f) * .7f;

        // Normalize to [0, 1] with smoother transition
        plasma = (plasma + 5.2f) / 10.4f;
        plasma = SmoothStep(plasma);  // Use quintic smoothing

        // Three-color gradient for richer appearance
        ImU32 finalColor;
        if (plasma < .5f)
        {
            finalColor = LerpColorU32(colA, colMid, plasma * 2.0f);
        }
        else
        {
            finalColor = LerpColorU32(colMid, colB, (plasma - .5f) * 2.0f);
        }

        list->VtxBuffer[i].col = finalColor;
    }
}

void AddTextScanline(ImDrawList* list,
                     ImFont* font,
                     float size,
                     const ImVec2& pos,
                     const char* text,
                     ImU32 baseL,
                     ImU32 baseR,
                     ImU32 scanColor,
                     float speed,
                     float scanWidth,
                     float intensity)
{
    TextVertexSetup s;
    if (!TextVertexSetup::Begin(s, list, font, size, pos, text))
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    float phase1 = std::sin(time * speed * PI) * .5f + .5f;
    float phase2 = std::sin(time * speed * PI + 2.0f) * .5f + .5f;

    const float bandWidth = (std::max)(scanWidth, .05f);
    const float bandHalf = bandWidth * .5f;

    for (int i = s.vtxStart; i < s.vtxEnd; ++i)
    {
        const ImVec2 p = list->VtxBuffer[i].pos;
        const float nx = s.normalizedX(p.x);
        const float ny = s.normalizedY(p.y);

        // Base gradient color
        ImU32 base = LerpColorU32(baseL, baseR, nx);

        // Primary scanline with smooth quintic falloff
        float d1 = std::abs(ny - phase1);
        float scan1 = .0f;
        if (d1 < bandHalf)
        {
            scan1 = 1.0f - SmoothStep(d1 / bandHalf);
        }

        // Secondary scanline
        float d2 = std::abs(ny - phase2);
        float scan2 = .0f;
        if (d2 < bandHalf * .7f)
        {
            scan2 = (1.0f - SmoothStep(d2 / (bandHalf * .7f))) * .4f;
        }

        // Subtle horizontal scan lines
        float crtLines = std::sin(ny * s.height() * .5f) * .5f + .5f;
        crtLines = crtLines * .08f;  // Very subtle

        // Combine effects
        float totalScan = Saturate((scan1 + scan2) * intensity + crtLines);

        // Add slight glow around the main scanline
        float glow = std::exp(-d1 * d1 * 20.0f) * .15f * intensity;
        totalScan = Saturate(totalScan + glow);

        list->VtxBuffer[i].col = LerpColorU32(base, scanColor, totalScan);
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
        int layerAlpha = (int)(baseAlpha * intensity * layers[layer].alphaMul);
        layerAlpha = std::clamp(layerAlpha, 0, 255);
        if (layerAlpha < 3)
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

        int numOffsets = (samples > 4) ? 8 : 4;
        for (int i = 0; i < numOffsets; ++i)
        {
            list->AddText(
                font, size, ImVec2(pos.x + offsets[i][0], pos.y + offsets[i][1]), col, text);
        }
    }
}

}  // namespace TextEffects
