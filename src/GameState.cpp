#include "GameState.hpp"

#include "PCH.hpp"

namespace GameState
{
namespace
{
/**
 * @fn bool IsWorldReady()
 * @brief Check the game-thread world and menu gates shared by plates and captures.
 * @author Alex (<https://github.com/lextpf>)
 */
bool IsWorldReady()
{
    auto* main = RE::Main::GetSingleton();
    if (!main)
    {
        return false;
    }

    if (!main->gameActive)
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
            "RaceSex Menu",
            "MessageBoxMenu",
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

    return true;
}
}  // namespace

bool CanCaptureDeck()
{
    return IsWorldReady();
}

bool CanDrawOverlay()
{
    if (!IsWorldReady())
    {
        return false;
    }

    // Hide labels in combat to avoid revealing enemy positions; card captures remain available.
    const auto* player = RE::PlayerCharacter::GetSingleton();
    return player && !player->IsInCombat();
}
}  // namespace GameState
