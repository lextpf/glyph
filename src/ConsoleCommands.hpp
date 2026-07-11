#pragma once

/**
 * @namespace ConsoleCommands
 * @brief Session-only glyph console commands in the TestSeenData slot.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Core
 *
 * Sub-commands accept any nonempty prefix; `clear` and its `all` argument require full
 * spelling. Actor commands target the console selection, or the player if none is selected.
 * Multi-word titles need no quotes. Icon INI gates and the title's `%t` format gate still
 * apply. No command writes glyph.ini; toggles and overrides reset on restart.
 *
 * Toggle arguments accept on/1/true/yes and off/0/false/no; other or absent arguments toggle.
 * `ActorOverrides` defines title and icon grammar and state vocabulary.
 *
 * @code{.text}
 * > glyph help
 * glyph - sub-commands (any unambiguous prefix works, e.g. n / d / t / i;
 *   'clear' must be typed in full):
 *   glyph                              Toggle nameplate rendering
 *   glyph help | ?                     Show this help
 *   glyph status | s                   Print current state
 *   glyph nameplates | n [on|off]      Enable / disable / toggle nameplates
 *   glyph plates | p [on|off]          Alias for 'nameplates'
 *   glyph debug | d [on|off]           Enable / disable / toggle debug overlay
 *   glyph title | t [text|hide|auto]   Custom title for the selected actor (or you)
 *   glyph icon | i <slot> <state>      Force a badge slot; state, hide or auto
 *   glyph icon | i <slot> <state> <i>  Same, drawing duotone icon <i> in the slot
 *   glyph icon add <icon> [r,g,b]      Append an extra badge (at most 4)
 *   glyph icon remove <icon>           Remove an extra badge
 *   glyph icon clear                   Remove the actor's icon overrides
 *   glyph clear [all]                  Remove the actor's (or every) override
 *
 * > glyph title Thane of Whiterun
 * glyph: Lydia: title "Thane of Whiterun"
 * > glyph icon relationship ally
 * glyph: Lydia: relationship=ally (INI gates still apply)
 * @endcode
 */
namespace ConsoleCommands
{
/**
 * @fn void Register()
 * @brief Install the dispatcher after the engine populates its command table.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @pre Call at `kDataLoaded`; the dispatcher runs on the game thread.
 * @post Nameplate commands write renderer atomics. Debug writes under `Settings::Mutex()`
 * and releases a new generation. Actor commands publish `ActorOverrides` records for
 * snapshots; the render thread drains dirty actors and observes new icon names.
 * @note Missing command table, existing `glyph`, or missing `TestSeenData` logs and skips
 * registration. Repeated calls are harmless.
 */
void Register();
}  // namespace ConsoleCommands
