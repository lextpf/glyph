#include "ActorOverrides.hpp"

#include "ConsoleParse.hpp"
#include "Utf8Utils.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>

namespace ActorOverrides
{
namespace
{

constexpr std::array<std::string_view, SLOT_COUNT> SLOT_NAMES = {"rank",
                                                                 "relationship",
                                                                 "creature",
                                                                 "role",
                                                                 "protection",
                                                                 "threat",
                                                                 "engagement",
                                                                 "sneak",
                                                                 "encumbered",
                                                                 "bounty"};

constexpr std::array<std::string_view, 4> RELATIONSHIP_STATES = {
    "hostile", "neutral", "ally", "follower"};
constexpr std::array<std::string_view, 5> CREATURE_STATES = {
    "humanoid", "beast", "undead", "daedra", "dragon"};
constexpr std::array<std::string_view, 3> ROLE_STATES = {"commoner", "merchant", "guard"};
constexpr std::array<std::string_view, 3> PROTECTION_STATES = {"mortal", "protected", "essential"};
constexpr std::array<std::string_view, 4> THREAT_STATES = {"weak", "even", "strong", "deadly"};
constexpr std::array<std::string_view, 3> NPC_ENGAGEMENT_STATES = {"idle", "alert", "combat"};
constexpr std::array<std::string_view, 2> PLAYER_ENGAGEMENT_STATES = {"idle", "combat"};
constexpr std::array<std::string_view, 3> SNEAK_STATES = {"off", "hidden", "detected"};
constexpr std::array<std::string_view, 2> ENCUMBERED_STATES = {"normal", "encumbered"};
constexpr std::array<std::string_view, 2> BOUNTY_STATES = {"clear", "wanted"};

/**
 * @fn std::span<const std::string_view> StateWords(Slot slot, bool isPlayer)
 * @brief Select the state vocabulary for the actor plate and slot.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Rank and slots absent from the plate have no state vocabulary.
 */
std::span<const std::string_view> StateWords(Slot slot, bool isPlayer)
{
    if (!SlotAppliesTo(slot, isPlayer))
    {
        return {};
    }
    switch (slot)
    {
        case Slot::Relationship:
            return RELATIONSHIP_STATES;
        case Slot::Creature:
            return CREATURE_STATES;
        case Slot::Role:
            return ROLE_STATES;
        case Slot::Protection:
            return PROTECTION_STATES;
        case Slot::Threat:
            return THREAT_STATES;
        case Slot::Engagement:
            if (isPlayer)
            {
                return PLAYER_ENGAGEMENT_STATES;
            }
            return NPC_ENGAGEMENT_STATES;
        case Slot::Sneak:
            return SNEAK_STATES;
        case Slot::Encumbered:
            return ENCUMBERED_STATES;
        case Slot::Bounty:
            return BOUNTY_STATES;
        case Slot::Rank:
        case Slot::Count:
            break;
    }
    return {};
}

/**
 * @fn std::string Plate(bool isPlayer)
 * @brief Name the plate type in console refusal messages.
 * @author Alex (<https://github.com/lextpf>)
 */
std::string Plate(bool isPlayer)
{
    return isPlayer ? "the player plate" : "an NPC plate";
}

/**
 * @struct Store
 * @brief Process-lifetime records and render-thread notifications.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The mutex protects containers. Atomic counters publish empty-store and icon-addition
 * state without exposing container storage to readers.
 */
struct Store
{
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::shared_ptr<const Record>> records;
    std::set<std::string> iconNames;  // Sorted, so IconNames() is deterministic
    std::vector<std::uint32_t> dirty;
    std::atomic<std::uint32_t> iconVersion{0};
    std::atomic<std::size_t> live{0};
};

/**
 * @fn Store& GetStore()
 * @brief Access the process-lifetime override store.
 * @author Alex (<https://github.com/lextpf>)
 */
Store& GetStore()
{
    static Store store;
    return store;
}

/**
 * @fn void CollectIconNames(const Record& record, std::set<std::string>& names)
 * @brief Add all nonempty icon names referenced by one record.
 * @author Alex (<https://github.com/lextpf>)
 */
void CollectIconNames(const Record& record, std::set<std::string>& names)
{
    for (const auto& slot : record.slots)
    {
        if (slot.icon && !slot.icon->empty())
        {
            names.insert(*slot.icon);
        }
    }
    for (const auto& extra : record.extras)
    {
        if (!extra.icon.empty())
        {
            names.insert(extra.icon);
        }
    }
}

/**
 * @fn std::set<std::string> IconNamesWithLocked(const Store& store, std::uint32_t formID, const
 *     Record& candidate)
 * @brief Build the icon-name set after a proposed actor replacement.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @pre Hold the store mutex.
 */
std::set<std::string> IconNamesWithLocked(const Store& store,
                                          std::uint32_t formID,
                                          const Record& candidate)
{
    std::set<std::string> names;
    for (const auto& [id, record] : store.records)
    {
        if (id != formID)
        {
            CollectIconNames(*record, names);
        }
    }
    CollectIconNames(candidate, names);
    return names;
}

/**
 * @fn void RebuildIconNamesLocked(Store& store)
 * @brief Refresh referenced icon names and publish additions.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Only newly added names advance the version.
 *
 * @pre Hold the store mutex.
 */
void RebuildIconNamesLocked(Store& store)
{
    std::set<std::string> names;
    for (const auto& [formID, record] : store.records)
    {
        CollectIconNames(*record, names);
    }
    const bool grew = std::ranges::any_of(
        names, [&](const std::string& name) { return !store.iconNames.contains(name); });
    store.iconNames = std::move(names);
    if (grew)
    {
        store.iconVersion.fetch_add(1, std::memory_order_release);
    }
}

/**
 * @fn void MarkDirtyLocked(Store& store, std::uint32_t formID)
 * @brief Queue one reveal reset per actor until the next drain.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @pre Hold the store mutex.
 */
void MarkDirtyLocked(Store& store, std::uint32_t formID)
{
    if (std::ranges::find(store.dirty, formID) == store.dirty.end())
    {
        store.dirty.push_back(formID);
    }
}
}  // namespace

std::optional<Slot> ParseSlot(std::string_view word)
{
    const std::string lower = ConsoleParse::ToLowerAscii(std::string(word));
    if (lower == "weight")
    {
        return Slot::Encumbered;
    }
    for (std::size_t i = 0; i < SLOT_COUNT; ++i)
    {
        if (lower == SLOT_NAMES[i])
        {
            return static_cast<Slot>(i);
        }
    }
    return std::nullopt;
}

std::string_view SlotName(Slot slot)
{
    const auto index = static_cast<std::size_t>(slot);
    return index < SLOT_COUNT ? SLOT_NAMES[index] : std::string_view{};
}

std::optional<std::uint8_t> ParseState(Slot slot, bool isPlayer, std::string_view word)
{
    const std::string lower = ConsoleParse::ToLowerAscii(std::string(word));
    const auto words = StateWords(slot, isPlayer);
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (lower == words[i])
        {
            return static_cast<std::uint8_t>(i);
        }
    }
    return std::nullopt;
}

std::string_view StateName(Slot slot, bool isPlayer, std::uint8_t state)
{
    const auto words = StateWords(slot, isPlayer);
    return state < words.size() ? words[state] : std::string_view{};
}

bool SlotAppliesTo(Slot slot, bool isPlayer)
{
    switch (slot)
    {
        case Slot::Rank:
        case Slot::Engagement:
            return true;
        case Slot::Relationship:
        case Slot::Creature:
        case Slot::Role:
        case Slot::Protection:
        case Slot::Threat:
            return !isPlayer;
        case Slot::Sneak:
        case Slot::Encumbered:
        case Slot::Bounty:
            return isPlayer;
        case Slot::Count:
            break;
    }
    return false;
}

TitleCommand ParseTitleCommand(std::string_view rest)
{
    TitleCommand cmd;
    if (rest.empty())
    {
        cmd.kind = TitleCommand::Kind::Show;
        return cmd;
    }
    if (rest.front() == '=')
    {
        std::string_view literal = rest.substr(1);
        while (!literal.empty() && std::isspace(static_cast<unsigned char>(literal.front())) != 0)
        {
            literal.remove_prefix(1);
        }
        if (literal.empty())
        {
            cmd.kind = TitleCommand::Kind::Error;
            cmd.error = "nothing after '='";
            return cmd;
        }
        cmd.kind = TitleCommand::Kind::Set;
        cmd.text = std::string(literal);
        return cmd;
    }
    const std::string lower = ConsoleParse::ToLowerAscii(std::string(rest));
    if (lower == "hide")
    {
        cmd.kind = TitleCommand::Kind::Hide;
        return cmd;
    }
    if (lower == "auto")
    {
        cmd.kind = TitleCommand::Kind::Auto;
        return cmd;
    }
    cmd.text = ConsoleParse::StripSurroundingQuotes(std::string(rest));
    cmd.kind = cmd.text.empty() ? TitleCommand::Kind::Hide : TitleCommand::Kind::Set;
    return cmd;
}

IconCommand ParseIconCommand(const std::vector<std::string>& args, bool isPlayer)
{
    IconCommand cmd;
    const auto fail = [&](std::string message)
    {
        cmd.kind = IconCommand::Kind::Error;
        cmd.error = std::move(message);
        return cmd;
    };

    if (args.empty())
    {
        cmd.kind = IconCommand::Kind::Show;
        return cmd;
    }

    const std::string verb = ConsoleParse::ToLowerAscii(args[0]);
    if (verb == "add" || verb == "remove")
    {
        if (args.size() < 2)
        {
            return fail(verb + " needs an icon name");
        }
        if (!ConsoleParse::IsSafeIconName(args[1]))
        {
            return fail("icon names use letters, digits, '-' and '_' only");
        }
        cmd.name = args[1];
        if (verb == "remove")
        {
            cmd.kind = IconCommand::Kind::Remove;
            return cmd;
        }
        if (args.size() > 2)
        {
            std::string joined;
            for (std::size_t i = 2; i < args.size(); ++i)
            {
                joined += args[i];
            }
            cmd.color = ConsoleParse::ParseColorTriplet(joined);
            if (!cmd.color)
            {
                return fail("colour must be r,g,b with each channel 0 to 1");
            }
        }
        cmd.kind = IconCommand::Kind::Add;
        return cmd;
    }
    if (verb == "clear")
    {
        cmd.kind = IconCommand::Kind::Clear;
        return cmd;
    }

    const auto slot = ParseSlot(args[0]);
    if (!slot)
    {
        return fail("unknown slot '" + args[0] + "' (type the full slot word)");
    }
    cmd.slot = *slot;
    if (!SlotAppliesTo(*slot, isPlayer))
    {
        return fail(std::string(SlotName(*slot)) + " is not a slot on " + Plate(isPlayer));
    }
    if (args.size() < 2)
    {
        return fail(std::string(SlotName(*slot)) + " needs a state, hide or auto");
    }
    if (args.size() > 3)
    {
        return fail("too many arguments");
    }

    const std::string second = ConsoleParse::ToLowerAscii(args[1]);
    if (*slot == Slot::Rank && second != "hide" && !(second == "auto" && args.size() == 2))
    {
        return fail("rank takes hide or auto only");
    }
    if (second == "hide")
    {
        if (args.size() > 2)
        {
            return fail("hide takes no icon name");
        }
        cmd.kind = IconCommand::Kind::SetSlot;
        cmd.slotOverride.hidden = true;
        return cmd;
    }
    if (second == "auto" && args.size() == 2)
    {
        cmd.kind = IconCommand::Kind::ClearSlot;
        return cmd;
    }
    if (second != "auto")
    {
        const auto state = ParseState(*slot, isPlayer, second);
        if (!state)
        {
            return fail("unknown state '" + args[1] + "' for " + std::string(SlotName(*slot)) +
                        " on " + Plate(isPlayer));
        }
        cmd.slotOverride.state = state;
    }
    if (args.size() == 3)
    {
        if (*slot == Slot::Rank)
        {
            return fail("rank takes hide or auto only");
        }
        if (!ConsoleParse::IsSafeIconName(args[2]))
        {
            return fail("icon names use letters, digits, '-' and '_' only");
        }
        cmd.slotOverride.icon = args[2];
    }
    cmd.kind = IconCommand::Kind::SetSlot;
    return cmd;
}

std::optional<std::string> ValidateTitle(const std::string& title)
{
    const std::size_t count = Utf8Utils::Utf8CharCount(title.c_str());
    if (count > static_cast<std::size_t>(RenderConstants::MAX_OVERRIDE_TITLE_CHARS))
    {
        return "title is " + std::to_string(count) + " characters; the limit is " +
               std::to_string(RenderConstants::MAX_OVERRIDE_TITLE_CHARS);
    }
    return std::nullopt;
}

std::string DescribeTitle(const Record& record)
{
    if (!record.title)
    {
        return "no title override";
    }
    return record.title->empty() ? "title hidden" : "title \"" + *record.title + "\"";
}

std::string DescribeIcons(const Record& record, bool isPlayer)
{
    std::vector<std::string> parts;
    for (std::size_t i = 0; i < SLOT_COUNT; ++i)
    {
        const auto& slot = record.slots[i];
        if (slot.Empty())
        {
            continue;
        }
        std::string part = std::string(SLOT_NAMES[i]) + "=";
        if (slot.hidden)
        {
            part += "hidden";
        }
        else
        {
            part += slot.state ? std::string(StateName(static_cast<Slot>(i), isPlayer, *slot.state))
                               : "auto";
            if (slot.icon)
            {
                part += "+" + *slot.icon;
            }
        }
        parts.push_back(std::move(part));
    }
    if (!record.extras.empty())
    {
        std::string part = "extras ";
        for (std::size_t i = 0; i < record.extras.size(); ++i)
        {
            if (i > 0)
            {
                part += ", ";
            }
            part += record.extras[i].icon;
        }
        parts.push_back(std::move(part));
    }
    if (parts.empty())
    {
        return "no icon overrides";
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            out += "; ";
        }
        out += parts[i];
    }
    return out;
}

std::string Describe(const Record& record, bool isPlayer)
{
    if (record.Empty())
    {
        return "no overrides";
    }
    std::string out;
    if (record.title)
    {
        out = DescribeTitle(record);
    }
    const bool hasIcons =
        !record.extras.empty() || !std::ranges::all_of(record.slots, &SlotOverride::Empty);
    if (hasIcons)
    {
        if (!out.empty())
        {
            out += "; ";
        }
        out += DescribeIcons(record, isPlayer);
    }
    return out;
}

std::shared_ptr<const Record> Get(std::uint32_t formID)
{
    auto& store = GetStore();
    if (store.live.load(std::memory_order_acquire) == 0)
    {
        return nullptr;
    }
    const std::lock_guard<std::mutex> lock(store.mutex);
    const auto it = store.records.find(formID);
    return it == store.records.end() ? nullptr : it->second;
}

ModifyResult Modify(std::uint32_t formID, const std::function<void(Record&)>& fn)
{
    auto& store = GetStore();
    Record copy;
    {
        const std::lock_guard<std::mutex> lock(store.mutex);
        const auto it = store.records.find(formID);
        if (it != store.records.end())
        {
            copy = *it->second;
        }
    }

    fn(copy);

    const std::lock_guard<std::mutex> lock(store.mutex);
    const auto it = store.records.find(formID);
    const bool unchanged = (it == store.records.end()) ? copy.Empty() : (*it->second == copy);
    if (unchanged)
    {
        return ModifyResult::Unchanged;
    }
    if (copy.extras.size() > static_cast<std::size_t>(RenderConstants::MAX_EXTRA_BADGES))
    {
        return ModifyResult::TooManyExtras;
    }
    if (IconNamesWithLocked(store, formID, copy).size() >
        static_cast<std::size_t>(RenderConstants::MAX_OVERRIDE_ICON_NAMES))
    {
        return ModifyResult::TooManyIconNames;
    }

    if (copy.Empty())
    {
        store.records.erase(formID);
    }
    else
    {
        store.records[formID] = std::make_shared<const Record>(std::move(copy));
    }
    store.live.store(store.records.size(), std::memory_order_release);
    RebuildIconNamesLocked(store);
    MarkDirtyLocked(store, formID);
    return ModifyResult::Applied;
}

void Erase(std::uint32_t formID)
{
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    if (store.records.erase(formID) == 0)
    {
        return;
    }
    store.live.store(store.records.size(), std::memory_order_release);
    RebuildIconNamesLocked(store);
    MarkDirtyLocked(store, formID);
}

void Clear()
{
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    for (const auto& [formID, record] : store.records)
    {
        MarkDirtyLocked(store, formID);
    }
    store.records.clear();
    store.live.store(0, std::memory_order_release);
    RebuildIconNamesLocked(store);
}

void EraseDynamic()
{
    constexpr std::uint32_t DYNAMIC_FORM_ID_START = 0xFF000000;
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    bool changed = false;
    for (auto it = store.records.begin(); it != store.records.end();)
    {
        if (it->first >= DYNAMIC_FORM_ID_START)
        {
            MarkDirtyLocked(store, it->first);
            it = store.records.erase(it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }
    if (changed)
    {
        store.live.store(store.records.size(), std::memory_order_release);
        RebuildIconNamesLocked(store);
    }
}

std::size_t Count()
{
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    return store.records.size();
}

std::vector<std::string> IconNames()
{
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    return {store.iconNames.begin(), store.iconNames.end()};
}

std::size_t IconNameCount()
{
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    return store.iconNames.size();
}

bool HasIconName(std::string_view name)
{
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    return store.iconNames.contains(std::string(name));
}

std::uint32_t IconSetVersion()
{
    return GetStore().iconVersion.load(std::memory_order_acquire);
}

void DrainDirty(std::vector<std::uint32_t>& out)
{
    auto& store = GetStore();
    const std::lock_guard<std::mutex> lock(store.mutex);
    out = std::move(store.dirty);
    store.dirty.clear();
}
}  // namespace ActorOverrides
