#pragma once

#include "AppearanceTemplate.hpp"
#include "PCH.hpp"
#include "Settings.hpp"

#include <SKSE/SKSE.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

namespace AppearanceTemplate
{
/**
 * @namespace AppearanceTemplate
 * @brief Internal state and helpers for the AppearanceTemplate module.
 * @author Alex (https://github.com/lextpf)
 * @ingroup AppearanceTemplate
 *
 * Companion to AppearanceTemplate.hpp. The public header defines what
 * the module does; this header defines the shared mutable state and
 * the synchronization contract that the .cpp files agree to.
 *
 * ## :material-shield-lock: Thread Safety Contract
 *
 * | Item                           | Synchronization                        |
 * |--------------------------------|----------------------------------------|
 * | GetState()                     | Caller must hold GetStateMutex()       |
 * | IsAppliedState() (and setters) | Acquires GetStateMutex() internally    |
 * | s_applyInProgress              | Atomic CAS guard, no mutex needed      |
 * | ApplyConfig                    | Immutable snapshot, copy-by-value safe |
 *
 * ## :material-code-tags: Reading State Safely
 *
 * ```cpp
 * // From any thread, when you need both fields atomically:
 * std::lock_guard lock(AppearanceTemplate::GetStateMutex());
 * const auto& st = AppearanceTemplate::GetState();
 * if (st.applied)
 * {
 *     logger::info("template: {}:{:08X}", st.plugin, st.formID);
 * }
 * ```
 *
 * Use the convenience accessors (`IsAppliedState()` etc.) for single-field
 * reads -- they take the lock internally.
 */

/**
 * @brief Tracks whether a template appearance has been applied.
 *
 * Stores which plugin/formID was used. Guarded by GetStateMutex().
 */
struct TemplateState
{
    bool applied = false;   ///< Whether appearance was successfully applied
    std::string plugin;     ///< Plugin filename that provided the template
    RE::FormID formID = 0;  ///< FormID of the template NPC
};

/**
 * @brief Returns the singleton TemplateState.
 *
 * Access must be guarded by GetStateMutex().
 */
TemplateState& GetState();

/**
 * @brief Returns the mutex protecting TemplateState reads and writes.
 */
std::mutex& GetStateMutex();

/**
 * @brief Thread-safe check of TemplateState::applied.
 *
 * Acquires GetStateMutex() internally.
 *
 * @return Current value of the applied flag.
 */
bool IsAppliedState();

/**
 * @brief Thread-safe setter for TemplateState::applied.
 *
 * @param value New applied state.
 */
void SetAppliedState(bool value);

/**
 * @brief Thread-safe setter for the plugin and formID fields.
 *
 * @param plugin Plugin filename that provided the template.
 * @param formID FormID of the template NPC.
 */
void SetTemplateStateInfo(const std::string& plugin, RE::FormID formID);

/**
 * @brief Atomic guard preventing concurrent ApplyIfConfigured() calls.
 *
 * True while an apply operation is in flight.
 */
extern std::atomic<bool> s_applyInProgress;

/**
 * @brief Immutable snapshot of `[Appearance]` settings.
 *
 * Captured at the start of ApplyIfConfigured() so the apply operation
 * does not re-read Settings mid-flight (avoids partial reads during
 * hot reload).
 */
struct ApplyConfig
{
    bool useTemplateAppearance = false;  ///< `UseTemplateAppearance` flag.
    std::string templateFormID;        ///< Hex string; resolved to RE::FormID after plugin lookup.
    std::string templatePlugin;        ///< Plugin filename (e.g. "Inigo.esp").
    bool templateIncludeRace = false;  ///< Copy template race (required for cross-race templates).
    bool templateIncludeBody = false;  ///< Copy height in addition to face.
    bool templateCopyFaceGen = false;  ///< Copy FaceGen NIF/tint files from disk.
    bool templateCopyOutfit = false;   ///< Equip the template's worn armor on the player.
};

}  // namespace AppearanceTemplate
