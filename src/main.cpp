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
#include "PCH.h"

#include <string>

#include "AppearanceTemplate.h"
#include "Hooks.h"
#include "Renderer.h"
#include "Settings.h"

/**
 * @namespace ConsoleCommands
 * @brief Console command registration and handlers.
 * @ingroup ConsoleCommands
 *
 * Provides the `glyph` console command for toggling nameplate rendering.
 * Registers by replacing an unused vanilla command slot at plugin load.
 */
namespace ConsoleCommands
{
    /**
     * glyph console command.
     * Usage: Type 'glyph' in console to toggle nameplate rendering on/off.
     */
    bool GlyphExecute(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
                      RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*,
                      double&, std::uint32_t&)
    {
        auto console = RE::ConsoleLog::GetSingleton();
        bool newState = Renderer::ToggleEnabled();

        if (console) {
            if (newState) {
                console->Print("glyph: Nameplate rendering ENABLED");
            } else {
                console->Print("glyph: Nameplate rendering DISABLED");
            }
        }

        logger::info("glyph: Rendering toggled to {}", newState ? "ON" : "OFF");

        return true;
    }

    void Register()
    {
        logger::info("Registering glyph console command...");

        auto* commands = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand();
        if (!commands) {
            logger::error("Failed to get console command table");
            return;
        }

        std::uint32_t commandCount = RE::SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd -
                                     RE::SCRIPT_FUNCTION::Commands::kConsoleOpBase;

        for (std::uint32_t i = 0; i < commandCount; ++i) {
            auto* cmd = &commands[i];
            if (cmd && cmd->functionName && _stricmp(cmd->functionName, "glyph") == 0) {
                logger::info("Console command 'glyph' already registered");
                return;
            }
        }

        // Find an unused command slot to replace
        bool found = false;
        for (std::uint32_t i = 0; i < commandCount && !found; ++i) {
            auto* cmd = &commands[i];
            if (!cmd || !cmd->functionName) continue;

            // Replace TestSeenData
            if (_stricmp(cmd->functionName, "TestSeenData") == 0) {
                cmd->functionName = "glyph";
                cmd->shortName = "";
                cmd->helpString = "Toggle nameplate rendering on/off";
                cmd->referenceFunction = false;
                cmd->executeFunction = GlyphExecute;
                cmd->numParams = 0;
                cmd->params = nullptr;
                found = true;
                logger::info("Registered 'glyph' console command");
            }
        }

        if (!found) {
            logger::warn("Could not find slot for glyph command");
        } else {
            logger::info("Usage: Type 'glyph' to toggle nameplate rendering");
        }
    }
}

/**
 * SKSE message handler - key lifecycle states:
 * - kDataLoaded: register console commands, retry NiOverride
 * - kPostLoadGame / kNewGame: queue pending appearance template apply
 */
void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    auto readTemplateSettingsSnapshot = []() {
        struct Snapshot {
            bool enabled = false;
            std::string formID;
            std::string plugin;
        } snapshot;

        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        snapshot.enabled = Settings::UseTemplateAppearance;
        snapshot.formID = Settings::TemplateFormID;
        snapshot.plugin = Settings::TemplatePlugin;
        return snapshot;
    };

    switch (a_msg->type) {
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
            // Loading a save, player should be available soon
            logger::debug("Post load game event received");
            if (readTemplateSettingsSnapshot().enabled) {
                AppearanceTemplate::SetPendingAppearanceApply();
            }
            // Test overlay interface after game load
            AppearanceTemplate::TestOverlayOnPlayer();
            break;

        case SKSE::MessagingInterface::kNewGame:
            // New game, player won't exist until after character creation
            {
                const auto snapshot = readTemplateSettingsSnapshot();
                logger::debug("New game event received - will apply after character creation");
                logger::info("UseTemplateAppearance={}, FormID={}, Plugin={}",
                    snapshot.enabled, snapshot.formID, snapshot.plugin);
                if (snapshot.enabled) {
                    AppearanceTemplate::SetPendingAppearanceApply();
                    logger::info("Pending appearance flag set to TRUE");
                } else {
                    logger::warn("UseTemplateAppearance is FALSE, not setting pending flag");
                }
            }
            break;
    }
}

/**
 * SKSE plugin load entry point.
 *
 * Called by SKSE after the plugin DLL is loaded. Initializes logging,
 * loads settings, and installs hooks.
 *
 * @param a_skse SKSE load interface.
 *
 * @return `true` if initialization succeeded.
 */
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    using namespace std::literals;

    SKSE::Init(a_skse);
    SKSE::AllocTrampoline(1 << 8);

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
    if (messaging) {
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

/**
 * SKSE plugin version information.
 *
 * Provides version, name, and compatibility info to SKSE.
 */
extern "C" __declspec(dllexport) constinit const auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData version;
    version.PluginVersion(REL::Version(0, 1, 0, 0));
    version.PluginName("glyph");
    version.AuthorName("lextpf | powerof3 | expired6978");
    version.UsesAddressLibrary(true);
    return version;
}();

/**
 * SKSE plugin query entry point.
 *
 * Called by SKSE during plugin enumeration. Provides basic plugin info.
 *
 * @param a_info Output plugin information structure.
 *
 * @return `true` always.
 */
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "glyph";
    a_info->version = 1;
    return true;
}
