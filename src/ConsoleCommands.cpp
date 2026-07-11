#include "PCH.hpp"

#include "ConsoleCommands.hpp"

#include "ActorOverrides.hpp"
#include "ConsoleParse.hpp"
#include "RenderConstants.hpp"
#include "Renderer.hpp"
#include "Settings.hpp"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <vector>

namespace ConsoleCommands
{
namespace
{
using ActorOverrides::ModifyResult;
using ActorOverrides::Record;
using ConsoleParse::IsPrefixOf;
using ConsoleParse::ParseTriState;
using ConsoleParse::ToLowerAscii;
using ConsoleParse::TriState;

/**
 * @fn void Echo(const std::string& msg)
 * @brief Write literal console output and mirror it to the log.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Typed text can contain percent markers, so pass it as data to the console formatter.
 */
void Echo(const std::string& msg)
{
    if (auto* console = RE::ConsoleLog::GetSingleton())
    {
        console->Print("%s", msg.c_str());
    }
    logger::info("glyph console: {}", msg);
}

/**
 * @fn bool ReadDebugOverlayEnabled()
 * @brief Read the debug flag under the settings shared lock.
 * @author Alex (<https://github.com/lextpf>)
 */
bool ReadDebugOverlayEnabled()
{
    const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
    return Settings::Display().EnableDebugOverlay;
}

/**
 * @fn void WriteDebugOverlayEnabled(bool enabled)
 * @brief Publish a debug flag change to settings snapshot readers.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Publish a new generation after writing so the renderer refreshes its snapshot.
 */
void WriteDebugOverlayEnabled(bool enabled)
{
    {
        const std::unique_lock<std::shared_mutex> lock(Settings::Mutex());
        Settings::Display().EnableDebugOverlay = enabled;
    }
    Settings::Generation().fetch_add(1, std::memory_order_release);
}

/**
 * @struct Target
 * @brief Actor identity copied while the selected reference remains alive.
 * @author Alex (<https://github.com/lextpf>)
 *
 * No engine pointer escapes. An unavailable or non-actor target leaves `ok` false.
 */
struct Target
{
    bool ok = false;
    std::uint32_t formID = 0;
    std::string name;
    bool isPlayer = false;
};

/**
 * @fn Target ResolveTarget()
 * @brief Copy the selected actor identity, or use the player when nothing is selected.
 * @author Alex (<https://github.com/lextpf>)
 */
Target ResolveTarget()
{
    Target target;
    RE::Actor* actor = nullptr;
    const RE::NiPointer<RE::TESObjectREFR> selected = RE::Console::GetSelectedRef();
    if (selected)
    {
        actor = selected->As<RE::Actor>();
        if (!actor)
        {
            Echo("glyph: the selected reference is not an actor");
            return target;
        }
    }
    else
    {
        actor = RE::PlayerCharacter::GetSingleton();
    }
    if (!actor)
    {
        Echo("glyph: no actor to edit");
        return target;
    }
    target.ok = true;
    target.formID = actor->GetFormID();
    target.isPlayer = actor->IsPlayerRef();
    const char* rawName = actor->GetDisplayFullName();
    target.name = ConsoleParse::Trim(rawName ? rawName : "");
    if (target.name.empty())
    {
        target.name = target.isPlayer ? "Player" : "Unknown Actor";
    }
    return target;
}

/**
 * @struct IconFolderInfo
 * @brief Badge settings copied before filesystem access.
 * @author Alex (<https://github.com/lextpf>)
 */
struct IconFolderInfo
{
    bool enabled = false;
    std::string folder;
};

/**
 * @fn IconFolderInfo IconFolder()
 * @brief Copy badge availability settings before filesystem access.
 * @author Alex (<https://github.com/lextpf>)
 */
IconFolderInfo IconFolder()
{
    const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
    const auto& icons = Settings::Icons();
    return {icons.Enabled, icons.Folder};
}

/**
 * @fn bool TitleFormatDrawsTitle()
 * @brief Check whether the current title row expands the title token.
 * @author Alex (<https://github.com/lextpf>)
 */
bool TitleFormatDrawsTitle()
{
    const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
    return Settings::TitleFormat().find("%t") != std::string::npos;
}

/**
 * @fn bool AcceptIconName(const std::string& name)
 * @brief Check badge visibility and file availability before accepting an edit.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Check availability at edit time. Later folder changes are reported by the loader;
 * the store enforces the distinct-name cap.
 */
bool AcceptIconName(const std::string& name)
{
    const IconFolderInfo icons = IconFolder();
    if (!icons.enabled)
    {
        Echo("glyph: status icons are disabled in glyph.ini (IconsEnabled)");
        return false;
    }
    if (icons.folder.empty())
    {
        Echo("glyph: no IconFolder is configured in glyph.ini");
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(icons.folder + "/" + name + ".svg", ec))
    {
        Echo("glyph: no icon '" + name + "' in " + icons.folder);
        return false;
    }
    return true;
}

/**
 * @fn bool ReportRefusal(ModifyResult result, const Target& target)
 * @brief Print a rejected edit.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True when the edit was refused and its reason printed.
 */
bool ReportRefusal(ModifyResult result, const Target& target)
{
    switch (result)
    {
        case ModifyResult::TooManyExtras:
            Echo("glyph: " + target.name + " already has " +
                 std::to_string(RenderConstants::MAX_EXTRA_BADGES) + " extra badges");
            return true;
        case ModifyResult::TooManyIconNames:
            Echo("glyph: this session already uses " +
                 std::to_string(RenderConstants::MAX_OVERRIDE_ICON_NAMES) +
                 " different override icons; 'glyph clear all' frees them");
            return true;
        case ModifyResult::Applied:
        case ModifyResult::Unchanged:
            break;
    }
    return false;
}

/**
 * @fn void EchoTitle(const Target& target)
 * @brief Print the selected actor title override.
 * @author Alex (<https://github.com/lextpf>)
 */
void EchoTitle(const Target& target)
{
    const auto record = ActorOverrides::Get(target.formID);
    Echo("glyph: " + target.name + ": " +
         (record ? ActorOverrides::DescribeTitle(*record) : std::string("no title override")));
}

/**
 * @fn void EchoIcons(const Target& target)
 * @brief Print the selected actor badge overrides.
 * @author Alex (<https://github.com/lextpf>)
 */
void EchoIcons(const Target& target)
{
    const auto record = ActorOverrides::Get(target.formID);
    Echo("glyph: " + target.name + ": " +
         (record ? ActorOverrides::DescribeIcons(*record, target.isPlayer)
                 : std::string("no icon overrides")) +
         " (INI gates still apply)");
}

/**
 * @fn void PrintHelp()
 * @brief Print console command syntax and actor-targeting rules.
 * @author Alex (<https://github.com/lextpf>)
 */
void PrintHelp()
{
    Echo("glyph - sub-commands (any unambiguous prefix works, e.g. n / d / t / i;");
    Echo("  'clear' must be typed in full):");
    Echo("  glyph                              toggle nameplate rendering");
    Echo("  glyph help | ?                     show this help");
    Echo("  glyph status | s                   print current state");
    Echo("  glyph nameplates | n [on|off]      enable / disable / toggle nameplates");
    Echo("  glyph plates | p [on|off]          alias for 'nameplates'");
    Echo("  glyph debug | d [on|off]           enable / disable / toggle debug overlay");
    Echo("  glyph title | t [text|hide|auto]   custom title for the selected actor (or you)");
    Echo("  glyph icon | i <slot> <state>      force a badge slot; state, hide or auto");
    Echo("  glyph icon | i <slot> <state> <i>  same, drawing duotone icon <i> in the slot");
    Echo("  glyph icon add <icon> [r,g,b]      append an extra badge (at most 4)");
    Echo("  glyph icon remove <icon>           remove an extra badge");
    Echo("  glyph icon clear                   remove the actor's icon overrides");
    Echo("  glyph clear [all]                  remove the actor's (or every) override");
}

/**
 * @fn void HandleNameplates(const std::vector<std::string>& tokens, std::size_t argIdx)
 * @brief Set or toggle nameplate visibility and print the resulting state.
 * @author Alex (<https://github.com/lextpf>)
 */
void HandleNameplates(const std::vector<std::string>& tokens, std::size_t argIdx)
{
    const TriState target =
        (argIdx < tokens.size()) ? ParseTriState(tokens[argIdx]) : TriState::Toggle;
    bool newState;
    if (target == TriState::Toggle)
    {
        newState = Renderer::ToggleEnabled();
    }
    else
    {
        newState = (target == TriState::On);
        Renderer::SetEnabled(newState);
    }
    Echo(newState ? "glyph: nameplates ENABLED" : "glyph: nameplates DISABLED");
}

/**
 * @fn void HandleDebug(const std::vector<std::string>& tokens, std::size_t argIdx)
 * @brief Set or toggle debug visibility and publish the change.
 * @author Alex (<https://github.com/lextpf>)
 */
void HandleDebug(const std::vector<std::string>& tokens, std::size_t argIdx)
{
    const TriState target =
        (argIdx < tokens.size()) ? ParseTriState(tokens[argIdx]) : TriState::Toggle;
    const bool newState =
        (target == TriState::Toggle) ? !ReadDebugOverlayEnabled() : (target == TriState::On);
    WriteDebugOverlayEnabled(newState);
    Echo(newState ? "glyph: debug overlay ENABLED" : "glyph: debug overlay DISABLED");
}

/**
 * @fn void HandleStatus()
 * @brief Print runtime visibility flags and the override record count.
 * @author Alex (<https://github.com/lextpf>)
 */
void HandleStatus()
{
    Echo(Renderer::IsEnabled() ? "glyph: nameplates ENABLED" : "glyph: nameplates DISABLED");
    Echo(ReadDebugOverlayEnabled() ? "glyph: debug overlay ENABLED"
                                   : "glyph: debug overlay DISABLED");
    Echo("glyph: actor overrides: " + std::to_string(ActorOverrides::Count()));
}

/**
 * @fn void HandleTitle(const std::string& line, std::size_t argIdx)
 * @brief Parse the unsplit title argument and apply an actor override.
 * @author Alex (<https://github.com/lextpf>)
 */
void HandleTitle(const std::string& line, std::size_t argIdx)
{
    using Kind = ActorOverrides::TitleCommand::Kind;
    const auto cmd = ActorOverrides::ParseTitleCommand(ConsoleParse::RestAfterTokens(line, argIdx));
    if (cmd.kind == Kind::Error)
    {
        Echo("glyph: " + cmd.error);
        return;
    }
    const Target target = ResolveTarget();
    if (!target.ok)
    {
        return;
    }

    const std::string formatNote =
        TitleFormatDrawsTitle() ? "" : " (TitleFormat has no %t, so no title draws)";
    switch (cmd.kind)
    {
        case Kind::Show:
            EchoTitle(target);
            return;
        case Kind::Set:
            if (const auto refusal = ActorOverrides::ValidateTitle(cmd.text))
            {
                Echo("glyph: " + *refusal);
                return;
            }
            ActorOverrides::Modify(target.formID, [&](Record& r) { r.title = cmd.text; });
            Echo("glyph: " + target.name + ": title \"" + cmd.text + "\"" + formatNote);
            return;
        case Kind::Hide:
            ActorOverrides::Modify(target.formID, [](Record& r) { r.title = ""; });
            Echo("glyph: " + target.name + ": title hidden" + formatNote);
            return;
        case Kind::Auto:
            ActorOverrides::Modify(target.formID, [](Record& r) { r.title.reset(); });
            Echo("glyph: " + target.name + ": title back to automatic");
            return;
        case Kind::Error:
            return;
    }
}

/**
 * @fn void HandleIcon(const std::vector<std::string>& tokens, std::size_t argIdx)
 * @brief Validate a badge command and apply the accepted actor edit.
 * @author Alex (<https://github.com/lextpf>)
 */
void HandleIcon(const std::vector<std::string>& tokens, std::size_t argIdx)
{
    using Kind = ActorOverrides::IconCommand::Kind;
    const Target target = ResolveTarget();
    if (!target.ok)
    {
        return;
    }
    const std::vector<std::string> args(tokens.begin() + static_cast<std::ptrdiff_t>(argIdx),
                                        tokens.end());
    const auto cmd = ActorOverrides::ParseIconCommand(args, target.isPlayer);
    const auto slotIndex = static_cast<std::size_t>(cmd.slot);
    switch (cmd.kind)
    {
        case Kind::Show:
            EchoIcons(target);
            return;
        case Kind::Error:
            Echo("glyph: " + cmd.error);
            return;
        case Kind::SetSlot:
        {
            if (cmd.slotOverride.icon && !AcceptIconName(*cmd.slotOverride.icon))
            {
                return;
            }
            const auto result = ActorOverrides::Modify(
                target.formID, [&](Record& r) { r.slots[slotIndex] = cmd.slotOverride; });
            if (!ReportRefusal(result, target))
            {
                EchoIcons(target);
            }
            return;
        }
        case Kind::ClearSlot:
            ActorOverrides::Modify(target.formID, [&](Record& r) { r.slots[slotIndex] = {}; });
            EchoIcons(target);
            return;
        case Kind::Add:
        {
            if (!AcceptIconName(cmd.name))
            {
                return;
            }
            const auto result = ActorOverrides::Modify(
                target.formID,
                [&](Record& r)
                {
                    const auto it =
                        std::ranges::find(r.extras, cmd.name, &ActorOverrides::ExtraBadge::icon);
                    if (it != r.extras.end())
                    {
                        if (cmd.color)
                        {
                            it->color = *cmd.color;
                        }
                        return;
                    }
                    ActorOverrides::ExtraBadge extra;
                    extra.icon = cmd.name;
                    if (cmd.color)
                    {
                        extra.color = *cmd.color;
                    }
                    r.extras.push_back(std::move(extra));
                });
            if (!ReportRefusal(result, target))
            {
                EchoIcons(target);
            }
            return;
        }
        case Kind::Remove:
        {
            const auto result = ActorOverrides::Modify(
                target.formID,
                [&](Record& r)
                {
                    const auto it =
                        std::ranges::find(r.extras, cmd.name, &ActorOverrides::ExtraBadge::icon);
                    if (it != r.extras.end())
                    {
                        r.extras.erase(it);
                    }
                });
            if (result == ModifyResult::Unchanged)
            {
                Echo("glyph: " + target.name + " has no extra badge '" + cmd.name + "'");
                return;
            }
            EchoIcons(target);
            return;
        }
        case Kind::Clear:
            ActorOverrides::Modify(target.formID,
                                   [](Record& r)
                                   {
                                       r.slots.fill({});
                                       r.extras.clear();
                                   });
            EchoIcons(target);
            return;
    }
}

/**
 * @fn void HandleClear(const std::vector<std::string>& tokens, std::size_t argIdx)
 * @brief Clear the selected actor overrides or all session overrides.
 * @author Alex (<https://github.com/lextpf>)
 */
void HandleClear(const std::vector<std::string>& tokens, std::size_t argIdx)
{
    if (argIdx < tokens.size())
    {
        if (ToLowerAscii(tokens[argIdx]) == "all")
        {
            const std::size_t count = ActorOverrides::Count();
            ActorOverrides::Clear();
            Echo("glyph: cleared the overrides of " + std::to_string(count) + " actors");
        }
        else
        {
            Echo("glyph: 'clear' takes no argument, or 'all'");
        }
        return;
    }
    const Target target = ResolveTarget();
    if (!target.ok)
    {
        return;
    }
    ActorOverrides::Erase(target.formID);
    Echo("glyph: " + target.name + ": overrides cleared");
}

/**
 * @fn bool GlyphExecute(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
 *     RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script* a_scriptObj, RE::ScriptLocals*, double&,
 *     std::uint32_t&)
 * @brief Dispatch the raw console command on the game thread.
 * @author Alex (<https://github.com/lextpf>)
 */
bool GlyphExecute(const RE::SCRIPT_PARAMETER*,
                  RE::SCRIPT_FUNCTION::ScriptData*,
                  RE::TESObjectREFR*,
                  RE::TESObjectREFR*,
                  RE::Script* a_scriptObj,
                  RE::ScriptLocals*,
                  double&,
                  std::uint32_t&)
{
    // GetCommand() returns by value; retain the string while slicing title arguments.
    const std::string line = a_scriptObj ? a_scriptObj->GetCommand() : std::string{};
    const auto tokens = ConsoleParse::Tokenize(line);

    // Engine versions differ; remove the leading command word only when present.
    std::size_t cmdIdx = 0;
    if (!tokens.empty() && ToLowerAscii(tokens.front()) == "glyph")
    {
        cmdIdx = 1;
    }

    if (cmdIdx >= tokens.size())
    {
        HandleNameplates(tokens, tokens.size());
        return true;
    }

    const auto sub = ToLowerAscii(tokens[cmdIdx]);
    const std::size_t argIdx = cmdIdx + 1;

    if (IsPrefixOf(sub, "help") || sub == "?")
    {
        PrintHelp();
    }
    else if (IsPrefixOf(sub, "status"))
    {
        HandleStatus();
    }
    else if (IsPrefixOf(sub, "nameplates") || IsPrefixOf(sub, "plates"))
    {
        HandleNameplates(tokens, argIdx);
    }
    else if (IsPrefixOf(sub, "debug"))
    {
        HandleDebug(tokens, argIdx);
    }
    else if (IsPrefixOf(sub, "title"))
    {
        HandleTitle(line, argIdx);
    }
    else if (IsPrefixOf(sub, "icon"))
    {
        HandleIcon(tokens, argIdx);
    }
    else if (sub == "clear")
    {
        HandleClear(tokens, argIdx);
    }
    else
    {
        Echo("glyph: unknown sub-command '" + sub + "' (try 'glyph help')");
    }
    return true;
}
}  // namespace

void Register()
{
    logger::info("Registering glyph console command...");

    auto* commands = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand();
    if (!commands)
    {
        logger::error("Failed to get console command table");
        return;
    }

    const std::uint32_t commandCount = RE::SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd -
                                       RE::SCRIPT_FUNCTION::Commands::kConsoleOpBase;

    RE::SCRIPT_FUNCTION* targetSlot = nullptr;
    for (std::uint32_t i = 0; i < commandCount; ++i)
    {
        auto* cmd = &commands[i];
        if (cmd == nullptr || cmd->functionName == nullptr)
        {
            continue;
        }

        if (_stricmp(cmd->functionName, "glyph") == 0)
        {
            logger::info("Console command 'glyph' already registered");
            return;
        }

        if (targetSlot == nullptr && _stricmp(cmd->functionName, "TestSeenData") == 0)
        {
            targetSlot = cmd;
        }
    }

    if (targetSlot == nullptr)
    {
        logger::warn("Could not find slot for glyph command");
        return;
    }

    targetSlot->functionName = "glyph";
    targetSlot->shortName = "";
    targetSlot->helpString = "Glyph plugin: type 'glyph help' for usage";
    targetSlot->referenceFunction = false;
    targetSlot->executeFunction = GlyphExecute;
    // Zero parameters prevent the console from resolving hex tokens as form references.
    targetSlot->numParams = 0;
    targetSlot->params = nullptr;
    logger::info("Registered 'glyph' console command");
    logger::info("Usage: type 'glyph help' for the full command list");
}
}  // namespace ConsoleCommands
