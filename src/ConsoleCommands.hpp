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
 * tokenized in-process; the SCRIPT_PARAMETER table is left empty so the
 * console does not auto-resolve hex tokens like `0xD62` as form references.
 *
 * | Sub-command                              | Effect                                      |
 * |------------------------------------------|---------------------------------------------|
 * | `glyph`                                  | Toggle nameplate rendering (legacy default) |
 * | `glyph help`                             | Print available sub-commands                |
 * | `glyph status`                           | Print current state of every toggle         |
 * | `glyph nameplates [on\|off]`             | Set / toggle nameplate rendering            |
 * | `glyph plates [on\|off]`                 | Alias for `nameplates`                      |
 * | `glyph debug [on\|off]`                  | Set / toggle debug overlay                  |
 * | `glyph appearance`                       | Print appearance-template state             |
 * | `glyph appearance on`                    | Apply template using `[Appearance]` target  |
 * | `glyph appearance on <formid> <plugin>`  | Apply template with explicit target         |
 * | `glyph appearance off`                   | Clear applied flag (see revert caveat)      |
 *
 * ## :material-console: Example Session
 *
 * ```text
 * > glyph help
 * glyph                          - toggle nameplate rendering
 * glyph status                   - print current state
 * glyph nameplates on|off        - set nameplate rendering
 * glyph debug on|off             - set debug overlay
 * glyph appearance               - print template state
 * glyph appearance on            - apply [Appearance] target
 * glyph appearance on <hex> <esp>- apply explicit target
 * glyph appearance off           - clear applied flag
 *
 * > glyph appearance on 0xD62 Inigo.esp
 * appearance: applied Inigo.esp:0x00000D62
 *
 * > glyph debug on
 * debug overlay: ON
 * ```
 *
 * @note In-session revert is not supported: template application mutates the
 * player's TESNPC record and loads new FaceGen meshes; to see your original
 * appearance again, reload your save without saving while the template is
 * applied.
 */
namespace ConsoleCommands
{
/**
 * @brief Register the `glyph` console command dispatcher.
 *
 * Replaces the unused `TestSeenData` slot in the SCRIPT_FUNCTION table
 * with the `glyph` command and an empty parameter list (parameters are
 * tokenized in-process so the console does not eagerly resolve hex
 * tokens like `0xD62` to form references).
 *
 * @pre Must be called once during SKSE plugin load, after the message
 *      interface is available (see `main.cpp`'s kInputLoaded handler).
 * @post `glyph` and its sub-commands are available at the in-game console.
 * @note If a future game patch removes the `TestSeenData` slot, this
 *       registration becomes a no-op and is logged as a warning.
 */
void Register();
}  // namespace ConsoleCommands
