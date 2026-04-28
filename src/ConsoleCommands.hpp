#pragma once

/**
 * @namespace ConsoleCommands
 * @brief Skyrim console command handlers for the glyph plugin.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Core
 *
 * Replaces the engine's unused `TestSeenData` console-command slot with a
 * `glyph` dispatcher that exposes multiple sub-commands. The raw command
 * text is read from the `Script*` parameter via `Script::GetCommand()` and
 * tokenized in-process.
 *
 * | Sub-command                              | Effect                                      |
 * |------------------------------------------|---------------------------------------------|
 * | `glyph`                                  | Toggle nameplate rendering (legacy default) |
 * | `glyph help`                             | Print available sub-commands                |
 * | `glyph status`                           | Print current state of every toggle         |
 * | `glyph nameplates [on\|off]`             | Set / toggle nameplate rendering            |
 * | `glyph plates [on\|off]`                 | Alias for `nameplates`                      |
 * | `glyph debug [on\|off]`                  | Set / toggle debug overlay                  |
 *
 * ## :material-console: Example Session
 *
 * ```text
 * > glyph help
 * glyph                          - toggle nameplate rendering
 * glyph status                   - print current state
 * glyph nameplates on|off        - set nameplate rendering
 * glyph debug on|off             - set debug overlay
 *
 * > glyph debug on
 * debug overlay: ON
 * ```
 */
namespace ConsoleCommands
{
/**
 * @brief Register the `glyph` console command dispatcher.
 *
 * Replaces the unused `TestSeenData` slot in the SCRIPT_FUNCTION table
 * with the `glyph` command and an empty parameter list.
 *
 * @pre Must be called once during SKSE plugin load, after the message
 *      interface is available (see `main.cpp`'s kInputLoaded handler).
 * @post `glyph` and its sub-commands are available at the in-game console.
 * @note If a future game patch removes the `TestSeenData` slot, this
 *       registration becomes a no-op and is logged as a warning.
 */
void Register();
}  // namespace ConsoleCommands
