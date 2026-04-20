#include "TextEffectsInternal.h"

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

// Draw soft glowing orb with gradient layers
static void DrawSoftOrb(
    ImDrawList* list, const ImVec2& pos, float size, int r, int g, int b, int baseAlpha)
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
    // Bright core
    list->AddCircleFilled(pos, size * .25f, IM_COL32(255, 255, 255, baseAlpha / 2), 12);
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

// Shared per-particle state passed to each style's render function.
struct ParticleContext
{
    ImDrawList* list;
    ImVec2 center;
    float radiusX, radiusY;
    float alpha, particleSize, timeScaled;
    int texStyleId, texCount;
    bool hasTextures;
    int particleIndex;
    float golden, phase;
    float minRadius, radialAnchor;
    float jitterAngle, jitterDist;
    float alphaVariation;
    int r, g, b;
    int r2, g2, b2;  // Secondary gradient color
    bool hasSecondaryColor;
    ParticleTextures::BlendMode texBlendMode;
};

// Twinkling stars with multi-frequency twinkle and rotation.
static void RenderStarParticle(const ParticleContext& ctx)
{
    float orbit = ctx.phase + ctx.timeScaled * .5f;
    float radiusWave = .5f + .5f * std::sin(ctx.golden);
    float radiusMod =
        ctx.minRadius + (1.0f - ctx.minRadius) * (.72f * ctx.radialAnchor + .28f * radiusWave);
    float x = ctx.center.x + std::cos(orbit) * ctx.radiusX * radiusMod +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist;
    float y = ctx.center.y + std::sin(orbit) * ctx.radiusY * radiusMod +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist;

    float twinkle1 = std::sin(ctx.timeScaled * 3.0f + ctx.golden * 3.0f);
    float twinkle2 = std::sin(ctx.timeScaled * 5.0f + ctx.golden * 2.0f) * .3f;
    float twinkle = .5f + .5f * (twinkle1 + twinkle2) / 1.3f;
    if (twinkle < .1f)
    {
        return;
    }

    float finalAlpha = ctx.alpha * (.3f + .7f * twinkle) * ctx.alphaVariation;
    float breathe = 1.0f + .08f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f);
    float finalSize = ctx.particleSize * breathe;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    int glowA = std::clamp((int)(finalAlpha * 60.0f), 0, 255);

    // Gradient color from tier left/right
    float phaseLerp = .5f + .5f * std::sin(ctx.phase);
    int sr, sg, sb;
    if (ctx.hasSecondaryColor)
    {
        float brightness = twinkle * .6f + .4f;
        sr = std::clamp((int)(ctx.r + (ctx.r2 - ctx.r) * phaseLerp * brightness), 0, 255);
        sg = std::clamp((int)(ctx.g + (ctx.g2 - ctx.g) * phaseLerp * brightness), 0, 255);
        sb = std::clamp((int)(ctx.b + (ctx.b2 - ctx.b) * phaseLerp * brightness), 0, 255);
    }
    else
    {
        float brightness = twinkle * .6f + .4f;
        sr = std::clamp((int)(80 + 175 * brightness * brightness), 0, 255);
        sg = std::clamp((int)(120 + 135 * brightness), 0, 255);
        sb = std::clamp((int)(180 + 75 * brightness), 0, 255);
    }

    float rotation = ctx.timeScaled * .5f + ctx.golden;

    if (ctx.hasTextures)
    {
        float prevRotation = rotation - .15f;
        int trailA = std::clamp(static_cast<int>(a * .25f), 0, 255);
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(x, y),
                                              finalSize * 6.0f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, trailA),
                                              ctx.texBlendMode,
                                              prevRotation);
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(x, y),
                                              finalSize * 6.0f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, a),
                                              ctx.texBlendMode,
                                              rotation);
    }
    else
    {
        if (ctx.particleIndex % 3 == 0)
        {
            DrawStar6(ctx.list,
                      ImVec2(x, y),
                      finalSize,
                      IM_COL32(sr, sg, sb, a),
                      IM_COL32(sr, sg, sb, glowA),
                      rotation);
        }
        else
        {
            DrawStar4(ctx.list,
                      ImVec2(x, y),
                      finalSize * .9f,
                      IM_COL32(sr, sg, sb, a),
                      IM_COL32(sr, sg, sb, glowA),
                      rotation);
        }
        if (twinkle > .85f)
        {
            float flashSize = finalSize * .3f * (twinkle - .85f) / .15f;
            ctx.list->AddCircleFilled(ImVec2(x, y), flashSize, IM_COL32(220, 240, 255, a / 2), 8);
        }
    }
}

// Yellowish sparks that shoot outward with trailing embers.
static void RenderSparkParticle(const ParticleContext& ctx)
{
    float sparkTime = ctx.timeScaled * 2.0f + ctx.golden;
    float sparkPhase = std::fmod(sparkTime, TWO_PI);
    float life = sparkPhase / TWO_PI;

    float sparkMinDist = std::clamp(ctx.minRadius - .05f, .52f, .85f);
    float dist = sparkMinDist + life * (1.0f - sparkMinDist);
    float baseAngle = ctx.phase + std::sin(ctx.golden * 2.0f) * .5f;
    float curveAngle = baseAngle + life * .3f * std::sin(ctx.golden);

    float x = ctx.center.x + std::cos(curveAngle) * ctx.radiusX * dist +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist * .5f;
    float y = ctx.center.y + std::sin(curveAngle) * ctx.radiusY * dist - life * ctx.radiusY * .4f +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist * .5f;

    float flicker = .8f + .2f * std::sin(ctx.timeScaled * 15.0f + ctx.golden * 5.0f);
    float finalAlpha = ctx.alpha * (1.0f - life * life) * flicker * ctx.alphaVariation;
    if (finalAlpha < .05f)
    {
        return;
    }

    float finalSize =
        ctx.particleSize * (1.0f + .08f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f));
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    float heatFade = 1.0f - life * .5f;
    int sr = std::clamp((int)(255 * heatFade), 180, 255);
    int sg = std::clamp((int)(220 * heatFade - life * 80), 120, 220);
    int sb = std::clamp((int)(80 - life * 60), 20, 80);

    if (ctx.hasTextures)
    {
        float trailDx = -std::cos(curveAngle);
        float trailDy = -std::sin(curveAngle);
        float trailSpacing = finalSize * 3.6f;
        for (int t = 2; t >= 1; --t)
        {
            float tf = static_cast<float>(t) / 3.0f;
            float tx = x + trailDx * trailSpacing * tf;
            float ty = y + trailDy * trailSpacing * tf;
            int trailA = std::clamp(static_cast<int>(a * (.3f - .1f * t)), 0, 255);
            ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                                  ImVec2(tx, ty),
                                                  finalSize * 6.0f,
                                                  ctx.texStyleId,
                                                  ctx.particleIndex,
                                                  IM_COL32(ctx.r, ctx.g, ctx.b, trailA),
                                                  ctx.texBlendMode,
                                                  curveAngle);
        }
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(x, y),
                                              finalSize * 6.0f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, a),
                                              ctx.texBlendMode,
                                              curveAngle);
    }
    else
    {
        DrawSpark(ctx.list, ImVec2(x, y), finalSize, curveAngle, sr, sg, sb, a, life);
    }
}

// Ethereal flowing wisps with pale/blue tint and trailing echoes.
static void RenderWispParticle(const ParticleContext& ctx)
{
    float wispTime = ctx.timeScaled * .3f;
    float wave1 = std::sin(wispTime + ctx.golden) * .3f;
    float wave2 = std::sin(wispTime * 1.7f + ctx.golden * 1.3f) * .15f;
    float orbit = ctx.phase + wispTime + wave1 + wave2;

    float radiusWave = .5f + .5f * std::sin(ctx.golden + wispTime * .5f);
    float radiusMod =
        ctx.minRadius + (1.0f - ctx.minRadius) * (.78f * ctx.radialAnchor + .22f * radiusWave);
    float x = ctx.center.x + std::cos(orbit) * ctx.radiusX * radiusMod +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist;
    float y = ctx.center.y + std::sin(orbit * .7f) * ctx.radiusY * radiusMod +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist;

    float pulse = .52f + .28f * std::sin(wispTime * 2.0f + ctx.golden * 2.0f);
    float finalAlpha = ctx.alpha * pulse * ctx.alphaVariation * .82f;
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize =
        ctx.particleSize * (.88f + .06f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f));
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    int wr = std::min(255, ctx.r + 24);
    int wg = std::min(255, ctx.g + 30);
    int wb = std::min(255, ctx.b + 36);

    float moveAngle = orbit + wave1 * 2.0f;
    float trailLength = 1.12f + .32f * std::sin(ctx.golden);

    if (ctx.hasTextures)
    {
        float echoDist = finalSize * 5.0f;
        float ex = x - std::cos(moveAngle) * echoDist;
        float ey = y - std::sin(moveAngle) * echoDist;
        int echoA = std::clamp(static_cast<int>(a * .24f), 0, 255);
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(ex, ey),
                                              finalSize * 4.8f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, echoA),
                                              ctx.texBlendMode,
                                              moveAngle);
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(x, y),
                                              finalSize * 5.8f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, a),
                                              ctx.texBlendMode,
                                              moveAngle);
    }
    else
    {
        DrawWisp(ctx.list, ImVec2(x, y), finalSize, moveAngle, wr, wg, wb, a, trailLength);
    }
}

// Orbiting magical rune symbols with pulsing surge.
static void RenderRuneParticle(const ParticleContext& ctx)
{
    float runeOrbit = ctx.phase + ctx.timeScaled * .4f;
    float wobble = std::sin(ctx.timeScaled + ctx.golden) * .1f;
    float floatY = std::sin(ctx.timeScaled * 1.5f + ctx.golden * 2.0f) * ctx.radiusY * .08f;

    float x = ctx.center.x + std::cos(runeOrbit + wobble) * ctx.radiusX * .9f +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist;
    float y = ctx.center.y + std::sin(runeOrbit + wobble) * ctx.radiusY * .65f + floatY +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist;

    float pulse = .7f + .3f * std::sin(ctx.timeScaled * 2.0f + ctx.golden);
    float finalAlpha = ctx.alpha * pulse * ctx.alphaVariation;
    float finalSize =
        ctx.particleSize * (1.0f + .08f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f));
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    if (ctx.hasTextures)
    {
        float surgeCycle = std::sin(ctx.timeScaled * .35f + ctx.golden * 2.5f);
        float surgeT = std::clamp((surgeCycle - .7f) / .3f, .0f, 1.0f);
        int surgedA = std::clamp(static_cast<int>(a * (1.0f + .4f * surgeT)), 0, 255);
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(x, y),
                                              finalSize * 6.0f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, surgedA),
                                              ctx.texBlendMode,
                                              runeOrbit + wobble);
    }
    else
    {
        DrawRune(ctx.list, ImVec2(x, y), finalSize, ctx.r, ctx.g, ctx.b, a, ctx.particleIndex);
    }
}

// Soft glowing orbs with breathing motion.
static void RenderOrbParticle(const ParticleContext& ctx)
{
    float orbTime = ctx.timeScaled * .4f;
    float orbit = ctx.phase + orbTime;

    float breathe = .85f + .15f * std::sin(orbTime * 1.5f + ctx.golden);
    float floatY = std::sin(orbTime * 2.0f + ctx.golden * 1.5f) * ctx.radiusY * .1f;

    float radiusWave = .5f + .5f * std::sin(ctx.golden);
    float radiusMod =
        (ctx.minRadius + (1.0f - ctx.minRadius) * (.74f * ctx.radialAnchor + .26f * radiusWave)) *
        breathe;
    float x = ctx.center.x + std::cos(orbit) * ctx.radiusX * radiusMod +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist;
    float y = ctx.center.y + std::sin(orbit * .8f) * ctx.radiusY * radiusMod + floatY +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist;

    float glow = .5f + .5f * std::sin(orbTime * 2.0f + ctx.golden * 2.0f);
    float finalAlpha = ctx.alpha * glow * ctx.alphaVariation;
    float finalSize =
        ctx.particleSize * (1.0f + .08f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f));
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    if (ctx.hasTextures)
    {
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(x, y),
                                              finalSize * 7.0f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, a),
                                              ctx.texBlendMode,
                                              .0f);
    }
    else
    {
        DrawSoftOrb(ctx.list, ImVec2(x, y), finalSize, ctx.r, ctx.g, ctx.b, a);
    }
}

// Crystalline shapes with slow rotation, floating, and facet flash.
static void RenderCrystalParticle(const ParticleContext& ctx)
{
    float crystalTime = ctx.timeScaled * .3f;
    float orbit = ctx.phase + crystalTime;

    // Gentle floating with angular wobble
    float wobble = std::sin(crystalTime * 1.2f + ctx.golden) * .08f;
    float floatY = std::sin(crystalTime * 1.8f + ctx.golden * 1.5f) * ctx.radiusY * .1f;

    float radiusWave = .5f + .5f * std::sin(ctx.golden);
    float radiusMod =
        (ctx.minRadius + (1.0f - ctx.minRadius) * (.76f * ctx.radialAnchor + .24f * radiusWave));
    float x = ctx.center.x + std::cos(orbit + wobble) * ctx.radiusX * radiusMod +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist;
    float y = ctx.center.y + std::sin(orbit * .85f + wobble) * ctx.radiusY * radiusMod + floatY +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist;

    // Slow rotation
    float rotation = crystalTime * .4f + ctx.golden;

    // Facet flash: brief brightness pulse at specific phase
    float flashCycle = std::sin(crystalTime * 2.0f + ctx.golden * 3.0f);
    float flash = std::clamp((flashCycle - .85f) / .15f, .0f, 1.0f);

    float pulse = .7f + .3f * std::sin(crystalTime * 1.5f + ctx.golden * 2.0f);
    float finalAlpha = ctx.alpha * pulse * ctx.alphaVariation * (1.0f + flash * .3f);
    float finalSize =
        ctx.particleSize * (1.0f + .08f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f));
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    if (ctx.hasTextures)
    {
        ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                              ImVec2(x, y),
                                              finalSize * 6.0f,
                                              ctx.texStyleId,
                                              ctx.particleIndex,
                                              IM_COL32(ctx.r, ctx.g, ctx.b, a),
                                              ctx.texBlendMode,
                                              rotation);
    }
    else
    {
        // Procedural: simple hexagonal shape with glow
        int glowA = std::clamp((int)(finalAlpha * 40.0f), 0, 255);
        ctx.list->AddCircleFilled(
            ImVec2(x, y), finalSize * 1.5f, IM_COL32(ctx.r, ctx.g, ctx.b, glowA), 6);
        ctx.list->AddCircleFilled(ImVec2(x, y), finalSize, IM_COL32(ctx.r, ctx.g, ctx.b, a), 6);
        ctx.list->AddCircleFilled(ImVec2(x, y), finalSize * .3f, IM_COL32(255, 255, 255, a / 2), 6);
    }
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

    for (int i = 0; i < params.particleCount; ++i)
    {
        // Per-particle distribution uses the golden angle (~137.508 deg = pi(3-sqrt(5)) ~ 2.39996
        // rad) for quasi-random angular placement with minimal clustering. Constants:
        //   2.399963f  = golden angle in radians
        //   0.618034f  = 1/phi (golden ratio conjugate), low-discrepancy sequence for jitter
        //   97, 13, 31 = prime multipliers for per-style offset to avoid pattern repetition
        float golden = (float)(i + params.styleIndex * 97) * 2.399963f;
        float hashJitter = std::fmod((float)(i * 7 + params.styleIndex * 13) * .6180339887f, 1.0f);
        float phase = golden + hashJitter * 1.2f;

        float styleBandT = (params.enabledStyleCount > 0)
                               ? (static_cast<float>(params.styleIndex) + .5f) /
                                     static_cast<float>(params.enabledStyleCount)
                               : .5f;
        float minRadius = std::clamp(.58f + .20f * styleBandT, .58f, .88f);
        float radialSeed =
            std::fmod(static_cast<float>(i) * .6180339887f + styleBandT * .31f, 1.0f);
        // sqrt() converts uniform [0,1] to area-uniform disk distribution (standard disk sampling)
        float radialAnchor = std::sqrt(radialSeed);

        float jitterAngle =
            std::fmod((float)(i * 17 + params.styleIndex * 31) * .3819660113f, 1.0f) * TWO_PI;
        float jitterDist =
            std::fmod((float)(i * 23 + params.styleIndex * 7) * .6180339887f, 1.0f) * .25f;

        float alphaVariation = .6f + .4f * (.5f + .5f * std::sin(golden * 1.7f + timeScaled * .3f));

        int r = baseR, g = baseG, b = baseB;
        // Hue rotation in RGB space using Rec. 709 luminance weights (0.213, 0.715, 0.072).
        // The 3x3 rotation matrix preserves perceived brightness while shifting hue.
        {
            float hueShift = std::sin(golden * 2.3f + timeScaled * .25f) * .08f;
            float satMod = 1.0f + .08f * std::sin(golden * 1.5f);

            float hueAngle = hueShift * TWO_PI;
            float cosH = std::cos(hueAngle);
            float sinH = std::sin(hueAngle);

            float newR = baseR * (.213f + .787f * cosH - .213f * sinH) +
                         baseG * (.213f - .213f * cosH + .143f * sinH) +
                         baseB * (.213f - .213f * cosH - .928f * sinH);
            float newG = baseR * (.715f - .715f * cosH - .715f * sinH) +
                         baseG * (.715f + .285f * cosH + .140f * sinH) +
                         baseB * (.715f - .715f * cosH + .283f * sinH);
            float newB = baseR * (.072f - .072f * cosH + .928f * sinH) +
                         baseG * (.072f - .072f * cosH - .283f * sinH) +
                         baseB * (.072f + .928f * cosH + .072f * sinH);

            // Saturation adjustment using Rec. 601 luminance weights (intentionally different
            // from the Rec. 709 rotation above -- Rec. 601 produces more visually uniform results
            // for saturation scaling in the [0-255] integer domain)
            float gray = .299f * newR + .587f * newG + .114f * newB;
            r = std::clamp((int)(gray + (newR - gray) * satMod), 0, 255);
            g = std::clamp((int)(gray + (newG - gray) * satMod), 0, 255);
            b = std::clamp((int)(gray + (newB - gray) * satMod), 0, 255);
        }

        ParticleContext ctx{params.list,
                            params.center,
                            params.radiusX,
                            params.radiusY,
                            alpha,
                            params.particleSize,
                            timeScaled,
                            texStyleId,
                            texCount,
                            hasTextures,
                            i,
                            golden,
                            phase,
                            minRadius,
                            radialAnchor,
                            jitterAngle,
                            jitterDist,
                            alphaVariation,
                            r,
                            g,
                            b,
                            baseR2,
                            baseG2,
                            baseB2,
                            hasSecondaryColor,
                            texBlend};

        switch (params.style)
        {
            case Settings::ParticleStyle::Stars:
            default:
                RenderStarParticle(ctx);
                break;
            case Settings::ParticleStyle::Sparks:
                RenderSparkParticle(ctx);
                break;
            case Settings::ParticleStyle::Wisps:
                RenderWispParticle(ctx);
                break;
            case Settings::ParticleStyle::Runes:
                RenderRuneParticle(ctx);
                break;
            case Settings::ParticleStyle::Orbs:
                RenderOrbParticle(ctx);
                break;
            case Settings::ParticleStyle::Crystals:
                RenderCrystalParticle(ctx);
                break;
        }
    }
}

}  // namespace TextEffects
