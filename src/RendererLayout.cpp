#include "RendererInternal.h"

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

// Replace %n, %l, %t placeholders in a format string.
// Single-pass to avoid expanding placeholders embedded in substitution values.
std::string FormatString(const std::string& fmt,
                         const std::string_view nameVal,
                         int levelVal,
                         const char* titleVal)
{
    const std::string lStr = std::to_string(levelVal);

    std::string result;
    result.reserve(fmt.size() + nameVal.size());

    for (size_t i = 0; i < fmt.size(); ++i)
    {
        if (fmt[i] == '%' && i + 1 < fmt.size())
        {
            switch (fmt[i + 1])
            {
                case 'n':
                    result.append(nameVal.data(), nameVal.size());
                    ++i;
                    continue;
                case 'l':
                    result.append(lStr);
                    ++i;
                    continue;
                case 't':
                    if (titleVal)
                    {
                        result.append(titleVal);
                        ++i;
                        continue;
                    }
                    break;
            }
        }
        result += fmt[i];
    }
    return result;
}

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

// Compute all color values and pack them into the LabelStyle.
static void ComputeTierColors(LabelStyle& style,
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
    const float tierIntensity = under100 ? .5f : 1.0f;

    // Base tier colors are intentionally softened at low levels, then later
    // re-saturated by the user-controlled vibrancy knobs.
    auto Pastelize = [&](const Settings::Color3& c) -> ImVec4
    {
        float t = snap.nameColorMix + (1.0f - snap.nameColorMix) * levelT;
        if (under100)
        {
            // Early earthy tiers carry much darker secondary hues than the
            // prestige bands. Keep them lifted so the name fill stays readable.
            t = std::clamp(t - .18f, .0f, 1.0f);
        }
        return ImVec4(
            1.0f + (c.r - 1.0f) * t, 1.0f + (c.g - 1.0f) * t, 1.0f + (c.b - 1.0f) * t, 1.0f);
    };

    style.Lc = Pastelize(tier.leftColor);
    style.Rc = Pastelize(tier.rightColor);

    const float effectAlphaMul =
        snap.effectAlphaMin + (snap.effectAlphaMax - snap.effectAlphaMin) * levelT;
    style.effectAlpha = alpha * tierIntensity * effectAlphaMul;

    auto MixToWhite = [](ImVec4 c, float amount)
    {
        amount = std::clamp(amount, .0f, 1.0f);
        return ImVec4(1.0f + (c.x - 1.0f) * amount,
                      1.0f + (c.y - 1.0f) * amount,
                      1.0f + (c.z - 1.0f) * amount,
                      c.w);
    };
    auto MixColors = [](const ImVec4& a, const ImVec4& b, float t)
    {
        t = std::clamp(t, .0f, 1.0f);
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
    };

    const float baseColorAmount = under100 ? (.35f + .65f * tierIntensity) : 1.0f;
    const ImVec4 highlightColor(
        tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, 1.0f);
    const ImVec4 vividNameL(tier.leftColor.r, tier.leftColor.g, tier.leftColor.b, 1.0f);
    const ImVec4 vividNameR(tier.rightColor.r, tier.rightColor.g, tier.rightColor.b, 1.0f);
    ImVec4 vividLevelL = vividNameL;
    ImVec4 vividLevelR = vividNameR;
    ImVec4 vividTitleL = vividNameL;
    ImVec4 vividTitleR = vividNameR;

    // Name colors stay anchored to the tier's main gradient.
    style.LcName = style.Lc;
    style.RcName = style.Rc;

    // Level colors: use per-tier override if set, else derive a softer supporting accent
    // from the name colors plus a touch of the tier highlight.
    if (tier.levelLeftColor)
    {
        vividLevelL =
            ImVec4(tier.levelLeftColor->r, tier.levelLeftColor->g, tier.levelLeftColor->b, 1.0f);
        style.LcLevel = Pastelize(*tier.levelLeftColor);
    }
    else
    {
        vividLevelL = MixColors(vividNameL, highlightColor, .18f + .10f * levelT);
        style.LcLevel = MixToWhite(vividLevelL, baseColorAmount * .90f);
    }
    if (tier.levelRightColor)
    {
        vividLevelR =
            ImVec4(tier.levelRightColor->r, tier.levelRightColor->g, tier.levelRightColor->b, 1.0f);
        style.RcLevel = Pastelize(*tier.levelRightColor);
    }
    else
    {
        vividLevelR = MixColors(vividNameR, highlightColor, .18f + .10f * levelT);
        style.RcLevel = MixToWhite(vividLevelR, baseColorAmount * .90f);
    }

    // Title colors: use per-tier override if set, else derive a companion accent from
    // the same tier palette rather than cloning the name band.
    if (tier.titleLeftColor)
    {
        vividTitleL =
            ImVec4(tier.titleLeftColor->r, tier.titleLeftColor->g, tier.titleLeftColor->b, 1.0f);
        style.LcTitle = Pastelize(*tier.titleLeftColor);
    }
    else
    {
        vividTitleL = MixColors(MixColors(vividNameL, vividNameR, .18f), highlightColor, .34f);
        style.LcTitle = MixToWhite(vividTitleL, baseColorAmount * .82f);
    }
    if (tier.titleRightColor)
    {
        vividTitleR =
            ImVec4(tier.titleRightColor->r, tier.titleRightColor->g, tier.titleRightColor->b, 1.0f);
        style.RcTitle = Pastelize(*tier.titleRightColor);
    }
    else
    {
        vividTitleR = MixColors(MixColors(vividNameR, vividNameL, .18f), highlightColor, .42f);
        style.RcTitle = MixToWhite(vividTitleR, baseColorAmount * .82f);
    }

    style.specialGlowColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    if (style.specialTitle)
    {
        const auto* st = style.specialTitle;
        ImVec4 specialCol(st->color.r, st->color.g, st->color.b, 1.0f);
        style.specialGlowColor = ImVec4(st->glowColor.r, st->glowColor.g, st->glowColor.b, 1.0f);

        style.Lc = specialCol;
        style.Rc = specialCol;
        style.LcLevel = specialCol;
        style.RcLevel = specialCol;
        style.LcName = specialCol;
        style.RcName = specialCol;
        style.LcTitle = specialCol;
        style.RcTitle = specialCol;
    }

    // Higher tiers can claw some saturation back from the softened base palette.
    if (snap.tierVibrancyBoost > .0f && snap.tiers.size() > 1 && !style.specialTitle)
    {
        float tierProgress =
            static_cast<float>(style.tierIdx) / static_cast<float>(snap.tiers.size() - 1);
        float boost = snap.tierVibrancyBoost * tierProgress;
        auto Vivify = [&](ImVec4& washed, const ImVec4& vivid)
        {
            washed.x += (vivid.x - washed.x) * boost;
            washed.y += (vivid.y - washed.y) * boost;
            washed.z += (vivid.z - washed.z) * boost;
        };
        // Preserve element-specific palette roles instead of collapsing every
        // tier back toward the main name band at higher vibrancy settings.
        Vivify(style.LcName, vividNameL);
        Vivify(style.RcName, vividNameR);
        Vivify(style.LcLevel, vividLevelL);
        Vivify(style.RcLevel, vividLevelR);
        Vivify(style.LcTitle, vividTitleL);
        Vivify(style.RcTitle, vividTitleR);
    }

    // Global saturation boost: pull text colors away from gray without changing alpha.
    if (snap.textSaturationBoost > .0f)
    {
        const float s = 1.0f + snap.textSaturationBoost;
        auto BoostSat = [s](ImVec4& c) { BoostSaturation(c, s); };
        BoostSat(style.LcName);
        BoostSat(style.RcName);
        BoostSat(style.LcLevel);
        BoostSat(style.RcLevel);
        BoostSat(style.LcTitle);
        BoostSat(style.RcTitle);
    }

    if (style.specialTitle)
    {
        style.supportName = ImVec4(style.LcName.x, style.LcName.y, style.LcName.z, 1.0f);
        style.supportLevel = style.supportName;
        style.supportTitle = style.supportName;
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

    // Pack colors to ImU32
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
    style.highlight = ImGui::ColorConvertFloat4ToU32(ImVec4(
        tier.highlightColor.r, tier.highlightColor.g, tier.highlightColor.b, style.effectAlpha));

    // Outline width data (actual widths computed after font sizes are known)
    style.baseOutlineWidth = snap.outlineWidthMin + snap.outlineWidthMax;
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
    const float tierIntensity = under100 ? .5f : 1.0f;

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

    float tierAnimSpeed = snap.animSpeedLowTier;
    if (snap.tiers.size() > 1)
    {
        float tierRatio =
            static_cast<float>(style.tierIdx) / static_cast<float>(snap.tiers.size() - 1);
        if (tierRatio >= .9f)
        {
            tierAnimSpeed = snap.animSpeedHighTier;
        }
        else if (tierRatio >= .8f)
        {
            tierAnimSpeed = snap.animSpeedMidTier;
        }
    }
    if (under100)
    {
        tierAnimSpeed *= .75f;
    }

    const float phaseSeed = (formID & 1023) / 1023.0f;
    style.phase01 = frac(time * tierAnimSpeed + phaseSeed);

    style.strength =
        tierIntensity * (snap.strengthMin + (snap.strengthMax - snap.strengthMin) * levelT);
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

    // Disposition color
    if (d.dispo == Disposition::Enemy)
    {
        style.dispoCol = ImVec4(.9f, .2f, .2f, alpha);
    }
    else if (d.dispo == Disposition::AllyOrFriend)
    {
        style.dispoCol = ImVec4(.2f, .6f, 1.0f, alpha);
    }
    else
    {
        style.dispoCol = ImVec4(.9f, .9f, .9f, alpha);
    }

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
    ComputeTierColors(style, *tierPtr, lv, alpha, snap);
    ComputeAnimationParams(style, lv, d.formID, time, snap);

    return style;
}

// ============================================================================
// ComputeLabelLayout helpers
// ============================================================================

// Build rendered segments from the display format, applying typewriter truncation.
static void BuildSegments(LabelLayout& layout,
                          const ActorDrawData& d,
                          const LabelStyle& style,
                          float textSizeScale,
                          int typewriterCharsToShow,
                          int& totalCharsProcessed,
                          const RenderSettingsSnapshot& snap)
{
    const char* safeName = d.name.empty() ? " " : d.name.c_str();

    layout.mainLineWidth = .0f;
    layout.mainLineHeight = .0f;

    static const std::vector<Settings::Segment> kDefaultDisplayFormat = {{"%n", false},
                                                                         {" Lv.%l", true}};

    const auto& fmtList = snap.displayFormat.empty() ? kDefaultDisplayFormat : snap.displayFormat;

    for (const auto& fmt : fmtList)
    {
        RenderSeg seg;
        seg.text = FormatString(fmt.format, safeName, d.level);
        seg.isLevel = fmt.useLevelFont;
        seg.font = seg.isLevel ? layout.fontLevel : layout.fontName;
        seg.fontSize = seg.isLevel ? layout.levelFontSize : layout.nameFontSize;
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

        layout.segments.push_back(seg);
        layout.mainLineWidth += seg.size.x;
        if (seg.size.y > layout.mainLineHeight)
        {
            layout.mainLineHeight = seg.size.y;
        }
    }

    const float spacingScale = snap.proportionalSpacing ? textSizeScale : 1.0f;
    layout.segmentPadding = snap.segmentPadding * spacingScale;
    if (!layout.segments.empty())
    {
        layout.mainLineWidth += (layout.segments.size() - 1) * layout.segmentPadding;
    }
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
    const char* safeName = d.name.empty() ? " " : d.name.c_str();
    const float outlineWidth = style.outlineWidth;

    // Title
    const char* titleToUse =
        style.specialTitle ? style.specialTitle->displayTitle.c_str() : style.tier->title.c_str();
    layout.titleStr = FormatString(snap.titleFormat, safeName, d.level, titleToUse);
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
    if (titleText && *titleText)
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

    const float spacingScale =
        snap.proportionalSpacing ? (layout.nameFontSize / layout.fontName->FontSize) : 1.0f;
    const float titleShadowY = snap.titleShadowOffsetY * spacingScale;
    const float mainShadowY = snap.mainShadowOffsetY * spacingScale;
    float titleBottomDraw = titleBottom + titleShadowY;
    float mainTopDraw = mainTop - outlineWidth;
    float mainBottomDraw = mainBottom + outlineWidth + mainShadowY;

    layout.mainLineY = -mainBottomDraw;
    const float titleGap = snap.titleMainGap * spacingScale;
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

    layout.totalWidth = std::max(layout.mainLineWidth, layout.titleSize.x);

    layout.nameplateTop = layout.startPos.y + layout.titleY + titleTop;
    layout.nameplateBottom = layout.startPos.y + layout.mainLineY + mainBottom;
    layout.nameplateLeft = layout.startPos.x - layout.totalWidth * .5f;
    layout.nameplateRight = layout.startPos.x + layout.totalWidth * .5f;
    layout.nameplateWidth = layout.totalWidth;
    layout.nameplateHeight = layout.nameplateBottom - layout.nameplateTop;
    layout.nameplateCenter =
        ImVec2(layout.startPos.x, (layout.nameplateTop + layout.nameplateBottom) * .5f);
    layout.mainLineCenterY = layout.startPos.y + layout.mainLineY + (mainTop + mainBottom) * .5f;
}

// Measure text and compute all positions for a label.
LabelLayout ComputeLabelLayout(const ActorDrawData& d,
                               ActorCache& entry,
                               const LabelStyle& style,
                               float textSizeScale,
                               const RenderSettingsSnapshot& snap)
{
    LabelLayout layout{};

    // Typewriter character count
    int typewriterCharsToShow = -1;
    if (snap.enableTypewriter && !entry.typewriterComplete)
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

    return layout;
}

}  // namespace Renderer
