/**
 * Unit tests for glyph settings parsing using Google Test.
 *
 * Tests INI parsing logic including tier definitions, color parsing,
 * effect type parsing, and format string handling.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Re-implement parsing helpers (same logic as Settings.cpp)
// ============================================================================

static std::string Trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first)
        return str;
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static float ParseFloat(const std::string& str, float defaultVal)
{
    try
    {
        return std::stof(str);
    }
    catch (...)
    {
        return defaultVal;
    }
}

static int ParseInt(const std::string& str, int defaultVal)
{
    try
    {
        return std::stoi(str);
    }
    catch (...)
    {
        return defaultVal;
    }
}

static void ParseColor3(const std::string& str, float out[3])
{
    std::istringstream ss(str);
    std::string token;
    int idx = 0;
    while (std::getline(ss, token, ',') && idx < 3)
    {
        out[idx++] = ParseFloat(Trim(token), 1.0f);
    }
}

enum class EffectType
{
    None,
    Gradient,
    VerticalGradient,
    DiagonalGradient,
    RadialGradient,
    Shimmer,
    Ember,
    Aurora,
    Sparkle,
    Enchant,
    Frost,
    Breathe,
    Drift,
    Mote,
    Wander
};

static EffectType ParseEffectType(const std::string& str)
{
    std::string s = Trim(str);
    if (s == "None")
        return EffectType::None;
    if (s == "Gradient")
        return EffectType::Gradient;
    if (s == "VerticalGradient")
        return EffectType::VerticalGradient;
    if (s == "DiagonalGradient")
        return EffectType::DiagonalGradient;
    if (s == "RadialGradient")
        return EffectType::RadialGradient;
    if (s == "Shimmer")
        return EffectType::Shimmer;
    if (s == "Ember")
        return EffectType::Ember;
    if (s == "Aurora")
        return EffectType::Aurora;
    if (s == "Sparkle")
        return EffectType::Sparkle;
    if (s == "Enchant")
        return EffectType::Enchant;
    if (s == "Frost")
        return EffectType::Frost;
    if (s == "Breathe")
        return EffectType::Breathe;
    if (s == "Drift")
        return EffectType::Drift;
    if (s == "Mote")
        return EffectType::Mote;
    if (s == "Wander")
        return EffectType::Wander;
    return EffectType::Gradient;  // Default
}

struct Segment
{
    std::string format;
    bool useLevelFont = false;
    bool dropIfBlank = false;  // Set by trailing "?" after closing quote.
};

// Mirrors ParseQuotedSegments in Settings.cpp. `forceLevelFont` is the
// InfoFormat-row mode; when false (Format-row), title segments (`%t`) are
// extracted into `titleFormat` and other segments use level font iff they
// contain `%l`.
static std::vector<Segment> ParseFormat(const std::string& val,
                                        std::string& titleFormat,
                                        bool forceLevelFont = false)
{
    std::vector<Segment> segments;
    bool inQuote = false;
    bool justClosed = false;
    std::string current;
    Segment* lastPushed = nullptr;

    for (size_t i = 0; i < val.size(); ++i)
    {
        char c = val[i];
        if (c == '\\' && i + 1 < val.size())
        {
            if (inQuote)
            {
                current += val[++i];
            }
            justClosed = false;
            continue;
        }
        if (c == '"')
        {
            if (inQuote)
            {
                if (!forceLevelFont && current.find("%t") != std::string::npos)
                {
                    titleFormat = current;
                    lastPushed = nullptr;
                }
                else
                {
                    bool isLevel = forceLevelFont || current.find("%l") != std::string::npos;
                    segments.push_back({current, isLevel, false});
                    lastPushed = &segments.back();
                }
                current.clear();
                inQuote = false;
                justClosed = true;
            }
            else
            {
                inQuote = true;
                justClosed = false;
            }
            continue;
        }
        if (inQuote)
        {
            current += c;
            justClosed = false;
            continue;
        }
        if (justClosed && c == '?')
        {
            if (lastPushed != nullptr)
            {
                lastPushed->dropIfBlank = true;
            }
        }
        justClosed = false;
    }
    return segments;
}

// ============================================================================
// Tests: Trim
// ============================================================================

TEST(TrimTest, RemovesLeadingSpaces)
{
    EXPECT_EQ(Trim("   hello"), "hello");
}

TEST(TrimTest, RemovesTrailingSpaces)
{
    EXPECT_EQ(Trim("hello   "), "hello");
}

TEST(TrimTest, RemovesBoth)
{
    EXPECT_EQ(Trim("   hello   "), "hello");
}

TEST(TrimTest, HandlesTabsAndNewlines)
{
    EXPECT_EQ(Trim("\t\nhello\r\n"), "hello");
}

TEST(TrimTest, PreservesInternalSpaces)
{
    EXPECT_EQ(Trim("  hello world  "), "hello world");
}

TEST(TrimTest, HandlesEmptyString)
{
    std::string result = Trim("");
    EXPECT_TRUE(result.empty() || result == "");
}

// ============================================================================
// Tests: ParseFloat
// ============================================================================

TEST(ParseFloatTest, Valid)
{
    EXPECT_NEAR(ParseFloat("3.14", 0.0f), 3.14f, 0.001f);
}

TEST(ParseFloatTest, WithSpaces)
{
    EXPECT_NEAR(ParseFloat(" 2.5 ", 0.0f), 2.5f, 0.001f);
}

TEST(ParseFloatTest, Negative)
{
    EXPECT_NEAR(ParseFloat("-1.5", 0.0f), -1.5f, 0.001f);
}

TEST(ParseFloatTest, InvalidReturnsDefault)
{
    EXPECT_NEAR(ParseFloat("abc", 42.0f), 42.0f, 0.001f);
}

TEST(ParseFloatTest, EmptyReturnsDefault)
{
    EXPECT_NEAR(ParseFloat("", 99.0f), 99.0f, 0.001f);
}

// ============================================================================
// Tests: ParseInt
// ============================================================================

TEST(ParseIntTest, Valid)
{
    EXPECT_EQ(ParseInt("42", 0), 42);
}

TEST(ParseIntTest, Negative)
{
    EXPECT_EQ(ParseInt("-10", 0), -10);
}

TEST(ParseIntTest, InvalidReturnsDefault)
{
    EXPECT_EQ(ParseInt("xyz", 99), 99);
}

TEST(ParseIntTest, FloatTruncates)
{
    // stoi stops at decimal point
    EXPECT_EQ(ParseInt("3.14", 0), 3);
}

// ============================================================================
// Tests: ParseColor3
// ============================================================================

TEST(ParseColor3Test, RGB)
{
    float color[3] = {0, 0, 0};
    ParseColor3("0.5, 0.75, 1.0", color);
    EXPECT_NEAR(color[0], 0.5f, 0.01f);
    EXPECT_NEAR(color[1], 0.75f, 0.01f);
    EXPECT_NEAR(color[2], 1.0f, 0.01f);
}

TEST(ParseColor3Test, NoSpaces)
{
    float color[3] = {0, 0, 0};
    ParseColor3("0.1,0.2,0.3", color);
    EXPECT_NEAR(color[0], 0.1f, 0.01f);
    EXPECT_NEAR(color[1], 0.2f, 0.01f);
    EXPECT_NEAR(color[2], 0.3f, 0.01f);
}

TEST(ParseColor3Test, PartialDefaults)
{
    float color[3] = {0, 0, 0};
    ParseColor3("0.5", color);
    EXPECT_NEAR(color[0], 0.5f, 0.01f);
    // Only first value parsed, others remain 0
}

// ============================================================================
// Tests: ParseEffectType
// ============================================================================

TEST(ParseEffectTypeTest, None)
{
    EXPECT_EQ(ParseEffectType("None"), EffectType::None);
}

TEST(ParseEffectTypeTest, Gradient)
{
    EXPECT_EQ(ParseEffectType("Gradient"), EffectType::Gradient);
}

TEST(ParseEffectTypeTest, Aurora)
{
    EXPECT_EQ(ParseEffectType("Aurora"), EffectType::Aurora);
}

TEST(ParseEffectTypeTest, Ember)
{
    EXPECT_EQ(ParseEffectType("Ember"), EffectType::Ember);
}

TEST(ParseEffectTypeTest, Enchant)
{
    EXPECT_EQ(ParseEffectType("Enchant"), EffectType::Enchant);
}

TEST(ParseEffectTypeTest, Frost)
{
    EXPECT_EQ(ParseEffectType("Frost"), EffectType::Frost);
}

TEST(ParseEffectTypeTest, Breathe)
{
    EXPECT_EQ(ParseEffectType("Breathe"), EffectType::Breathe);
}

TEST(ParseEffectTypeTest, Drift)
{
    EXPECT_EQ(ParseEffectType("Drift"), EffectType::Drift);
}

TEST(ParseEffectTypeTest, Mote)
{
    EXPECT_EQ(ParseEffectType("Mote"), EffectType::Mote);
}

TEST(ParseEffectTypeTest, Wander)
{
    EXPECT_EQ(ParseEffectType("Wander"), EffectType::Wander);
}

TEST(ParseEffectTypeTest, WithWhitespace)
{
    EXPECT_EQ(ParseEffectType("  Shimmer  "), EffectType::Shimmer);
}

TEST(ParseEffectTypeTest, UnknownDefaultsToGradient)
{
    EXPECT_EQ(ParseEffectType("Unknown"), EffectType::Gradient);
    EXPECT_EQ(ParseEffectType(""), EffectType::Gradient);
}

// ============================================================================
// Tests: ParseFormat
// ============================================================================

TEST(ParseFormatTest, SimpleName)
{
    std::string title;
    auto segs = ParseFormat("\"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].useLevelFont);
}

TEST(ParseFormatTest, NameAndLevel)
{
    std::string title;
    auto segs = ParseFormat("\"%n\" \"Lv.%l\"", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].useLevelFont);
    EXPECT_EQ(segs[1].format, "Lv.%l");
    EXPECT_TRUE(segs[1].useLevelFont);
}

TEST(ParseFormatTest, ExtractsTitle)
{
    std::string title;
    auto segs = ParseFormat("\"%t\" \"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);  // Only %n segment
    EXPECT_EQ(title, "%t");
}

TEST(ParseFormatTest, EscapedQuotes)
{
    std::string title;
    auto segs = ParseFormat("\"\\\"hello\\\"\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "\"hello\"");
}

TEST(ParseFormatTest, Empty)
{
    std::string title;
    auto segs = ParseFormat("", title);
    EXPECT_EQ(segs.size(), 0u);
}

TEST(ParseFormatTest, DropIfBlankDefaultsToFalse)
{
    std::string title;
    auto segs = ParseFormat("\"%n\"", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_FALSE(segs[0].dropIfBlank);
}

TEST(ParseFormatTest, TrailingQuestionMarkSetsDropIfBlank)
{
    std::string title;
    auto segs = ParseFormat("\"%n\" \"%r\"?", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].format, "%n");
    EXPECT_FALSE(segs[0].dropIfBlank);
    EXPECT_EQ(segs[1].format, "%r");
    EXPECT_TRUE(segs[1].dropIfBlank);
}

TEST(ParseFormatTest, MultipleDropIfBlankSegments)
{
    std::string title;
    auto segs = ParseFormat("\"%n\" \"  %r\"? \"  %d\"? \"  %c\"?", title);
    ASSERT_EQ(segs.size(), 4u);
    EXPECT_FALSE(segs[0].dropIfBlank);
    EXPECT_TRUE(segs[1].dropIfBlank);
    EXPECT_TRUE(segs[2].dropIfBlank);
    EXPECT_TRUE(segs[3].dropIfBlank);
}

TEST(ParseFormatTest, QuestionMarkAfterWhitespaceIsLiteral)
{
    // The "?" must be immediately adjacent to the closing quote.
    // "?" separated from the close by whitespace is ignored (parser eats it
    // along with other non-quote, non-special chars outside quotes).
    std::string title;
    auto segs = ParseFormat("\"%n\" ? \"%r\"", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_FALSE(segs[0].dropIfBlank);
    EXPECT_FALSE(segs[1].dropIfBlank);
}

TEST(ParseFormatTest, BackToBackDropSegments)
{
    // "%n"?"%r"? -- no whitespace between segments must still work.
    std::string title;
    auto segs = ParseFormat("\"%n\"?\"%r\"?", title);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_TRUE(segs[0].dropIfBlank);
    EXPECT_TRUE(segs[1].dropIfBlank);
}

TEST(ParseFormatTest, TitleSegmentNotMarkedDroppable)
{
    // "?" after a title-bearing segment must not affect the display vector
    // (title is hoisted, no segment is pushed).
    std::string title;
    auto segs = ParseFormat("\"%t\"? \"%n\"", title);
    EXPECT_EQ(title, "%t");
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_FALSE(segs[0].dropIfBlank);
}

TEST(ParseFormatTest, EscapedQuotesDoNotTriggerJustClosed)
{
    // The closing quote of a segment containing an escaped \" must still
    // accept a trailing "?" -- escapes inside quotes don't break tracking.
    std::string title;
    auto segs = ParseFormat("\"\\\"hi\\\"\"?", title);
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].format, "\"hi\"");
    EXPECT_TRUE(segs[0].dropIfBlank);
}

// ============================================================================
// Tests: ParseFormat -- InfoFormat mode (forceLevelFont = true)
// ============================================================================

TEST(InfoFormatTest, ForceLevelFontOverridesAutoDetect)
{
    std::string title;
    auto segs = ParseFormat("\"%r\" \"%d\" \"Lv.%l\"", title, /*forceLevelFont*/ true);
    ASSERT_EQ(segs.size(), 3u);
    EXPECT_TRUE(segs[0].useLevelFont);
    EXPECT_TRUE(segs[1].useLevelFont);
    EXPECT_TRUE(segs[2].useLevelFont);
}

TEST(InfoFormatTest, NoTitleExtractionInInfoMode)
{
    // In InfoFormat mode, segments with %t are NOT hoisted to title;
    // they render inline like any other segment.
    std::string title;
    auto segs = ParseFormat("\"%t\" \"%r\"", title, /*forceLevelFont*/ true);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].format, "%t");
    EXPECT_TRUE(title.empty());
}

TEST(InfoFormatTest, DropIfBlankStillWorks)
{
    std::string title;
    auto segs = ParseFormat("\"%r\"? \"  %d\"? \"  %c\"?", title, /*forceLevelFont*/ true);
    ASSERT_EQ(segs.size(), 3u);
    for (const auto& s : segs)
    {
        EXPECT_TRUE(s.useLevelFont);
        EXPECT_TRUE(s.dropIfBlank);
    }
}

// ============================================================================
// Focus Target Expanded Nameplate settings
// ============================================================================
//
// Mirrors Settings::FocusSettings + the clamping rules registered for it in
// Settings.cpp's kSettings binding table.  Production code uses a descriptor
// table for clamping; here we re-implement the same rules so we can validate
// them without linking the SKSE plugin.

namespace focus_test
{

struct FocusSettings
{
    bool Enabled = false;
    float ConeAngleDegrees = 8.0f;
    float MaxDistance = 0.0f;
    float AmbientDimFactor = 0.55f;
    float SettleTime = 0.25f;
    bool IgnoreOccluded = true;
};

// Apply the same per-field validation rules as kSettings in Settings.cpp.
static void ClampFocus(FocusSettings& f)
{
    f.ConeAngleDegrees = std::clamp(f.ConeAngleDegrees, 0.5f, 45.0f);
    f.AmbientDimFactor = std::clamp(f.AmbientDimFactor, 0.05f, 1.0f);
    f.SettleTime = std::clamp(f.SettleTime, 0.0f, 2.0f);
    if (f.MaxDistance < 0.0f)
    {
        f.MaxDistance = 0.0f;
    }
}

// Mirror of Settings.cpp's ParseBool -- accept true/false/1/0/yes/no.
static bool ParseBool(const std::string& s)
{
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

}  // namespace focus_test

TEST(FocusSettings, DefaultsMatchProduction)
{
    focus_test::FocusSettings f;
    EXPECT_FALSE(f.Enabled);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 8.0f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 0.0f);
    EXPECT_FLOAT_EQ(f.AmbientDimFactor, 0.55f);
    EXPECT_FLOAT_EQ(f.SettleTime, 0.25f);
    EXPECT_TRUE(f.IgnoreOccluded);
}

TEST(FocusSettings, ClampConeAngleBelowMin)
{
    focus_test::FocusSettings f;
    f.ConeAngleDegrees = 0.1f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 0.5f);
}

TEST(FocusSettings, ClampConeAngleAboveMax)
{
    focus_test::FocusSettings f;
    f.ConeAngleDegrees = 90.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 45.0f);
}

TEST(FocusSettings, ConeAngleInRangePreserved)
{
    focus_test::FocusSettings f;
    f.ConeAngleDegrees = 12.5f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.ConeAngleDegrees, 12.5f);
}

TEST(FocusSettings, ClampAmbientDimFactorBelowMin)
{
    focus_test::FocusSettings f;
    f.AmbientDimFactor = 0.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.AmbientDimFactor, 0.05f);
}

TEST(FocusSettings, ClampAmbientDimFactorAboveMax)
{
    focus_test::FocusSettings f;
    f.AmbientDimFactor = 2.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.AmbientDimFactor, 1.0f);
}

TEST(FocusSettings, ClampSettleTimeNegative)
{
    focus_test::FocusSettings f;
    f.SettleTime = -0.5f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.SettleTime, 0.0f);
}

TEST(FocusSettings, ClampSettleTimeAboveMax)
{
    focus_test::FocusSettings f;
    f.SettleTime = 5.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.SettleTime, 2.0f);
}

TEST(FocusSettings, SettleTimeZeroAllowed)
{
    // SettleTime = 0 is a legitimate "instant focus" mode -- must not be clamped up.
    focus_test::FocusSettings f;
    f.SettleTime = 0.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.SettleTime, 0.0f);
}

TEST(FocusSettings, MaxDistanceZeroIsSentinel)
{
    // 0.0 means "reuse MaxScanDistance"; it must survive validation unchanged.
    focus_test::FocusSettings f;
    f.MaxDistance = 0.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 0.0f);
}

TEST(FocusSettings, MaxDistanceNegativeClampedToZero)
{
    focus_test::FocusSettings f;
    f.MaxDistance = -100.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 0.0f);
}

TEST(FocusSettings, MaxDistancePositivePreserved)
{
    focus_test::FocusSettings f;
    f.MaxDistance = 1500.0f;
    focus_test::ClampFocus(f);
    EXPECT_FLOAT_EQ(f.MaxDistance, 1500.0f);
}

TEST(FocusSettings, BoolParseTrueVariants)
{
    EXPECT_TRUE(focus_test::ParseBool("true"));
    EXPECT_TRUE(focus_test::ParseBool("True"));
    EXPECT_TRUE(focus_test::ParseBool("TRUE"));
    EXPECT_TRUE(focus_test::ParseBool("1"));
    EXPECT_TRUE(focus_test::ParseBool("yes"));
    EXPECT_TRUE(focus_test::ParseBool("on"));
}

TEST(FocusSettings, BoolParseFalseVariants)
{
    EXPECT_FALSE(focus_test::ParseBool("false"));
    EXPECT_FALSE(focus_test::ParseBool("False"));
    EXPECT_FALSE(focus_test::ParseBool("0"));
    EXPECT_FALSE(focus_test::ParseBool("no"));
    EXPECT_FALSE(focus_test::ParseBool(""));
    EXPECT_FALSE(focus_test::ParseBool("anything-else"));
}
