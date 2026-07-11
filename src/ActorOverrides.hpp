#pragma once

#include "RenderConstants.hpp"
#include "Settings.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @namespace ActorOverrides
 * @brief Session-only title and badge overrides keyed by reference FormID.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Core
 *
 * The game thread attaches records to `ActorDrawData::overrides`. Immutable shared
 * records remain valid across edits; their strings may be destroyed on the render thread.
 * Records persist until erased, cleared, or process exit; no disk writes occur.
 *
 * Writers are game-thread only. The render thread reads icon names, their version,
 * and the deduplicated dirty list. The store mutex is a leaf lock: never acquire
 * `Settings::Mutex()`, `snapshotLock`, or the `BadgeTextures` mutex while holding it.
 * `Modify` enforces `MAX_EXTRA_BADGES` per actor and `MAX_OVERRIDE_ICON_NAMES` per session.
 *
 * Forced states use enum ordinals, or 0/1 for player bools. The renderer clamps excessive
 * ordinals to the final enumerator.
 *
 * | Slot         | States, muted default first                              | Plate  |
 * |--------------|----------------------------------------------------------|--------|
 * | rank         | hide, auto only                                          | both   |
 * | relationship | neutral=1, hostile=0, ally=2, follower=3                 | NPC    |
 * | creature     | humanoid=0, beast=1, undead=2, daedra=3, dragon=4        | NPC    |
 * | role         | commoner=0, merchant=1, guard=2                          | NPC    |
 * | protection   | mortal=0, protected=1, essential=2                       | NPC    |
 * | threat       | even=1, weak=0, strong=2, deadly=3                       | NPC    |
 * | engagement   | NPC: idle=0, alert=1, combat=2; player: idle=0, combat=1 | both   |
 * | sneak        | off=0, hidden=1, detected=2                              | player |
 * | encumbered   | normal=0, encumbered=1; alias: weight                    | player |
 * | bounty       | clear=0, wanted=1                                        | player |
 */
namespace ActorOverrides
{
/**
 * @enum Slot
 * @brief Status-badge slots a console command can address.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The order is the NPC strip order and the storage order in `Record::slots`; the player
 * strip emits `Sneak` before `Engagement`. `Engagement` covers the NPC `EngagementKind` and
 * the player's in-combat bool.
 */
enum class Slot : std::uint8_t
{
    Rank,          ///< Rank identity mark; hide or auto only
    Relationship,  ///< NPC relationship to the player
    Creature,      ///< NPC creature kind
    Role,          ///< NPC social role
    Protection,    ///< NPC essential or protected flag
    Threat,        ///< NPC level delta
    Engagement,    ///< NPC awareness, or the player's combat state
    Sneak,         ///< Player stealth state
    Encumbered,    ///< Player carry weight
    Bounty,        ///< Player bounty
    Count          ///< Number of slots; not a slot
};

inline constexpr std::size_t SLOT_COUNT = static_cast<std::size_t>(Slot::Count);

/**
 * @struct SlotOverride
 * @brief Console override for one badge slot.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A default-constructed value means no override. `hidden` wins over `state` and `icon`.
 */
struct SlotOverride
{
    bool hidden = false;                ///< Skip the slot on this actor
    std::optional<std::uint8_t> state;  ///< Forced state per the vocabulary table; empty = live
    std::optional<std::string> icon;    ///< Duotone icon name; empty = the state's own icon

    /**
     * @fn bool Empty() const
     * @brief True when every field is at its default.
     * @author Alex (<https://github.com/lextpf>)
     */
    [[nodiscard]] bool Empty() const { return !hidden && !state && !icon; }
    bool operator==(const SlotOverride&) const = default;
};

/**
 * @struct ExtraBadge
 * @brief One free-form badge appended after the actor's status slots.
 * @author Alex (<https://github.com/lextpf>)
 */
struct ExtraBadge
{
    std::string icon;                             ///< Duotone icon name
    Settings::Color3 color{0.80f, 0.74f, 0.62f};  ///< Tint; parchment unless the command set one
    bool operator==(const ExtraBadge&) const = default;
};

/**
 * @struct Record
 * @brief Every console override for one actor.
 * @author Alex (<https://github.com/lextpf>)
 *
 * `title` has three states: empty optional is no override, an empty string hides the
 * title text, any other text is the custom title.
 */
struct Record
{
    std::optional<std::string> title;              ///< Custom title; see the struct description
    std::array<SlotOverride, SLOT_COUNT> slots{};  ///< Per-slot overrides in `Slot` order
    std::vector<ExtraBadge> extras;                ///< At most `MAX_EXTRA_BADGES` entries

    /**
     * @fn bool Empty() const
     * @brief Test whether the actor has no title, slot, or extra-badge override.
     * @author Alex (<https://github.com/lextpf>)
     */
    [[nodiscard]] bool Empty() const
    {
        return !title && extras.empty() && std::ranges::all_of(slots, &SlotOverride::Empty);
    }
    bool operator==(const Record&) const = default;
};

/**
 * @fn std::optional<Slot> ParseSlot(std::string_view word)
 * @brief Match a slot word exactly, case-insensitively; weight is an alias for encumbered.
 * @author Alex (<https://github.com/lextpf>)
 */
[[nodiscard]] std::optional<Slot> ParseSlot(std::string_view word);

/**
 * @fn std::string_view SlotName(Slot slot)
 * @brief The slot's command word.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Static command text, or an empty view for an invalid slot.
 */
[[nodiscard]] std::string_view SlotName(Slot slot);

/**
 * @fn std::optional<std::uint8_t> ParseState(Slot slot, bool isPlayer, std::string_view word)
 * @brief Match an exact, case-insensitive state for the selected plate.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Encoded state, or empty for an unknown slot/state combination.
 */
[[nodiscard]] std::optional<std::uint8_t> ParseState(Slot slot,
                                                     bool isPlayer,
                                                     std::string_view word);

/**
 * @fn std::string_view StateName(Slot slot, bool isPlayer, std::uint8_t state)
 * @brief Command spelling for an encoded state.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Empty if the state is out of range.
 */
[[nodiscard]] std::string_view StateName(Slot slot, bool isPlayer, std::uint8_t state);

/**
 * @fn bool SlotAppliesTo(Slot slot, bool isPlayer)
 * @brief True when the plate draws this slot at all.
 * @author Alex (<https://github.com/lextpf>)
 */
[[nodiscard]] bool SlotAppliesTo(Slot slot, bool isPlayer);

/**
 * @struct TitleCommand
 * @brief Parsed title command and its optional literal text.
 * @author Alex (<https://github.com/lextpf>)
 */
struct TitleCommand
{
    /**
     * @enum Kind
     * @brief What the command asks for.
     * @author Alex (<https://github.com/lextpf>)
     */
    enum class Kind : std::uint8_t
    {
        Show,  ///< No argument: echo the current override
        Set,   ///< Custom title in `text`
        Hide,  ///< Hide the title text
        Auto,  ///< Remove the override
        Error  ///< Refused; `error` says why
    };
    Kind kind = Kind::Show;
    std::string text;   ///< The title for `Set`
    std::string error;  ///< Message for `Error`
};

/**
 * @fn TitleCommand ParseTitleCommand(std::string_view rest)
 * @brief Parse a title command after its sub-command word.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Bare `hide` and `auto` are reserved, case-insensitively. Leading `=` takes the remainder
 * as literal text after whitespace; it preserves quotes. Otherwise, strip one outer
 * quote pair; an empty result hides the title. The caller must validate the title length.
 *
 * @param rest Text after the command word, with surrounding whitespace already trimmed.
 */
[[nodiscard]] TitleCommand ParseTitleCommand(std::string_view rest);

/**
 * @struct IconCommand
 * @brief Parsed badge command and its optional slot or extra-badge data.
 * @author Alex (<https://github.com/lextpf>)
 */
struct IconCommand
{
    /**
     * @enum Kind
     * @brief What the command asks for.
     * @author Alex (<https://github.com/lextpf>)
     */
    enum class Kind : std::uint8_t
    {
        Show,       ///< No argument: echo the current overrides
        SetSlot,    ///< Replace `slot`'s override with `slotOverride`
        ClearSlot,  ///< Remove `slot`'s override
        Add,        ///< Append the extra badge `name` with `color`
        Remove,     ///< Remove the extra badge `name`
        Clear,      ///< Remove every icon override on the actor
        Error       ///< Refused; `error` says why
    };
    Kind kind = Kind::Show;
    Slot slot = Slot::Rank;                 ///< Target slot for `SetSlot` and `ClearSlot`
    SlotOverride slotOverride;              ///< New override for `SetSlot`
    std::string name;                       ///< Icon name for `Add` and `Remove`
    std::optional<Settings::Color3> color;  ///< Tint for `Add`; empty keeps the default
    std::string error;                      ///< Message for `Error`
};

/**
 * @fn IconCommand ParseIconCommand(const std::vector<std::string>& args, bool isPlayer)
 * @brief Parse icon verbs before exact slot and state words.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Icon names must pass `ConsoleParse::IsSafeIconName`. Join all tokens after an added
 * icon name to parse optional RGB color. File availability and store limits are checked
 * when applying the command, after this parser succeeds.
 * @param args Tokens after the sub-command word.
 */
[[nodiscard]] IconCommand ParseIconCommand(const std::vector<std::string>& args, bool isPlayer);

/**
 * @fn std::optional<std::string> ValidateTitle(const std::string& title)
 * @brief Validate the UTF-8 title length.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Count with `Utf8Utils::Utf8CharCount` up to the first null byte and compare against
 * `MAX_OVERRIDE_TITLE_CHARS`. This enforces length only; malformed UTF-8 is not refused.
 *
 * @return Refusal message, or empty on success.
 */
[[nodiscard]] std::optional<std::string> ValidateTitle(const std::string& title);

/**
 * @fn std::string DescribeTitle(const Record& record)
 * @brief Console title summary.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return `title "text"`, `title hidden`, or `no title override`.
 */
[[nodiscard]] std::string DescribeTitle(const Record& record);

/**
 * @fn std::string DescribeIcons(const Record& record, bool isPlayer)
 * @brief Console icon summary in slot order, then extras.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Semicolon-separated overrides, or `no icon overrides`.
 */
[[nodiscard]] std::string DescribeIcons(const Record& record, bool isPlayer);

/**
 * @fn std::string Describe(const Record& record, bool isPlayer)
 * @brief Console summary with title before icons.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Semicolon-separated nonempty halves, or `no overrides`.
 */
[[nodiscard]] std::string Describe(const Record& record, bool isPlayer);

/**
 * @fn std::shared_ptr<const Record> Get(std::uint32_t formID)
 * @brief Immutable record lookup; lock-free when the store is empty.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Null when the actor has no record.
 */
[[nodiscard]] std::shared_ptr<const Record> Get(std::uint32_t formID);

/**
 * @enum ModifyResult
 * @brief Outcome of an edit through Modify.
 * @author Alex (<https://github.com/lextpf>)
 */
enum class ModifyResult : std::uint8_t
{
    Applied,           ///< The edit was published and the actor marked dirty
    Unchanged,         ///< The edit left the record as it was; nothing was published
    TooManyExtras,     ///< Refused: the copy holds more than `MAX_EXTRA_BADGES` extras
    TooManyIconNames,  ///< Refused: the session would exceed `MAX_OVERRIDE_ICON_NAMES` names
};

/**
 * @fn ModifyResult Modify(std::uint32_t formID, const std::function<void(Record&)>& fn)
 * @brief Publish an edited copy and mark the actor dirty.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Copy under the store lock, call `fn` without it, then compare and publish under lock.
 * Unchanged copies publish nothing; empty copies erase the entry. Caps apply to the
 * resulting set, so replacing an icon name at the cap is allowed.
 * Refused edits leave the record and dirty list unchanged. Callers validate title length,
 * slot applicability, and icon names before this call; the store enforces only its caps.
 *
 * @param fn May read the store; must not modify or erase this actor, since the outer
 * publish would overwrite that edit.
 * @return Edit outcome, including cap refusals.
 */
ModifyResult Modify(std::uint32_t formID, const std::function<void(Record&)>& fn);

/**
 * @fn void Erase(std::uint32_t formID)
 * @brief Remove an actor's record and mark the actor dirty.
 * @author Alex (<https://github.com/lextpf>)
 */
void Erase(std::uint32_t formID);

/**
 * @fn void Clear()
 * @brief Remove every record and mark every affected actor dirty.
 * @author Alex (<https://github.com/lextpf>)
 */
void Clear();

/**
 * @fn void EraseDynamic()
 * @brief Remove the records of runtime-created references (FormID at or above 0xFF000000).
 * @author Alex (<https://github.com/lextpf>)
 */
void EraseDynamic();

/**
 * @fn std::size_t Count()
 * @brief Number of actors with a record.
 * @author Alex (<https://github.com/lextpf>)
 */
[[nodiscard]] std::size_t Count();

/**
 * @fn std::vector<std::string> IconNames()
 * @brief Every icon name any record references, sorted, without duplicates.
 * @author Alex (<https://github.com/lextpf>)
 */
[[nodiscard]] std::vector<std::string> IconNames();

/**
 * @fn std::size_t IconNameCount()
 * @brief Number of distinct icon names the records reference.
 * @author Alex (<https://github.com/lextpf>)
 */
[[nodiscard]] std::size_t IconNameCount();

/**
 * @fn bool HasIconName(std::string_view name)
 * @brief True when some record already references this icon name.
 * @author Alex (<https://github.com/lextpf>)
 */
[[nodiscard]] bool HasIconName(std::string_view name);

/**
 * @fn std::uint32_t IconSetVersion()
 * @brief Version for incremental badge texture loading.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Advances only when a name enters the set; removals and repeated edits leave it unchanged.
 */
[[nodiscard]] std::uint32_t IconSetVersion();

/**
 * @fn void DrainDirty(std::vector<std::uint32_t>& out)
 * @brief Drain edited actors for render-thread reveal resets.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param out Replaces its previous contents with deduplicated FormIDs since the last drain.
 * @pre Call once per drawn frame on the render thread.
 */
void DrainDirty(std::vector<std::uint32_t>& out);
}  // namespace ActorOverrides
