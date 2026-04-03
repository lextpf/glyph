#pragma once

#include "AppearanceTemplate.h"
#include "PCH.h"
#include "Settings.h"

#include <SKSE/SKSE.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

namespace AppearanceTemplate
{
/**
 * @brief Internal state and helpers for the AppearanceTemplate module.
 *
 * ## Thread Safety
 *
 * - `GetState()`, `IsAppliedState()`, `SetAppliedState()`, `SetTemplateStateInfo()`:
 *   All acquire `GetStateMutex()` internally; safe to call from any thread.
 * - `s_applyInProgress`: Atomic; used as a CAS guard by `ApplyIfConfigured()`.
 * - `ApplyConfig`: Immutable after construction; no synchronization needed.
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
 * @brief Immutable snapshot of appearance settings.
 *
 * Captured at the start of ApplyIfConfigured() so the apply operation
 * does not re-read Settings mid-flight.
 */
struct ApplyConfig
{
    bool useTemplateAppearance = false;
    std::string templateFormID;
    std::string templatePlugin;
    bool templateIncludeRace = false;
    bool templateIncludeBody = false;
    bool templateCopyFaceGen = false;
    bool templateCopyOutfit = false;
};

}  // namespace AppearanceTemplate
