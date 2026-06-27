/**
 * Unit tests for glyph utility functions using Google Test.
 *
 * Tests core math and color manipulation functions that are independent
 * of the game runtime.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Re-implement functions under test (same logic as TextEffects.cpp)
// ============================================================================

float Saturate(float x) {
    return std::clamp(x, 0.0f, 1.0f);
}

float SmoothStep(float t) {
    t = Saturate(t);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// ImGui color format macros
#define IM_COL32_R_SHIFT 0
#define IM_COL32_G_SHIFT 8
#define IM_COL32_B_SHIFT 16
#define IM_COL32_A_SHIFT 24
#define IM_COL32(R,G,B,A) (((unsigned int)(A)<<IM_COL32_A_SHIFT) | ((unsigned int)(B)<<IM_COL32_B_SHIFT) | ((unsigned int)(G)<<IM_COL32_G_SHIFT) | ((unsigned int)(R)<<IM_COL32_R_SHIFT))

using ImU32 = unsigned int;

ImU32 LerpColorU32(ImU32 a, ImU32 b, float t) {
    t = Saturate(t);
    const int ar = (a >> IM_COL32_R_SHIFT) & 0xFF;
    const int ag = (a >> IM_COL32_G_SHIFT) & 0xFF;
    const int ab = (a >> IM_COL32_B_SHIFT) & 0xFF;
    const int aa = (a >> IM_COL32_A_SHIFT) & 0xFF;
    const int br = (b >> IM_COL32_R_SHIFT) & 0xFF;
    const int bg = (b >> IM_COL32_G_SHIFT) & 0xFF;
    const int bb = (b >> IM_COL32_B_SHIFT) & 0xFF;
    const int ba = (b >> IM_COL32_A_SHIFT) & 0xFF;
    const int rr = (int)(ar + (br - ar) * t + 0.5f);
    const int rg = (int)(ag + (bg - ag) * t + 0.5f);
    const int rb = (int)(ab + (bb - ab) * t + 0.5f);
    const int ra = (int)(aa + (ba - aa) * t + 0.5f);
    return IM_COL32(rr, rg, rb, ra);
}

static inline float Frac(float x) { return x - std::floor(x); }

float LerpRange(float minVal, float maxVal, float t) {
    t = Saturate(t);
    return minVal + (maxVal - minVal) * t;
}

bool IsPrimaryTextBody(int r, int g, int b, int a, int maxBatchAlpha) {
    const float lum = (r * .299f + g * .587f + b * .114f) / 255.0f;
    const int maxCh = std::max({r, g, b});
    const int bodyAlphaThreshold = std::max(8, (maxBatchAlpha * 5 + 5) / 6);
    return lum >= .22f && maxCh >= 80 && a >= bodyAlphaThreshold;
}

struct ImVec4 { float x, y, z, w; };

ImVec4 HSVtoRGB(float h, float s, float v, float a) {
    h = Frac(h);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(Frac(h * 6.0f) * 2.0f - 1.0f));
    const float m = v - c;
    float r=0, g=0, b=0;
    const int i = (int)std::floor(h * 6.0f);
    switch (i % 6) {
    case 0: r=c; g=x; b=0; break;
    case 1: r=x; g=c; b=0; break;
    case 2: r=0; g=c; b=x; break;
    case 3: r=0; g=x; b=c; break;
    case 4: r=x; g=0; b=c; break;
    case 5: r=c; g=0; b=x; break;
    }
    return {r + m, g + m, b + m, a};
}

// ============================================================================
// Tests: Saturate
// ============================================================================

TEST(SaturateTest, ClampsAboveOne) {
    EXPECT_FLOAT_EQ(Saturate(1.5f), 1.0f);
    EXPECT_FLOAT_EQ(Saturate(100.0f), 1.0f);
}

TEST(SaturateTest, ClampsBelowZero) {
    EXPECT_FLOAT_EQ(Saturate(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(Saturate(-100.0f), 0.0f);
}

TEST(SaturateTest, PreservesValidRange) {
    EXPECT_FLOAT_EQ(Saturate(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(Saturate(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(Saturate(1.0f), 1.0f);
}

// ============================================================================
// Tests: SmoothStep (quintic)
// ============================================================================

TEST(SmoothStepTest, ReturnsZeroAtZero) {
    EXPECT_FLOAT_EQ(SmoothStep(0.0f), 0.0f);
}

TEST(SmoothStepTest, ReturnsOneAtOne) {
    EXPECT_FLOAT_EQ(SmoothStep(1.0f), 1.0f);
}

TEST(SmoothStepTest, ReturnsHalfAtMidpoint) {
    EXPECT_FLOAT_EQ(SmoothStep(0.5f), 0.5f);
}

TEST(SmoothStepTest, ClampsBelowZero) {
    EXPECT_FLOAT_EQ(SmoothStep(-1.0f), 0.0f);
}

TEST(SmoothStepTest, ClampsAboveOne) {
    EXPECT_FLOAT_EQ(SmoothStep(2.0f), 1.0f);
}

TEST(SmoothStepTest, HasZeroDerivativeAtEdges) {
    // Test that the curve is smooth at boundaries by checking nearby values
    float eps = 0.001f;
    float at0 = SmoothStep(0.0f);
    float near0 = SmoothStep(eps);
    float slope0 = (near0 - at0) / eps;
    EXPECT_LT(slope0, 0.01f);  // Derivative should be ~0 at t=0

    float at1 = SmoothStep(1.0f);
    float near1 = SmoothStep(1.0f - eps);
    float slope1 = (at1 - near1) / eps;
    EXPECT_LT(slope1, 0.01f);  // Derivative should be ~0 at t=1
}

// ============================================================================
// Tests: LerpColorU32
// ============================================================================

TEST(LerpColorTest, ReturnsFirstAtZero) {
    ImU32 a = IM_COL32(100, 150, 200, 255);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 0.0f);
    EXPECT_EQ(result, a);
}

TEST(LerpColorTest, ReturnsSecondAtOne) {
    ImU32 a = IM_COL32(100, 150, 200, 255);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 1.0f);
    EXPECT_EQ(result, b);
}

TEST(LerpColorTest, InterpolatesAtHalf) {
    ImU32 a = IM_COL32(0, 0, 0, 0);
    ImU32 b = IM_COL32(200, 100, 50, 128);
    ImU32 result = LerpColorU32(a, b, 0.5f);

    int r = (result >> IM_COL32_R_SHIFT) & 0xFF;
    int g = (result >> IM_COL32_G_SHIFT) & 0xFF;
    int bl = (result >> IM_COL32_B_SHIFT) & 0xFF;
    int al = (result >> IM_COL32_A_SHIFT) & 0xFF;

    EXPECT_EQ(r, 100);
    EXPECT_EQ(g, 50);
    EXPECT_EQ(bl, 25);
    EXPECT_EQ(al, 64);
}

TEST(LerpColorTest, ClampsT) {
    ImU32 a = IM_COL32(100, 100, 100, 255);
    ImU32 b = IM_COL32(200, 200, 200, 255);

    ImU32 below = LerpColorU32(a, b, -1.0f);
    ImU32 above = LerpColorU32(a, b, 2.0f);

    EXPECT_EQ(below, a);
    EXPECT_EQ(above, b);
}

// ============================================================================
// Tests: HSVtoRGB
// ============================================================================

TEST(HSVtoRGBTest, RedAtHueZero) {
    ImVec4 rgb = HSVtoRGB(0.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 1.0f, 0.01f);  // R = 1
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);  // G = 0
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);  // B = 0
}

TEST(HSVtoRGBTest, GreenAtHueThird) {
    ImVec4 rgb = HSVtoRGB(1.0f/3.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);  // R = 0
    EXPECT_NEAR(rgb.y, 1.0f, 0.01f);  // G = 1
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);  // B = 0
}

TEST(HSVtoRGBTest, BlueAtHueTwoThirds) {
    ImVec4 rgb = HSVtoRGB(2.0f/3.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);  // R = 0
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);  // G = 0
    EXPECT_NEAR(rgb.z, 1.0f, 0.01f);  // B = 1
}

TEST(HSVtoRGBTest, WhiteAtZeroSaturation) {
    ImVec4 rgb = HSVtoRGB(0.5f, 0.0f, 1.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 1.0f, 0.01f);
    EXPECT_NEAR(rgb.y, 1.0f, 0.01f);
    EXPECT_NEAR(rgb.z, 1.0f, 0.01f);
}

TEST(HSVtoRGBTest, BlackAtZeroValue) {
    ImVec4 rgb = HSVtoRGB(0.5f, 1.0f, 0.0f, 1.0f);
    EXPECT_NEAR(rgb.x, 0.0f, 0.01f);
    EXPECT_NEAR(rgb.y, 0.0f, 0.01f);
    EXPECT_NEAR(rgb.z, 0.0f, 0.01f);
}

TEST(HSVtoRGBTest, PreservesAlpha) {
    ImVec4 rgb = HSVtoRGB(0.0f, 1.0f, 1.0f, 0.5f);
    EXPECT_NEAR(rgb.w, 0.5f, 0.01f);
}

TEST(HSVtoRGBTest, WrapsHue) {
    ImVec4 rgb1 = HSVtoRGB(0.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 rgb2 = HSVtoRGB(1.0f, 1.0f, 1.0f, 1.0f);  // Should wrap to same as 0
    ImVec4 rgb3 = HSVtoRGB(2.0f, 1.0f, 1.0f, 1.0f);  // Should also wrap

    EXPECT_NEAR(rgb1.x, rgb2.x, 0.01f);
    EXPECT_NEAR(rgb1.y, rgb2.y, 0.01f);
    EXPECT_NEAR(rgb1.z, rgb2.z, 0.01f);

    EXPECT_NEAR(rgb1.x, rgb3.x, 0.01f);
}

// ============================================================================
// Tests: Frac
// ============================================================================

TEST(FracTest, ReturnsDecimalPart) {
    EXPECT_NEAR(Frac(1.25f), 0.25f, 0.0001f);
    EXPECT_NEAR(Frac(3.75f), 0.75f, 0.0001f);
}

TEST(FracTest, HandlesNegative) {
    EXPECT_NEAR(Frac(-0.25f), 0.75f, 0.0001f);  // -0.25 - floor(-0.25) = -0.25 - (-1) = 0.75
}

TEST(FracTest, HandlesWholeNumbers) {
    EXPECT_NEAR(Frac(5.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(Frac(0.0f), 0.0f, 0.0001f);
}

// ============================================================================
// Tests: LerpRange
// ============================================================================

TEST(LerpRangeTest, ReturnsMinimumAtZero) {
    EXPECT_FLOAT_EQ(LerpRange(0.2f, 0.6f, 0.0f), 0.2f);
}

TEST(LerpRangeTest, ReturnsMaximumAtOne) {
    EXPECT_FLOAT_EQ(LerpRange(0.2f, 0.6f, 1.0f), 0.6f);
}

TEST(LerpRangeTest, InterpolatesMidpoint) {
    EXPECT_NEAR(LerpRange(0.15f, 0.60f, 0.5f), 0.375f, 0.0001f);
}

// ============================================================================
// Tests: IsPrimaryTextBody
// ============================================================================

TEST(TextBodyHeuristicTest, IncludesMainFillAtCurrentBatchAlpha) {
    EXPECT_TRUE(IsPrimaryTextBody(210, 180, 120, 96, 96));
}

TEST(TextBodyHeuristicTest, ExcludesLowAlphaSupportPass) {
    EXPECT_FALSE(IsPrimaryTextBody(210, 180, 120, 48, 255));
}

TEST(TextBodyHeuristicTest, ExcludesDarkOutlinePass) {
    EXPECT_FALSE(IsPrimaryTextBody(24, 24, 24, 255, 255));
}

// ============================================================================
// Re-implement particle distribution math (same logic as TextEffectsParticle.cpp
// and ParticleTextures.cpp)
// ============================================================================

uint32_t PMixU32(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

float PTrait(int particleIndex, int styleIndex, uint32_t salt) {
    uint32_t h = PMixU32(static_cast<uint32_t>(particleIndex) +
                         PMixU32(static_cast<uint32_t>(styleIndex) + PMixU32(salt)));
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

float AnnulusRadius(float bandFloor, float u) {
    float f2 = bandFloor * bandFloor;
    return std::sqrt(f2 + (1.0f - f2) * u);
}

float PInterleavedGradientNoise(int x, int y) {
    float f = 0.06711056f * static_cast<float>(x) + 0.00583715f * static_cast<float>(y);
    f -= std::floor(f);
    float v = 52.9829189f * f;
    return v - std::floor(v);
}

// ============================================================================
// Tests: PTrait (per-particle hash traits)
// ============================================================================

TEST(ParticleTraitTest, StaysInUnitRange) {
    for (int i = 0; i < 1000; ++i) {
        for (uint32_t salt = 1; salt <= 9; ++salt) {
            float v = PTrait(i, i % 6, salt);
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }
}

TEST(ParticleTraitTest, IsDeterministic) {
    EXPECT_FLOAT_EQ(PTrait(17, 3, 4), PTrait(17, 3, 4));
    EXPECT_FLOAT_EQ(PTrait(0, 0, 1), PTrait(0, 0, 1));
}

TEST(ParticleTraitTest, MeanIsCentered) {
    double sum = 0.0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
        sum += PTrait(i, 2, 4);
    }
    EXPECT_NEAR(sum / n, 0.5, 0.05);
}

TEST(ParticleTraitTest, SaltStreamsAreDecorrelated) {
    const int n = 1000;
    double sumA = 0.0, sumB = 0.0;
    for (int i = 0; i < n; ++i) {
        sumA += PTrait(i, 1, 2);
        sumB += PTrait(i, 1, 4);
    }
    const double meanA = sumA / n;
    const double meanB = sumB / n;
    double cov = 0.0, varA = 0.0, varB = 0.0;
    for (int i = 0; i < n; ++i) {
        const double da = PTrait(i, 1, 2) - meanA;
        const double db = PTrait(i, 1, 4) - meanB;
        cov += da * db;
        varA += da * da;
        varB += db * db;
    }
    const double pearson = cov / std::sqrt(varA * varB);
    EXPECT_LT(std::abs(pearson), 0.1);
}

// ============================================================================
// Tests: AnnulusRadius (area-uniform band sampling)
// ============================================================================

TEST(AnnulusRadiusTest, HitsBandEdges) {
    EXPECT_NEAR(AnnulusRadius(0.58f, 0.0f), 0.58f, 1e-5f);
    EXPECT_NEAR(AnnulusRadius(0.58f, 1.0f), 1.0f, 1e-5f);
    EXPECT_NEAR(AnnulusRadius(0.30f, 0.0f), 0.30f, 1e-5f);
    EXPECT_NEAR(AnnulusRadius(0.30f, 1.0f), 1.0f, 1e-5f);
}

TEST(AnnulusRadiusTest, IsMonotonicInU) {
    float prev = AnnulusRadius(0.45f, 0.0f);
    for (int i = 1; i <= 100; ++i) {
        float r = AnnulusRadius(0.45f, static_cast<float>(i) / 100.0f);
        EXPECT_GT(r, prev);
        prev = r;
    }
}

TEST(AnnulusRadiusTest, IsAreaUniform) {
    // Equal density per unit area: the fraction of samples with r < m must
    // equal the annulus area ratio (m^2 - f^2) / (1 - f^2).
    const float f = 0.58f;
    const float m = 0.8f;
    const int n = 10000;
    int below = 0;
    for (int i = 0; i < n; ++i) {
        float u = (static_cast<float>(i) + 0.5f) / n;
        if (AnnulusRadius(f, u) < m) {
            ++below;
        }
    }
    const float expected = (m * m - f * f) / (1.0f - f * f);
    EXPECT_NEAR(static_cast<float>(below) / n, expected, 0.01f);
}

// ============================================================================
// Tests: PInterleavedGradientNoise (dither pattern)
// ============================================================================

TEST(InterleavedGradientNoiseTest, StaysInUnitRange) {
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            float v = PInterleavedGradientNoise(x, y);
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }
}

TEST(InterleavedGradientNoiseTest, MeanIsCentered) {
    double sum = 0.0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            sum += PInterleavedGradientNoise(x, y);
        }
    }
    EXPECT_NEAR(sum / (64.0 * 64.0), 0.5, 0.05);
}

// ============================================================================
// Re-implement status icon badge helpers (same logic as RendererLayout.cpp)
// ============================================================================

// Simplified mirrors of the renderer-side enums and snapshot icon config.
// Always-on slot model: every enabled slot renders, neutral/inactive states
// muted.  Keep in sync with RendererLayout.cpp::ComposeBadges.
enum class RelKind { Hostile, Neutral, Ally, Follower };
enum class LvlDelta { Weak, Even, Strong, Deadly };
enum class CritKind { NPC, Beast, Undead, Daedra, Dragon };
enum class RoleK { Commoner, Merchant, Guard };
enum class ProtK { Mortal, Protected, Essential };
enum class EngK { Idle, Alert, Combat };
enum class SneakK { Off, Hidden, Detected };

struct TestColor { float r = 1.0f, g = 1.0f, b = 1.0f; };

struct IconCfg {
    bool enabled = true;
    bool deadlyPulse = true;
    // Original three slots.
    std::string icoFollower = "shield-halved", icoAlly = "handshake";
    std::string icoHostile = "skull-crossbones";
    std::string icoWeak = "caret-down", icoStrong = "caret-up", icoDeadly = "skull";
    std::string icoBeast = "paw", icoUndead = "ghost", icoDaedra = "fire", icoDragon = "dragon";
    // Expanded always-on slots.
    std::string icoNeutral = "circle", icoHumanoid = "user", icoEven = "equals";
    std::string icoGuard = "helmet-battle", icoMerchant = "coins", icoCommoner = "house";
    std::string icoEssential = "certificate", icoProtected = "shield-check", icoMortal = "heart";
    std::string icoCombat = "swords", icoAlert = "eye", icoIdle = "moon";
    std::string icoSneakHidden = "eye-slash", icoSneakDetected = "eye";
    std::string icoSneakOff = "person-walking";
    std::string icoEncumbered = "weight-hanging", icoNormalWeight = "feather";
    std::string icoWanted = "gavel", icoBountyClear = "scale-balanced";
    TestColor colFollower{0.46f, 0.68f, 0.84f};
    TestColor colAlly{0.52f, 0.74f, 0.50f};
    TestColor colHostile{0.86f, 0.36f, 0.32f};
    TestColor colWeak{0.54f, 0.66f, 0.80f};
    TestColor colStrong{0.86f, 0.62f, 0.32f};
    TestColor colDeadly{0.90f, 0.28f, 0.24f};
    TestColor colCreature{0.80f, 0.74f, 0.62f};
    TestColor colGuard{0.60f, 0.68f, 0.84f};
    TestColor colMerchant{0.84f, 0.74f, 0.42f};
    TestColor colEssential{0.86f, 0.78f, 0.46f};
    TestColor colProtected{0.54f, 0.72f, 0.86f};
    TestColor colCombat{0.88f, 0.42f, 0.30f};
    TestColor colAlert{0.86f, 0.76f, 0.40f};
    TestColor colSneakHidden{0.50f, 0.64f, 0.84f};
    TestColor colSneakDetected{0.86f, 0.36f, 0.32f};
    TestColor colEncumbered{0.82f, 0.64f, 0.40f};
    TestColor colWanted{0.84f, 0.34f, 0.30f};
    TestColor colNeutral{0.56f, 0.62f, 0.70f};
    TestColor colHumanoid{0.74f, 0.68f, 0.58f};
    TestColor colCommoner{0.60f, 0.68f, 0.54f};
    TestColor colMortal{0.76f, 0.58f, 0.60f};
    TestColor colEven{0.60f, 0.70f, 0.72f};
    TestColor colIdle{0.56f, 0.60f, 0.76f};
    TestColor colSneakOff{0.64f, 0.68f, 0.60f};
    TestColor colNormalWeight{0.64f, 0.76f, 0.70f};
    TestColor colBountyClear{0.50f, 0.70f, 0.68f};
    TestColor colMuted{0.62f, 0.64f, 0.68f};
    bool relationshipEnabled = true, creatureEnabled = true, threatEnabled = true;
    bool roleEnabled = true, protectionEnabled = true, engagementEnabled = true;
    bool combatStateEnabled = true, alertStateEnabled = true;
    bool sneakEnabled = true, playerCombatEnabled = true;
    bool encumberedEnabled = true, bountyEnabled = true;
};

// Mirror of ActorDrawData's badge-relevant facts.
struct Facts {
    bool isPlayer = false;
    RelKind relationship = RelKind::Neutral;
    LvlDelta levelDelta = LvlDelta::Even;
    CritKind creatureKind = CritKind::NPC;
    RoleK role = RoleK::Commoner;
    ProtK protection = ProtK::Mortal;
    EngK engagement = EngK::Idle;
    SneakK sneak = SneakK::Off;
    bool playerInCombat = false;
    bool encumbered = false;
    bool wanted = false;
};

struct BadgeSlot {
    std::string icon;
    TestColor color;
    bool muted = false;
    bool pulse = false;
};

struct BadgeComposition {
    std::vector<BadgeSlot> slots;
    void push(const std::string& icon, TestColor color, bool muted, bool pulse = false) {
        if (!icon.empty()) slots.push_back({icon, color, muted, pulse});
    }
};

// Map actor facts to an ordered set of badge slots.  Mirrors
// RendererLayout.cpp::ComposeBadges -- keep the logic in sync.
BadgeComposition ComposeBadges(const Facts& d, const IconCfg& cfg) {
    BadgeComposition out{};
    if (!cfg.enabled) return out;

    auto add = [&](bool enabled, const std::string& icon, TestColor color, bool muted,
                   bool pulse = false) {
        if (enabled) out.push(icon, color, muted, pulse);
    };

    if (!d.isPlayer) {
        switch (d.relationship) {
        case RelKind::Hostile:  add(cfg.relationshipEnabled, cfg.icoHostile, cfg.colHostile, false); break;
        case RelKind::Ally:     add(cfg.relationshipEnabled, cfg.icoAlly, cfg.colAlly, false); break;
        case RelKind::Follower: add(cfg.relationshipEnabled, cfg.icoFollower, cfg.colFollower, false); break;
        case RelKind::Neutral:  add(cfg.relationshipEnabled, cfg.icoNeutral, cfg.colNeutral, true); break;
        }
        switch (d.creatureKind) {
        case CritKind::Dragon: add(cfg.creatureEnabled, cfg.icoDragon, cfg.colCreature, false); break;
        case CritKind::Daedra: add(cfg.creatureEnabled, cfg.icoDaedra, cfg.colCreature, false); break;
        case CritKind::Undead: add(cfg.creatureEnabled, cfg.icoUndead, cfg.colCreature, false); break;
        case CritKind::Beast:  add(cfg.creatureEnabled, cfg.icoBeast, cfg.colCreature, false); break;
        case CritKind::NPC:    add(cfg.creatureEnabled, cfg.icoHumanoid, cfg.colHumanoid, true); break;
        }
        switch (d.role) {
        case RoleK::Guard:    add(cfg.roleEnabled, cfg.icoGuard, cfg.colGuard, false); break;
        case RoleK::Merchant: add(cfg.roleEnabled, cfg.icoMerchant, cfg.colMerchant, false); break;
        case RoleK::Commoner: add(cfg.roleEnabled, cfg.icoCommoner, cfg.colCommoner, true); break;
        }
        switch (d.protection) {
        case ProtK::Essential: add(cfg.protectionEnabled, cfg.icoEssential, cfg.colEssential, false); break;
        case ProtK::Protected: add(cfg.protectionEnabled, cfg.icoProtected, cfg.colProtected, false); break;
        case ProtK::Mortal:    add(cfg.protectionEnabled, cfg.icoMortal, cfg.colMortal, true); break;
        }
        switch (d.levelDelta) {
        case LvlDelta::Deadly: add(cfg.threatEnabled, cfg.icoDeadly, cfg.colDeadly, false, cfg.deadlyPulse); break;
        case LvlDelta::Strong: add(cfg.threatEnabled, cfg.icoStrong, cfg.colStrong, false); break;
        case LvlDelta::Weak:   add(cfg.threatEnabled, cfg.icoWeak, cfg.colWeak, false); break;
        case LvlDelta::Even:   add(cfg.threatEnabled, cfg.icoEven, cfg.colEven, true); break;
        }
        switch (d.engagement) {
        case EngK::Combat: add(cfg.engagementEnabled && cfg.combatStateEnabled, cfg.icoCombat, cfg.colCombat, false); break;
        case EngK::Alert:  add(cfg.engagementEnabled && cfg.alertStateEnabled, cfg.icoAlert, cfg.colAlert, false); break;
        case EngK::Idle:   add(cfg.engagementEnabled, cfg.icoIdle, cfg.colIdle, true); break;
        }
        return out;
    }

    switch (d.sneak) {
    case SneakK::Detected: add(cfg.sneakEnabled, cfg.icoSneakDetected, cfg.colSneakDetected, false); break;
    case SneakK::Hidden:   add(cfg.sneakEnabled, cfg.icoSneakHidden, cfg.colSneakHidden, false); break;
    case SneakK::Off:      add(cfg.sneakEnabled, cfg.icoSneakOff, cfg.colSneakOff, true); break;
    }
    add(cfg.playerCombatEnabled, d.playerInCombat ? cfg.icoCombat : cfg.icoIdle,
        d.playerInCombat ? cfg.colCombat : cfg.colIdle, !d.playerInCombat);
    add(cfg.encumberedEnabled, d.encumbered ? cfg.icoEncumbered : cfg.icoNormalWeight,
        d.encumbered ? cfg.colEncumbered : cfg.colNormalWeight, !d.encumbered);
    add(cfg.bountyEnabled, d.wanted ? cfg.icoWanted : cfg.icoBountyClear,
        d.wanted ? cfg.colWanted : cfg.colBountyClear, !d.wanted);
    return out;
}

// Pure classifier mirrors (the game-thread RE:: reads can't be unit-tested,
// but the classification logic + priority can).  Keep in sync with
// RendererSnapshot.cpp.
ProtK ClassifyProtection(bool essential, bool prot) {
    if (essential) return ProtK::Essential;
    if (prot) return ProtK::Protected;
    return ProtK::Mortal;
}
RoleK ClassifyRole(bool guard, bool vendor) {
    if (guard) return RoleK::Guard;
    if (vendor) return RoleK::Merchant;
    return RoleK::Commoner;
}
EngK ClassifyEngagement(bool inCombat, bool weaponDrawn, int detection) {
    if (inCombat) return EngK::Combat;
    if (weaponDrawn && detection > 0) return EngK::Alert;
    return EngK::Idle;
}

// Mirror of the DrawBadges muted desaturation (toward luma by `desat`).
TestColor ApplyMutedDesat(TestColor c, float desat) {
    const float luma = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    return {c.r + (luma - c.r) * desat, c.g + (luma - c.g) * desat, c.b + (luma - c.b) * desat};
}

// ============================================================================
// Tests: ComposeBadges (always-on slot model)
// ============================================================================

TEST(ComposeBadgesTest, NpcAlwaysHasSixSlots) {
    auto s = ComposeBadges(Facts{}, IconCfg{});  // all-neutral commoner
    ASSERT_EQ(s.slots.size(), 6u);
    for (const auto& slot : s.slots) {
        EXPECT_TRUE(slot.muted);  // every neutral slot renders muted
    }
}

TEST(ComposeBadgesTest, NpcSlotOrderRelationshipFirst) {
    Facts f;
    f.relationship = RelKind::Follower;
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 6u);
    EXPECT_EQ(s.slots[0].icon, "shield-halved");  // relationship is slot 0
    EXPECT_FALSE(s.slots[0].muted);
    EXPECT_FLOAT_EQ(s.slots[0].color.b, 0.84f);  // colFollower
}

TEST(ComposeBadgesTest, NeutralRelationshipUsesRestingColor) {
    auto s = ComposeBadges(Facts{}, IconCfg{});
    EXPECT_EQ(s.slots[0].icon, "circle");
    EXPECT_TRUE(s.slots[0].muted);  // still drawn dimmed, but in its own hue
    EXPECT_FLOAT_EQ(s.slots[0].color.r, 0.56f);  // colNeutral
    EXPECT_FLOAT_EQ(s.slots[0].color.g, 0.62f);
}

TEST(ComposeBadgesTest, NewNpcSlotsLitWhenActive) {
    Facts f;
    f.role = RoleK::Merchant;
    f.protection = ProtK::Essential;
    f.engagement = EngK::Combat;
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 6u);
    EXPECT_EQ(s.slots[2].icon, "coins");        // role
    EXPECT_FALSE(s.slots[2].muted);
    EXPECT_EQ(s.slots[3].icon, "certificate");  // protection
    EXPECT_FALSE(s.slots[3].muted);
    EXPECT_EQ(s.slots[5].icon, "swords");       // engagement
    EXPECT_FALSE(s.slots[5].muted);
}

TEST(ComposeBadgesTest, DeadlyThreatPulsesWeakDoesNot) {
    Facts deadly;
    deadly.levelDelta = LvlDelta::Deadly;
    auto sd = ComposeBadges(deadly, IconCfg{});
    EXPECT_EQ(sd.slots[4].icon, "skull");  // threat is slot 4
    EXPECT_TRUE(sd.slots[4].pulse);
    Facts weak;
    weak.levelDelta = LvlDelta::Weak;
    auto sw = ComposeBadges(weak, IconCfg{});
    EXPECT_EQ(sw.slots[4].icon, "caret-down");
    EXPECT_FALSE(sw.slots[4].pulse);
}

TEST(ComposeBadgesTest, PlayerGetsFourPlayerSlots) {
    Facts f;
    f.isPlayer = true;
    f.sneak = SneakK::Hidden;
    f.playerInCombat = true;
    f.encumbered = true;
    f.wanted = true;
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 4u);
    EXPECT_EQ(s.slots[0].icon, "eye-slash");       // sneak hidden
    EXPECT_EQ(s.slots[1].icon, "swords");          // combat
    EXPECT_EQ(s.slots[2].icon, "weight-hanging");  // encumbered
    EXPECT_EQ(s.slots[3].icon, "gavel");           // wanted
    for (const auto& slot : s.slots) {
        EXPECT_FALSE(slot.muted);
    }
}

TEST(ComposeBadgesTest, PlayerNeutralSlotsMuted) {
    Facts f;
    f.isPlayer = true;  // not sneaking, not in combat, unencumbered, no bounty
    auto s = ComposeBadges(f, IconCfg{});
    ASSERT_EQ(s.slots.size(), 4u);
    EXPECT_EQ(s.slots[0].icon, "person-walking");
    EXPECT_EQ(s.slots[3].icon, "scale-balanced");
    for (const auto& slot : s.slots) {
        EXPECT_TRUE(slot.muted);
    }
}

TEST(ComposeBadgesTest, PlayerDetectedUsesDetectedIcon) {
    Facts f;
    f.isPlayer = true;
    f.sneak = SneakK::Detected;
    auto s = ComposeBadges(f, IconCfg{});
    EXPECT_EQ(s.slots[0].icon, "eye");
    EXPECT_FALSE(s.slots[0].muted);
}

TEST(ComposeBadgesTest, DisabledGetsNothing) {
    IconCfg cfg{};
    cfg.enabled = false;
    Facts f;
    f.relationship = RelKind::Hostile;
    f.levelDelta = LvlDelta::Deadly;
    EXPECT_TRUE(ComposeBadges(f, cfg).slots.empty());
}

TEST(ComposeBadgesTest, PerSlotEnableDropsSlot) {
    IconCfg cfg{};
    cfg.roleEnabled = false;
    auto s = ComposeBadges(Facts{}, cfg);
    EXPECT_EQ(s.slots.size(), 5u);  // role slot dropped
    for (const auto& slot : s.slots) {
        EXPECT_NE(slot.icon, "house");
        EXPECT_NE(slot.icon, "coins");
    }
}

TEST(ComposeBadgesTest, DisabledCombatStateDropsCombatBadge) {
    IconCfg cfg{};
    cfg.combatStateEnabled = false;
    Facts f;
    f.engagement = EngK::Combat;
    auto s = ComposeBadges(f, cfg);
    EXPECT_EQ(s.slots.size(), 5u);  // engagement slot empty for this actor
    for (const auto& slot : s.slots) {
        EXPECT_NE(slot.icon, "swords");
    }
}

TEST(ComposeBadgesTest, EmptyIconNameDropsSlot) {
    IconCfg cfg{};
    cfg.icoFollower = "";
    Facts f;
    f.relationship = RelKind::Follower;
    auto s = ComposeBadges(f, cfg);
    EXPECT_EQ(s.slots.size(), 5u);
    for (const auto& slot : s.slots) {
        EXPECT_NE(slot.icon, "shield-halved");
    }
}

TEST(ComposeBadgesTest, DragonCreatureLitInCreatureColor) {
    Facts f;
    f.creatureKind = CritKind::Dragon;
    auto s = ComposeBadges(f, IconCfg{});
    EXPECT_EQ(s.slots[1].icon, "dragon");  // creature is slot 1
    EXPECT_FALSE(s.slots[1].muted);
    EXPECT_FLOAT_EQ(s.slots[1].color.r, 0.80f);  // colCreature
}

// ============================================================================
// Tests: status classifiers + muted styling
// ============================================================================

TEST(ClassifyTest, ProtectionEssentialBeatsProtected) {
    EXPECT_EQ(ClassifyProtection(true, true), ProtK::Essential);
    EXPECT_EQ(ClassifyProtection(false, true), ProtK::Protected);
    EXPECT_EQ(ClassifyProtection(false, false), ProtK::Mortal);
}

TEST(ClassifyTest, RoleGuardBeatsMerchant) {
    EXPECT_EQ(ClassifyRole(true, true), RoleK::Guard);
    EXPECT_EQ(ClassifyRole(false, true), RoleK::Merchant);
    EXPECT_EQ(ClassifyRole(false, false), RoleK::Commoner);
}

TEST(ClassifyTest, EngagementCombatBeatsAlert) {
    EXPECT_EQ(ClassifyEngagement(true, true, 100), EngK::Combat);
    EXPECT_EQ(ClassifyEngagement(false, true, 1), EngK::Alert);
    EXPECT_EQ(ClassifyEngagement(false, true, 0), EngK::Idle);   // no detection
    EXPECT_EQ(ClassifyEngagement(false, false, 100), EngK::Idle);  // weapon sheathed
}

TEST(MutedStyleTest, FullDesatGoesToLuma) {
    auto c = ApplyMutedDesat(TestColor{1.0f, 0.0f, 0.0f}, 1.0f);
    EXPECT_NEAR(c.r, 0.299f, 1e-5f);
    EXPECT_NEAR(c.g, 0.299f, 1e-5f);
    EXPECT_NEAR(c.b, 0.299f, 1e-5f);
}

TEST(MutedStyleTest, ZeroDesatKeepsColor) {
    auto c = ApplyMutedDesat(TestColor{0.2f, 0.5f, 0.9f}, 0.0f);
    EXPECT_FLOAT_EQ(c.r, 0.2f);
    EXPECT_FLOAT_EQ(c.g, 0.5f);
    EXPECT_FLOAT_EQ(c.b, 0.9f);
}
