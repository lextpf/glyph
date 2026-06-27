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
 *      displaying actor information and allows copying NPC appearance
 *      templates onto the player character via the game's D3D11 pipeline.
 *
 *    ----------------------------------------------------------------------
 *
 *      Repository:   https://github.com/lextpf/glyph
 *      License:      MIT
 */
#include "PCH.hpp"

#include "AppearanceTemplate.hpp"
#include "ConsoleCommands.hpp"
#include "Hooks.hpp"
#include "Renderer.hpp"
#include "Settings.hpp"

#include <string>

// SKSE message handler - key lifecycle states:
// - kDataLoaded: register console commands, retry NiOverride
// - kPostLoadGame / kNewGame: no auto-apply; the appearance template is applied
//   only via the 'glyph appearance on' console command.
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
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            logger::debug("Data loaded event received");
            ConsoleCommands::Register();
            // Retry getting NiOverride interface, SKEE should be fully loaded by now
            AppearanceTemplate::RetryNiOverrideInterface();
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            // Loading a save. The appearance template is never auto-applied;
            // trigger it on demand with the 'glyph appearance on' console command.
            logger::debug("Post load game event received");
            AppearanceTemplate::TestOverlayOnPlayer();
            break;

        case SKSE::MessagingInterface::kNewGame:
            // New game. The appearance template is never auto-applied; trigger it
            // on demand with the 'glyph appearance on' console command.
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

    // Register for SKSE messages
    auto messaging = SKSE::GetMessagingInterface();
    if (messaging)
    {
        messaging->RegisterListener(MessageHandler);
        logger::debug("Registered SKSE message listener");

        // Register for NiOverride/SKEE interface exchange
        // This must happen before PostLoad so we receive the interface broadcast
        // TODO: This does not work at all on 1.5.97, newer Racemenu (4.19 on 1.6.xx)
        AppearanceTemplate::QueryNiOverrideInterface();
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
