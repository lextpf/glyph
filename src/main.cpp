// The plugin initializes settings and asset paths before installing the D3D hooks.

#include "PCH.hpp"

#include "ActorOverrides.hpp"
#include "ConsoleCommands.hpp"
#include "Hooks.hpp"
#include "HudCompat.hpp"
#include "ProjectManifest.hpp"
#include "Renderer.hpp"
#include "Settings.hpp"

#include <string>

namespace
{
/**
 * @class RaceMenuCloseSink
 * @brief Forward RaceMenu closure to the renderer's identity-refresh request.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Menu events run on the game thread. The render thread consumes the request and
 * owns the cache mutation. The sink has process lifetime after registration.
 */
class RaceMenuCloseSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    /**
     * @fn static RaceMenuCloseSink* GetSingleton()
     * @brief Access the menu event sink with process lifetime.
     * @author Alex (<https://github.com/lextpf>)
     */
    static RaceMenuCloseSink* GetSingleton()
    {
        static RaceMenuCloseSink s;
        return &s;
    }
    /**
     * @fn RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
     *     RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
     * @brief Request a player identity refresh when RaceMenu closes.
     * @author Alex (<https://github.com/lextpf>)
     */
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* e,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        // The literal avoids BSFixedString/string_view comparison ambiguity.
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

// Address library selects SE/AE IDs; CommonLib handles pre/post-1.6.629 layouts.
SKSEPluginInfo(.Version = REL::Version(0, 1, 0, 0),
               .Name = "glyph",
               .Author = "lextpf | powerof3 | expired6978",
               .StructCompatibility = SKSE::StructCompatibility::Independent,
               .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

    /**
     * @fn void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
     * @brief Initialize event-dependent services and clear recycled actor overrides.
     * @author Alex (<https://github.com/lextpf>)
     */
    void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type)
    {
        case SKSE::MessagingInterface::kPostLoad:
            logger::debug("Post load event received");
            break;

        case SKSE::MessagingInterface::kPostPostLoad:
            logger::debug("PostPostLoad event received");
            HudCompat::Initialize();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            logger::debug("Data loaded event received");
            ConsoleCommands::Register();

            if (auto* ui = RE::UI::GetSingleton())
            {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(RaceMenuCloseSink::GetSingleton());
            }
            break;

        // Runtime FormIDs are recycled across loads; keep overrides only for persistent references.
        case SKSE::MessagingInterface::kPostLoadGame:
            logger::debug("Post load game event received");
            ActorOverrides::EraseDynamic();
            break;

        case SKSE::MessagingInterface::kNewGame:
            logger::debug("New game event received");
            ActorOverrides::EraseDynamic();
            break;
    }
}

/**
 * @fn bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
 * @brief Initialize plugin services before enabling D3D hooks.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Hooks run last because device creation can immediately require settings, manifest
 * paths, and trampoline memory. Logging truncates glyph.log for each process launch.
 *
 * @param a_skse Non-null SKSE interface supplied by the loader.
 * @return False for an unsupported runtime or unavailable SKSE log directory.
 * Hook installation failures are logged and do not change the successful load result.
 */
extern "C" __declspec(dllexport) bool __cdecl SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    using namespace std::literals;

    const auto runtime = REL::Module::GetRuntime();
    if (runtime != REL::Module::Runtime::SE && runtime != REL::Module::Runtime::AE)
    {
        return false;
    }

    SKSE::Init(a_skse);

    static constexpr std::size_t TRAMPOLINE_SIZE = 256;
    SKSE::AllocTrampoline(TRAMPOLINE_SIZE);

    // glyph.log in the SKSE log directory, truncated on each launch.
    auto path = logger::log_directory();
    if (!path)
    {
        return false;
    }

    *path /= "glyph.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

    log->set_level(spdlog::level::debug);  // Debug level carries the draw diagnostics
    log->flush_on(spdlog::level::debug);   // Flush at debug too, so a crash keeps the tail

    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v"s);

    logger::info("glyph loaded on Skyrim {} ({})",
                 REL::Module::get().version().string("."),
                 runtime == REL::Module::Runtime::AE ? "AE/GOG" : "SE");
    Settings::Load();

    ProjectManifest::Load();

    auto messaging = SKSE::GetMessagingInterface();
    if (messaging)
    {
        messaging->RegisterListener(MessageHandler);
        logger::debug("Registered SKSE message listener");
    }

    Hooks::Install();

    return true;
}
