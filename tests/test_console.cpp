// the dispatcher requires game code; this suite covers its parsing and override store.

#include "ActorOverrides.hpp"
#include "ConsoleParse.hpp"
#include "RenderConstants.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
using ActorOverrides::Record;
using ActorOverrides::Slot;

TEST(ConsoleParseTest, TokenizeSplitsOnWhitespaceRuns)
{
    const auto tokens = ConsoleParse::Tokenize("glyph  title\tThe Grey   Fox ");
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0], "glyph");
    EXPECT_EQ(tokens[1], "title");
    EXPECT_EQ(tokens[2], "The");
    EXPECT_EQ(tokens[3], "Grey");
    EXPECT_EQ(tokens[4], "Fox");
}

TEST(ConsoleParseTest, RestAfterTokensKeepsInteriorSpacingAndTrimsTail)
{
    EXPECT_EQ(ConsoleParse::RestAfterTokens("glyph title The  Grey Fox  ", 2), "The  Grey Fox");
}

TEST(ConsoleParseTest, RestAfterTokensWorksWithoutTheLeadingCommandWord)
{
    EXPECT_EQ(ConsoleParse::RestAfterTokens("title a  b", 1), "a  b");
}

TEST(ConsoleParseTest, RestAfterTokensPastTheEndIsEmpty)
{
    EXPECT_EQ(ConsoleParse::RestAfterTokens("glyph title", 2), "");
    EXPECT_EQ(ConsoleParse::RestAfterTokens("glyph title   ", 2), "");
    EXPECT_EQ(ConsoleParse::RestAfterTokens("", 0), "");
}

TEST(ConsoleParseTest, StripSurroundingQuotesRemovesOnlyOneMatchingPair)
{
    EXPECT_EQ(ConsoleParse::StripSurroundingQuotes("\"a b\""), "a b");
    EXPECT_EQ(ConsoleParse::StripSurroundingQuotes("\"\"a\"\""), "\"a\"");
    EXPECT_EQ(ConsoleParse::StripSurroundingQuotes("\"a b"), "\"a b");
    EXPECT_EQ(ConsoleParse::StripSurroundingQuotes("a \"b\""), "a \"b\"");
    EXPECT_EQ(ConsoleParse::StripSurroundingQuotes("\"\""), "");
    EXPECT_EQ(ConsoleParse::StripSurroundingQuotes("\""), "\"");
}

TEST(ConsoleParseTest, ParseColorTripletAcceptsSpacedAndTightForms)
{
    const auto tight = ConsoleParse::ParseColorTriplet("0.9,0.3,0.3");
    ASSERT_TRUE(tight.has_value());
    EXPECT_FLOAT_EQ(tight->r, 0.9f);
    EXPECT_FLOAT_EQ(tight->g, 0.3f);
    EXPECT_FLOAT_EQ(tight->b, 0.3f);

    const auto spaced = ConsoleParse::ParseColorTriplet("0.9, 0.3, 0.3");
    ASSERT_TRUE(spaced.has_value());
    EXPECT_EQ(*spaced, *tight);
}

TEST(ConsoleParseTest, ParseColorTripletClampsToUnitRange)
{
    const auto c = ConsoleParse::ParseColorTriplet("2,-1,0.5");
    ASSERT_TRUE(c.has_value());
    EXPECT_FLOAT_EQ(c->r, 1.0f);
    EXPECT_FLOAT_EQ(c->g, 0.0f);
    EXPECT_FLOAT_EQ(c->b, 0.5f);
}

TEST(ConsoleParseTest, ParseColorTripletRejectsWrongArityAndText)
{
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("0.1,0.2").has_value());
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("0.1,0.2,0.3,0.4").has_value());
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("a,b,c").has_value());
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("").has_value());
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("0.1,,0.3").has_value());
}

TEST(ConsoleParseTest, IsSafeIconNameRejectsEmptyAndPathCharacters)
{
    EXPECT_TRUE(ConsoleParse::IsSafeIconName("anchor"));
    EXPECT_TRUE(ConsoleParse::IsSafeIconName("shield-halved"));
    EXPECT_TRUE(ConsoleParse::IsSafeIconName("user_shield2"));
    EXPECT_FALSE(ConsoleParse::IsSafeIconName(""));
    EXPECT_FALSE(ConsoleParse::IsSafeIconName(".."));
    EXPECT_FALSE(ConsoleParse::IsSafeIconName("a/b"));
    EXPECT_FALSE(ConsoleParse::IsSafeIconName("a\\b"));
    EXPECT_FALSE(ConsoleParse::IsSafeIconName("a.svg"));
    EXPECT_FALSE(ConsoleParse::IsSafeIconName("caf\xC3\xA9"));
}

TEST(ConsoleParseTest, ParseColorTripletRejectsNonFiniteChannels)
{
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("nan,nan,nan").has_value());
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("inf,0,0").has_value());
    EXPECT_FALSE(ConsoleParse::ParseColorTriplet("0,-inf,0").has_value());
}

TEST(ConsoleParseTest, TrimRemovesOuterWhitespaceOnly)
{
    EXPECT_EQ(ConsoleParse::Trim("  a  b 	"), "a  b");
    EXPECT_EQ(ConsoleParse::Trim("ab"), "ab");
    EXPECT_EQ(ConsoleParse::Trim("   "), "");
    EXPECT_EQ(ConsoleParse::Trim(""), "");
}

TEST(ConsoleParseTest, ParseTriStateKeepsTheExistingVocabulary)
{
    EXPECT_EQ(ConsoleParse::ParseTriState("on"), ConsoleParse::TriState::On);
    EXPECT_EQ(ConsoleParse::ParseTriState("YES"), ConsoleParse::TriState::On);
    EXPECT_EQ(ConsoleParse::ParseTriState("0"), ConsoleParse::TriState::Off);
    EXPECT_EQ(ConsoleParse::ParseTriState("false"), ConsoleParse::TriState::Off);
    EXPECT_EQ(ConsoleParse::ParseTriState("maybe"), ConsoleParse::TriState::Toggle);
}

TEST(ConsoleParseTest, IsPrefixOfRequiresANonEmptyLeadingPrefix)
{
    EXPECT_TRUE(ConsoleParse::IsPrefixOf("t", "title"));
    EXPECT_TRUE(ConsoleParse::IsPrefixOf("title", "title"));
    EXPECT_FALSE(ConsoleParse::IsPrefixOf("", "title"));
    EXPECT_FALSE(ConsoleParse::IsPrefixOf("titles", "title"));
    EXPECT_FALSE(ConsoleParse::IsPrefixOf("itle", "title"));
}

TEST(ConsoleVocabularyTest, EverySlotRoundTripsThroughItsName)
{
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Slot::Count); ++i)
    {
        const auto slot = static_cast<Slot>(i);
        const auto parsed = ActorOverrides::ParseSlot(ActorOverrides::SlotName(slot));
        ASSERT_TRUE(parsed.has_value()) << "slot " << static_cast<int>(i);
        EXPECT_EQ(*parsed, slot);
    }
}

TEST(ConsoleVocabularyTest, SlotNamesFollowTheIniVocabulary)
{
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Rank), "rank");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Relationship), "relationship");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Creature), "creature");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Role), "role");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Protection), "protection");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Threat), "threat");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Engagement), "engagement");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Sneak), "sneak");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Encumbered), "encumbered");
    EXPECT_EQ(ActorOverrides::SlotName(Slot::Bounty), "bounty");
}

TEST(ConsoleVocabularyTest, WeightIsAnAliasForEncumbered)
{
    const auto slot = ActorOverrides::ParseSlot("weight");
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(*slot, Slot::Encumbered);
}

TEST(ConsoleVocabularyTest, ParseSlotMatchesExactlyNeverByPrefix)
{
    EXPECT_FALSE(ActorOverrides::ParseSlot("rel").has_value());
    EXPECT_FALSE(ActorOverrides::ParseSlot("").has_value());
    EXPECT_TRUE(ActorOverrides::ParseSlot("Relationship").has_value());  // case folds
}

TEST(ConsoleVocabularyTest, NpcStatesEncodeInDeclarationOrder)
{
    struct Case
    {
        Slot slot;
        const char* word;
        std::uint8_t value;
    };
    const Case cases[] = {
        {Slot::Relationship, "hostile", 0}, {Slot::Relationship, "neutral", 1},
        {Slot::Relationship, "ally", 2},    {Slot::Relationship, "follower", 3},
        {Slot::Creature, "humanoid", 0},    {Slot::Creature, "beast", 1},
        {Slot::Creature, "undead", 2},      {Slot::Creature, "daedra", 3},
        {Slot::Creature, "dragon", 4},      {Slot::Role, "commoner", 0},
        {Slot::Role, "merchant", 1},        {Slot::Role, "guard", 2},
        {Slot::Protection, "mortal", 0},    {Slot::Protection, "protected", 1},
        {Slot::Protection, "essential", 2}, {Slot::Threat, "weak", 0},
        {Slot::Threat, "even", 1},          {Slot::Threat, "strong", 2},
        {Slot::Threat, "deadly", 3},        {Slot::Engagement, "idle", 0},
        {Slot::Engagement, "alert", 1},     {Slot::Engagement, "combat", 2},
    };
    for (const auto& c : cases)
    {
        const auto state = ActorOverrides::ParseState(c.slot, false, c.word);
        ASSERT_TRUE(state.has_value()) << c.word;
        EXPECT_EQ(*state, c.value) << c.word;
        EXPECT_EQ(ActorOverrides::StateName(c.slot, false, c.value), c.word);
    }
}

TEST(ConsoleVocabularyTest, PlayerStatesEncodeBoolSlotsAsZeroAndOne)
{
    struct Case
    {
        Slot slot;
        const char* word;
        std::uint8_t value;
    };
    const Case cases[] = {
        {Slot::Engagement, "idle", 0},
        {Slot::Engagement, "combat", 1},
        {Slot::Sneak, "off", 0},
        {Slot::Sneak, "hidden", 1},
        {Slot::Sneak, "detected", 2},
        {Slot::Encumbered, "normal", 0},
        {Slot::Encumbered, "encumbered", 1},
        {Slot::Bounty, "clear", 0},
        {Slot::Bounty, "wanted", 1},
    };
    for (const auto& c : cases)
    {
        const auto state = ActorOverrides::ParseState(c.slot, true, c.word);
        ASSERT_TRUE(state.has_value()) << c.word;
        EXPECT_EQ(*state, c.value) << c.word;
        EXPECT_EQ(ActorOverrides::StateName(c.slot, true, c.value), c.word);
    }
}

TEST(ConsoleVocabularyTest, PlayerEngagementHasNoAlertState)
{
    EXPECT_FALSE(ActorOverrides::ParseState(Slot::Engagement, true, "alert").has_value());
    EXPECT_TRUE(ActorOverrides::ParseState(Slot::Engagement, false, "alert").has_value());
}

TEST(ConsoleVocabularyTest, RankHasNoStates)
{
    EXPECT_FALSE(ActorOverrides::ParseState(Slot::Rank, false, "low").has_value());
    EXPECT_FALSE(ActorOverrides::ParseState(Slot::Rank, true, "high").has_value());
}

TEST(ConsoleVocabularyTest, SlotAppliesToMatchesThePlateTables)
{
    const Slot npcOnly[] = {
        Slot::Relationship, Slot::Creature, Slot::Role, Slot::Protection, Slot::Threat};
    const Slot playerOnly[] = {Slot::Sneak, Slot::Encumbered, Slot::Bounty};
    const Slot both[] = {Slot::Rank, Slot::Engagement};
    for (const auto s : npcOnly)
    {
        EXPECT_TRUE(ActorOverrides::SlotAppliesTo(s, false));
        EXPECT_FALSE(ActorOverrides::SlotAppliesTo(s, true));
    }
    for (const auto s : playerOnly)
    {
        EXPECT_FALSE(ActorOverrides::SlotAppliesTo(s, false));
        EXPECT_TRUE(ActorOverrides::SlotAppliesTo(s, true));
    }
    for (const auto s : both)
    {
        EXPECT_TRUE(ActorOverrides::SlotAppliesTo(s, false));
        EXPECT_TRUE(ActorOverrides::SlotAppliesTo(s, true));
    }
}

using ActorOverrides::IconCommand;

TEST(ConsoleVocabularyTest, ParseIconCommandWithNoArgumentsShows)
{
    const auto cmd = ActorOverrides::ParseIconCommand({}, false);
    EXPECT_EQ(cmd.kind, IconCommand::Kind::Show);
}

TEST(ConsoleVocabularyTest, ParseIconCommandMatchesVerbsBeforeSlots)
{
    const auto add = ActorOverrides::ParseIconCommand({"add", "anchor"}, false);
    ASSERT_EQ(add.kind, IconCommand::Kind::Add);
    EXPECT_EQ(add.name, "anchor");
    EXPECT_FALSE(add.color.has_value());

    const auto colored =
        ActorOverrides::ParseIconCommand({"add", "anchor", "0.9,", "0.3,", "0.3"}, false);
    ASSERT_EQ(colored.kind, IconCommand::Kind::Add);
    ASSERT_TRUE(colored.color.has_value());
    EXPECT_FLOAT_EQ(colored.color->r, 0.9f);
    EXPECT_FLOAT_EQ(colored.color->b, 0.3f);

    const auto remove = ActorOverrides::ParseIconCommand({"remove", "anchor"}, false);
    ASSERT_EQ(remove.kind, IconCommand::Kind::Remove);
    EXPECT_EQ(remove.name, "anchor");

    EXPECT_EQ(ActorOverrides::ParseIconCommand({"clear"}, false).kind, IconCommand::Kind::Clear);
}

TEST(ConsoleVocabularyTest, ParseIconCommandRejectsABadColour)
{
    const auto cmd = ActorOverrides::ParseIconCommand({"add", "anchor", "red"}, false);
    EXPECT_EQ(cmd.kind, IconCommand::Kind::Error);
    EXPECT_FALSE(cmd.error.empty());
}

TEST(ConsoleVocabularyTest, ParseIconCommandForcesAState)
{
    const auto cmd = ActorOverrides::ParseIconCommand({"relationship", "ally"}, false);
    ASSERT_EQ(cmd.kind, IconCommand::Kind::SetSlot);
    EXPECT_EQ(cmd.slot, Slot::Relationship);
    EXPECT_FALSE(cmd.slotOverride.hidden);
    ASSERT_TRUE(cmd.slotOverride.state.has_value());
    EXPECT_EQ(*cmd.slotOverride.state, 2);
    EXPECT_FALSE(cmd.slotOverride.icon.has_value());
}

TEST(ConsoleVocabularyTest, ParseIconCommandForcesAStateWithAnIcon)
{
    const auto cmd = ActorOverrides::ParseIconCommand({"role", "guard", "anchor"}, false);
    ASSERT_EQ(cmd.kind, IconCommand::Kind::SetSlot);
    ASSERT_TRUE(cmd.slotOverride.state.has_value());
    EXPECT_EQ(*cmd.slotOverride.state, 2);
    ASSERT_TRUE(cmd.slotOverride.icon.has_value());
    EXPECT_EQ(*cmd.slotOverride.icon, "anchor");
}

TEST(ConsoleVocabularyTest, ParseIconCommandAutoWithAnIconKeepsTheLiveState)
{
    const auto cmd = ActorOverrides::ParseIconCommand({"role", "auto", "anchor"}, false);
    ASSERT_EQ(cmd.kind, IconCommand::Kind::SetSlot);
    EXPECT_FALSE(cmd.slotOverride.state.has_value());
    ASSERT_TRUE(cmd.slotOverride.icon.has_value());
    EXPECT_EQ(*cmd.slotOverride.icon, "anchor");
}

TEST(ConsoleVocabularyTest, ParseIconCommandHideAndAuto)
{
    const auto hide = ActorOverrides::ParseIconCommand({"role", "hide"}, false);
    ASSERT_EQ(hide.kind, IconCommand::Kind::SetSlot);
    EXPECT_TRUE(hide.slotOverride.hidden);

    const auto reset = ActorOverrides::ParseIconCommand({"role", "auto"}, false);
    EXPECT_EQ(reset.kind, IconCommand::Kind::ClearSlot);
    EXPECT_EQ(reset.slot, Slot::Role);
}

TEST(ConsoleVocabularyTest, ParseIconCommandRefusesPrefixesAndUnknownStates)
{
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"rel", "ally"}, false).kind,
              IconCommand::Kind::Error);
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"relationship", "friend"}, false).kind,
              IconCommand::Kind::Error);
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"relationship"}, false).kind,
              IconCommand::Kind::Error);
}

TEST(ConsoleVocabularyTest, ParseIconCommandRefusesANameOnRank)
{
    const auto bare = ActorOverrides::ParseIconCommand({"rank", "anchor"}, false);
    EXPECT_EQ(bare.kind, IconCommand::Kind::Error);
    EXPECT_NE(bare.error.find("hide or auto"), std::string::npos) << bare.error;
    const auto withAuto = ActorOverrides::ParseIconCommand({"rank", "auto", "anchor"}, false);
    EXPECT_EQ(withAuto.kind, IconCommand::Kind::Error);
    EXPECT_NE(withAuto.error.find("hide or auto"), std::string::npos) << withAuto.error;
    const auto hide = ActorOverrides::ParseIconCommand({"rank", "hide"}, true);
    ASSERT_EQ(hide.kind, IconCommand::Kind::SetSlot);
    EXPECT_TRUE(hide.slotOverride.hidden);
}

TEST(ConsoleVocabularyTest, ParseIconCommandRefusesASlotFromTheOtherPlate)
{
    const auto npc = ActorOverrides::ParseIconCommand({"sneak", "hidden"}, false);
    EXPECT_EQ(npc.kind, IconCommand::Kind::Error);
    EXPECT_NE(npc.error.find("NPC plate"), std::string::npos) << npc.error;
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"role", "guard"}, true).kind,
              IconCommand::Kind::Error);
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"engagement", "alert"}, true).kind,
              IconCommand::Kind::Error);
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"engagement", "combat"}, true).kind,
              IconCommand::Kind::SetSlot);
}

TEST(ConsoleVocabularyTest, ParseIconCommandRefusesTooManyArguments)
{
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"relationship", "ally", "anchor", "x"}, false).kind,
              IconCommand::Kind::Error);
}

TEST(ConsoleVocabularyTest, ParseIconCommandRefusesAnUnsafeIconName)
{
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"role", "auto", "../x"}, false).kind,
              IconCommand::Kind::Error);
    EXPECT_EQ(ActorOverrides::ParseIconCommand({"add", "a/b"}, false).kind,
              IconCommand::Kind::Error);
}

using ActorOverrides::TitleCommand;

TEST(ConsoleVocabularyTest, ParseTitleCommandDistinguishesShowHideAutoAndText)
{
    EXPECT_EQ(ActorOverrides::ParseTitleCommand("").kind, TitleCommand::Kind::Show);
    EXPECT_EQ(ActorOverrides::ParseTitleCommand("hide").kind, TitleCommand::Kind::Hide);
    EXPECT_EQ(ActorOverrides::ParseTitleCommand("HIDE").kind, TitleCommand::Kind::Hide);
    EXPECT_EQ(ActorOverrides::ParseTitleCommand("auto").kind, TitleCommand::Kind::Auto);

    const auto text = ActorOverrides::ParseTitleCommand("The Grey Fox");
    ASSERT_EQ(text.kind, TitleCommand::Kind::Set);
    EXPECT_EQ(text.text, "The Grey Fox");
}

TEST(ConsoleVocabularyTest, ParseTitleCommandQuotesAndEqualsForceLiteralText)
{
    const auto quoted = ActorOverrides::ParseTitleCommand("\"hide\"");
    ASSERT_EQ(quoted.kind, TitleCommand::Kind::Set);
    EXPECT_EQ(quoted.text, "hide");

    const auto stripped = ActorOverrides::ParseTitleCommand("\"The Grey Fox\"");
    ASSERT_EQ(stripped.kind, TitleCommand::Kind::Set);
    EXPECT_EQ(stripped.text, "The Grey Fox");

    const auto literal = ActorOverrides::ParseTitleCommand("= hide");
    ASSERT_EQ(literal.kind, TitleCommand::Kind::Set);
    EXPECT_EQ(literal.text, "hide");

    const auto literalQuotes = ActorOverrides::ParseTitleCommand("= \"quoted\"");
    ASSERT_EQ(literalQuotes.kind, TitleCommand::Kind::Set);
    EXPECT_EQ(literalQuotes.text, "\"quoted\"");

    EXPECT_EQ(ActorOverrides::ParseTitleCommand("=").kind, TitleCommand::Kind::Error);
    EXPECT_EQ(ActorOverrides::ParseTitleCommand("= ").kind, TitleCommand::Kind::Error);
}

TEST(ConsoleVocabularyTest, ParseTitleCommandEmptyQuotesHideTheTitle)
{
    EXPECT_EQ(ActorOverrides::ParseTitleCommand("\"\"").kind, TitleCommand::Kind::Hide);
}

TEST(ConsoleVocabularyTest, ValidateTitleCapsAtSixtyFourCharacters)
{
    const std::string ascii64(RenderConstants::MAX_OVERRIDE_TITLE_CHARS, 'a');
    EXPECT_FALSE(ActorOverrides::ValidateTitle(ascii64).has_value());
    EXPECT_TRUE(ActorOverrides::ValidateTitle(ascii64 + "a").has_value());

    std::string accented;
    for (int i = 0; i < RenderConstants::MAX_OVERRIDE_TITLE_CHARS; ++i)
    {
        accented += "\xC3\xA9";  // e acute, two bytes, one character
    }
    EXPECT_FALSE(ActorOverrides::ValidateTitle(accented).has_value());
    EXPECT_TRUE(ActorOverrides::ValidateTitle(accented + "\xC3\xA9").has_value());
}

class ConsoleOverrideStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ActorOverrides::Clear();
        std::vector<std::uint32_t> drained;
        ActorOverrides::DrainDirty(drained);
    }
};

TEST_F(ConsoleOverrideStoreTest, GetReturnsNullWhenNothingIsStored)
{
    EXPECT_EQ(ActorOverrides::Get(0x14), nullptr);
    EXPECT_EQ(ActorOverrides::Count(), 0u);
}

TEST_F(ConsoleOverrideStoreTest, ModifyPublishesANewRecordAndKeepsTheOldOneReadable)
{
    ActorOverrides::Modify(0x14, [](Record& r) { r.title = "A"; });
    const auto first = ActorOverrides::Get(0x14);
    ASSERT_NE(first, nullptr);

    ActorOverrides::Modify(0x14, [](Record& r) { r.title = "B"; });
    const auto second = ActorOverrides::Get(0x14);
    ASSERT_NE(second, nullptr);

    EXPECT_NE(first, second);
    ASSERT_TRUE(first->title.has_value());
    EXPECT_EQ(*first->title, "A");
    ASSERT_TRUE(second->title.has_value());
    EXPECT_EQ(*second->title, "B");
    EXPECT_EQ(ActorOverrides::Count(), 1u);
}

TEST_F(ConsoleOverrideStoreTest, ModifyReportsAppliedOrUnchangedAndMarksDirtyOnlyWhenApplied)
{
    using ActorOverrides::ModifyResult;
    EXPECT_EQ(ActorOverrides::Modify(0x14, [](Record& r) { r.title = "A"; }),
              ModifyResult::Applied);
    std::vector<std::uint32_t> dirty;
    ActorOverrides::DrainDirty(dirty);
    ASSERT_EQ(dirty.size(), 1u);

    EXPECT_EQ(ActorOverrides::Modify(0x14, [](Record& r) { r.title = "A"; }),
              ModifyResult::Unchanged);
    EXPECT_EQ(ActorOverrides::Modify(0x15, [](Record&) {}), ModifyResult::Unchanged);
    ActorOverrides::DrainDirty(dirty);
    EXPECT_TRUE(dirty.empty());
    EXPECT_EQ(ActorOverrides::Count(), 1u);
}

TEST_F(ConsoleOverrideStoreTest, ModifyRefusesAFifthExtraBadge)
{
    using ActorOverrides::ModifyResult;
    for (int i = 0; i < RenderConstants::MAX_EXTRA_BADGES; ++i)
    {
        EXPECT_EQ(
            ActorOverrides::Modify(
                0x14, [i](Record& r) { r.extras.push_back({"extra" + std::to_string(i), {}}); }),
            ModifyResult::Applied);
    }
    std::vector<std::uint32_t> dirty;
    ActorOverrides::DrainDirty(dirty);
    EXPECT_EQ(ActorOverrides::Modify(0x14, [](Record& r) { r.extras.push_back({"one-more", {}}); }),
              ModifyResult::TooManyExtras);
    const auto record = ActorOverrides::Get(0x14);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->extras.size(), static_cast<std::size_t>(RenderConstants::MAX_EXTRA_BADGES));
    ActorOverrides::DrainDirty(dirty);
    EXPECT_TRUE(dirty.empty());
}

TEST_F(ConsoleOverrideStoreTest, ModifyRefusesADistinctIconNamePastTheSessionCap)
{
    using ActorOverrides::ModifyResult;
    const int cap = RenderConstants::MAX_OVERRIDE_ICON_NAMES;
    for (int i = 0; i < cap; ++i)
    {
        EXPECT_EQ(ActorOverrides::Modify(0x1000 + static_cast<std::uint32_t>(i),
                                         [i](Record& r)
                                         { r.extras.push_back({"icon" + std::to_string(i), {}}); }),
                  ModifyResult::Applied);
    }
    EXPECT_EQ(ActorOverrides::IconNameCount(), static_cast<std::size_t>(cap));

    EXPECT_EQ(ActorOverrides::Modify(0x14, [](Record& r) { r.extras.push_back({"icon-new", {}}); }),
              ModifyResult::TooManyIconNames);
    EXPECT_EQ(ActorOverrides::Get(0x14), nullptr);

    EXPECT_EQ(ActorOverrides::Modify(0x14, [](Record& r) { r.extras.push_back({"icon0", {}}); }),
              ModifyResult::Applied);

    EXPECT_EQ(ActorOverrides::Modify(0x1001, [](Record& r) { r.extras[0].icon = "icon-swap"; }),
              ModifyResult::Applied);
    EXPECT_EQ(ActorOverrides::IconNameCount(), static_cast<std::size_t>(cap));
}

TEST_F(ConsoleOverrideStoreTest, ModifyErasesTheRecordWhenItBecomesEmpty)
{
    ActorOverrides::Modify(0x14, [](Record& r) { r.title = "A"; });
    ActorOverrides::Modify(0x14, [](Record& r) { r.title.reset(); });
    EXPECT_EQ(ActorOverrides::Get(0x14), nullptr);
    EXPECT_EQ(ActorOverrides::Count(), 0u);
}

TEST_F(ConsoleOverrideStoreTest, EmptyRecordReportsEmptyOnlyWithNoOverrides)
{
    Record r;
    EXPECT_TRUE(r.Empty());
    r.title = "";
    EXPECT_FALSE(r.Empty());
    r.title.reset();
    r.slots[static_cast<std::size_t>(Slot::Role)].hidden = true;
    EXPECT_FALSE(r.Empty());
    r.slots[static_cast<std::size_t>(Slot::Role)].hidden = false;
    r.extras.push_back({"anchor", {}});
    EXPECT_FALSE(r.Empty());
}

TEST_F(ConsoleOverrideStoreTest, IconSetVersionBumpsOnlyWhenANewNameEnters)
{
    const auto v0 = ActorOverrides::IconSetVersion();
    ActorOverrides::Modify(0x14, [](Record& r) { r.extras.push_back({"anchor", {}}); });
    const auto v1 = ActorOverrides::IconSetVersion();
    EXPECT_GT(v1, v0);
    EXPECT_EQ(ActorOverrides::IconNameCount(), 1u);
    EXPECT_TRUE(ActorOverrides::HasIconName("anchor"));

    ActorOverrides::Modify(0x15, [](Record& r) { r.extras.push_back({"anchor", {}}); });
    EXPECT_EQ(ActorOverrides::IconSetVersion(), v1);

    ActorOverrides::Modify(0x14, [](Record& r) { r.extras.clear(); });
    EXPECT_EQ(ActorOverrides::IconSetVersion(), v1);
    EXPECT_EQ(ActorOverrides::IconNameCount(), 1u);

    ActorOverrides::Modify(0x15, [](Record& r) { r.extras.clear(); });
    EXPECT_EQ(ActorOverrides::IconSetVersion(), v1);
    EXPECT_EQ(ActorOverrides::IconNameCount(), 0u);
    EXPECT_FALSE(ActorOverrides::HasIconName("anchor"));
}

TEST_F(ConsoleOverrideStoreTest, IconNamesCollectSlotIconsAndExtras)
{
    ActorOverrides::Modify(0x14,
                           [](Record& r)
                           {
                               r.slots[static_cast<std::size_t>(Slot::Role)].icon = "anchor";
                               r.extras.push_back({"star", {}});
                               r.extras.push_back({"anchor", {}});
                           });
    const auto names = ActorOverrides::IconNames();
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "anchor");
    EXPECT_EQ(names[1], "star");
}

TEST_F(ConsoleOverrideStoreTest, ModifyAndClearMarkTheActorDirty)
{
    ActorOverrides::Modify(0x14, [](Record& r) { r.title = "A"; });
    ActorOverrides::Modify(0x14, [](Record& r) { r.title = "B"; });
    std::vector<std::uint32_t> dirty;
    ActorOverrides::DrainDirty(dirty);
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0], 0x14u);

    ActorOverrides::DrainDirty(dirty);
    EXPECT_TRUE(dirty.empty());

    ActorOverrides::Modify(0x15, [](Record& r) { r.title = "C"; });
    ActorOverrides::DrainDirty(dirty);
    ActorOverrides::Clear();
    ActorOverrides::DrainDirty(dirty);
    ASSERT_EQ(dirty.size(), 2u);
    EXPECT_EQ(ActorOverrides::Count(), 0u);
}

TEST_F(ConsoleOverrideStoreTest, EraseMarksDirtyAndDropsTheRecord)
{
    ActorOverrides::Modify(0x14, [](Record& r) { r.title = "A"; });
    std::vector<std::uint32_t> dirty;
    ActorOverrides::DrainDirty(dirty);
    ActorOverrides::Erase(0x14);
    EXPECT_EQ(ActorOverrides::Get(0x14), nullptr);
    ActorOverrides::DrainDirty(dirty);
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0], 0x14u);
}

TEST_F(ConsoleOverrideStoreTest, EraseDynamicKeepsThePlayerAndDropsRuntimeReferences)
{
    ActorOverrides::Modify(0x14, [](Record& r) { r.title = "A"; });
    ActorOverrides::Modify(0xFF000001, [](Record& r) { r.title = "B"; });
    std::vector<std::uint32_t> dirty;
    ActorOverrides::DrainDirty(dirty);

    ActorOverrides::EraseDynamic();
    EXPECT_EQ(ActorOverrides::Count(), 1u);
    EXPECT_NE(ActorOverrides::Get(0x14), nullptr);
    EXPECT_EQ(ActorOverrides::Get(0xFF000001), nullptr);
    ActorOverrides::DrainDirty(dirty);
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0], 0xFF000001u);
}

TEST(ConsoleDescribeTest, EmptyRecordSaysSo)
{
    EXPECT_EQ(ActorOverrides::Describe(Record{}, false), "no overrides");
}

TEST(ConsoleDescribeTest, MixedRecordListsEveryOverrideInStripOrder)
{
    Record r;
    r.title = "Thane";
    r.slots[static_cast<std::size_t>(Slot::Threat)].hidden = true;
    r.slots[static_cast<std::size_t>(Slot::Relationship)].state = 2;
    r.slots[static_cast<std::size_t>(Slot::Role)].icon = "anchor";
    r.slots[static_cast<std::size_t>(Slot::Protection)].state = 2;
    r.slots[static_cast<std::size_t>(Slot::Protection)].icon = "star";
    r.extras.push_back({"anchor", {}});
    r.extras.push_back({"star", {}});
    EXPECT_EQ(ActorOverrides::Describe(r, false),
              "title \"Thane\"; relationship=ally; role=auto+anchor; "
              "protection=essential+star; threat=hidden; extras anchor, star");
}

TEST(ConsoleDescribeTest, TitleAndIconHalvesDescribeSeparately)
{
    Record r;
    r.title = "Thane";
    r.slots[static_cast<std::size_t>(Slot::Role)].hidden = true;
    EXPECT_EQ(ActorOverrides::DescribeTitle(r), "title \"Thane\"");
    EXPECT_EQ(ActorOverrides::DescribeIcons(r, false), "role=hidden");
    EXPECT_EQ(ActorOverrides::DescribeTitle(Record{}), "no title override");
    EXPECT_EQ(ActorOverrides::DescribeIcons(Record{}, false), "no icon overrides");
}

TEST(ConsoleDescribeTest, HiddenTitleAndPlayerStatesRead)
{
    Record r;
    r.title = "";
    r.slots[static_cast<std::size_t>(Slot::Engagement)].state = 1;
    EXPECT_EQ(ActorOverrides::Describe(r, true), "title hidden; engagement=combat");
}
}  // namespace
