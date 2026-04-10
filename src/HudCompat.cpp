#include "PCH.hpp"

#include "HudCompat.hpp"

#include "TrueHUDAPI.h"

#include <SKSE/SKSE.h>

namespace HudCompat
{
namespace
{
TRUEHUD_API::IVTrueHUD3* g_trueHUD = nullptr;
bool g_moreHUD = false;
}  // namespace

void Initialize()
{
    g_trueHUD = static_cast<TRUEHUD_API::IVTrueHUD3*>(
        TRUEHUD_API::RequestPluginAPI(TRUEHUD_API::InterfaceVersion::V3));
    logger::info("HudCompat: TrueHUD API {}", g_trueHUD ? "acquired (V3)" : "not present");

    g_moreHUD = GetModuleHandleA("AHZmoreHUDPlugin.dll") != nullptr;
    if (!g_moreHUD)
    {
        if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
        {
            g_moreHUD = dataHandler->LookupModByName("AHZmoreHUD.esp") != nullptr ||
                        dataHandler->LookupModByName("AHZmoreHUD.esl") != nullptr;
        }
    }
    logger::info("HudCompat: moreHUD {}", g_moreHUD ? "detected" : "not present");
}

bool HasTrueHUD()
{
    return g_trueHUD != nullptr;
}

bool HasMoreHUD()
{
    return g_moreHUD;
}

bool TrueHUDShowsBarFor(RE::Actor* actor)
{
    if (!g_trueHUD || !actor)
    {
        return false;
    }
    // Floating-only: bars anchored over the actor's head are the ones a
    // nameplate would stack against; the docked boss bar lives elsewhere.
    return g_trueHUD->HasInfoBar(actor->GetHandle(), true);
}

std::uint32_t CrosshairTargetFormID()
{
    auto* pick = RE::CrosshairPickData::GetSingleton();
    if (!pick)
    {
        return 0;
    }
    const auto refr = pick->target.get();
    return refr ? refr->GetFormID() : 0;
}
}  // namespace HudCompat
