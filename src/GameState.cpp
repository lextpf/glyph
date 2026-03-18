#include "GameState.h"

#include "PCH.h"

namespace GameState
{
bool CanDrawOverlay()
{
    auto* main = RE::Main::GetSingleton();
    if (!main)
    {
        return false;
    }

    // "Loading is basically still happening" window
    if (!main->gameActive || main->freezeTime || main->freezeNextFrame || main->fullReset ||
        main->resetGame || main->reloadContent)
    {
        return false;
    }

    if (auto ui = RE::UI::GetSingleton())
    {
        static constexpr const char* SUPPRESSED_MENUS[] = {
            "Loading Menu",
            "Main Menu",
            "MapMenu",
            "Fader Menu",
            "Menu",
            "Console",
            "TweenMenu",
            "Journal Menu",
            "InventoryMenu",
            "MagicMenu",
            "ContainerMenu",
            "BarterMenu",
            "GiftMenu",
            "Crafting Menu",
            "FavoritesMenu",
            "Lockpicking Menu",
            "Sleep/Wait Menu",
            "StatsMenu",
        };
        for (const auto* menu : SUPPRESSED_MENUS)
        {
            if (ui->IsMenuOpen(menu))
            {
                return false;
            }
        }
    }

    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell() || !player->GetParentCell()->IsAttached())
    {
        return false;
    }

    // Unconditionally hide during combat - floating names above enemies would
    // reveal hidden NPCs and clutter the screen during action sequences.
    if (player->IsInCombat())
    {
        return false;
    }

    return true;
}
}  // namespace GameState
