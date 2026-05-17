#include "AppearanceTemplateInternal.hpp"

#include "OutfitCopy.hpp"

#include <atomic>
#include <format>

namespace AppearanceTemplate
{
namespace
{
enum class ApplyResult
{
    Applied,
    RetryableFailure,
    PermanentFailure
};
}

TemplateState& GetState()
{
    static TemplateState state;
    return state;
}

std::mutex& GetStateMutex()
{
    static std::mutex stateMutex;
    return stateMutex;
}

bool IsAppliedState()
{
    std::lock_guard<std::mutex> lock(GetStateMutex());
    return GetState().applied;
}

void SetAppliedState(bool value)
{
    std::lock_guard<std::mutex> lock(GetStateMutex());
    GetState().applied = value;
}

void SetTemplateStateInfo(const std::string& plugin, RE::FormID formID)
{
    std::lock_guard<std::mutex> lock(GetStateMutex());
    auto& state = GetState();
    state.plugin = plugin;
    state.formID = formID;
}

std::atomic<bool> s_applyInProgress{false};

void ResetAppliedFlag()
{
    SetAppliedState(false);
    logger::info("AppearanceTemplate: Applied flag reset");
}

bool IsApplied()
{
    return IsAppliedState();
}

std::string AppliedTargetDescription()
{
    std::lock_guard<std::mutex> lock(GetStateMutex());
    const auto& state = GetState();
    if (!state.applied || state.plugin.empty() || state.formID == 0)
    {
        return {};
    }
    return std::format("{}:0x{:08X}", state.plugin, state.formID);
}

// Stubs for overlay interface functions
void QueryNiOverrideInterface()
{ /* NiOverride not used; kept for API compatibility */
}
void RetryNiOverrideInterface()
{ /* NiOverride not used; kept for API compatibility */
}
void TestOverlayOnPlayer()
{
    logger::info("Overlay system not implemented");
}

// Resolve config to a template NPC form. Returns nullptr on failure.
static RE::TESNPC* ResolveTemplateNPC(const ApplyConfig& cfg)
{
    RE::FormID resolvedID = ResolveFormID(cfg.templateFormID, cfg.templatePlugin);
    if (resolvedID == 0)
    {
        logger::error("AppearanceTemplate: Failed to resolve FormID");
        return nullptr;
    }

    SetTemplateStateInfo(cfg.templatePlugin, resolvedID);

    auto form = RE::TESForm::LookupByID(resolvedID);
    if (!form)
    {
        logger::error("AppearanceTemplate: Form not found for ID {:08X}", resolvedID);
        return nullptr;
    }

    auto templateNPC = form->As<RE::TESNPC>();
    if (!templateNPC)
    {
        logger::error("AppearanceTemplate: Form {:08X} is not an NPC (type: {})",
                      resolvedID,
                      static_cast<int>(form->GetFormType()));
        return nullptr;
    }

    return templateNPC;
}

static void ProcessSpawnedActor(RE::ObjectRefHandle handle, int framesRemaining)
{
    if (framesRemaining > 0)
    {
        if (auto* task = SKSE::GetTaskInterface())
        {
            task->AddTask([handle, framesRemaining]()
                          { ProcessSpawnedActor(handle, framesRemaining - 1); });
        }
        else
        {
            ProcessSpawnedActor(handle, 0);
        }
        return;
    }

    auto spawnedRef = handle.get();
    if (!spawnedRef)
    {
        logger::warn("AppearanceTemplate: Spawned actor no longer valid");
        return;
    }

    auto* tempActor = spawnedRef->As<RE::Actor>();
    auto* player = RE::PlayerCharacter::GetSingleton();

    bool copyOutfit = false;
    {
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        copyOutfit = Settings::Appearance().TemplateCopyOutfit;
    }

    if (tempActor && player && copyOutfit)
    {
        logger::info("AppearanceTemplate: Copying outfit from temporary actor...");
        OutfitCopy::CopyOutfitBetweenActors(tempActor, player);
    }

    spawnedRef->Disable();
    // Do NOT call SetDelete(true) here.  It zeroes the FormID immediately while
    // the engine's movement controller still holds a live reference, causing an
    // access violation.  Disabled non-persistent refs are cleaned up by the
    // engine on cell reset.
    logger::info("AppearanceTemplate: Temporary actor disabled");
}

// Copy outfit from template to player, spawning a temporary actor if needed.
static void ApplyOutfitFromTemplate(RE::TESNPC* templateNPC, bool copyOutfit)
{
    if (!copyOutfit)
    {
        return;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player)
    {
        return;
    }

    RE::Actor* templateActor = OutfitCopy::FindLoadedActorByBase(templateNPC);
    if (templateActor)
    {
        logger::info("AppearanceTemplate: Found loaded actor for template NPC, copying outfit");
        OutfitCopy::CopyOutfitBetweenActors(templateActor, player);
        return;
    }

    if (player->IsInCombat())
    {
        logger::warn("AppearanceTemplate: Skipping temporary outfit actor spawn during combat");
        return;
    }
    if (!player->GetParentCell() || !player->GetParentCell()->IsAttached())
    {
        logger::warn(
            "AppearanceTemplate: Skipping temporary outfit actor spawn while player cell is "
            "not attached");
        return;
    }
    if (templateNPC == player->GetActorBase())
    {
        logger::warn(
            "AppearanceTemplate: Skipping temporary outfit actor spawn because template is "
            "player base");
        return;
    }

    logger::info(
        "AppearanceTemplate: No loaded actor found, spawning temporary actor for outfit...");
    auto spawned = player->PlaceObjectAtMe(templateNPC, false);
    if (!spawned)
    {
        logger::warn("AppearanceTemplate: Failed to spawn temporary actor");
        return;
    }

    auto* spawnedActor = spawned->As<RE::Actor>();
    if (spawnedActor)
    {
        // @author Codex (https://github.com/codex)
        // The temp actor must stay enabled ~5 frames so the engine's default
        // outfit pipeline (async, runs post-spawn) populates inventory.
        // Disable()-ing immediately leaves it empty, so the later
        // OutfitCopy::CopyOutfitBetweenActors finds nothing -- the bug this
        // workaround dodges.
        //
        // Hide the actor by sinking it 10000 units below the floor: draw-
        // distance culling, audio falloff, and the player's interaction
        // raycast all reject it at that depth. Do NOT use a no-collision
        // flag here -- the engine bypasses outfit equip for some
        // no-collision states.
        auto pos = spawnedActor->GetPosition();
        pos.z -= 10000.0f;
        spawnedActor->SetPosition(pos, false);

        logger::info("AppearanceTemplate: Spawned temporary actor {:08X}",
                     spawnedActor->GetFormID());
        RE::ObjectRefHandle spawnedHandle = spawnedActor->GetHandle();
        ProcessSpawnedActor(spawnedHandle, 5);
    }
    else
    {
        logger::warn("AppearanceTemplate: Spawned reference is not an actor");
        spawned->Disable();
    }
}

// Snapshot the [Appearance] section into an immutable ApplyConfig.
// Used by both the auto-apply flow and the console-triggered manual apply.
static ApplyConfig BuildConfigFromSettings()
{
    ApplyConfig cfg;
    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
    const auto& app = Settings::Appearance();
    cfg.useTemplateAppearance = app.UseTemplateAppearance;
    cfg.templateFormID = app.TemplateFormID;
    cfg.templatePlugin = app.TemplatePlugin;
    cfg.templateIncludeRace = app.TemplateIncludeRace;
    cfg.templateIncludeBody = app.TemplateIncludeBody;
    cfg.templateCopyFaceGen = app.TemplateCopyFaceGen;
    cfg.templateCopyOutfit = app.TemplateCopyOutfit;
    return cfg;
}

// Core apply pipeline. forceApply=true bypasses the "already applied" and
// "feature enabled in INI" gates so manual console-driven applies always run.
static ApplyResult ApplyTemplateCore(const ApplyConfig& cfg, bool forceApply)
{
    if (!forceApply && IsAppliedState())
    {
        logger::debug("AppearanceTemplate: Already applied this session");
        return ApplyResult::Applied;
    }

    bool expected = false;
    if (!s_applyInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        logger::debug("AppearanceTemplate: Apply already in progress");
        return ApplyResult::RetryableFailure;
    }
    struct ApplyScope
    {
        ~ApplyScope() { s_applyInProgress.store(false, std::memory_order_release); }
    } applyScope;

    if (!forceApply && !cfg.useTemplateAppearance)
    {
        logger::debug("AppearanceTemplate: Feature disabled in settings");
        return ApplyResult::PermanentFailure;
    }
    if (cfg.templateFormID.empty() || cfg.templatePlugin.empty())
    {
        logger::warn("AppearanceTemplate: No template configured (FormID/plugin empty)");
        return ApplyResult::PermanentFailure;
    }

    logger::info(
        "AppearanceTemplate: Applying template {}|{}", cfg.templateFormID, cfg.templatePlugin);

    RE::TESNPC* templateNPC = ResolveTemplateNPC(cfg);
    if (!templateNPC)
    {
        return ApplyResult::PermanentFailure;
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    auto playerBase = player ? player->GetActorBase() : nullptr;
    if (!playerBase)
    {
        logger::error("AppearanceTemplate: Player base not available");
        return ApplyResult::RetryableFailure;
    }

    const bool sexMismatch = templateNPC->IsFemale() != playerBase->IsFemale();
    if (sexMismatch && !cfg.templateIncludeRace)
    {
        logger::info(
            "AppearanceTemplate: Deferring template - sex mismatch with TemplateIncludeRace=false "
            "(player: {}, template: {})",
            playerBase->IsFemale() ? "Female" : "Male",
            templateNPC->IsFemale() ? "Female" : "Male");
        return ApplyResult::RetryableFailure;
    }

    bool racesCompatible = IsRaceCompatible(templateNPC);
    if (!racesCompatible && !cfg.templateIncludeRace)
    {
        logger::warn("AppearanceTemplate: Race mismatch detected!");
        logger::warn(
            "AppearanceTemplate: Deferring template until player race stabilizes or "
            "TemplateIncludeRace is enabled");
        return ApplyResult::RetryableFailure;
    }

    if (!CopyAppearanceToPlayer(templateNPC, cfg.templateIncludeRace, cfg.templateIncludeBody))
    {
        logger::error("AppearanceTemplate: Failed to copy appearance");
        return ApplyResult::PermanentFailure;
    }

    if (cfg.templateCopyFaceGen)
    {
        if (racesCompatible || cfg.templateIncludeRace)
        {
            if (!ApplyFaceGen(templateNPC))
            {
                logger::warn(
                    "AppearanceTemplate: FaceGen not applied - falling back to record data only");
            }
        }
        else
        {
            logger::warn("AppearanceTemplate: Skipping FaceGen due to race mismatch");
            logger::warn(
                "AppearanceTemplate: Enable TemplateIncludeRace to copy FaceGen across races");
        }
    }
    else
    {
        logger::info("AppearanceTemplate: FaceGen copy disabled in settings");
    }

    ApplyOutfitFromTemplate(templateNPC, cfg.templateCopyOutfit);
    UpdatePlayerAppearance();

    SetAppliedState(true);
    logger::info("AppearanceTemplate: Successfully applied template appearance");

    return ApplyResult::Applied;
}

bool ApplyIfConfigured()
{
    return ApplyTemplateCore(BuildConfigFromSettings(), /*forceApply=*/false) ==
           ApplyResult::Applied;
}

bool ApplyWithTarget(const std::string& formIdStr, const std::string& pluginName)
{
    ApplyConfig cfg = BuildConfigFromSettings();
    cfg.templateFormID = formIdStr;
    cfg.templatePlugin = pluginName;
    // Force-apply: ignore the global feature toggle and any previous applied state
    // so the console user can re-apply with a different target at any time.
    cfg.useTemplateAppearance = true;
    SetAppliedState(false);
    return ApplyTemplateCore(cfg, /*forceApply=*/true) == ApplyResult::Applied;
}

static std::atomic<bool> s_pendingAppearanceApply{false};
static std::atomic<int> s_checkCount{0};
static std::atomic<int> s_readyStreak{0};
static std::atomic<int> s_applyAttempts{0};

void SetPendingAppearanceApply()
{
    s_pendingAppearanceApply.store(true);
    s_checkCount.store(0);
    s_readyStreak.store(0);
    s_applyAttempts.store(0);
}

// @author Claude (https://github.com/claude)
// SKSE kPostLoadGame / kNewGame fire long before the player is *visually*
// ready: cell still attaching, player 3D may be unloaded, Race / Fader menus
// may be active, and tint / FaceGen swaps in that window flicker or get
// clobbered by the engine's own init. No clean SKSE "fully loaded" event,
// so we poll.
//
// Two counters:
//   s_checkCount  - total polls since the apply was requested (diagnostic
//                   logging cadence only).
//   s_readyStreak - consecutive polls where the readiness predicate is TRUE;
//                   reset on any FALSE to debounce transient unloads (e.g.
//                   a one-frame menu).
//
// 120-frame stable streak (~2s at 60fps) is the empirical minimum where
// outfit / FaceGen copies stick across fade-in. Lower values let the engine
// re-apply the player's default tint a few frames in and revert FaceGen.
void CheckPendingAppearanceTemplate()
{
    static std::atomic<bool> loggedOnce{false};
    if (!loggedOnce.exchange(true, std::memory_order_relaxed))
    {
        logger::info("CheckPendingAppearanceTemplate called for first time");
    }

    if (!s_pendingAppearanceApply.load())
    {
        return;
    }

    int count = s_checkCount.fetch_add(1) + 1;

    auto player = RE::PlayerCharacter::GetSingleton();
    auto playerBase = player ? player->GetActorBase() : nullptr;
    auto* playerCell = player ? player->GetParentCell() : nullptr;
    bool isCellAttached = playerCell && playerCell->IsAttached();
    bool is3DLoaded = player ? player->Is3DLoaded() : false;
    bool gameActive = true;
    if (auto* main = RE::Main::GetSingleton())
    {
        gameActive = main->gameActive;
    }

    const char* blockingMenu = nullptr;
    if (auto* ui = RE::UI::GetSingleton())
    {
        static constexpr const char* BLOCKING_MENUS[] = {
            "Loading Menu",
            "Main Menu",
            "Fader Menu",
            "RaceSex Menu",
            "MessageBoxMenu",
            "Menu",
            "TweenMenu",
        };
        for (const auto* menu : BLOCKING_MENUS)
        {
            if (ui->IsMenuOpen(menu))
            {
                blockingMenu = menu;
                break;
            }
        }
    }

    const bool ready = gameActive && player != nullptr && playerBase != nullptr && isCellAttached &&
                       is3DLoaded && blockingMenu == nullptr;

    int readyStreak = 0;
    if (ready)
    {
        readyStreak = s_readyStreak.fetch_add(1) + 1;
    }
    else
    {
        s_readyStreak.store(0);
    }

    const bool shouldLog = (count % 60 == 0) || (ready && readyStreak == 1);
    if (shouldLog)
    {
        logger::debug(
            "Appearance check #{}: player={}, base={}, cellAttached={}, 3D={}, gameActive={}, "
            "blockingMenu={}, readyStreak={}",
            count,
            player != nullptr,
            playerBase != nullptr,
            isCellAttached,
            is3DLoaded,
            gameActive,
            blockingMenu ? blockingMenu : "<none>",
            readyStreak);
    }

    constexpr int REQUIRED_READY_STREAK = 120;
    if (ready && readyStreak >= REQUIRED_READY_STREAK)
    {
        logger::info("Player ready after {} checks, applying appearance template", count);
        s_pendingAppearanceApply.store(false);
        s_checkCount.store(0);
        s_readyStreak.store(0);
        if (auto* task = SKSE::GetTaskInterface())
        {
            task->AddTask(
                []()
                {
                    constexpr int MAX_RETRYABLE_ATTEMPTS = 6;
                    ApplyResult result =
                        ApplyTemplateCore(BuildConfigFromSettings(), /*forceApply=*/false);
                    if (result == ApplyResult::RetryableFailure)
                    {
                        int attempts = s_applyAttempts.fetch_add(1) + 1;
                        if (attempts < MAX_RETRYABLE_ATTEMPTS)
                        {
                            logger::info(
                                "AppearanceTemplate: Transient apply failure, retrying ({}/{})",
                                attempts,
                                MAX_RETRYABLE_ATTEMPTS);
                            s_pendingAppearanceApply.store(true);
                            s_checkCount.store(0);
                            s_readyStreak.store(0);
                        }
                        else
                        {
                            logger::warn(
                                "AppearanceTemplate: Giving up after {} transient apply retries",
                                attempts);
                            s_applyAttempts.store(0);
                        }
                    }
                    else
                    {
                        s_applyAttempts.store(0);
                    }
                });
        }
        else
        {
            constexpr int MAX_RETRYABLE_ATTEMPTS = 6;
            ApplyResult result = ApplyTemplateCore(BuildConfigFromSettings(), /*forceApply=*/false);
            if (result == ApplyResult::RetryableFailure)
            {
                int attempts = s_applyAttempts.fetch_add(1) + 1;
                if (attempts < MAX_RETRYABLE_ATTEMPTS)
                {
                    logger::info("AppearanceTemplate: Transient apply failure, retrying ({}/{})",
                                 attempts,
                                 MAX_RETRYABLE_ATTEMPTS);
                    s_pendingAppearanceApply.store(true);
                    s_checkCount.store(0);
                    s_readyStreak.store(0);
                }
                else
                {
                    logger::warn("AppearanceTemplate: Giving up after {} transient apply retries",
                                 attempts);
                    s_applyAttempts.store(0);
                }
            }
            else
            {
                s_applyAttempts.store(0);
            }
        }
    }
}
}  // namespace AppearanceTemplate
