/*  ============================================================================================  *
 *                                                             ⠀    ⠀⠀⡄⠀⠀⠀⠀⠀⠀⣠⠀⠀⢀⠀⠀⢠⠀⠀⠀
 *                                                             ⠀     ⢸⣧⠀⠀⠀⠀⢠⣾⣇⣀⣴⣿⠀⠀⣼⡇⠀⠀
 *                                                                ⠀⠀⣾⣿⣧⠀⠀⢀⣼⣿⣿⣿⣿⣿⠀⣼⣿⣷⠀⠀
 *                                                                ⠀⢸⣿⣿⣿⡀⠀⠸⠿⠿⣿⣿⣿⡟⢀⣿⣿⣿⡇⠀
 *        ::::::::  :::     :::   ::: :::::::::  :::    :::       ⠀⣾⣿⣿⣿⣿⡀⠀⢀⣼⣿⣿⡿⠁⣿⣿⣿⣿⣷⠀
 *       :+:    :+: :+:     :+:   :+: :+:    :+: :+:    :+:       ⢸⣿⣿⣿⣿⠁⣠⣤⣾⣿⣿⣯⣤⣄⠙⣿⣿⣿⣿⡇
 *       +:+        +:+      +:+ +:+  +:+    +:+ +:+    +:+       ⣿⣿⣿⣿⣿⣶⣿⣿⣿⣿⣿⣿⣿⣿⣶⣿⣿⣿⣿⣿
 *       :#:        +#+       +#++:   +#++:++#+  +#++:++#++       ⠘⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡏
 *       +#+   +#+# +#+        +#+    +#+        +#+    +#+       ⠀⠘⢿⣿⣿⣿⠛⠻⢿⣿⣿⣿⠹⠟⣿⣿⣿⣿⣿⠀
 *       #+#    #+# #+#        #+#    #+#        #+#    #+#       ⠀⠀⠘⢿⣿⣿⣦⡄⢸⣿⣿⣿⡇⠠⣿⣿⣿⣿⡇⠀
 *        ########  ########## ###    ###        ###    ###       ⠀⠀⠀⠘⢿⣿⣿⠀⣸⣿⣿⣿⠇⠀⠙⣿⣿⣿⠁⠀
 *                                                                ⠀⠀⠀⠀⠘⣿⠃⢰⣿⣿⣿⡇⠀⠀⠀⠈⢻⡇⠀⠀
 *                                                                ⠀⠀⠀⠀⠀⠈⠀⠈⢿⣿⣿⣿⣶⡶⠂⠀⠀⠁⠀⠀
 *                                << S K Y R I M   P L U G I N >>         ⠀⠀⠈⠻⣿⡿⠋⠀⠀⠀⠀⠀⠀⠀
 *
 *  ============================================================================================  *
 *
 *      An SKSE plugin for Skyrim SE/AE that renders an ImGui overlay
 *      displaying actor
 * nameplates via the game's D3D11 pipeline.
 *
 *    ----------------------------------------------------------------------
 *
 *      Repository:   https://github.com/lextpf/glyph
 *      License:      MIT
 */
#include "PCH.hpp"

#include "ConsoleCommands.hpp"
#include "Hooks.hpp"
#include "HudCompat.hpp"
#include "ProjectManifest.hpp"
#include "Renderer.hpp"
#include "Settings.hpp"

#include <string>

namespace
{
// RaceMenu (RaceSex Menu) close -> force a player identity refresh, and log the
// live name so we can confirm the engine reflects the rename pre-reload.
class RaceMenuCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    static RaceMenuCloseSink* GetSingleton()
    {
        static RaceMenuCloseSink s;
        return &s;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        // String literal (not RE::RaceSexMenu::MENU_NAME) to avoid a
        // BSFixedString-vs-string_view comparison ambiguity; the value is stable.
        if (e && !e->opening && e->menuName == "RaceSex Menu")
        {
            if (auto* pc = RE::PlayerCharacter::GetSingleton())
            {
                const char* nm = pc->GetDisplayFullName();
                logger::info("RaceSex closed; player GetDisplayFullName() = '{}'",
                             nm ? nm : "(null)");
            }
            Renderer::RequestIdentityRefresh();
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};
}  // namespace

// SKSE message handler for plugin lifecycle events.
void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type)
    {
        case SKSE::MessagingInterface::kPostLoad:
            logger::debug("Post load event received");
            break;

        case SKSE::MessagingInterface::kPostPostLoad:
            // All PostLoad handlers have run, SKEE might send interface here
            logger::debug("PostPostLoad event received");
            // All plugins are loaded -- request the TrueHUD API and probe
            // for moreHUD (One Voice Per Actor deconfliction).
            HudCompat::Initialize();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            logger::debug("Data loaded event received");
            ConsoleCommands::Register();
            // RaceMenu rename -> prompt player-name refresh (see RaceMenuCloseSink).
            if (auto* ui = RE::UI::GetSingleton())
            {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(RaceMenuCloseSink::GetSingleton());
            }
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            logger::debug("Post load game event received");
            break;

        case SKSE::MessagingInterface::kNewGame:
            logger::debug("New game event received");
            break;
    }
}

// SKSE plugin load entry point. Initializes logging, loads settings, and
// installs hooks.
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    using namespace std::literals;

    SKSE::Init(a_skse);

    static constexpr std::size_t TRAMPOLINE_SIZE = 256;
    SKSE::AllocTrampoline(TRAMPOLINE_SIZE);

    // Setup logging
    auto path = logger::log_directory();
    if (!path)
    {
        return false;
    }

    *path /= "glyph.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

    log->set_level(spdlog::level::debug);  // Enable debug to see draw diagnostics
    log->flush_on(spdlog::level::debug);   // Flush debug too so heartbeat shows in file

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v"s);

    logger::debug("glyph loaded");
    Settings::Load();

    // Resolve the obfuscated asset manifest (fonts / tier emblems / particles).
    // A missing or bad manifest is non-fatal: each loader falls back to its
    // built-in default.
    ProjectManifest::Load();

    // Register for SKSE messages
    auto messaging = SKSE::GetMessagingInterface();
    if (messaging)
    {
        messaging->RegisterListener(MessageHandler);
        logger::debug("Registered SKSE message listener");
    }

    Hooks::Install();

    return true;
}

// SKSE plugin version information.
//
// Provides version, name, and compatibility info to SKSE.
extern "C" __declspec(dllexport) constinit const auto SKSEPlugin_Version = []()
{
    SKSE::PluginVersionData version;
    version.PluginVersion(REL::Version(0, 1, 0, 0));
    version.PluginName("glyph");
    version.AuthorName("lextpf | powerof3 | expired6978");
    version.UsesAddressLibrary(true);
    // glyph targets Skyrim SE 1.5.97 only (AE/VR untested, and the D3D hook
    // patches a compile-time SE call offset). An explicit allow-list overrides
    // the address-library version-independence flag, so SKSE refuses to load us
    // on any other runtime instead of crashing at startup.
    version.CompatibleVersions({SKSE::RUNTIME_SSE_1_5_97});
    return version;
}();

// SKSE plugin query entry point. Called during plugin enumeration; provides
// basic plugin info.
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Query(const SKSE::QueryInterface*,
                                                               SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "glyph";
    a_info->version = 1;
    return true;
}
