// RendererLayout - measurement, placeholder expansion, style resolution and badge
// placement for one nameplate.
//
// render thread only. every input is plain data: the ActorDrawData the game thread
// published under snapshotLock, plus the per-frame RenderSettingsSnapshot. nothing
// here dereferences an RE::* object and nothing here draws. the output is a
// LabelStyle (per-role colors, effect selection, animation phase) and a LabelLayout
// (fonts, segments, positions, bounding box) that Renderer.cpp and
// RendererEffects.cpp consume in the same frame.
//
// vertical stack of one plate, in screen pixels relative to startPos.y, with y
// growing downward. the main row is anchored so its lowest drawn pixel - ink bottom
// plus outline plus shadow - lands exactly on startPos.y:
//
//     emblem row                    optional, added by BuildBadges
//     icon strip                    optional, added by BuildBadges
//                                   BADGE_ROW_GAP between each of those rows
//   titleY     ---- title line-box top
//                  title ink runs titleTop .. titleBottom, plus titleShadowOffsetY
//                  TITLE_MAIN_GAP
//   mainLineY  ---- main line-box top
//                  main ink runs mainTop .. mainBottom, widened by outlineWidth on
//                  both sides and by mainShadowOffsetY at the bottom
//   0.0        ---- startPos.y, because mainLineY = -mainBottomDraw
//                  INFO_LINE_GAP
//   infoLineY  ---- info line-box top, present only when infoSegments is non-empty
//
// the gaps are reference-scale constants from RenderConstants. each one is scaled by
// the plate's text scale before use, so the stack keeps its proportions at distance.

#include "RendererInternal.hpp"

#include "BadgeTextures.hpp"
#include "NameFit.hpp"
#include "TierEmblem.hpp"

namespace Renderer
{
// Glyph offsets are pixels below the line-box top; missing glyphs yield zero bounds.
void CalcTightYBoundsFromTop(
    ImFont* font, float fontSize, const char* text, float& outTop, float& outBottom)
{
    outTop = +FLT_MAX;
    outBottom = -FLT_MAX;

    if (!font || !text || !*text)
    {
        outTop = .0f;
        outBottom = .0f;
        return;
    }

    const float scale = fontSize / font->FontSize;

    for (const char* p = text; *p;)
    {
        unsigned int cp;
        p = Utf8Next(p, cp);

        if (cp == '\n' || cp == '\r')
        {
            continue;
        }

        const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
        if (!g)
        {
            continue;  // Character not in font
        }

        // Y0/Y1 are line-box offsets with y downward.
        outTop = std::min(outTop, g->Y0 * scale);
        outBottom = std::max(outBottom, g->Y1 * scale);
    }

    if (outTop == +FLT_MAX)
    {
        outTop = .0f;
        outBottom = .0f;
    }
}

std::string FormatString(const std::string& fmt, const ActorLabelContext& ctx)
{
    const std::string lStr = std::to_string(ctx.level);

    std::string result;
    result.reserve(fmt.size() + ctx.name.size() + 64);

    for (size_t i = 0; i < fmt.size(); ++i)
    {
        if (fmt[i] == '%' && i + 1 < fmt.size())
        {
            switch (fmt[i + 1])
            {
                case 'n':
                    result.append(ctx.name.data(), ctx.name.size());
                    ++i;
                    continue;
                case 'l':
                    result.append(lStr);
                    ++i;
                    continue;
                case 't':
                    if (ctx.title != nullptr)
                    {
                        result.append(ctx.title);
                        ++i;
                        continue;
                    }
                    break;
                case 'r':
                    result.append(ctx.relationship.data(), ctx.relationship.size());
                    ++i;
                    continue;
                case 'd':
                    result.append(ctx.levelDelta.data(), ctx.levelDelta.size());
                    ++i;
                    continue;
                case 'c':
                    result.append(ctx.creatureKind.data(), ctx.creatureKind.size());
                    ++i;
                    continue;
            }
        }
        result += fmt[i];
    }
    return result;
}

namespace
{
/**
 * @fn std::string_view LabelFor(RelationshipKind r, const RenderSettingsSnapshot::LabelTokens& lbl)
 * @brief Resolve a snapshot classification to its configured label view.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Views alias lbl; invalid enum values expand to empty text.
 */
std::string_view LabelFor(RelationshipKind r, const RenderSettingsSnapshot::LabelTokens& lbl)
{
    switch (r)
    {
        case RelationshipKind::Hostile:
            return lbl.relHostile;
        case RelationshipKind::Neutral:
            return lbl.relNeutral;
        case RelationshipKind::Ally:
            return lbl.relAlly;
        case RelationshipKind::Follower:
            return lbl.relFollower;
    }
    return {};
}

/**
 * @fn std::string_view LabelFor(LevelDelta d, const RenderSettingsSnapshot::LabelTokens& lbl)
 * @brief Resolve a snapshot classification to its configured label view.
 * @author Alex (<https://github.com/lextpf>)
 */
std::string_view LabelFor(LevelDelta d, const RenderSettingsSnapshot::LabelTokens& lbl)
{
    switch (d)
    {
        case LevelDelta::Weak:
            return lbl.ldWeak;
        case LevelDelta::Even:
            return lbl.ldEven;
        case LevelDelta::Strong:
            return lbl.ldStrong;
        case LevelDelta::Deadly:
            return lbl.ldDeadly;
    }
    return {};
}

/**
 * @fn std::string_view LabelFor(CreatureKind k, const RenderSettingsSnapshot::LabelTokens& lbl)
 * @brief Resolve a snapshot classification to its configured label view.
 * @author Alex (<https://github.com/lextpf>)
 */
std::string_view LabelFor(CreatureKind k, const RenderSettingsSnapshot::LabelTokens& lbl)
{
    switch (k)
    {
        case CreatureKind::NPC:
            return lbl.ctNPC;
        case CreatureKind::Beast:
            return lbl.ctBeast;
        case CreatureKind::Undead:
            return lbl.ctUndead;
        case CreatureKind::Daedra:
            return lbl.ctDaedra;
        case CreatureKind::Dragon:
            return lbl.ctDragon;
    }
    return {};
}

/**
 * @fn ActorLabelContext BuildLabelContext(const ActorDrawData& d, const RenderSettingsSnapshot&
 *     snap)
 * @brief Build non-owning format views for the current actor and settings.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Views alias snap.labels and the caller ActorDrawData. Set title separately;
 * otherwise %t remains literal.
 */
ActorLabelContext BuildLabelContext(const ActorDrawData& d, const RenderSettingsSnapshot& snap)
{
    ActorLabelContext ctx{};
    ctx.name = d.name.empty() ? std::string_view{" "} : std::string_view{d.name};
    ctx.level = static_cast<int>(d.level);
    ctx.title = nullptr;
    ctx.relationship = LabelFor(d.relationship, snap.labels);
    ctx.levelDelta = LabelFor(d.levelDelta, snap.labels);
    ctx.creatureKind = LabelFor(d.creatureKind, snap.labels);
    ctx.formID = d.formID;
    return ctx;
}

/**
 * @fn bool IsAllWhitespace(std::string_view s)
 * @brief Check whether a segment has any non-whitespace byte.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Empty text counts as whitespace.
 */
bool IsAllWhitespace(std::string_view s)
{
    for (char c : s)
    {
        if (std::isspace(static_cast<unsigned char>(c)) == 0)
        {
            return false;
        }
    }
    return true;
}
}  // namespace

const Settings::TierDefinition& GetFallbackTier()
{
    static const Settings::TierDefinition fallback = []
    {
        Settings::TierDefinition t{};
        t.minLevel = 1;
        t.maxLevel = 250;
        t.title = "Unknown";
        t.leftColor = Settings::Color3::White();
        t.rightColor = Settings::Color3::White();
        t.highlightColor = Settings::Color3::White();
        t.titleEffect.type = Settings::EffectType::Gradient;
        t.nameEffect.type = Settings::EffectType::Gradient;
        t.levelEffect.type = Settings::EffectType::Gradient;
        t.particleCount = 0;
        return t;
    }();
    return fallback;
}

/**
 * @fn static ImVec4 MixVec4(const ImVec4& a, const ImVec4& b, float t)
 * @brief Blend RGB channels and return an opaque color.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Discard input alpha; style opacity is stored separately.
 */
static ImVec4 MixVec4(const ImVec4& a, const ImVec4& b, float t)
{
    t = std::clamp(t, .0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
}

/**
 * @fn static void BoostSaturation(ImVec4& c, float amount)
 * @brief Scale chroma around Rec.601 luminance and clamp the color channels.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Scale chroma about Rec.601 luma. Channel clamping can shift saturated hues.
 */
static void BoostSaturation(ImVec4& c, float amount)
{
    float gray = c.x * .299f + c.y * .587f + c.z * .114f;
    c.x = std::clamp(gray + (c.x - gray) * amount, .0f, 1.0f);
    c.y = std::clamp(gray + (c.y - gray) * amount, .0f, 1.0f);
    c.z = std::clamp(gray + (c.z - gray) * amount, .0f, 1.0f);
}

/**
 * @fn static ImVec4 DeriveSupportTint(const ImVec4& left, const ImVec4& right, const
 *     Settings::Color3& highlight, float highlightMix, float saturationBoost)
 * @brief Resolve an opaque support tint from the gradient and tier highlight.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Derive opaque support tint from the gradient midpoint and tier highlight.
 */
static ImVec4 DeriveSupportTint(const ImVec4& left,
                                const ImVec4& right,
                                const Settings::Color3& highlight,
                                float highlightMix,
                                float saturationBoost)
{
    ImVec4 support = MixVec4(left, right, .5f);
    support = MixVec4(support, ImVec4(highlight.r, highlight.g, highlight.b, 1.0f), highlightMix);
    BoostSaturation(support, saturationBoost);
    support.w = 1.0f;
    return support;
}

/**
 * @struct TierLevelColors
 * @brief Resolved left and right colors for tier level text.
 * @author Alex (<https://github.com/lextpf>)
 */
struct TierLevelColors
{
    ImVec4 left;
    ImVec4 right;
};

/**
 * @fn static TierLevelColors ResolveTierLevelColors(const Settings::TierDefinition& tier)
 * @brief Resolve explicit level colors or derive them from the name gradient.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Explicit level colors win; otherwise blend the name gradient 40% toward white.
 */
static TierLevelColors ResolveTierLevelColors(const Settings::TierDefinition& tier)
{
    const auto ToVec = [](const Settings::Color3& color)
    { return ImVec4(color.r, color.g, color.b, 1.0f); };
    const auto LerpToWhite = [](const ImVec4& color, float amount)
    {
        return ImVec4(color.x + (1.0f - color.x) * amount,
                      color.y + (1.0f - color.y) * amount,
                      color.z + (1.0f - color.z) * amount,
                      1.0f);
    };

    const ImVec4 nameLeft = ToVec(tier.leftColor);
    const ImVec4 nameRight = ToVec(tier.rightColor);
    return {tier.levelLeftColor ? ToVec(*tier.levelLeftColor) : LerpToWhite(nameLeft, .40f),
            tier.levelRightColor ? ToVec(*tier.levelRightColor) : LerpToWhite(nameRight, .40f)};
}

/**
 * @fn static int MatchTier(uint16_t level, const RenderSettingsSnapshot& snap)
 * @brief Select a containing level range or the nearest configured tier.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Requires nonempty tiers. First containing range wins; otherwise nearest range,
 * with the lowest index breaking ties.
 */
static int MatchTier(uint16_t level, const RenderSettingsSnapshot& snap)
{
    int matchedTier = -1;
    for (size_t i = 0; i < snap.tiers.size(); ++i)
    {
        if (level >= snap.tiers[i].minLevel && level <= snap.tiers[i].maxLevel)
        {
            matchedTier = static_cast<int>(i);
            break;
        }
    }

    if (matchedTier < 0)
    {
        int bestIdx = 0;
        int bestDistance = std::numeric_limits<int>::max();
        for (size_t i = 0; i < snap.tiers.size(); ++i)
        {
            const int minLevel = static_cast<int>(snap.tiers[i].minLevel);
            const int maxLevel = static_cast<int>(snap.tiers[i].maxLevel);
            int distance = 0;
            if (static_cast<int>(level) < minLevel)
            {
                distance = minLevel - static_cast<int>(level);
            }
            else if (static_cast<int>(level) > maxLevel)
            {
                distance = static_cast<int>(level) - maxLevel;
            }

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIdx = static_cast<int>(i);
            }
        }
        matchedTier = bestIdx;
    }

    return std::clamp(matchedTier, 0, static_cast<int>(snap.tiers.size()) - 1);
}

static constexpr Settings::EffectParams kNoneEffect{.type = Settings::EffectType::None};
// Static gradients keep ordinary NPC text free of tier animation.
static constexpr Settings::EffectParams kNpcTierAccentEffect{.type =
                                                                 Settings::EffectType::Gradient};

/**
 * @fn static void PackStyleColors(LabelStyle& style, float alpha, const RenderSettingsSnapshot&
 *     snap)
 * @brief Pack resolved row colors with their independent opacity multipliers.
 * @author Alex (<https://github.com/lextpf>)
 */
static void PackStyleColors(LabelStyle& style, float alpha, const RenderSettingsSnapshot& snap)
{
    style.colL = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, alpha));
    style.colR = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.RcName.x, style.RcName.y, style.RcName.z, alpha));

    style.titleAlpha = alpha * snap.visual.TitleAlphaMultiplier;
    style.levelAlpha = alpha * snap.visual.LevelAlphaMultiplier;

    style.colLTitle = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.LcTitle.x, style.LcTitle.y, style.LcTitle.z, style.titleAlpha));
    style.colRTitle = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.RcTitle.x, style.RcTitle.y, style.RcTitle.z, style.titleAlpha));
    style.colLLevel = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.LcLevel.x, style.LcLevel.y, style.LcLevel.z, style.levelAlpha));
    style.colRLevel = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.RcLevel.x, style.RcLevel.y, style.RcLevel.z, style.levelAlpha));

    // Base width is the sum of INI bounds, not their average; row scaling follows.
    style.baseOutlineWidth = snap.outlineWidthMin + snap.outlineWidthMax;
}

/**
 * @fn static void ResolveTierStyleColors(LabelStyle& style, const Settings::TierDefinition& tier,
 *     uint16_t level, float alpha, const RenderSettingsSnapshot& snap)
 * @brief Resolve tier and special-title colors while preserving explicit choices.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Derive only omitted tier colors; explicit INI colors remain authoritative.
 */
static void ResolveTierStyleColors(LabelStyle& style,
                                   const Settings::TierDefinition& tier,
                                   uint16_t level,
                                   float alpha,
                                   const RenderSettingsSnapshot& snap)
{
    float levelT = .0f;
    if (tier.maxLevel > tier.minLevel)
    {
        levelT = (level <= tier.minLevel) ? .0f
                 : (level >= tier.maxLevel)
                     ? 1.0f
                     : (float)(level - tier.minLevel) / (float)(tier.maxLevel - tier.minLevel);
    }
    levelT = std::clamp(levelT, .0f, 1.0f);

    const bool under100 = (level < 100);
    // Keep low-level intensity near .85 so stacked strength caps remain visible.
    const float tierIntensity = under100 ? .85f : 1.0f;

    const float effectAlphaMul =
        RenderConstants::EFFECT_ALPHA_MIN +
        (RenderConstants::EFFECT_ALPHA_MAX - RenderConstants::EFFECT_ALPHA_MIN) * levelT;
    style.effectAlpha = alpha * tierIntensity * effectAlphaMul;

    auto ToVec = [](const Settings::Color3& c) { return ImVec4(c.r, c.g, c.b, 1.0f); };
    auto LerpToWhite = [](const ImVec4& c, float t)
    {
        return ImVec4(c.x + (1.0f - c.x) * t, c.y + (1.0f - c.y) * t, c.z + (1.0f - c.z) * t, 1.0f);
    };

    style.LcName = ToVec(tier.leftColor);
    style.RcName = ToVec(tier.rightColor);

    const auto levelColors = ResolveTierLevelColors(tier);
    style.LcLevel = levelColors.left;
    style.RcLevel = levelColors.right;
    style.LcTitle =
        tier.titleLeftColor ? ToVec(*tier.titleLeftColor) : LerpToWhite(style.LcName, .25f);
    style.RcTitle =
        tier.titleRightColor ? ToVec(*tier.titleRightColor) : LerpToWhite(style.RcName, .25f);

    style.specialGlowColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    if (style.specialTitle)
    {
        const auto* st = style.specialTitle;
        const ImVec4 specialCol = ToVec(st->color);
        style.specialGlowColor = ToVec(st->glowColor);

        style.LcName = specialCol;
        style.RcName = specialCol;
        style.LcLevel = specialCol;
        style.RcLevel = specialCol;
        style.LcTitle = specialCol;
        style.RcTitle = specialCol;

        style.supportName = specialCol;
        style.supportLevel = specialCol;
        style.supportTitle = specialCol;
    }
    else
    {
        style.supportName =
            DeriveSupportTint(style.LcName, style.RcName, tier.highlightColor, .14f, 1.08f);
        style.supportLevel =
            DeriveSupportTint(style.LcLevel, style.RcLevel, tier.highlightColor, .18f, 1.12f);
        style.supportTitle =
            DeriveSupportTint(style.LcTitle, style.RcTitle, tier.highlightColor, .24f, 1.16f);
    }

    style.highlight = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, style.effectAlpha));

    style.nameEffect = &tier.nameEffect;
    style.levelEffect = &tier.levelEffect;
    style.titleEffect = &tier.titleEffect;
    style.usesTierVisuals = true;
}

/**
 * @fn static void ResolveNpcStyleColors(LabelStyle& style, const Settings::TierDefinition& tier,
 *     RelationshipKind relationship, float alpha, const RenderSettingsSnapshot& snap, bool
 *     preserveTierEffects)
 * @brief Apply NPC name and support colors to the resolved style.
 * @author Alex (<https://github.com/lextpf>)
 *
 * NPC names stay white; title/level use tier level colors. preserveTierEffects
 * keeps only effect selection and strength. Color assignments still replace special-title
 * color and GlowColor; only the player retains those authored colors.
 */
static void ResolveNpcStyleColors(LabelStyle& style,
                                  const Settings::TierDefinition& tier,
                                  RelationshipKind relationship,
                                  float alpha,
                                  const RenderSettingsSnapshot& snap,
                                  bool preserveTierEffects)
{
    auto ToVec = [](const Settings::Color3& c) { return ImVec4(c.r, c.g, c.b, 1.0f); };

    const Settings::Color3& relationshipTint =
        relationship == RelationshipKind::Hostile    ? snap.npcColors.hostile
        : relationship == RelationshipKind::Follower ? snap.npcColors.follower
                                                     : snap.npcColors.neutral;
    const auto levelColors = ResolveTierLevelColors(tier);

    style.LcName = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.RcName = style.LcName;
    style.LcLevel = levelColors.left;
    style.RcLevel = levelColors.right;
    style.LcTitle = levelColors.left;
    style.RcTitle = levelColors.right;

    style.supportName = ToVec(relationshipTint);
    style.supportLevel = ToVec(snap.npcColors.level);
    style.supportTitle = ToVec(snap.npcColors.title);
    style.specialGlowColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.highlight = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));

    if (!preserveTierEffects)
    {
        style.effectAlpha = alpha;
        style.nameEffect = &kNoneEffect;
        style.levelEffect = &kNpcTierAccentEffect;
        style.titleEffect = &kNpcTierAccentEffect;
        style.usesTierVisuals = false;
    }
}

/**
 * @fn static void ComputeAnimationParams(LabelStyle& style, uint16_t level, uint32_t formID, float
 *     time, const RenderSettingsSnapshot& snap)
 * @brief Resolve per-actor animation phase and level-scaled effect strength.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Phase wraps elapsed time in [0,1), offset by the low 10 FormID bits.
 * Strength scales amplitude, not phase rate.
 */
static void ComputeAnimationParams(LabelStyle& style,
                                   uint16_t level,
                                   uint32_t formID,
                                   float time,
                                   const RenderSettingsSnapshot& snap)
{
    auto frac = [](float x) { return x - std::floor(x); };

    const bool under100 = (level < 100);
    const float tierIntensity = under100 ? .85f : 1.0f;

    float levelT = .0f;
    if (style.tier->maxLevel > style.tier->minLevel)
    {
        levelT = (level <= style.tier->minLevel) ? .0f
                 : (level >= style.tier->maxLevel)
                     ? 1.0f
                     : (float)(level - style.tier->minLevel) /
                           (float)(style.tier->maxLevel - style.tier->minLevel);
    }
    levelT = std::clamp(levelT, .0f, 1.0f);

    float tierAnimSpeed = RenderConstants::ANIM_SPEED_LOW_TIER;
    if (snap.tiers.size() > 1)
    {
        float tierRatio =
            static_cast<float>(style.tierIdx) / static_cast<float>(snap.tiers.size() - 1);
        if (tierRatio >= .9f)
        {
            tierAnimSpeed = RenderConstants::ANIM_SPEED_HIGH_TIER;
        }
        else if (tierRatio >= .8f)
        {
            tierAnimSpeed = RenderConstants::ANIM_SPEED_MID_TIER;
        }
    }
    if (under100)
    {
        tierAnimSpeed *= .85f;
    }

    const float phaseSeed = (formID & 1023) / 1023.0f;
    style.phase01 = frac(time * tierAnimSpeed + phaseSeed);

    style.strength =
        tierIntensity *
        (RenderConstants::EFFECT_STRENGTH_MIN +
         (RenderConstants::EFFECT_STRENGTH_MAX - RenderConstants::EFFECT_STRENGTH_MIN) * levelT);
}

LabelStyle ComputeLabelStyle(const ActorDrawData& d,
                             const std::string& nameLower,
                             float alpha,
                             float time,
                             const RenderSettingsSnapshot& snap)
{
    LabelStyle style{};
    style.alpha = alpha;

    // Cap modded/scripted levels to the configured tier range.
    const uint16_t lv = (uint16_t)std::min<int>(d.level, 9999);

    const Settings::TierDefinition* tierPtr = nullptr;
    if (snap.tiers.empty())
    {
        style.tierIdx = 0;
        tierPtr = &GetFallbackTier();
    }
    else
    {
        style.tierIdx = MatchTier(lv, snap);
        tierPtr = &snap.tiers[style.tierIdx];
    }
    style.tier = tierPtr;

    style.tierAllowsGlow =
        !snap.visual.EnableTierEffectGating || style.tierIdx >= snap.visual.GlowMinTier;
    style.tierAllowsParticles =
        !snap.visual.EnableTierEffectGating || style.tierIdx >= snap.visual.ParticleMinTier;
    style.tierAllowsOrnaments =
        !snap.visual.EnableTierEffectGating || style.tierIdx >= snap.visual.OrnamentMinTier;

    // The first nonempty keyword match wins in descending priority order.
    style.specialTitle = nullptr;
    {
        const auto& sortedSpecials = snap.sortedSpecialTitles;
        if (!sortedSpecials.empty() && !nameLower.empty())
        {
            for (const auto* st : sortedSpecials)
            {
                if (st->keywordLower.empty())
                {
                    continue;
                }
                if (nameLower.find(st->keywordLower) != std::string::npos)
                {
                    style.specialTitle = st;
                    break;
                }
            }
        }
    }

    style.distToPlayer = d.distToPlayer;

    if (d.isPlayer)
    {
        ResolveTierStyleColors(style, *tierPtr, lv, alpha, snap);
    }
    else
    {
        const bool preserveTierEffects = style.specialTitle != nullptr;
        if (preserveTierEffects)
        {
            ResolveTierStyleColors(style, *tierPtr, lv, alpha, snap);
        }
        ResolveNpcStyleColors(style, *tierPtr, d.relationship, alpha, snap, preserveTierEffects);
    }
    PackStyleColors(style, alpha, snap);
    ComputeAnimationParams(style, lv, d.formID, time, snap);

    return style;
}

/**
 * @fn static void BuildLineSegments(std::vector<RenderSeg>& outSegs, float& outLineWidth, float&
 *     outLineHeight, const std::vector<Settings::Segment>& fmtList, const ActorLabelContext& ctx,
 *     ImFont* fontName, float nameFontSize, ImFont* fontLevel, float levelFontSize, float
 *     segmentPadding, int typewriterCharsToShow, int& totalCharsProcessed, bool dropLevelSegments =
 *     false)
 * @brief Expand and measure one formatted row with a shared reveal budget.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Dropped segments contribute no width, padding or typewriter characters.
 * Measure full text to keep bounds fixed during reveal. A budget of -1 disables
 * typewriter accounting. Main and info rows share this path.
 */
static void BuildLineSegments(std::vector<RenderSeg>& outSegs,
                              float& outLineWidth,
                              float& outLineHeight,
                              const std::vector<Settings::Segment>& fmtList,
                              const ActorLabelContext& ctx,
                              ImFont* fontName,
                              float nameFontSize,
                              ImFont* fontLevel,
                              float levelFontSize,
                              float segmentPadding,
                              int typewriterCharsToShow,
                              int& totalCharsProcessed,
                              bool dropLevelSegments = false)
{
    outSegs.clear();
    outLineWidth = .0f;
    outLineHeight = .0f;

    for (const auto& fmt : fmtList)
    {
        // moreHUD level yield drops the entire %l segment.
        if (dropLevelSegments && fmt.format.find("%l") != std::string::npos)
        {
            continue;
        }

        RenderSeg seg{};
        seg.text = FormatString(fmt.format, ctx);

        if (fmt.dropIfBlank && IsAllWhitespace(seg.text))
        {
            continue;
        }

        seg.isLevel = fmt.useLevelFont;
        seg.containsName = fmt.format.find("%n") != std::string::npos;
        seg.font = seg.isLevel ? fontLevel : fontName;
        seg.fontSize = seg.isLevel ? levelFontSize : nameFontSize;
        seg.size = seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, .0f, seg.text.c_str());

        if (typewriterCharsToShow >= 0)
        {
            size_t segCharCount = Utf8CharCount(seg.text.c_str());
            int charsRemaining = typewriterCharsToShow - totalCharsProcessed;
            if (charsRemaining <= 0)
            {
                seg.displayText = "";
            }
            else if (static_cast<size_t>(charsRemaining) >= segCharCount)
            {
                seg.displayText = seg.text;
            }
            else
            {
                seg.displayText = Utf8Truncate(seg.text.c_str(), charsRemaining);
            }
            totalCharsProcessed += static_cast<int>(segCharCount);
        }
        else
        {
            seg.displayText = seg.text;
        }

        seg.displaySize =
            seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, .0f, seg.displayText.c_str());

        if (seg.containsName)
        {
            // Fit the entire name-bearing segment, including punctuation and level text.
            // Remeasure after font scaling; ScaleNewVerticesX reapplies horizontal compression.
            const NameFit::Result fit = NameFit::Compute(seg.size.x, seg.fontSize);
            if (fit.fontScale < .9999f)
            {
                seg.fontSize *= fit.fontScale;
                seg.size = seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, .0f, seg.text.c_str());
                seg.displaySize =
                    seg.font->CalcTextSizeA(seg.fontSize, FLT_MAX, .0f, seg.displayText.c_str());
            }
            seg.horizontalScale = fit.horizontalScale;
            seg.size.x *= seg.horizontalScale;
            seg.displaySize.x *= seg.horizontalScale;
        }

        outSegs.push_back(seg);
        outLineWidth += seg.size.x;
        if (seg.size.y > outLineHeight)
        {
            outLineHeight = seg.size.y;
        }
    }

    if (!outSegs.empty())
    {
        outLineWidth += (outSegs.size() - 1) * segmentPadding;
    }
}

/**
 * @fn static void BuildSegments(LabelLayout& layout, const ActorDrawData& d, const LabelStyle&,
 *     float textSizeScale, int typewriterCharsToShow, int& totalCharsProcessed, const
 *     RenderSettingsSnapshot& snap)
 * @brief Build main and info rows in typewriter reveal order.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Reveal main segments before info. Empty main formats use name plus lv.%l;
 * empty info formats omit the row. moreHUD level yield affects only the main row.
 */
static void BuildSegments(LabelLayout& layout,
                          const ActorDrawData& d,
                          const LabelStyle&,
                          float textSizeScale,
                          int typewriterCharsToShow,
                          int& totalCharsProcessed,
                          const RenderSettingsSnapshot& snap)
{
    static const std::vector<Settings::Segment> kDefaultDisplayFormat = {{"%n", false, false},
                                                                         {" Lv.%l", true, false}};

    const auto& mainFmt = snap.displayFormat.empty() ? kDefaultDisplayFormat : snap.displayFormat;

    const ActorLabelContext ctx = BuildLabelContext(d, snap);

    const float spacingScale = textSizeScale;
    layout.segmentPadding = RenderConstants::SEGMENT_PADDING * spacingScale;

    BuildLineSegments(layout.segments,
                      layout.mainLineWidth,
                      layout.mainLineHeight,
                      mainFmt,
                      ctx,
                      layout.fontName,
                      layout.nameFontSize,
                      layout.fontLevel,
                      layout.levelFontSize,
                      layout.segmentPadding,
                      typewriterCharsToShow,
                      totalCharsProcessed,
                      d.yieldLevel);

    BuildLineSegments(layout.infoSegments,
                      layout.infoLineWidth,
                      layout.infoLineHeight,
                      snap.infoFormat,
                      ctx,
                      layout.fontName,
                      layout.nameFontSize,
                      layout.fontLevel,
                      layout.levelFontSize,
                      layout.segmentPadding,
                      typewriterCharsToShow,
                      totalCharsProcessed);
}

/**
 * @fn static void ComputePositionAndBounds(LabelLayout& layout, const LabelStyle& style, const
 *     ActorDrawData& d, ActorCache& entry, int typewriterCharsToShow, int totalCharsProcessed,
 *     const RenderSettingsSnapshot& snap)
 * @brief Position measured rows and derive the complete plate bounds.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Row y offsets are line-box tops relative to startPos; see the stack diagram.
 */
static void ComputePositionAndBounds(LabelLayout& layout,
                                     const LabelStyle& style,
                                     const ActorDrawData& d,
                                     ActorCache& entry,
                                     int typewriterCharsToShow,
                                     int totalCharsProcessed,
                                     const RenderSettingsSnapshot& snap)
{
    const float outlineWidth = style.outlineWidth;

    // Title precedence: console, special title, honorific, tier.
    // An empty console title hides text but preserves the row gap.
    const bool titleOverridden = d.overrides && d.overrides->title.has_value();
    const char* titleToUse = titleOverridden        ? d.overrides->title->c_str()
                             : style.specialTitle   ? style.specialTitle->displayTitle.c_str()
                             : !d.honorific.empty() ? d.honorific.c_str()
                                                    : style.tier->title.c_str();
    ActorLabelContext titleCtx = BuildLabelContext(d, snap);
    titleCtx.title = titleToUse;
    layout.titleStr = (titleOverridden && d.overrides->title->empty())
                          ? std::string{}
                          : FormatString(snap.titleFormat, titleCtx);
    layout.titleDisplayStr = layout.titleStr;

    if (typewriterCharsToShow >= 0)
    {
        size_t titleCharCount = Utf8CharCount(layout.titleStr.c_str());
        int charsRemainingForTitle = typewriterCharsToShow - totalCharsProcessed;
        if (charsRemainingForTitle <= 0)
        {
            layout.titleDisplayStr = "";
        }
        else if (static_cast<size_t>(charsRemainingForTitle) >= titleCharCount)
        {
            layout.titleDisplayStr = layout.titleStr;
        }
        else
        {
            layout.titleDisplayStr = Utf8Truncate(layout.titleStr.c_str(), charsRemainingForTitle);
        }
        totalCharsProcessed += static_cast<int>(titleCharCount);

        if (!entry.typewriterComplete && typewriterCharsToShow >= totalCharsProcessed)
        {
            entry.typewriterComplete = true;
            entry.revealArmed = false;
        }
    }

    // Measure the full title so bounds stay fixed during reveal.
    const char* titleText = layout.titleStr.c_str();

    float titleTop = .0f, titleBottom = .0f;
    if (*titleText)
    {
        CalcTightYBoundsFromTop(
            layout.fontTitle, layout.titleFontSize, titleText, titleTop, titleBottom);
    }
    layout.titleSize =
        layout.fontTitle->CalcTextSizeA(layout.titleFontSize, FLT_MAX, .0f, titleText);

    float mainTop = +FLT_MAX;
    float mainBottom = -FLT_MAX;
    bool any = false;
    for (const auto& seg : layout.segments)
    {
        float sTop = .0f, sBottom = .0f;
        CalcTightYBoundsFromTop(seg.font, seg.fontSize, seg.text.c_str(), sTop, sBottom);
        float vOffset = (layout.mainLineHeight - seg.size.y) * .5f;
        mainTop = std::min(mainTop, vOffset + sTop);
        mainBottom = std::max(mainBottom, vOffset + sBottom);
        any = true;
    }
    if (!any)
    {
        mainTop = .0f;
        mainBottom = .0f;
    }

    const float spacingScale = layout.nameFontSize / layout.fontName->FontSize;
    const float titleShadowY = snap.titleShadowOffsetY * spacingScale;
    const float mainShadowY = snap.mainShadowOffsetY * spacingScale;
    float titleBottomDraw = titleBottom + titleShadowY;
    float mainTopDraw = mainTop - outlineWidth;
    float mainBottomDraw = mainBottom + outlineWidth + mainShadowY;

    layout.mainLineY = -mainBottomDraw;
    const float titleGap = RenderConstants::TITLE_MAIN_GAP * spacingScale;
    layout.titleY = layout.mainLineY + mainTopDraw - titleBottomDraw - titleGap;

    layout.startPos = entry.smooth;
    // Correct advance-width centering once for the entire plate, scaled with text.
    layout.startPos.x += snap.horizontalOffset * spacingScale;
    if (snap.visual.EnableOverlapPrevention)
    {
        auto oIt = OverlapOffsets().find(d.formID);
        if (oIt != OverlapOffsets().end())
        {
            layout.startPos.y += oIt->second;
        }
    }

    // Only surviving info segments add row height.
    float infoTop = .0f;
    float infoBottom = .0f;
    if (!layout.infoSegments.empty())
    {
        infoTop = +FLT_MAX;
        infoBottom = -FLT_MAX;
        bool anyInfo = false;
        for (const auto& seg : layout.infoSegments)
        {
            float sTop = .0f, sBottom = .0f;
            CalcTightYBoundsFromTop(seg.font, seg.fontSize, seg.text.c_str(), sTop, sBottom);
            float vOffset = (layout.infoLineHeight - seg.size.y) * .5f;
            infoTop = std::min(infoTop, vOffset + sTop);
            infoBottom = std::max(infoBottom, vOffset + sBottom);
            anyInfo = true;
        }
        if (!anyInfo)
        {
            infoTop = .0f;
            infoBottom = .0f;
        }

        // mainLineY + mainBottomDraw = 0; info ink starts one gap below startPos.y.
        const float infoGap = RenderConstants::INFO_LINE_GAP * spacingScale;
        layout.infoLineY = infoGap - infoTop;
    }

    layout.totalWidth = std::max({layout.mainLineWidth, layout.titleSize.x, layout.infoLineWidth});

    // Bounds follow tight ink, excluding spacing allowances; BuildBadges widens them.
    layout.nameplateTop = layout.startPos.y + layout.titleY + titleTop;
    layout.nameplateBottom = layout.infoSegments.empty()
                                 ? layout.startPos.y + layout.mainLineY + mainBottom
                                 : layout.startPos.y + layout.infoLineY + infoBottom;
    layout.nameplateLeft = layout.startPos.x - layout.totalWidth * .5f;
    layout.nameplateRight = layout.startPos.x + layout.totalWidth * .5f;
    layout.nameplateWidth = layout.totalWidth;
    layout.nameplateHeight = layout.nameplateBottom - layout.nameplateTop;
    layout.nameplateCenter =
        ImVec2(layout.startPos.x, (layout.nameplateTop + layout.nameplateBottom) * .5f);
    // Anchor ornaments between cap top and baseline; descenders must not lower them.
    float mainBaseline = -FLT_MAX;
    for (const auto& seg : layout.segments)
    {
        const float vOffset = (layout.mainLineHeight - seg.size.y) * .5f;
        const float ascentPx = seg.font->Ascent * (seg.fontSize / seg.font->FontSize);
        mainBaseline = std::max(mainBaseline, vOffset + ascentPx);
    }
    layout.mainLineCenterY =
        layout.segments.empty()
            ? layout.startPos.y + layout.mainLineY + (mainTop + mainBottom) * .5f
            : layout.startPos.y + layout.mainLineY + (mainTop + mainBaseline) * .5f;
}

/**
 * @fn static int TierBandIndex(int tierIdx, int tierCount)
 * @brief Assign a tier to the low, middle, or high rank band.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Tier thirds; mirrored in tests/test_utils.cpp. Keep in sync.
 */
static int TierBandIndex(int tierIdx, int tierCount)
{
    if (tierCount <= 1)
    {
        return 0;
    }
    return std::clamp(tierIdx * 3 / tierCount, 0, 2);
}

// Mirrored in tests/test_utils.cpp; keep badge rules synchronized. The mirror shares the
// real TierEmblem::Select, so only the slot rules need syncing.
// Views must alias cfg or *d.overrides, never temporary strings.
BadgeComposition ComposeBadges(const ActorDrawData& d,
                               const RenderSettingsSnapshot::IconTokens& cfg,
                               int tierIdx,
                               int tierCount,
                               bool includeRank,
                               int explicitBadge)
{
    BadgeComposition out{};
    if (!cfg.enabled)
    {
        return out;
    }

    using ActorOverrides::Slot;
    using ActorOverrides::SlotOverride;

    // Muting changes treatment, not the slot hue.
    const auto add = [&](bool enabled,
                         std::string_view icon,
                         Settings::Color3 color,
                         bool muted,
                         bool pulse = false)
    {
        if (enabled)
        {
            out.push(icon, color, muted, pulse);
        }
    };

    const auto slotOverride = [&](Slot slot) -> const SlotOverride*
    { return d.overrides ? &d.overrides->slots[static_cast<std::size_t>(slot)] : nullptr; };
    const auto hidden = [](const SlotOverride* ov) { return ov && ov->hidden; };
    // Console states encode enum order. These pins prevent reordered enums from
    // silently remapping saved overrides.
    static_assert(static_cast<int>(RelationshipKind::Follower) == 3);
    static_assert(static_cast<int>(CreatureKind::Dragon) == 4);
    static_assert(static_cast<int>(RoleKind::Guard) == 2);
    static_assert(static_cast<int>(ProtectionKind::Essential) == 2);
    static_assert(static_cast<int>(LevelDelta::Deadly) == 3);
    static_assert(static_cast<int>(EngagementKind::Combat) == 2);
    static_assert(static_cast<int>(SneakKind::Detected) == 2);
    const auto forced = [](const SlotOverride* ov, int live, int maxState) -> int
    { return (ov && ov->state) ? std::min<int>(*ov->state, maxState) : live; };
    // Both icon choices are lvalues that outlive this call.
    const auto iconOf = [](const SlotOverride* ov, const std::string& cfgIcon) -> std::string_view
    { return (ov && ov->icon) ? std::string_view(*ov->icon) : std::string_view(cfgIcon); };
    const auto addExtras = [&]()
    {
        if (!d.overrides)
        {
            return;
        }
        for (const auto& extra : d.overrides->extras)
        {
            out.push(extra.icon, extra.color, false, false);
        }
    };

    // Rank badges do not enable tier typography; ComputeLabelStyle owns that gate.
    const auto addRank = [&]()
    {
        if (hidden(slotOverride(Slot::Rank)) || !includeRank)
        {
            return;
        }
        if (cfg.tierBadgeImages && cfg.tierImageCount > 0)
        {
            out.pushTierImage(
                cfg.tierEnabled,
                TierEmblem::Select(
                    explicitBadge, tierIdx, tierCount, cfg.tierImageCount, cfg.tierBadgeGamma));
            return;
        }

        const int band = TierBandIndex(tierIdx, tierCount);
        if (cfg.tierEnabled)
        {
            out.pushRank(
                band == 2 ? cfg.icoTierHigh : (band == 1 ? cfg.icoTierMid : cfg.icoTierLow),
                band == 2 ? cfg.colTierHigh : (band == 1 ? cfg.colTierMid : cfg.colTierLow),
                false);
        }
    };

    if (!d.isPlayer)
    {
        addRank();

        if (const auto* ov = slotOverride(Slot::Relationship); !hidden(ov))
        {
            switch (static_cast<RelationshipKind>(forced(ov, static_cast<int>(d.relationship), 3)))
            {
                case RelationshipKind::Hostile:
                    add(cfg.relationshipEnabled, iconOf(ov, cfg.icoHostile), cfg.colHostile, false);
                    break;
                case RelationshipKind::Ally:
                    add(cfg.relationshipEnabled, iconOf(ov, cfg.icoAlly), cfg.colAlly, false);
                    break;
                case RelationshipKind::Follower:
                    add(cfg.relationshipEnabled,
                        iconOf(ov, cfg.icoFollower),
                        cfg.colFollower,
                        false);
                    break;
                case RelationshipKind::Neutral:
                    add(cfg.relationshipEnabled, iconOf(ov, cfg.icoNeutral), cfg.colNeutral, true);
                    break;
            }
        }

        if (const auto* ov = slotOverride(Slot::Creature); !hidden(ov))
        {
            switch (static_cast<CreatureKind>(forced(ov, static_cast<int>(d.creatureKind), 4)))
            {
                case CreatureKind::Dragon:
                    add(cfg.creatureEnabled, iconOf(ov, cfg.icoDragon), cfg.colCreature, false);
                    break;
                case CreatureKind::Daedra:
                    add(cfg.creatureEnabled, iconOf(ov, cfg.icoDaedra), cfg.colCreature, false);
                    break;
                case CreatureKind::Undead:
                    add(cfg.creatureEnabled, iconOf(ov, cfg.icoUndead), cfg.colCreature, false);
                    break;
                case CreatureKind::Beast:
                    add(cfg.creatureEnabled, iconOf(ov, cfg.icoBeast), cfg.colCreature, false);
                    break;
                case CreatureKind::NPC:
                    add(cfg.creatureEnabled, iconOf(ov, cfg.icoHumanoid), cfg.colHumanoid, true);
                    break;
            }
        }

        if (const auto* ov = slotOverride(Slot::Role); !hidden(ov))
        {
            switch (static_cast<RoleKind>(forced(ov, static_cast<int>(d.role), 2)))
            {
                case RoleKind::Guard:
                    add(cfg.roleEnabled, iconOf(ov, cfg.icoGuard), cfg.colGuard, false);
                    break;
                case RoleKind::Merchant:
                    add(cfg.roleEnabled, iconOf(ov, cfg.icoMerchant), cfg.colMerchant, false);
                    break;
                case RoleKind::Commoner:
                    add(cfg.roleEnabled, iconOf(ov, cfg.icoCommoner), cfg.colCommoner, true);
                    break;
            }
        }

        if (const auto* ov = slotOverride(Slot::Protection); !hidden(ov))
        {
            switch (static_cast<ProtectionKind>(forced(ov, static_cast<int>(d.protection), 2)))
            {
                case ProtectionKind::Essential:
                    add(cfg.protectionEnabled,
                        iconOf(ov, cfg.icoEssential),
                        cfg.colEssential,
                        false);
                    break;
                case ProtectionKind::Protected:
                    add(cfg.protectionEnabled,
                        iconOf(ov, cfg.icoProtected),
                        cfg.colProtected,
                        false);
                    break;
                case ProtectionKind::Mortal:
                    add(cfg.protectionEnabled, iconOf(ov, cfg.icoMortal), cfg.colMortal, true);
                    break;
            }
        }

        if (const auto* ov = slotOverride(Slot::Threat); !hidden(ov))
        {
            switch (static_cast<LevelDelta>(forced(ov, static_cast<int>(d.levelDelta), 3)))
            {
                case LevelDelta::Deadly:
                    add(cfg.threatEnabled,
                        iconOf(ov, cfg.icoDeadly),
                        cfg.colDeadly,
                        false,
                        cfg.deadlyPulse);
                    break;
                case LevelDelta::Strong:
                    add(cfg.threatEnabled, iconOf(ov, cfg.icoStrong), cfg.colStrong, false);
                    break;
                case LevelDelta::Weak:
                    add(cfg.threatEnabled, iconOf(ov, cfg.icoWeak), cfg.colWeak, false);
                    break;
                case LevelDelta::Even:
                    add(cfg.threatEnabled, iconOf(ov, cfg.icoEven), cfg.colEven, true);
                    break;
            }
        }

        if (const auto* ov = slotOverride(Slot::Engagement); !hidden(ov))
        {
            switch (static_cast<EngagementKind>(forced(ov, static_cast<int>(d.engagement), 2)))
            {
                case EngagementKind::Combat:
                    add(cfg.engagementEnabled && cfg.combatStateEnabled,
                        iconOf(ov, cfg.icoCombat),
                        cfg.colCombat,
                        false);
                    break;
                case EngagementKind::Alert:
                    add(cfg.engagementEnabled && cfg.alertStateEnabled,
                        iconOf(ov, cfg.icoAlert),
                        cfg.colAlert,
                        false);
                    break;
                case EngagementKind::Idle:
                    add(cfg.engagementEnabled, iconOf(ov, cfg.icoIdle), cfg.colIdle, true);
                    break;
            }
        }

        addExtras();
        return out;
    }

    addRank();

    if (const auto* ov = slotOverride(Slot::Sneak); !hidden(ov))
    {
        switch (static_cast<SneakKind>(forced(ov, static_cast<int>(d.sneak), 2)))
        {
            case SneakKind::Detected:
                add(cfg.sneakEnabled,
                    iconOf(ov, cfg.icoSneakDetected),
                    cfg.colSneakDetected,
                    false);
                break;
            case SneakKind::Hidden:
                add(cfg.sneakEnabled, iconOf(ov, cfg.icoSneakHidden), cfg.colSneakHidden, false);
                break;
            case SneakKind::Off:
                add(cfg.sneakEnabled, iconOf(ov, cfg.icoSneakOff), cfg.colSneakOff, true);
                break;
        }
    }

    if (const auto* ov = slotOverride(Slot::Engagement); !hidden(ov))
    {
        const bool inCombat = forced(ov, d.playerInCombat ? 1 : 0, 1) != 0;
        add(cfg.playerCombatEnabled,
            iconOf(ov, inCombat ? cfg.icoCombat : cfg.icoIdle),
            inCombat ? cfg.colCombat : cfg.colIdle,
            !inCombat);
    }

    if (const auto* ov = slotOverride(Slot::Encumbered); !hidden(ov))
    {
        const bool encumbered = forced(ov, d.encumbered ? 1 : 0, 1) != 0;
        add(cfg.encumberedEnabled,
            iconOf(ov, encumbered ? cfg.icoEncumbered : cfg.icoNormalWeight),
            encumbered ? cfg.colEncumbered : cfg.colNormalWeight,
            !encumbered);
    }

    if (const auto* ov = slotOverride(Slot::Bounty); !hidden(ov))
    {
        const bool wanted = forced(ov, d.wanted ? 1 : 0, 1) != 0;
        add(cfg.bountyEnabled,
            iconOf(ov, wanted ? cfg.icoWanted : cfg.icoBountyClear),
            wanted ? cfg.colWanted : cfg.colBountyClear,
            !wanted);
    }

    addExtras();
    return out;
}

/**
 * @fn static void BuildBadges(LabelLayout& layout, const ActorDrawData& d, const LabelStyle& style,
 *     float textSizeScale, const RenderSettingsSnapshot& snap)
 * @brief Append available badge textures and expand plate bounds around their rows.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Run after ComputePositionAndBounds. Badge rows widen overlap bounds without
 * moving text. Missing atlas, main text or icon textures simply omit badges.
 */
static void BuildBadges(LabelLayout& layout,
                        const ActorDrawData& d,
                        const LabelStyle& style,
                        float textSizeScale,
                        const RenderSettingsSnapshot& snap)
{
    layout.isPlayer = d.isPlayer;

    // Log the first layout attempt to diagnose missing badges.
    static std::atomic<bool> s_diagLogged{false};
    if (!s_diagLogged.exchange(true, std::memory_order_relaxed))
    {
        SKSE::log::info("Badges: first layout - iconsEnabled={}, texturesReady={}",
                        snap.icons.enabled,
                        BadgeTextures::IsInitialized());
    }

    layout.badges.clear();
    layout.tierEmblemShown = false;
    layout.tierEmblemTex = 0;
    if (!snap.icons.enabled || layout.segments.empty() || !BadgeTextures::IsInitialized())
    {
        return;
    }

    const BadgeComposition set = ComposeBadges(d,
                                               snap.icons,
                                               style.tierIdx,
                                               static_cast<int>(snap.tiers.size()),
                                               true,
                                               style.tier->badgeIndex);

    // Badge size follows the level row; emblems cannot be smaller than icons.
    const float iconSize =
        layout.levelFontSize * RenderConstants::BADGE_ICON_FACTOR * snap.icons.scale;
    const float emblemSize = iconSize * std::max(1.0f, snap.icons.tierBadgeScale);

    // Move full-color rank emblems to their own row above the icon strip.
    for (int i = 0; i < set.count; ++i)
    {
        const ComposedBadgeSlot& s = set.slots[i];
        if (s.tierImage >= 0)
        {
            const ImTextureID tex = BadgeTextures::GetTierImage(s.tierImage);
            if (tex != 0)
            {
                layout.tierEmblemTex = tex;
                layout.tierEmblemSize = ImVec2(emblemSize, emblemSize);
                layout.tierEmblemShown = true;
            }
            continue;
        }
        if (s.icon.empty())
        {
            continue;
        }
        BadgeDrawItem item{};
        item.tex = BadgeTextures::Get(std::string(s.icon));
        if (item.tex == 0)
        {
            continue;
        }
        item.color = s.color;
        item.pulse = s.pulse;
        item.muted = s.muted;
        item.isRank = s.isRank;
        item.size = ImVec2(iconSize, iconSize);
        layout.badges.push_back(item);
    }

    if (layout.badges.empty() && !layout.tierEmblemShown)
    {
        return;
    }

    const float rowGap = RenderConstants::BADGE_ROW_GAP * textSizeScale;

    // An empty title still reserves TITLE_MAIN_GAP above the outlined main row.
    float rowTop = layout.nameplateTop;
    if (!layout.badges.empty())
    {
        const float spacing = RenderConstants::BADGE_SPACING * textSizeScale;
        float stripWidth = spacing * static_cast<float>(layout.badges.size() - 1);
        for (const auto& b : layout.badges)
        {
            stripWidth += b.size.x;
        }
        rowTop = layout.nameplateTop - rowGap - iconSize;
        const float rowCenterY = rowTop + iconSize * .5f;
        float x = layout.startPos.x - stripWidth * .5f;
        for (auto& b : layout.badges)
        {
            b.pos.x = x;
            b.pos.y = rowCenterY - b.size.y * .5f;
            x += b.size.x + spacing;
        }
        for (const auto& b : layout.badges)
        {
            layout.nameplateLeft = std::min(layout.nameplateLeft, b.pos.x);
            layout.nameplateRight = std::max(layout.nameplateRight, b.pos.x + b.size.x);
        }
        layout.nameplateTop = std::min(layout.nameplateTop, rowTop);
    }

    // Emblems sit above the strip, or directly above the title when the strip is empty.
    if (layout.tierEmblemShown)
    {
        const float emblemTop = rowTop - rowGap - emblemSize;
        const float emblemX = layout.startPos.x - emblemSize * .5f;
        layout.tierEmblemPos = ImVec2(emblemX, emblemTop);
        layout.nameplateTop = std::min(layout.nameplateTop, emblemTop);
        layout.nameplateLeft = std::min(layout.nameplateLeft, emblemX);
        layout.nameplateRight = std::max(layout.nameplateRight, emblemX + emblemSize);
    }

    layout.nameplateHeight = layout.nameplateBottom - layout.nameplateTop;
    layout.nameplateCenter =
        ImVec2(layout.startPos.x, (layout.nameplateTop + layout.nameplateBottom) * .5f);
    layout.nameplateWidth = layout.nameplateRight - layout.nameplateLeft;
}

LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                               ActorCache& entry,
                               const LabelStyle& style,
                               float textSizeScale,
                               const RenderSettingsSnapshot& snap,
                               int forcedCharsToShow)
{
    LabelLayout layout{};

    // Character budget spans main, info and title. The death-fade override wins;
    // speed is characters/s and delay is seconds.
    int typewriterCharsToShow = -1;
    if (forcedCharsToShow >= 0)
    {
        typewriterCharsToShow = forcedCharsToShow;
    }
    else if (snap.enableTypewriter && !entry.typewriterComplete)
    {
        float effectiveTime = entry.typewriterTime - snap.typewriterDelay;
        if (effectiveTime > .0f)
        {
            typewriterCharsToShow = static_cast<int>(effectiveTime * snap.typewriterSpeed);
        }
        else
        {
            typewriterCharsToShow = 0;
        }
    }
    else if (!snap.enableTypewriter)
    {
        // A disabled reveal must not leave a console rearm pending.
        entry.revealArmed = false;
    }

    layout.fontName = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    layout.fontLevel = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    layout.fontTitle = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!layout.fontName || !layout.fontLevel || !layout.fontTitle)
    {
        return layout;
    }

    layout.nameFontSize = layout.fontName->FontSize * textSizeScale;
    layout.levelFontSize = layout.fontLevel->FontSize * textSizeScale;
    layout.titleFontSize = layout.fontTitle->FontSize * textSizeScale;

    int totalCharsProcessed = 0;
    BuildSegments(
        layout, d, style, textSizeScale, typewriterCharsToShow, totalCharsProcessed, snap);
    ComputePositionAndBounds(
        layout, style, d, entry, typewriterCharsToShow, totalCharsProcessed, snap);
    // Save text-only bounds relative to startPos before badges expand them.
    layout.textBoundsMin =
        ImVec2(layout.nameplateLeft - layout.startPos.x, layout.nameplateTop - layout.startPos.y);
    layout.textBoundsMax = ImVec2(layout.nameplateRight - layout.startPos.x,
                                  layout.nameplateBottom - layout.startPos.y);
    BuildBadges(layout, d, style, textSizeScale, snap);

    return layout;
}

// mixT <= 0 leaves style unchanged; values above 1 clamp. Highlight becomes target
// outright. style.alpha stays caller-owned. Repeated calls form a multi-stop ramp.
void ApplyDeathRiteTint(LabelStyle& style,
                        const ImVec4& target,
                        float mixT,
                        const RenderSettingsSnapshot& snap)
{
    if (mixT <= .0f)
    {
        return;
    }
    mixT = std::min(mixT, 1.0f);

    style.LcName = MixVec4(style.LcName, target, mixT);
    style.RcName = MixVec4(style.RcName, target, mixT);
    style.LcLevel = MixVec4(style.LcLevel, target, mixT);
    style.RcLevel = MixVec4(style.RcLevel, target, mixT);
    style.LcTitle = MixVec4(style.LcTitle, target, mixT);
    style.RcTitle = MixVec4(style.RcTitle, target, mixT);
    style.supportName = MixVec4(style.supportName, target, mixT);
    style.supportLevel = MixVec4(style.supportLevel, target, mixT);
    style.supportTitle = MixVec4(style.supportTitle, target, mixT);
    style.specialGlowColor = MixVec4(style.specialGlowColor, target, mixT);

    style.strength *= 1.0f - mixT;
    style.effectAlpha *= 1.0f - mixT;
    style.highlight =
        ImGui::ColorConvertFloat4ToU32(ImVec4(target.x, target.y, target.z, style.effectAlpha));

    PackStyleColors(style, style.alpha, snap);
}

// Negative bgLum or nonpositive candleStrength leaves style unchanged.
// Rec.709 luminance gates warmth below .35, capped at 6% of candleWarmth.
// Exposure gain is capped at 1 +/- candleStrength. Keep CandleGain in
//  tests/test_utils.cpp synchronized.
void ApplyCandlelight(LabelStyle& style,
                      float bgLum,
                      const float bgRGB[3],
                      const RenderSettingsSnapshot& snap)
{
    const float strength = snap.candleStrength;
    if (strength <= .0f || bgLum < .0f)
    {
        return;
    }

    // Mid-grey (.5) leaves exposure unchanged.
    const float gain = 1.0f + std::clamp((.5f - bgLum) * 2.0f * strength, -strength, strength);

    float warmT = .0f;
    ImVec4 sceneTint(1.0f, 1.0f, 1.0f, 1.0f);
    if (bgLum < .35f && snap.candleWarmth > .0f)
    {
        // Normalize to retain hue without importing scene darkness.
        const float maxC = std::max({bgRGB[0], bgRGB[1], bgRGB[2], .05f});
        sceneTint = ImVec4(bgRGB[0] / maxC, bgRGB[1] / maxC, bgRGB[2] / maxC, 1.0f);
        warmT = snap.candleWarmth * .06f * ((.35f - bgLum) / .35f);
    }

    const auto adapt = [&](ImVec4& c)
    {
        c.x = std::clamp(c.x * gain, .0f, 1.0f);
        c.y = std::clamp(c.y * gain, .0f, 1.0f);
        c.z = std::clamp(c.z * gain, .0f, 1.0f);
        if (warmT > .0f)
        {
            c = MixVec4(
                c, ImVec4(c.x * sceneTint.x, c.y * sceneTint.y, c.z * sceneTint.z, 1.0f), warmT);
        }
    };
    adapt(style.LcName);
    adapt(style.RcName);
    adapt(style.LcLevel);
    adapt(style.RcLevel);
    adapt(style.LcTitle);
    adapt(style.RcTitle);
    adapt(style.supportName);
    adapt(style.supportLevel);
    adapt(style.supportTitle);

    PackStyleColors(style, style.alpha, snap);
}

}  // namespace Renderer
