#include "TextEffectsInternal.hpp"

namespace TextEffects
{

// Draw 4-pointed star with glow
static void DrawStar4(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      ImU32 color,
                      ImU32 glowColor,
                      float rotation = .0f)
{
    const float innerRatio = .35f;  // Inner radius as fraction of outer
    const float outerR = size;
    const float innerR = size * innerRatio;

    // 4 points = 8 vertices alternating outer/inner
    ImVec2 points[8];
    for (int i = 0; i < 8; ++i)
    {
        float angle = rotation + (float)i * .785398f;  // 45 degrees = pi/4
        float radius = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
    }

    // Draw soft glow behind
    list->AddCircleFilled(pos, size * 1.8f, glowColor, 16);

    // Draw star shape
    list->AddConvexPolyFilled(points, 8, color);

    // Bright white core
    int coreAlpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    list->AddCircleFilled(pos, size * .3f, IM_COL32(255, 255, 255, coreAlpha / 2), 8);
}

// Draw 6-pointed star with glow
static void DrawStar6(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      ImU32 color,
                      ImU32 glowColor,
                      float rotation = .0f)
{
    const float innerRatio = .45f;
    const float outerR = size;
    const float innerR = size * innerRatio;

    // 6 points = 12 vertices
    ImVec2 points[12];
    for (int i = 0; i < 12; ++i)
    {
        float angle = rotation + (float)i * .5235988f;  // 30 degrees = pi/6
        float radius = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
    }

    // Draw soft glow layers
    list->AddCircleFilled(pos, size * 2.2f, glowColor, 16);
    list->AddCircleFilled(pos, size * 1.5f, glowColor, 16);

    // Draw star shape
    list->AddConvexPolyFilled(points, 12, color);

    // Bright white core
    int coreAlpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    list->AddCircleFilled(pos, size * .3f, IM_COL32(255, 255, 255, coreAlpha / 2), 8);
}

// Draw soft glowing orb with gradient layers. The outer layers carry the
// primary color; the core is a secondary-tinted ~70%-white jewel rather than
// pure white, so the procedural fallback echoes the warm-core/cool-edge read
// of the textured path. (C2 -- this path is non-textured by definition, so it
// never touches the pixel-art sprite.)
static void DrawSoftOrb(ImDrawList* list,
                        const ImVec2& pos,
                        float size,
                        int r,
                        int g,
                        int b,
                        int r2,
                        int g2,
                        int b2,
                        int baseAlpha)
{
    // Multiple layers for smooth gradient falloff
    const int layers = 6;
    for (int i = layers - 1; i >= 0; --i)
    {
        float t = (float)i / (float)(layers - 1);
        float radius = size * (.38f + .82f * t);
        int layerAlpha = (int)(baseAlpha * (1.0f - t * .62f) * (1.0f - t * .22f));
        layerAlpha = std::clamp(layerAlpha, 0, 255);
        list->AddCircleFilled(pos, radius, IM_COL32(r, g, b, layerAlpha), 16);
    }
    // Tinted-luminous core (~midway primary/secondary, then toward white).
    int cr = (r + r2) / 2;
    int cg = (g + g2) / 2;
    int cb = (b + b2) / 2;
    list->AddCircleFilled(pos,
                          size * .25f,
                          IM_COL32((cr + 255) / 2, (cg + 255) / 2, (cb + 255) / 2, baseAlpha / 2),
                          12);
}

// Draw ethereal wisp with flowing trail
static void DrawWisp(ImDrawList* list,
                     const ImVec2& pos,
                     float size,
                     float angle,
                     int r,
                     int g,
                     int b,
                     int baseAlpha,
                     float trailLength)
{
    // Trail direction
    float trailAngle = angle + PI;
    float dx = std::cos(trailAngle);
    float dy = std::sin(trailAngle);

    // Draw trail segments with decreasing alpha
    const int trailSegments = 6;
    for (int i = trailSegments - 1; i >= 0; --i)
    {
        float t = (float)i / (float)trailSegments;
        float segX = pos.x + dx * size * trailLength * t;
        float segY = pos.y + dy * size * trailLength * t;
        float segSize = size * (.94f - t * .48f);
        int segAlpha = (int)(baseAlpha * (.58f - t * .34f));
        segAlpha = std::clamp(segAlpha, 0, 255);
        list->AddCircleFilled(ImVec2(segX, segY), segSize, IM_COL32(r, g, b, segAlpha), 12);
    }

    // Main wisp body with glow
    list->AddCircleFilled(pos, size * 1.8f, IM_COL32(r, g, b, baseAlpha / 10), 16);
    list->AddCircleFilled(pos, size * 1.22f, IM_COL32(r, g, b, baseAlpha / 6), 14);
    list->AddCircleFilled(pos, size * .88f, IM_COL32(r, g, b, (int)(baseAlpha * .72f)), 12);
    list->AddCircleFilled(pos, size * .28f, IM_COL32(255, 255, 255, baseAlpha / 4), 8);
}

// Draw magical rune symbol with glow
static void DrawRune(ImDrawList* list,
                     const ImVec2& pos,
                     float size,
                     int r,
                     int g,
                     int b,
                     int baseAlpha,
                     int runeType)
{
    ImU32 glowCol = IM_COL32(r, g, b, baseAlpha / 3);
    ImU32 mainCol = IM_COL32(r, g, b, baseAlpha);
    ImU32 brightCol =
        IM_COL32(std::min(255, r + 50), std::min(255, g + 50), std::min(255, b + 50), baseAlpha);
    float thickness = size * .15f;

    // Draw glow behind
    list->AddCircleFilled(pos, size * 1.8f, IM_COL32(r, g, b, baseAlpha / 5), 16);

    // Different rune patterns based on type
    switch (runeType % 4)
    {
        case 0:
        {
            // Diamond with cross
            float s = size;
            // Outer diamond
            list->AddQuad(ImVec2(pos.x, pos.y - s),
                          ImVec2(pos.x + s * .7f, pos.y),
                          ImVec2(pos.x, pos.y + s),
                          ImVec2(pos.x - s * .7f, pos.y),
                          mainCol,
                          thickness);
            // Inner cross
            list->AddLine(ImVec2(pos.x, pos.y - s * .5f),
                          ImVec2(pos.x, pos.y + s * .5f),
                          brightCol,
                          thickness * .8f);
            list->AddLine(ImVec2(pos.x - s * .35f, pos.y),
                          ImVec2(pos.x + s * .35f, pos.y),
                          brightCol,
                          thickness * .8f);
            // Corner dots
            list->AddCircleFilled(ImVec2(pos.x, pos.y - s), size * .12f, brightCol, 8);
            list->AddCircleFilled(ImVec2(pos.x, pos.y + s), size * .12f, brightCol, 8);
            break;
        }
        case 1:
        {
            // Triangle with eye
            float s = size * .9f;
            ImVec2 p1(pos.x, pos.y - s);
            ImVec2 p2(pos.x - s * .866f, pos.y + s * .5f);
            ImVec2 p3(pos.x + s * .866f, pos.y + s * .5f);
            list->AddTriangle(p1, p2, p3, mainCol, thickness);
            // Inner circle (eye)
            list->AddCircle(pos, size * .35f, brightCol, 12, thickness * .7f);
            list->AddCircleFilled(pos, size * .15f, brightCol, 8);
            break;
        }
        case 2:
        {
            // Asterisk/star burst
            float s = size;
            for (int i = 0; i < 6; ++i)
            {
                float angle = (float)i * .5236f;  // 30 degrees
                ImVec2 outer(pos.x + std::cos(angle) * s, pos.y + std::sin(angle) * s);
                list->AddLine(pos, outer, mainCol, thickness);
            }
            list->AddCircleFilled(pos, size * .2f, brightCol, 10);
            break;
        }
        case 3:
        {
            // Concentric circles with dots
            list->AddCircle(pos, size * .9f, mainCol, 14, thickness * .7f);
            list->AddCircle(pos, size * .5f, mainCol, 12, thickness * .6f);
            list->AddCircleFilled(pos, size * .2f, brightCol, 8);
            // Cardinal dots
            for (int i = 0; i < 4; ++i)
            {
                float angle = (float)i * 1.5708f;  // 90 degrees
                ImVec2 dotPos(pos.x + std::cos(angle) * size * .7f,
                              pos.y + std::sin(angle) * size * .7f);
                list->AddCircleFilled(dotPos, size * .1f, brightCol, 6);
            }
            break;
        }
    }
}

// Draw a spark with motion trail
static void DrawSpark(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      float angle,
                      int r,
                      int g,
                      int b,
                      int baseAlpha,
                      float life)
{
    // Spark gets more orange/yellow as it's "hotter"
    float heat = 1.0f - life;
    int sr = std::min(255, r + (int)(100 * heat));
    int sg = std::min(255, g + (int)(50 * heat));
    int sb = std::max(0, b - (int)(30 * heat));

    // Trail behind spark
    float trailAngle = angle + PI;
    const int trailSegs = 4;
    for (int i = trailSegs - 1; i >= 0; --i)
    {
        float t = (float)(i + 1) / (float)(trailSegs + 1);
        float trailX = pos.x + std::cos(trailAngle) * size * 3.0f * t;
        float trailY = pos.y + std::sin(trailAngle) * size * 3.0f * t;
        float segSize = size * (1.0f - t * .7f);
        int segAlpha = (int)(baseAlpha * (1.0f - t) * .6f);
        list->AddCircleFilled(ImVec2(trailX, trailY), segSize, IM_COL32(sr, sg, sb, segAlpha), 8);
    }

    // Main spark with glow
    list->AddCircleFilled(pos, size * 2.0f, IM_COL32(sr, sg, sb, baseAlpha / 4), 12);
    list->AddCircleFilled(pos, size, IM_COL32(sr, sg, sb, baseAlpha), 10);
    list->AddCircleFilled(pos, size * .4f, IM_COL32(255, 255, 220, baseAlpha), 8);
}

// Independent per-particle trait streams. Each enumerator is a hash salt;
// using distinct salts guarantees traits are uncorrelated with each other,
// unlike the previous linear (i * prime) patterns which correlated across
// particles and traits.
enum class Trait : uint32_t
{
    Radial = 1,       ///< Radial home position within the style's band
    Speed = 2,        ///< Orbital/cycle speed multiplier
    Direction = 3,    ///< Orbit direction selector
    Depth = 4,        ///< Pseudo-depth (size + alpha modulation)
    Size = 5,         ///< Base size multiplier
    JitterAngle = 6,  ///< Static positional jitter direction
    JitterDist = 7,   ///< Static positional jitter distance
    Phase = 8,        ///< Angular phase jitter on top of the golden angle
    Bob = 9,          ///< Independent phase for bobbing/secondary motion
    HomeX = 10,       ///< Stratified horizontal home jitter for falling/flow types
    Hue = 11          ///< Decorrelated micro temperature jitter for color
};

// Centralized sprite size multiplier. Every textured particle draws at
// finalSize * kSpriteSizeMul (the crisp pixel-art quad); the additive glow
// halo multiplies kSpriteSizeMul * glowSize on top of that. The legacy 6.0f
// factor was tuned for 256px procedural sprites; the shipped 16px PNGs fill
// the whole quad, so this keeps them at an ambient-sparkle scale.
constexpr float kSpriteSizeMul = 2.5f;

// 32-bit finalizer (triple xorshift-multiply) -- decorrelates consecutive
// integers into uniformly distributed hash values.
static uint32_t PMixU32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

// Stateless per-particle trait in [0,1). Stable across frames (no flicker)
// because it depends only on particle index, style slot, and salt.
static float PTrait(int particleIndex, int styleIndex, Trait salt)
{
    uint32_t h =
        PMixU32(static_cast<uint32_t>(particleIndex) +
                PMixU32(static_cast<uint32_t>(styleIndex) + PMixU32(static_cast<uint32_t>(salt))));
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// Area-uniform radius in the annulus [bandFloor, 1]: equal particle density
// per unit area instead of crowding the inner edge.
static float AnnulusRadius(float bandFloor, float u)
{
    float f2 = bandFloor * bandFloor;
    return std::sqrt(f2 + (1.0f - f2) * u);
}

// Cheap stable hash in [0,1) from a float seed. Used to give fall/drift
// weather particles a per-particle horizontal home and phase offset without
// adding state -- depends only on the particle's golden-angle seed.
static float PHash01(float seed)
{
    float s = std::sin(seed) * 43758.5453f;
    return s - std::floor(s);
}

// Inner band edge when a style renders alone and may fill the nameplate
// interior. With multiple styles enabled the shared band formula keeps the
// styles radially separated instead. Only the orbiting weather types
// (Firefly/Dust/Mote) consult the radial band; fall/rise/flow types lay
// themselves out across the full aura and ignore it.
static float SoloBandFloor(Settings::ParticleStyle style)
{
    switch (style)
    {
        case Settings::ParticleStyle::Spark:
            return .58f;  // embers launch from the text edge
        case Settings::ParticleStyle::Wisp:
            return .45f;
        case Settings::ParticleStyle::Firefly:
            return .34f;
        case Settings::ParticleStyle::Dust:
        case Settings::ParticleStyle::Mote:
            return .28f;  // motes fill the whole region
        default:
            return .30f;  // fall/rise/flow types span the aura
    }
}

// Fraction of particles travelling in the reverse direction. Mixed directions
// break the rigid same-way look; only the orbiting/flow types use it.
static float CounterRotateChance(Settings::ParticleStyle style)
{
    switch (style)
    {
        case Settings::ParticleStyle::Firefly:
        case Settings::ParticleStyle::Dust:
        case Settings::ParticleStyle::Mote:
        case Settings::ParticleStyle::Aurora:
            return .5f;
        case Settings::ParticleStyle::Wisp:
            return .3f;  // mostly one flow direction with a few drifters
        default:
            return .0f;
    }
}

// Shared per-particle state passed to each style's render function.
struct ParticleContext
{
    ImDrawList* list;
    ImVec2 center;
    float radiusX, radiusY;
    float alpha, particleSize, timeScaled;
    int texStyleId;
    bool hasTextures;
    int particleIndex;
    int particleCount;
    int styleIndex;  ///< Index among enabled styles; needed by PTrait salts
    float golden, phase;
    float minRadius;     ///< Inner edge of this style's radial band [0,1]
    float radialAnchor;  ///< Absolute home radius in [minRadius, 1], area-uniform
    float jitterAngle, jitterDist;
    float alphaVariation;
    float speedVar;  ///< Per-particle speed multiplier [.72, 1.28]
    float dirSign;   ///< Orbit direction: +1 or -1
    float depth;     ///< Pseudo-depth [0,1]: 0 = far (small/dim), 1 = near
    float sizeVar;   ///< Per-particle base size multiplier [.85, 1.15]
    float bobPhase;  ///< Independent phase for bobbing/secondary motion
    int r, g, b;
    int r2, g2, b2;  // Secondary gradient color
    bool hasSecondaryColor;
    ParticleTextures::BlendMode texBlendMode;

    // Premium-pass live knobs (F6/C4/G5), shared per-plate air current (F3).
    float depthStrength;   ///< Scales the whole 3D read (F1/F2); 0 = flat sheet
    float warmth;          ///< Scales color temperature mix + apex pulse (C1/C3)
    float glowStrength;    ///< Additive backlight halo alpha multiplier (G1)
    float glowSize;        ///< Halo radius as a multiple of the crisp sprite (G1)
    float shineThreshold;  ///< Sine threshold for the rare specular glint (G3)
    float airX, airY;      ///< Shared per-plate air current, depth-weighted (F3)

    // F1: widen depth into a real near/far range, scaled live by depthStrength.
    // Far ~0.62x/0.55x, near ~1.28x/1.10x at strength 1; strength 0 -> 1.0.
    float DepthSizeScale() const
    {
        float raw = .62f + .66f * depth;
        return 1.0f + (raw - 1.0f) * depthStrength;
    }
    float DepthAlphaScale() const
    {
        float raw = .55f + .55f * depth;
        return 1.0f + (raw - 1.0f) * depthStrength;
    }
    // F2: depth parallax on motion AMPLITUDE only (never the sin time argument).
    // Far ~0.55x amplitude, near ~1.25x.
    float DepthParallaxScale() const { return 1.0f + (.55f + .70f * depth - 1.0f) * depthStrength; }
};

// G1/G2/G3: draw an additive cool-secondary BACKLIGHT halo behind the crisp
// pixel sprite, then the sprite itself, then -- rarely -- a tiny eased specular
// glint on top. The halo and glint are separate additive sprites that reuse the
// same point-sampled DrawSpriteWithIndex path (no extra ImGui blend-callback
// splits beyond one more textured quad), so the crisp 16px core always wins the
// read. The procedural fallback routes to DrawSoftOrb instead.
static void DrawHaloThenSprite(
    const ParticleContext& ctx, const ImVec2& pos, float finalSize, int a, float rotation)
{
    if (!ctx.hasTextures)
    {
        DrawSoftOrb(ctx.list, pos, finalSize, ctx.r, ctx.g, ctx.b, ctx.r2, ctx.g2, ctx.b2, a);
        return;
    }

    const float coreSize = finalSize * kSpriteSizeMul;

    // G1/G2: additive backlight, slightly wider near-camera.  `a` already carries
    // the depth-alpha (DepthAlphaScale, applied once in the caller), so the halo
    // inherits near-blooms-more without re-applying it.  Gated to visible particles.
    if (ctx.glowStrength > .0f && a > 24)
    {
        int ha = std::clamp((int)(a * ctx.glowStrength), 0, 255);
        if (ha > 2)
        {
            float haloSize = coreSize * ctx.glowSize * (1.0f + .15f * ctx.depth);
            ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                                  pos,
                                                  haloSize,
                                                  ctx.texStyleId,
                                                  ctx.particleIndex,
                                                  IM_COL32(ctx.r2, ctx.g2, ctx.b2, ha),
                                                  ParticleTextures::BlendMode::Additive,
                                                  rotation);
        }
    }

    // The crisp pixel sprite, point-sampled, in the chosen INI blend mode.
    ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                          pos,
                                          coreSize,
                                          ctx.texStyleId,
                                          ctx.particleIndex,
                                          IM_COL32(ctx.r, ctx.g, ctx.b, a),
                                          ctx.texBlendMode,
                                          rotation);

    // G3: rare eased specular twinkle, modulating ONLY the halo layer -- a slow
    // glint catching the key light, never a strobe and never the crisp quad.
    if (ctx.glowStrength > .0f && a > 24)
    {
        float tw = std::sin(ctx.timeScaled * .9f * ctx.speedVar + ctx.golden * 3.7f);
        float shine = SmoothStep((tw - ctx.shineThreshold) * 7.0f);
        if (shine > .02f)
        {
            int sa = std::clamp((int)(a * shine * .5f), 0, 255);
            if (sa > 2)
            {
                ParticleTextures::DrawSpriteWithIndex(
                    ctx.list,
                    pos,
                    coreSize * .5f,
                    ctx.texStyleId,
                    ctx.particleIndex,
                    IM_COL32((ctx.r2 + 255) / 2, (ctx.g2 + 255) / 2, (ctx.b2 + 255) / 2, sa),
                    ParticleTextures::BlendMode::Additive,
                    rotation);
            }
        }
    }
}

// Lazily wandering glow that blinks in place: a slow differential orbit plus
// an incommensurate-sine drift, with a firefly blink that is mostly soft and
// occasionally pulses bright. No spin -- the sprite stays upright so pixels
// stay crisp.
static void RenderFireflyParticle(const ParticleContext& ctx)
{
    float omega = .12f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;

    float par = ctx.DepthParallaxScale();  // F2: scales amplitude only
    float wanderX = (std::sin(ctx.timeScaled * .47f * ctx.speedVar + ctx.golden) +
                     .6f * std::sin(ctx.timeScaled * .83f + ctx.golden * 1.7f)) *
                    .07f * par;
    float wanderY = (std::sin(ctx.timeScaled * .53f * ctx.speedVar + ctx.golden * 2.1f) +
                     .6f * std::sin(ctx.timeScaled * .71f + ctx.bobPhase)) *
                    .07f * par;
    float radiusMod =
        ctx.radialAnchor * (.9f + .06f * par * std::sin(ctx.timeScaled * .3f + ctx.golden));
    // F3: shared per-plate air current, depth-weighted so near motes drift more.
    float x = ctx.center.x + std::cos(orbit) * ctx.radiusX * radiusMod + wanderX * ctx.radiusX +
              ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + std::sin(orbit) * ctx.radiusY * radiusMod + wanderY * ctx.radiusY +
              ctx.airY * ctx.depth * ctx.radiusY;

    float blink = std::sin(ctx.timeScaled * 2.4f * ctx.speedVar + ctx.golden * 3.0f);
    float glow = .35f + .65f * (.5f + .5f * blink);
    float finalAlpha = ctx.alpha * glow * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize * (.92f + .12f * glow) * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    // C3: whisper candle-warm pulse at the glow apex (Firefly glow only).
    ParticleContext warm = ctx;
    warm.r = std::min(255, ctx.r + (int)(14 * glow * ctx.warmth));
    DrawHaloThenSprite(warm, ImVec2(x, y), finalSize, a, .0f);
}

// Ember ballistics: sparks launch fast from the text edge, decelerate
// radially while buoyancy takes over, then cool, shrink, and fade. Trails
// align with the actual velocity direction.
static void RenderSparkParticle(const ParticleContext& ctx)
{
    float sparkTime = ctx.timeScaled * 1.6f * ctx.speedVar + ctx.golden;
    float life = std::fmod(sparkTime, TWO_PI) * INV_TWO_PI;

    // Ease-out launch: fast at birth, decelerating outward
    float launch = 1.0f - (1.0f - life) * (1.0f - life);
    float sparkMinDist = std::clamp(ctx.minRadius - .05f, .42f, .85f);
    float dist = sparkMinDist + launch * (1.0f - sparkMinDist);
    float baseAngle = ctx.phase + std::sin(ctx.golden * 2.0f) * .5f;
    float curveAngle = baseAngle + life * .3f * std::sin(ctx.golden);

    // Buoyant rise accelerates as the radial launch dies off
    float rise = life * life * .55f;
    float x = ctx.center.x + std::cos(curveAngle) * ctx.radiusX * dist +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist * .5f;
    float y = ctx.center.y + std::sin(curveAngle) * ctx.radiusY * dist - rise * ctx.radiusY +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist * .5f;

    float fadeIn = SmoothStep(life * (1.0f / .14f));  // no full-alpha pop at birth
    float flicker = .8f + .2f * std::sin(ctx.timeScaled * 15.0f + ctx.golden * 5.0f);
    float finalAlpha = ctx.alpha * fadeIn * (1.0f - life * life) * flicker * ctx.alphaVariation *
                       ctx.DepthAlphaScale();
    if (finalAlpha < .05f)
    {
        return;
    }

    // Embers shrink as they cool
    float cooling = 1.08f - .45f * life;
    float finalSize = ctx.particleSize * cooling * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    float heatFade = 1.0f - life * .5f;
    int sr = std::clamp((int)(255 * heatFade), 180, 255);
    int sg = std::clamp((int)(220 * heatFade - life * 80), 120, 220);
    int sb = std::clamp((int)(80 - life * 60), 20, 80);

    // Analytic velocity (d/d(life)) so trails oppose the true motion:
    // radial speed decays as 2(1-life), rise speed grows as 2*.55*life.
    float dDist = 2.0f * (1.0f - life) * (1.0f - sparkMinDist);
    float dRise = 1.1f * life;
    float vx = std::cos(curveAngle) * ctx.radiusX * dDist;
    float vy = std::sin(curveAngle) * ctx.radiusY * dDist - ctx.radiusY * dRise;
    float moveAngle = std::atan2(vy, vx);

    if (ctx.hasTextures)
    {
        float trailDx = -std::cos(moveAngle);
        float trailDy = -std::sin(moveAngle);
        float trailSpacing = finalSize * 3.6f;
        for (int t = 2; t >= 1; --t)
        {
            float tf = static_cast<float>(t) / 3.0f;
            float tx = x + trailDx * trailSpacing * tf;
            float ty = y + trailDy * trailSpacing * tf;
            int trailA = std::clamp(static_cast<int>(a * (.3f - .1f * t)), 0, 255);
            // Trail sprites stay crisp; the halo/glint pass is reserved for the
            // ember head so trails do not bloom into blobs.
            ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                                  ImVec2(tx, ty),
                                                  finalSize * kSpriteSizeMul,
                                                  ctx.texStyleId,
                                                  ctx.particleIndex,
                                                  IM_COL32(ctx.r, ctx.g, ctx.b, trailA),
                                                  ctx.texBlendMode,
                                                  moveAngle);
        }
        DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize, a, moveAngle);
    }
    else
    {
        DrawSpark(ctx.list, ImVec2(x, y), finalSize, moveAngle, sr, sg, sb, a, life);
    }
}

// Serpentine wisps on incommensurate Lissajous paths: x and y oscillate at
// irrational-ratio frequencies so the weave never visibly repeats. Trails
// follow the exact analytic velocity.
static void RenderWispParticle(const ParticleContext& ctx)
{
    float wx = .42f * ctx.speedVar * ctx.dirSign;
    float wy = .30f * ctx.speedVar * (1.0f + .07f * std::sin(ctx.golden));
    float waveX = .30f * std::sin(ctx.timeScaled * .31f * ctx.speedVar + ctx.golden);
    float waveY = .22f * std::sin(ctx.timeScaled * .43f * ctx.speedVar + ctx.golden * 1.3f);
    float thetaX = ctx.phase + ctx.timeScaled * wx + waveX;
    float thetaY = ctx.phase * 1.7f + ctx.timeScaled * wy + waveY;

    float radiusMod =
        ctx.radialAnchor * (.95f + .05f * std::sin(ctx.golden + ctx.timeScaled * .25f));
    float bob = .05f * std::sin(ctx.timeScaled * .8f + ctx.bobPhase);
    float x = ctx.center.x + std::cos(thetaX) * ctx.radiusX * radiusMod +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist;
    float y = ctx.center.y + std::sin(thetaY) * ctx.radiusY * radiusMod + bob * ctx.radiusY +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist;

    float pulse = .52f + .28f * std::sin(ctx.timeScaled * .6f * ctx.speedVar + ctx.golden * 2.0f);
    float finalAlpha = ctx.alpha * pulse * ctx.alphaVariation * .82f * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize = ctx.particleSize *
                      (.88f + .06f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f)) *
                      ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    int wr = std::min(255, ctx.r + 24);
    int wg = std::min(255, ctx.g + 30);
    int wb = std::min(255, ctx.b + 36);

    // Chain-rule velocity of the Lissajous + bob path
    float dThetaX = wx + .30f * .31f * ctx.speedVar *
                             std::cos(ctx.timeScaled * .31f * ctx.speedVar + ctx.golden);
    float dThetaY = wy + .22f * .43f * ctx.speedVar *
                             std::cos(ctx.timeScaled * .43f * ctx.speedVar + ctx.golden * 1.3f);
    float vx = -std::sin(thetaX) * ctx.radiusX * radiusMod * dThetaX;
    float vy = std::cos(thetaY) * ctx.radiusY * radiusMod * dThetaY +
               ctx.radiusY * .04f * std::cos(ctx.timeScaled * .8f + ctx.bobPhase);
    float moveAngle = std::atan2(vy, vx);
    float trailLength = 1.12f + .32f * std::sin(ctx.golden);

    if (ctx.hasTextures)
    {
        float echoDist = finalSize * 5.0f;
        float ex = x - std::cos(moveAngle) * echoDist;
        float ey = y - std::sin(moveAngle) * echoDist;
        int echoA = std::clamp(static_cast<int>(a * .24f), 0, 255);
        // Echo is a crisp trailing ghost (kSpriteSizeMul * 0.8 preserves its
        // historical 4.8/6.0 ratio); the halo/glint pass rides the main body.
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(ex, ey),
                                              finalSize * (kSpriteSizeMul * .8f),
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, echoA),
                                              ctx.texBlendMode,
                                              moveAngle);
        // Main body: preserve Wisp's 5.8/6.0 ratio by pre-scaling finalSize so
        // the crisp quad still lands at finalSize * .9667 * kSpriteSizeMul.
        DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize * (5.8f / 6.0f), a, moveAngle);
    }
    else
    {
        DrawWisp(ctx.list, ImVec2(x, y), finalSize, moveAngle, wr, wg, wb, a, trailLength);
    }
}

// Shared falling-particle layout. Returns screen position for a particle that
// loops top->bottom through the aura, distributed across its full width by a
// stable per-particle horizontal home. `fallRate` sets speed, `swayAmp` the
// horizontal oscillation, `swayRate` its frequency. `edge` is filled with a
// top/bottom fade factor and `fall` with the [0,1) vertical progress.
static ImVec2 FallLayout(const ParticleContext& ctx,
                         float fallRate,
                         float swayAmp,
                         float swayRate,
                         float& fall,
                         float& edge)
{
    // M2: stratified jittered cell -> one particle per equal-width column, full
    // width including the right edge. Independent integer-hash salts (HomeX,
    // Phase) keep home and fall phase decorrelated so columns don't lock-step.
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float homeX = (cell + jit) * 2.0f - 1.0f;
    float phaseOff = PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Phase);

    // F4: depth-biased terminal velocity (near flakes fall a touch faster) on a
    // wider per-particle spread. M1 owns the eased descent below; these only
    // multiply the rate, so they layer rather than fight.
    float rateVar = .6f + .8f * PTrait(ctx.particleIndex, ctx.styleIndex, Trait::JitterDist);
    fall = std::fmod(
        ctx.timeScaled * fallRate * ctx.speedVar * rateVar * (.85f + .30f * ctx.depth) + phaseOff,
        1.0f);

    // M1: two incommensurate sway octaves + slow FBM cross-drift so the lateral
    // path bends instead of tracing one clean sine. F2 scales the sway AMPLITUDE
    // by depth (parallax) without touching any sin time argument.
    float par = ctx.DepthParallaxScale();
    float ph = PHash01(ctx.golden * 2.3f + 1.7f) * TWO_PI;
    float sway = swayAmp * par *
                 (.7f * std::sin(ctx.timeScaled * swayRate + ctx.golden * 3.0f) +
                  .3f * std::sin(ctx.timeScaled * swayRate * 1.93f + ph));
    float drift = (FBMNoise(ctx.golden * .7f, fall * 2.0f + ctx.timeScaled * .05f, 3) - .5f) * par;

    // M1: gentle non-constant descent (air resistance hesitation), whisper-level.
    float fy = Saturate(fall + .06f * std::sin(fall * TWO_PI + ctx.golden));

    edge = (std::min)(SmoothStep(fall * 6.0f), SmoothStep((1.0f - fall) * 6.0f));
    float x = ctx.center.x + (homeX + sway + drift * swayAmp * .6f) * ctx.radiusX;
    float y = ctx.center.y + (-1.0f + 2.0f * fy) * ctx.radiusY;
    return ImVec2(x, y);
}

// Helper: draw the type's sprite (crisp core + additive backlight halo + rare
// glint), or a soft orb fallback if no texture loaded. See DrawHaloThenSprite.
static void DrawWeatherSprite(
    const ParticleContext& ctx, const ImVec2& pos, float finalSize, int a, float rotation)
{
    DrawHaloThenSprite(ctx, pos, finalSize, a, rotation);
}

// Fast straight downpour with a touch of wind drift. The streak sprite stays
// axis-aligned (no spin) so its pixels read crisp as it falls.
static void RenderRainParticle(const ParticleContext& ctx)
{
    float fall, edge;
    ImVec2 pos = FallLayout(ctx, 1.10f, .05f, 1.4f, fall, edge);
    pos.x += .14f * (fall - .5f) * ctx.radiusX;  // wind drift accumulates with the fall

    float finalAlpha = ctx.alpha * edge * (.7f + .3f * ctx.alphaVariation) * ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, pos, finalSize, a, .0f);
}

// Slow flakes that sway side to side as they settle.
static void RenderSnowParticle(const ParticleContext& ctx)
{
    float fall, edge;
    ImVec2 pos = FallLayout(ctx, .32f, .16f, .9f, fall, edge);

    float finalAlpha = ctx.alpha * edge * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize *
                      (.92f + .08f * std::sin(ctx.timeScaled * 1.1f + ctx.golden * 2.0f)) *
                      ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, pos, finalSize, a, .0f);
}

// Puffs that start low, rise, expand, and dissolve.
static void RenderSmokeParticle(const ParticleContext& ctx)
{
    // M2: stratified home (scaled to the narrower smoke column) + decorrelated
    // rise phase from an independent integer-hash salt.
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float homeX = ((cell + jit) * 2.0f - 1.0f) * .7f;
    float phaseOff = PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Phase);
    float rise = std::fmod(ctx.timeScaled * .26f * ctx.speedVar + phaseOff, 1.0f);
    float drift = .22f * std::sin(ctx.timeScaled * .5f + ctx.golden * 2.0f) * rise;
    float x = ctx.center.x + (homeX + drift) * ctx.radiusX;
    float y = ctx.center.y + (.85f - 1.85f * rise) * ctx.radiusY;  // bottom -> top

    float fade = SmoothStep(rise * 4.0f) * SmoothStep((1.0f - rise) * 3.0f);
    float finalAlpha = ctx.alpha * fade * .8f * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    // expands as it rises
    float finalSize = ctx.particleSize * (.7f + 1.1f * rise) * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, .0f);
}

// Leaves that tumble down with a wide sway and a gentle flutter rotation.
static void RenderLeafParticle(const ParticleContext& ctx)
{
    float fall, edge;
    ImVec2 pos = FallLayout(ctx, .42f, .24f, 1.1f, fall, edge);

    float finalAlpha = ctx.alpha * edge * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    float rotation = .5f * std::sin(ctx.timeScaled * 1.3f * ctx.speedVar + ctx.golden);  // flutter
    DrawWeatherSprite(ctx, pos, finalSize, a, rotation);
}

// Petals drifting down with a broad sway and a slow continuous spin.
static void RenderCherryBlossomParticle(const ParticleContext& ctx)
{
    float fall, edge;
    ImVec2 pos = FallLayout(ctx, .34f, .26f, .8f, fall, edge);

    float finalAlpha = ctx.alpha * edge * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    float rotation = ctx.timeScaled * .5f * ctx.dirSign * ctx.speedVar + ctx.golden;  // slow spin
    DrawWeatherSprite(ctx, pos, finalSize, a, rotation);
}

// A shimmering band that flows horizontally near the top of the aura, riding a
// slow vertical wave. No spin -- it reads as a curtain of light.
static void RenderAuroraParticle(const ParticleContext& ctx)
{
    // M2: stratified flow start-offset (one band slot per particle) + an
    // independent integer-hash salt for the vertical band offset.
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float startOff = cell + jit;
    float flow =
        std::fmod(ctx.timeScaled * .18f * ctx.speedVar * ctx.dirSign + startOff + 1.0f, 1.0f);
    float bandY =
        -.40f + .30f * PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Bob);  // upper third
    float par = ctx.DepthParallaxScale();                                      // F2: amplitude only
    float wave = .12f * par * std::sin(flow * TWO_PI * 1.5f + ctx.timeScaled * .6f + ctx.golden);
    // F3: shared per-plate air current, depth-weighted.
    float x =
        ctx.center.x + (-1.0f + 2.0f * flow) * ctx.radiusX + ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (bandY + wave) * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float edge = (std::min)(SmoothStep(flow * 6.0f), SmoothStep((1.0f - flow) * 6.0f));
    float shimmer = .5f + .5f * std::sin(ctx.timeScaled * 1.5f + ctx.golden * 2.0f);
    float finalAlpha =
        ctx.alpha * edge * (.45f + .55f * shimmer) * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize =
        ctx.particleSize * (.95f + .12f * shimmer) * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, .0f);
}

// Near-still motes: a very slow orbit plus a tiny two-axis drift, gently
// twinkling. The faint glint sprite suits the dim, dusty mood.
static void RenderDustParticle(const ParticleContext& ctx)
{
    float orbit = ctx.phase + ctx.timeScaled * .04f * ctx.speedVar * ctx.dirSign;
    float par = ctx.DepthParallaxScale();  // F2: amplitude only
    float driftX = .04f * par * std::sin(ctx.timeScaled * .20f + ctx.golden);
    float driftY = .04f * par * std::sin(ctx.timeScaled * .17f + ctx.bobPhase);
    // F3: shared per-plate air current, depth-weighted.
    float x = ctx.center.x + (std::cos(orbit) * ctx.radialAnchor + driftX) * ctx.radiusX +
              ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (std::sin(orbit) * ctx.radialAnchor + driftY) * ctx.radiusY +
              ctx.airY * ctx.depth * ctx.radiusY;

    float twinkle = .4f + .6f * (.5f + .5f * std::sin(ctx.timeScaled * .7f + ctx.golden * 2.0f));
    float finalAlpha = ctx.alpha * twinkle * .7f * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize = ctx.particleSize * .85f * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, .0f);
}

// Soft luminous motes drifting on a lazy Brownian wander with a breathing
// glow -- the calm, premium "ambient sparkle" layer.
static void RenderMoteParticle(const ParticleContext& ctx)
{
    float omega = .10f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    float par = ctx.DepthParallaxScale();  // F2: amplitude only
    float wanderX = (std::sin(ctx.timeScaled * .43f * ctx.speedVar + ctx.golden) +
                     .6f * std::sin(ctx.timeScaled * .67f + ctx.golden * 1.7f)) *
                    .05f * par;
    float wanderY = (std::sin(ctx.timeScaled * .49f * ctx.speedVar + ctx.golden * 2.1f) +
                     .6f * std::sin(ctx.timeScaled * .59f + ctx.bobPhase)) *
                    .05f * par;
    float radiusMod =
        ctx.radialAnchor * (.94f + .06f * par * std::sin(ctx.timeScaled * .5f + ctx.golden));
    // F3: shared per-plate air current, depth-weighted.
    float x = ctx.center.x + std::cos(orbit) * ctx.radiusX * radiusMod + wanderX * ctx.radiusX +
              ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + std::sin(orbit * .9f) * ctx.radiusY * radiusMod +
              wanderY * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float glow = .5f + .5f * std::sin(ctx.timeScaled * .7f * ctx.speedVar + ctx.golden * 2.0f);
    float finalAlpha = ctx.alpha * (.4f + .6f * glow) * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize = ctx.particleSize * (.92f + .14f * glow) * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    // C3: whisper candle-warm pulse at the glow apex (Mote glow only).
    ParticleContext warm = ctx;
    warm.r = std::min(255, ctx.r + (int)(14 * glow * ctx.warmth));
    DrawHaloThenSprite(warm, ImVec2(x, y), finalSize, a, .0f);
}

void DrawParticleAura(const ParticleAuraParams& params)
{
    if (!params.list || params.alpha <= .05f || params.particleCount <= 0)
    {
        return;
    }

    int texStyleId = static_cast<int>(params.style);
    bool useTextures = params.useParticleTextures && ParticleTextures::IsInitialized();
    int texCount = useTextures ? ParticleTextures::GetTextureCount(texStyleId) : 0;
    bool hasTextures = (texCount > 0);

    // Map settings int (0=Additive, 1=Screen, 2=Alpha) to ParticleTextures::BlendMode
    ParticleTextures::BlendMode texBlend = ParticleTextures::BlendMode::Additive;
    if (params.blendMode == 1)
    {
        texBlend = ParticleTextures::BlendMode::Screen;
    }
    else if (params.blendMode == 2)
    {
        texBlend = ParticleTextures::BlendMode::Alpha;
    }

    float alpha = params.alpha;
    if (params.enabledStyleCount > 1)
    {
        alpha /= (1.0f + .10f * (params.enabledStyleCount - 1));
    }

    int baseR = (params.color >> IM_COL32_R_SHIFT) & 0xFF;
    int baseG = (params.color >> IM_COL32_G_SHIFT) & 0xFF;
    int baseB = (params.color >> IM_COL32_B_SHIFT) & 0xFF;

    bool hasSecondaryColor = (params.colorSecondary != 0);
    int baseR2 = hasSecondaryColor ? ((params.colorSecondary >> IM_COL32_R_SHIFT) & 0xFF) : baseR;
    int baseG2 = hasSecondaryColor ? ((params.colorSecondary >> IM_COL32_G_SHIFT) & 0xFF) : baseG;
    int baseB2 = hasSecondaryColor ? ((params.colorSecondary >> IM_COL32_B_SHIFT) & 0xFF) : baseB;

    float timeScaled = params.time * params.speed;

    // F3: shared per-plate air current computed ONCE (2 sins/plate, amortized
    // across every particle). Skipped for Spark (ballistic) and the directional
    // fall/rise types -- only the orbiting/flow types read airX/airY.
    float airX = .055f * std::sin(timeScaled * .13f);
    float airY = .045f * std::sin(timeScaled * .11f + 1.7f);

    // Radial band for this style: with several styles enabled each one gets
    // its own annulus so the styles stay readable instead of mushing together.
    float styleBandT = (params.enabledStyleCount > 0)
                           ? (static_cast<float>(params.styleIndex) + .5f) /
                                 static_cast<float>(params.enabledStyleCount)
                           : .5f;
    float bandFloor = std::clamp(.58f + .20f * styleBandT, .58f, .88f);
    if (params.enabledStyleCount <= 1)
    {
        bandFloor = SoloBandFloor(params.style);
    }
    float counterChance = CounterRotateChance(params.style);

    for (int i = 0; i < params.particleCount; ++i)
    {
        // Quasi-random angular placement via the golden angle (~137.508 deg),
        // minimal clustering. Constants:
        //   2.399963f  = golden angle in radians (~ pi(3-sqrt(5)))
        //   97         = prime per-style offset, prevents pattern repetition
        float golden = (float)(i + params.styleIndex * 97) * 2.399963f;
        float phase = golden + PTrait(i, params.styleIndex, Trait::Phase) * 1.2f;

        float radialAnchor = AnnulusRadius(bandFloor, PTrait(i, params.styleIndex, Trait::Radial));
        float speedVar = .72f + .56f * PTrait(i, params.styleIndex, Trait::Speed);
        float dirSign =
            (PTrait(i, params.styleIndex, Trait::Direction) < counterChance) ? -1.0f : 1.0f;
        float depth = PTrait(i, params.styleIndex, Trait::Depth);
        float sizeVar = .85f + .30f * PTrait(i, params.styleIndex, Trait::Size);
        float jitterAngle = PTrait(i, params.styleIndex, Trait::JitterAngle) * TWO_PI;
        float jitterDist = PTrait(i, params.styleIndex, Trait::JitterDist) * .22f;
        float bobPhase = PTrait(i, params.styleIndex, Trait::Bob) * TWO_PI;

        float alphaVariation = .6f + .4f * (.5f + .5f * std::sin(golden * 1.7f + timeScaled * .3f));

        // C1: depth-keyed warm(primary)<->cool(secondary) temperature lerp that
        // finally CONSUMES the dead secondary color, plus a tiny decorrelated
        // micro temperature jitter. This replaces the old Rec.709 hue-rotation +
        // Rec.601 satmod block (a net perf win, and it harmonizes particles with
        // the tier hero/highlight instead of rotating away from the palette).
        // Far motes sit in the deeper primary, near ones lean to the luminous
        // highlight. `warmth` (C4) scales the whole mix; 0 = monochrome tier.
        float tMix = (.20f + .55f * depth) * params.colorWarmth;
        int r = std::clamp(baseR + (int)((baseR2 - baseR) * tMix), 0, 255);
        int g = std::clamp(baseG + (int)((baseG2 - baseG) * tMix), 0, 255);
        int b = std::clamp(baseB + (int)((baseB2 - baseB) * tMix), 0, 255);
        // Decorrelated micro temp jitter via an independent salt: warm push R,
        // cool pull B -- never a full hue spin.
        float hj = (PTrait(i, params.styleIndex, Trait::Hue) - .5f) * 2.0f;
        r = std::clamp(r + (int)(8 * hj * params.colorWarmth), 0, 255);
        b = std::clamp(b - (int)(8 * hj * params.colorWarmth), 0, 255);

        ParticleContext ctx{.list = params.list,
                            .center = params.center,
                            .radiusX = params.radiusX,
                            .radiusY = params.radiusY,
                            .alpha = alpha,
                            .particleSize = params.particleSize,
                            .timeScaled = timeScaled,
                            .texStyleId = texStyleId,
                            .hasTextures = hasTextures,
                            .particleIndex = i,
                            .particleCount = params.particleCount,
                            .styleIndex = params.styleIndex,
                            .golden = golden,
                            .phase = phase,
                            .minRadius = bandFloor,
                            .radialAnchor = radialAnchor,
                            .jitterAngle = jitterAngle,
                            .jitterDist = jitterDist,
                            .alphaVariation = alphaVariation,
                            .speedVar = speedVar,
                            .dirSign = dirSign,
                            .depth = depth,
                            .sizeVar = sizeVar,
                            .bobPhase = bobPhase,
                            .r = r,
                            .g = g,
                            .b = b,
                            .r2 = baseR2,
                            .g2 = baseG2,
                            .b2 = baseB2,
                            .hasSecondaryColor = hasSecondaryColor,
                            .texBlendMode = texBlend,
                            .depthStrength = params.depthStrength,
                            .warmth = params.colorWarmth,
                            .glowStrength = params.glowStrength,
                            .glowSize = params.glowSize,
                            .shineThreshold = params.shineThreshold,
                            .airX = airX,
                            .airY = airY};

        switch (params.style)
        {
            case Settings::ParticleStyle::Firefly:
            default:
                RenderFireflyParticle(ctx);
                break;
            case Settings::ParticleStyle::Rain:
                RenderRainParticle(ctx);
                break;
            case Settings::ParticleStyle::Snow:
                RenderSnowParticle(ctx);
                break;
            case Settings::ParticleStyle::Smoke:
                RenderSmokeParticle(ctx);
                break;
            case Settings::ParticleStyle::Spark:
                RenderSparkParticle(ctx);
                break;
            case Settings::ParticleStyle::Wisp:
                RenderWispParticle(ctx);
                break;
            case Settings::ParticleStyle::Leaf:
                RenderLeafParticle(ctx);
                break;
            case Settings::ParticleStyle::Aurora:
                RenderAuroraParticle(ctx);
                break;
            case Settings::ParticleStyle::CherryBlossom:
                RenderCherryBlossomParticle(ctx);
                break;
            case Settings::ParticleStyle::Dust:
                RenderDustParticle(ctx);
                break;
            case Settings::ParticleStyle::Mote:
                RenderMoteParticle(ctx);
                break;
        }
    }
}

}  // namespace TextEffects
