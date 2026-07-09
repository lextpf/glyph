#include "RendererInternal.hpp"

#include "BadgeTextures.hpp"

namespace Renderer
{
// Calculate tight vertical bounds of text glyphs
void CalcTightYBoundsFromTop(
    ImFont* font, float fontSize, const char* text, float& outTop, float& outBottom)
{
    // Initialize to extremes so first glyph will replace them
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

        // Skip newlines (shouldn't be in our text, but be safe)
        if (cp == '\n' || cp == '\r')
        {
            continue;
        }

        const ImFontGlyph* g = font->FindGlyph((ImWchar)cp);
        if (!g)
        {
            continue;  // Character not in font
        }

        // Y0 and Y1 are glyph offsets from the top of the line (not the baseline).
        // Y0 = top of glyph, Y1 = bottom of glyph (both positive, Y increases downward).
        // We want the tightest bounds, so min of Y0 (topmost), max of Y1 (bottommost).
        outTop = std::min(outTop, g->Y0 * scale);
        outBottom = std::max(outBottom, g->Y1 * scale);
    }

    // If no valid glyphs were found, return zeros
    if (outTop == +FLT_MAX)
    {
        outTop = .0f;
        outBottom = .0f;
    }
}

// Replace placeholders in a format string with values from an actor label context.
// Single-pass to avoid expanding placeholders embedded in substitution values.
// Supported placeholders: %n (name), %l (level), %t (title), %r (relationship),
// %d (level delta), %c (creature kind).
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
// Resolve enum -> label string from the per-frame settings snapshot.
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

// Build a label context for an actor.  String views point into snap.labels,
// which is stable for the lifetime of the cached snapshot (regenerated only
// when Settings::Generation() advances).  The title pointer must be set by
// the caller after `BuildLabelContext` returns.
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

// True when every character in `s` is ASCII whitespace.  Empty string is true.
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

static ImVec4 MixVec4(const ImVec4& a, const ImVec4& b, float t)
{
    t = std::clamp(t, .0f, 1.0f);
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
}

static void BoostSaturation(ImVec4& c, float amount)
{
    float gray = c.x * .299f + c.y * .587f + c.z * .114f;
    c.x = std::clamp(gray + (c.x - gray) * amount, .0f, 1.0f);
    c.y = std::clamp(gray + (c.y - gray) * amount, .0f, 1.0f);
    c.z = std::clamp(gray + (c.z - gray) * amount, .0f, 1.0f);
}

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

// ============================================================================
// ComputeLabelStyle helpers
// ============================================================================

// Find the tier index matching the given level (direct match or nearest range).
// Only called when snap.tiers is non-empty.
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
        // No direct match; find nearest range.
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

// Solid-fill effect used for NPC text (tier effects are player/special-title only).
static constexpr Settings::EffectParams kNoneEffect{.type = Settings::EffectType::None};

// Pack the resolved float colors into draw-ready ImU32 values.
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

    // Outline width data (actual widths computed after font sizes are known)
    style.baseOutlineWidth = snap.outlineWidthMin + snap.outlineWidthMax;
}

// Resolve tier-palette colors for the player (and special titles).  INI
// colors display as authored; derivation happens only for optional per-tier
// entries the INI omits.
static void ResolveTierStyleColors(LabelStyle& style,
                                   const Settings::TierDefinition& tier,
                                   uint16_t level,
                                   float alpha,
                                   const RenderSettingsSnapshot& snap)
{
    // Level position within tier [0, 1]
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
    // Early levels stay quieter, but only slightly -- every discount here
    // multiplies with the strength band and per-effect caps, and .7 stacked
    // into effects that read as absent rather than restrained.
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

    // Name colors: the tier's main gradient, as authored.
    style.LcName = ToVec(tier.leftColor);
    style.RcName = ToVec(tier.rightColor);

    // Level / title colors: per-tier INI overrides always win; otherwise
    // derive a softer companion by blending the name band toward white.
    style.LcLevel =
        tier.levelLeftColor ? ToVec(*tier.levelLeftColor) : LerpToWhite(style.LcName, .40f);
    style.RcLevel =
        tier.levelRightColor ? ToVec(*tier.levelRightColor) : LerpToWhite(style.RcName, .40f);
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

// Resolve flat white-leaning colors for NPC nameplates.  The name color is
// keyed by the actor's relationship to the player; level / title use fixed
// neutral accents.  Support layers tint from the role's own flat color, so
// glow / outline / shadow read neutral rather than tier-colored.  Text
// effects are disabled (solid fill).
static void ResolveNpcStyleColors(LabelStyle& style,
                                  RelationshipKind relationship,
                                  float alpha,
                                  const RenderSettingsSnapshot& snap)
{
    auto ToVec = [](const Settings::Color3& c) { return ImVec4(c.r, c.g, c.b, 1.0f); };

    const Settings::Color3& nameColor =
        relationship == RelationshipKind::Hostile    ? snap.npcColors.hostile
        : relationship == RelationshipKind::Follower ? snap.npcColors.follower
                                                     : snap.npcColors.neutral;

    style.LcName = ToVec(nameColor);
    style.RcName = style.LcName;
    style.LcLevel = ToVec(snap.npcColors.level);
    style.RcLevel = style.LcLevel;
    style.LcTitle = ToVec(snap.npcColors.title);
    style.RcTitle = style.LcTitle;

    style.supportName = style.LcName;
    style.supportLevel = style.LcLevel;
    style.supportTitle = style.LcTitle;
    style.specialGlowColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    style.effectAlpha = alpha;
    style.highlight = ImGui::ColorConvertFloat4ToU32(
        ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, alpha));

    style.nameEffect = &kNoneEffect;
    style.levelEffect = &kNoneEffect;
    style.titleEffect = &kNoneEffect;
    style.usesTierVisuals = false;
}

// Compute animation phase and strength parameters.
static void ComputeAnimationParams(LabelStyle& style,
                                   uint16_t level,
                                   uint32_t formID,
                                   float time,
                                   const RenderSettingsSnapshot& snap)
{
    auto frac = [](float x) { return x - std::floor(x); };

    const bool under100 = (level < 100);
    // Matches the effect-alpha tierIntensity above: quieter early, not mute.
    const float tierIntensity = under100 ? .85f : 1.0f;

    // Level position within tier [0, 1] (recalculated for strength)
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

// Compute all color, tier, and effect data for a label.
LabelStyle ComputeLabelStyle(const ActorDrawData& d,
                             const std::string& nameLower,
                             float alpha,
                             float time,
                             const RenderSettingsSnapshot& snap)
{
    LabelStyle style{};
    style.alpha = alpha;

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

    // Tier effect gating
    style.tierAllowsGlow =
        !snap.visual.EnableTierEffectGating || style.tierIdx >= snap.visual.GlowMinTier;
    style.tierAllowsParticles =
        !snap.visual.EnableTierEffectGating || style.tierIdx >= snap.visual.ParticleMinTier;
    style.tierAllowsOrnaments =
        !snap.visual.EnableTierEffectGating || style.tierIdx >= snap.visual.OrnamentMinTier;

    // Special title matching
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

    // Player and special titles use the tier palette + effects; NPCs resolve
    // to flat white-leaning colors keyed by relationship.
    if (d.isPlayer || style.specialTitle != nullptr)
    {
        ResolveTierStyleColors(style, *tierPtr, lv, alpha, snap);
    }
    else
    {
        ResolveNpcStyleColors(style, d.relationship, alpha, snap);
    }
    PackStyleColors(style, alpha, snap);
    ComputeAnimationParams(style, lv, d.formID, time, snap);

    return style;
}

// ============================================================================
// ComputeLabelLayout helpers
// ============================================================================

// Build one row of rendered segments from a format list and advance the
// typewriter accounting.  Shared by the main row (DisplayFormat) and info
// row (InfoFormat).  Segments marked `dropIfBlank` whose post-expansion
// text trims to empty are omitted entirely (no width, no padding).
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
        // One Voice Per Actor: when another HUD already shows this target's
        // level (moreHUD crosshair readout), the whole %l segment bows out.
        if (dropLevelSegments && fmt.format.find("%l") != std::string::npos)
        {
            continue;
        }

        RenderSeg seg;
        seg.text = FormatString(fmt.format, ctx);

        if (fmt.dropIfBlank && IsAllWhitespace(seg.text))
        {
            continue;
        }

        seg.isLevel = fmt.useLevelFont;
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

// Build main row + info row segments from snap.displayFormat / snap.infoFormat.
// Typewriter reveal continues across both rows (main -> info), so the info
// segments appear after the main line has finished typing.
static void BuildSegments(LabelLayout& layout,
                          const ActorDrawData& d,
                          const LabelStyle& /*style*/,
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

// Compute title text, vertical bounds, positions, and nameplate bounding box.
static void ComputePositionAndBounds(LabelLayout& layout,
                                     const LabelStyle& style,
                                     const ActorDrawData& d,
                                     ActorCache& entry,
                                     int typewriterCharsToShow,
                                     int totalCharsProcessed,
                                     const RenderSettingsSnapshot& snap)
{
    const float outlineWidth = style.outlineWidth;

    // Title precedence: special titles (name-keyword styling) stay strongest;
    // then a faction-earned honorific (Deeds, Not Words); then the tier title.
    const char* titleToUse = style.specialTitle     ? style.specialTitle->displayTitle.c_str()
                             : !d.honorific.empty() ? d.honorific.c_str()
                                                    : style.tier->title.c_str();
    ActorLabelContext titleCtx = BuildLabelContext(d, snap);
    titleCtx.title = titleToUse;
    layout.titleStr = FormatString(snap.titleFormat, titleCtx);
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
        }
    }

    const char* titleText = layout.titleStr.c_str();

    // Tight vertical bounds
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
    if (snap.visual.EnableOverlapPrevention)
    {
        auto oIt = OverlapOffsets().find(d.formID);
        if (oIt != OverlapOffsets().end())
        {
            layout.startPos.y += oIt->second;
        }
    }

    // Info row positioning -- drawn below the main line, separated by INFO_LINE_GAP.
    // Only contributes height when at least one info segment survived dropIfBlank.
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

        // mainLineY + mainBottomDraw is the anchor (~ 0).  Info top should be a
        // gap below the anchor.  infoLineY is the line-box top, with infoTop being
        // the offset of the topmost glyph from that line-box top.
        const float infoGap = RenderConstants::INFO_LINE_GAP * spacingScale;
        layout.infoLineY = infoGap - infoTop;
    }

    layout.totalWidth = std::max({layout.mainLineWidth, layout.titleSize.x, layout.infoLineWidth});

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
    // Ornament anchor: cap-band optical center of the main line -- topmost ink
    // down to the BASELINE, not the ink bottom. Descenders (g/j/p/q/y) in a
    // name extend the ink box downward and dragged the old ink-center anchor
    // (and the side ornaments with it) below the optical center of the letters.
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

// ============================================================================
// Status icon badges
// ============================================================================

/// One composed indicator slot.  `icon` points into the per-frame settings
/// snapshot (stable for the frame); an empty icon means the slot is skipped.
/// Muted slots render dimmed + desaturated at draw time.
struct BadgeSlot
{
    std::string_view icon;
    Settings::Color3 color{};
    bool muted = false;
    bool pulse = false;
    int tierImage = -1;  ///< >=0 -> full-color emblem texture by index (icon unused)
};

/// Ordered fixed-capacity set of slots for one actor.  Capacity covers the
/// widest set (six NPC slots); the player set uses five.  Bump
/// MAX_BADGE_SLOTS if NPC slots grow, or excess slots silently drop.
inline constexpr int MAX_BADGE_SLOTS = 6;

// Map a tier index onto the low/mid/high prestige band (integer thirds).
// Mirrored in tests/test_utils.cpp -- keep in sync.
static int TierBandIndex(int tierIdx, int tierCount)
{
    if (tierCount <= 1)
    {
        return 0;
    }
    return std::clamp(tierIdx * 3 / tierCount, 0, 2);
}

// Map a tier index onto a top-weighted emblem index in [0, imageCount).  A
// convex gamma (>1) makes the low emblems span more tiers and the rare high
// emblems span fewer, so the prestige climb steepens near the top of the
// ladder.  Mirrored in tests/test_utils.cpp -- keep in sync.
static int TierImageBandIndex(int tierIdx, int tierCount, int imageCount, float gamma)
{
    if (imageCount <= 1 || tierCount <= 1)
    {
        return 0;
    }
    const float t =
        std::clamp(static_cast<float>(tierIdx) / static_cast<float>(tierCount - 1), .0f, 1.0f);
    const float g = std::clamp(gamma, .1f, 8.0f);
    const int band = static_cast<int>(std::floor(std::pow(t, g) * static_cast<float>(imageCount)));
    return std::clamp(band, 0, imageCount - 1);
}
struct BadgeComposition
{
    BadgeSlot slots[MAX_BADGE_SLOTS];
    int count = 0;
    void push(std::string_view icon, Settings::Color3 color, bool muted, bool pulse = false)
    {
        if (count < MAX_BADGE_SLOTS && !icon.empty())
        {
            slots[count++] = {icon, color, muted, pulse};
        }
    }
    // Push a full-color emblem tier badge (resolved by texture index, untinted).
    void pushTierImage(bool enabled, int imageIndex)
    {
        if (enabled && count < MAX_BADGE_SLOTS && imageIndex >= 0)
        {
            slots[count++] = BadgeSlot{{}, {}, false, false, imageIndex};
        }
    }
};

// Map actor facts to an ordered set of badge slots.  Always-on model: every
// enabled slot renders, with neutral/inactive facts shown in their own resting
// color (dimmed + slightly desaturated at draw time) and notable facts shown
// lit.  NPCs get six slots (relationship, creature, role, protection, threat,
// engagement); the player gets five (tier, sneak, engagement, encumbered,
// bounty).  Mirrored in tests/test_utils.cpp -- keep the logic in sync.
static BadgeComposition ComposeBadges(const ActorDrawData& d,
                                      const RenderSettingsSnapshot::IconTokens& cfg,
                                      int tierIdx,
                                      int tierCount)
{
    BadgeComposition out{};
    if (!cfg.enabled)
    {
        return out;
    }

    // Push a slot honoring its enable flag.  Every slot carries its own color;
    // the `muted` flag drives only the calmer draw treatment (dim + no shadow +
    // slight desaturation), not the hue.
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

    if (!d.isPlayer)
    {
        switch (d.relationship)
        {
            case RelationshipKind::Hostile:
                add(cfg.relationshipEnabled, cfg.icoHostile, cfg.colHostile, false);
                break;
            case RelationshipKind::Ally:
                add(cfg.relationshipEnabled, cfg.icoAlly, cfg.colAlly, false);
                break;
            case RelationshipKind::Follower:
                add(cfg.relationshipEnabled, cfg.icoFollower, cfg.colFollower, false);
                break;
            case RelationshipKind::Neutral:
                add(cfg.relationshipEnabled, cfg.icoNeutral, cfg.colNeutral, true);
                break;
        }

        switch (d.creatureKind)
        {
            case CreatureKind::Dragon:
                add(cfg.creatureEnabled, cfg.icoDragon, cfg.colCreature, false);
                break;
            case CreatureKind::Daedra:
                add(cfg.creatureEnabled, cfg.icoDaedra, cfg.colCreature, false);
                break;
            case CreatureKind::Undead:
                add(cfg.creatureEnabled, cfg.icoUndead, cfg.colCreature, false);
                break;
            case CreatureKind::Beast:
                add(cfg.creatureEnabled, cfg.icoBeast, cfg.colCreature, false);
                break;
            case CreatureKind::NPC:
                add(cfg.creatureEnabled, cfg.icoHumanoid, cfg.colHumanoid, true);
                break;
        }

        switch (d.role)
        {
            case RoleKind::Guard:
                add(cfg.roleEnabled, cfg.icoGuard, cfg.colGuard, false);
                break;
            case RoleKind::Merchant:
                add(cfg.roleEnabled, cfg.icoMerchant, cfg.colMerchant, false);
                break;
            case RoleKind::Commoner:
                add(cfg.roleEnabled, cfg.icoCommoner, cfg.colCommoner, true);
                break;
        }

        switch (d.protection)
        {
            case ProtectionKind::Essential:
                add(cfg.protectionEnabled, cfg.icoEssential, cfg.colEssential, false);
                break;
            case ProtectionKind::Protected:
                add(cfg.protectionEnabled, cfg.icoProtected, cfg.colProtected, false);
                break;
            case ProtectionKind::Mortal:
                add(cfg.protectionEnabled, cfg.icoMortal, cfg.colMortal, true);
                break;
        }

        switch (d.levelDelta)
        {
            case LevelDelta::Deadly:
                add(cfg.threatEnabled, cfg.icoDeadly, cfg.colDeadly, false, cfg.deadlyPulse);
                break;
            case LevelDelta::Strong:
                add(cfg.threatEnabled, cfg.icoStrong, cfg.colStrong, false);
                break;
            case LevelDelta::Weak:
                add(cfg.threatEnabled, cfg.icoWeak, cfg.colWeak, false);
                break;
            case LevelDelta::Even:
                add(cfg.threatEnabled, cfg.icoEven, cfg.colEven, true);
                break;
        }

        switch (d.engagement)
        {
            case EngagementKind::Combat:
                add(cfg.engagementEnabled && cfg.combatStateEnabled,
                    cfg.icoCombat,
                    cfg.colCombat,
                    false);
                break;
            case EngagementKind::Alert:
                add(cfg.engagementEnabled && cfg.alertStateEnabled,
                    cfg.icoAlert,
                    cfg.colAlert,
                    false);
                break;
            case EngagementKind::Idle:
                add(cfg.engagementEnabled, cfg.icoIdle, cfg.colIdle, true);
                break;
        }

        return out;
    }

    // Player slot set: tier, sneak, engagement, encumbered, bounty.

    // Tier band: prestige indicator for the tier ladder.  Always lit -- it is
    // an identity fact, not an on/off state.  With emblem images loaded it is a
    // full-color top-weighted rank badge; otherwise the low/mid/high FA icon.
    if (cfg.tierBadgeImages && cfg.tierImageCount > 0)
    {
        out.pushTierImage(
            cfg.tierEnabled,
            TierImageBandIndex(tierIdx, tierCount, cfg.tierImageCount, cfg.tierBadgeGamma));
    }
    else
    {
        const int band = TierBandIndex(tierIdx, tierCount);
        add(cfg.tierEnabled,
            band == 2 ? cfg.icoTierHigh : (band == 1 ? cfg.icoTierMid : cfg.icoTierLow),
            band == 2 ? cfg.colTierHigh : (band == 1 ? cfg.colTierMid : cfg.colTierLow),
            false);
    }

    switch (d.sneak)
    {
        case SneakKind::Detected:
            add(cfg.sneakEnabled, cfg.icoSneakDetected, cfg.colSneakDetected, false);
            break;
        case SneakKind::Hidden:
            add(cfg.sneakEnabled, cfg.icoSneakHidden, cfg.colSneakHidden, false);
            break;
        case SneakKind::Off:
            add(cfg.sneakEnabled, cfg.icoSneakOff, cfg.colSneakOff, true);
            break;
    }

    add(cfg.playerCombatEnabled,
        d.playerInCombat ? cfg.icoCombat : cfg.icoIdle,
        d.playerInCombat ? cfg.colCombat : cfg.colIdle,
        !d.playerInCombat);

    add(cfg.encumberedEnabled,
        d.encumbered ? cfg.icoEncumbered : cfg.icoNormalWeight,
        d.encumbered ? cfg.colEncumbered : cfg.colNormalWeight,
        !d.encumbered);

    add(cfg.bountyEnabled,
        d.wanted ? cfg.icoWanted : cfg.icoBountyClear,
        d.wanted ? cfg.colWanted : cfg.colBountyClear,
        !d.wanted);

    return out;
}

// Resolve status badge indicators into a compact strip centered above the
// title row.  Slot order is relationship, creature, role, protection, threat,
// engagement for NPCs; tier, sneak, engagement, encumbered, bounty for the
// player.  Must run after ComputePositionAndBounds (needs the plate's top
// edge).  The strip extends the nameplate bounds used for overlap prevention
// but never moves the text itself.
static void BuildBadges(LabelLayout& layout,
                        const ActorDrawData& d,
                        const LabelStyle& style,
                        float textSizeScale,
                        const RenderSettingsSnapshot& snap)
{
    // One-shot diagnostic: log the badge pipeline state the first time an
    // actor reaches badge layout, so silent in-game failures are traceable.
    static std::atomic<bool> s_diagLogged{false};
    if (!s_diagLogged.exchange(true, std::memory_order_relaxed))
    {
        SKSE::log::info("Badges: first layout -- iconsEnabled={}, texturesReady={}",
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

    const BadgeComposition set =
        ComposeBadges(d, snap.icons, style.tierIdx, static_cast<int>(snap.tiers.size()));

    const float iconSize =
        layout.levelFontSize * RenderConstants::BADGE_ICON_FACTOR * snap.icons.scale;
    const float emblemSize = iconSize * std::max(1.0f, snap.icons.tierBadgeScale);

    // Resolve slots.  The tier emblem is lifted OUT of the horizontal strip and
    // rendered on its own row above it (larger, with its own glow); the other
    // icons fill the strip.  Empty/failed icons drop that indicator.
    for (int i = 0; i < set.count; ++i)
    {
        const BadgeSlot& s = set.slots[i];
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
        item.size = ImVec2(iconSize, iconSize);
        layout.badges.push_back(item);
    }

    if (layout.badges.empty() && !layout.tierEmblemShown)
    {
        return;
    }

    const float rowGap = RenderConstants::BADGE_ROW_GAP * textSizeScale;

    // Icon strip: a compact row centered one gap above the plate's current top
    // edge (the title row; the main row when the title is empty).  `rowTop`
    // becomes the Y the emblem row then sits above.
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

    // Tier emblem: its own row, centered above the icon strip (or above the
    // title directly when the strip is empty).
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

// Measure text and compute all positions for a label.
LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                               ActorCache& entry,
                               const LabelStyle& style,
                               float textSizeScale,
                               const RenderSettingsSnapshot& snap,
                               int forcedCharsToShow)
{
    LabelLayout layout{};

    // Typewriter character count.  A forced budget (Last Rites crumble)
    // overrides the entry-driven reveal.
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

    // Fonts
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
    BuildBadges(layout, d, style, textSizeScale, snap);

    return layout;
}

// Last Rites: pull every resolved style color toward `target` by `mixT`,
// calm the effect strength to match, and re-pack the draw-ready colors.
// Composable -- callers stage multi-stop ramps (sear-bright then dark) by
// applying successive tints to the freshly computed per-frame style.
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

    // The ink is going out -- animated effects calm with it.
    style.strength *= 1.0f - mixT;
    style.effectAlpha *= 1.0f - mixT;
    style.highlight =
        ImGui::ColorConvertFloat4ToU32(ImVec4(target.x, target.y, target.z, style.effectAlpha));

    PackStyleColors(style, style.alpha, snap);
}

// Candlelight Metering: adapt the resolved ink to the scene behind the
// plate.  Bright backgrounds dim the ink a touch; dark backgrounds lift it
// a few percent and pull it fractionally toward the scene's own chroma
// (torchlight warms nearby type).  All adjustments are hard-capped by
// snap.candleStrength -- nobody should ever *name* this effect, only miss
// it when it's gone.  Mapping mirrored in tests/test_utils.cpp.
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

    // Exposure: a mid-grey scene (0.5) leaves the ink untouched.
    const float gain = 1.0f + std::clamp((.5f - bgLum) * 2.0f * strength, -strength, strength);

    // Warmth: only in genuinely dark scenes, and only a whisper.
    float warmT = .0f;
    ImVec4 sceneTint(1.0f, 1.0f, 1.0f, 1.0f);
    if (bgLum < .35f && snap.candleWarmth > .0f)
    {
        // Normalize the scene color so the pull carries hue, not darkness.
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
