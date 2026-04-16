#include "PCH.hpp"

#include "ConsoleCommands.hpp"

#include "AppearanceTemplate.hpp"
#include "Renderer.hpp"
#include "Settings.hpp"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ConsoleCommands
{
namespace
{
// Print to the in-game console and also mirror to the SKSE log.
void Echo(const std::string& msg)
{
    if (auto* console = RE::ConsoleLog::GetSingleton())
    {
        console->Print(msg.c_str());
    }
    logger::info("glyph console: {}", msg);
}

// Split on ASCII whitespace, no quoting support -- Skyrim's console already
// passes through a single typed line, and our tokens (subcommand, on/off,
// hex FormID, plugin filename) never contain whitespace.
std::vector<std::string> Tokenize(std::string_view text)
{
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0)
        {
            ++i;
        }
        if (i >= text.size())
        {
            break;
        }
        const size_t start = i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0)
        {
            ++i;
        }
        tokens.emplace_back(text.substr(start, i - start));
    }
    return tokens;
}

std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(),
                   s.end(),
                   s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// True when `typed` is a non-empty leading prefix of `full`.  Lets any
// unambiguous abbreviation stand in for a sub-command ("n" -> "nameplates",
// "d" -> "debug").  The sub-command initials are all distinct, so a single
// letter is never ambiguous; a full word still matches itself since no
// command word is a prefix of another.
bool IsPrefixOf(std::string_view typed, std::string_view full)
{
    return !typed.empty() && typed.size() <= full.size() && full.substr(0, typed.size()) == typed;
}

enum class TriState : std::uint8_t
{
    On,
    Off,
    Toggle
};

TriState ParseTriState(const std::string& arg)
{
    const auto lower = ToLowerAscii(arg);
    if (lower == "on" || lower == "1" || lower == "true" || lower == "yes")
    {
        return TriState::On;
    }
    if (lower == "off" || lower == "0" || lower == "false" || lower == "no")
    {
        return TriState::Off;
    }
    return TriState::Toggle;
}

// Read a single boolean settings field under a shared lock.
bool ReadDebugOverlayEnabled()
{
    const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
    return Settings::Display().EnableDebugOverlay;
}

// Write a single boolean settings field under a unique lock, then bump the
// settings generation so the renderer re-captures its snapshot next frame.
void WriteDebugOverlayEnabled(bool enabled)
{
    {
        const std::unique_lock<std::shared_mutex> lock(Settings::Mutex());
        Settings::Display().EnableDebugOverlay = enabled;
    }
    Settings::Generation().fetch_add(1, std::memory_order_release);
}

void PrintHelp()
{
    Echo("glyph - sub-commands (any unambiguous prefix works, e.g. n / d / s):");
    Echo("  glyph                              toggle nameplate rendering");
    Echo("  glyph help | ?                     show this help");
    Echo("  glyph status | s                   print current state");
    Echo("  glyph nameplates | n [on|off]      enable / disable / toggle nameplates");
    Echo("  glyph plates | p [on|off]          alias for 'nameplates'");
    Echo("  glyph debug | d [on|off]           enable / disable / toggle debug overlay");
    Echo("  glyph appearance | a               print appearance template state");
    Echo("  glyph appearance on                apply template using INI target");
    Echo("  glyph appearance on <id> <plugin>  apply template with explicit target");
    Echo("                                     (e.g. 'glyph appearance on 0xD62 Inigo.esp')");
    Echo("  glyph appearance off               clear applied flag (see help on revert)");
}

void HandleNameplates(const std::vector<std::string>& tokens, size_t argIdx)
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

void HandleDebug(const std::vector<std::string>& tokens, size_t argIdx)
{
    const TriState target =
        (argIdx < tokens.size()) ? ParseTriState(tokens[argIdx]) : TriState::Toggle;
    const bool newState =
        (target == TriState::Toggle) ? !ReadDebugOverlayEnabled() : (target == TriState::On);
    WriteDebugOverlayEnabled(newState);
    Echo(newState ? "glyph: debug overlay ENABLED" : "glyph: debug overlay DISABLED");
}

void ReportApplyResult(bool ok)
{
    if (ok)
    {
        const auto desc = AppearanceTemplate::AppliedTargetDescription();
        if (desc.empty())
        {
            Echo("glyph: appearance applied");
        }
        else
        {
            Echo("glyph: appearance applied (" + desc + ")");
        }
    }
    else
    {
        Echo("glyph: appearance apply failed (see SKSE/glyph.log for details)");
    }
}

void HandleAppearanceStatus()
{
    if (AppearanceTemplate::IsApplied())
    {
        const auto desc = AppearanceTemplate::AppliedTargetDescription();
        Echo("glyph: appearance template applied" +
             (desc.empty() ? std::string{} : " (" + desc + ")"));
    }
    else
    {
        Echo("glyph: no appearance template applied");
    }
}

void HandleAppearanceOn(const std::vector<std::string>& tokens, size_t argIdx)
{
    // No target args: force-apply the INI-configured FormID/plugin. Uses
    // ApplyConfiguredNow so it works even with UseTemplateAppearance disabled --
    // the template is never auto-applied, so the console command is the trigger.
    if (argIdx >= tokens.size())
    {
        ReportApplyResult(AppearanceTemplate::ApplyConfiguredNow());
        return;
    }

    // Explicit target requires both FormID and plugin.
    if (argIdx + 1 >= tokens.size())
    {
        Echo("glyph: usage 'glyph appearance on <formid> <plugin>'");
        Echo("       e.g. 'glyph appearance on 0xD62 Inigo.esp'");
        return;
    }

    const std::string& formIdArg = tokens[argIdx];
    const std::string& pluginArg = tokens[argIdx + 1];
    ReportApplyResult(AppearanceTemplate::ApplyWithTarget(formIdArg, pluginArg));
}

void HandleAppearanceOff()
{
    if (!AppearanceTemplate::IsApplied())
    {
        Echo("glyph: no appearance template applied");
        return;
    }
    AppearanceTemplate::ResetAppliedFlag();
    Echo("glyph: applied flag cleared.");
    Echo(
        "       Reload your save to actually see the original character -- the template's record "
        "edits and FaceGen mesh remain loaded in memory until the next save/reload.");
}

void HandleAppearance(const std::vector<std::string>& tokens, size_t argIdx)
{
    if (argIdx >= tokens.size())
    {
        HandleAppearanceStatus();
        return;
    }

    const auto action = ToLowerAscii(tokens[argIdx]);
    if (action == "on" || action == "apply")
    {
        HandleAppearanceOn(tokens, argIdx + 1);
    }
    else if (action == "off" || action == "revert")
    {
        HandleAppearanceOff();
    }
    else if (action == "status")
    {
        HandleAppearanceStatus();
    }
    else
    {
        Echo("glyph: appearance: unknown action '" + action + "' (try on, off, status)");
    }
}

void HandleStatus()
{
    Echo(Renderer::IsEnabled() ? "glyph: nameplates ENABLED" : "glyph: nameplates DISABLED");
    Echo(ReadDebugOverlayEnabled() ? "glyph: debug overlay ENABLED"
                                   : "glyph: debug overlay DISABLED");
    HandleAppearanceStatus();
}

bool GlyphExecute(const RE::SCRIPT_PARAMETER*,
                  RE::SCRIPT_FUNCTION::ScriptData*,
                  RE::TESObjectREFR*,
                  RE::TESObjectREFR*,
                  RE::Script* a_scriptObj,
                  RE::ScriptLocals*,
                  double&,
                  std::uint32_t&)
{
    auto tokens = Tokenize(a_scriptObj ? a_scriptObj->GetCommand() : std::string{});

    // Defensive: Script::GetCommand() includes the leading "glyph", but the
    // shape of the returned string has historically varied across CommonLibSSE
    // versions and game patches.  Skip the leading "glyph" token if present.
    size_t cmdIdx = 0;
    if (!tokens.empty() && ToLowerAscii(tokens.front()) == "glyph")
    {
        cmdIdx = 1;
    }

    // Bare 'glyph' -- toggle nameplates (preserves the original single-purpose behavior).
    if (cmdIdx >= tokens.size())
    {
        HandleNameplates(tokens, tokens.size());
        return true;
    }

    const auto sub = ToLowerAscii(tokens[cmdIdx]);
    const size_t argIdx = cmdIdx + 1;

    // Any unambiguous prefix of a sub-command works (e.g. 'n', 'd', 's').
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
    else if (IsPrefixOf(sub, "appearance"))
    {
        HandleAppearance(tokens, argIdx);
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
    // Keep numParams = 0 so the console does not pre-resolve hex tokens
    // (e.g. "0xD62") as form references before our handler sees them.
    targetSlot->numParams = 0;
    targetSlot->params = nullptr;
    logger::info("Registered 'glyph' console command");
    logger::info("Usage: type 'glyph help' for the full command list");
}
}  // namespace ConsoleCommands
