#include "PCH.h"

#include "GameState.h"

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
        if (ui->IsMenuOpen("Loading Menu") ||
            ui->IsMenuOpen("Main Menu") ||
            ui->IsMenuOpen("MapMenu") ||
            ui->IsMenuOpen("Fader Menu") ||
            ui->IsMenuOpen("Menu") ||
            ui->IsMenuOpen("Console") ||
            ui->IsMenuOpen("TweenMenu") ||
            ui->IsMenuOpen("Journal Menu") ||
            ui->IsMenuOpen("InventoryMenu") ||
            ui->IsMenuOpen("MagicMenu") ||
            ui->IsMenuOpen("ContainerMenu") ||
            ui->IsMenuOpen("BarterMenu") ||
            ui->IsMenuOpen("GiftMenu") ||
            ui->IsMenuOpen("Crafting Menu") ||
            ui->IsMenuOpen("FavoritesMenu") ||
            ui->IsMenuOpen("Lockpicking Menu") ||
            ui->IsMenuOpen("Sleep/Wait Menu") ||
            ui->IsMenuOpen("StatsMenu"))
        {
            return false;
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
