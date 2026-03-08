#pragma once

#include <array>
#include <string>
#include <string_view>
#include <variant>

/**
 * @namespace Settings
 * @brief Declarative descriptor table for Settings scalars.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Core
 *
 * Each `SettingEntry` describes a single INI key: its canonical name,
 * optional alias, pointer to the backing variable, default value, and
 * validation rule. The table (`kSettings`, defined in Settings.cpp) is
 * walked once by `ResetToDefaults()`, `ClampAndValidate()`, and the INI
 * parser inside `Load()`, replacing the previous 5-way redundancy where
 * every scalar had to be repeated in five different functions.
 *
 * ## :material-cog-outline: Validation Rules
 *
 * |       Rule | Effect                                          |
 * |------------|-------------------------------------------------|
 * | ClampFloat | Clamp to `[lo, hi]` after parse                 |
 * |   MinFloat | Clamp to `>= lo`, no upper bound                |
 * |   ClampInt | Clamp to `[lo, hi]` after parse                 |
 * |     MinInt | Clamp to `>= lo`, no upper bound                |
 * | NoClamping | Accept the raw parsed value as-is               |
 *
 * ## :material-code-tags: Example Entry
 *
 * ```cpp
 * SettingEntry{
 *     "FadeStartDistance",            // canonical key
 *     "",                             // no alias
 *     &Distance().fadeStartDistance,  // backing variable
 *     2000.0f,                        // default
 *     MinFloat{0.0f},                 // validation
 * }
 * ```
 *
 * @see Settings::Load, Settings::ResetToDefaults
 */

namespace Settings
{

/// Aggregate lambda overload helper (C++17 deduction guide).
template <class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};

// --- Validation rules -------------------------------------------------------

/// Clamp parsed value to a closed interval [lo, hi].
struct ClampFloat
{
    float lo;  ///< Inclusive lower bound.
    float hi;  ///< Inclusive upper bound.
};

/// Clamp parsed value to be at least @c lo; no upper bound.
struct MinFloat
{
    float lo;  ///< Inclusive lower bound.
};

/// Clamp parsed value to a closed interval [lo, hi].
struct ClampInt
{
    int lo;  ///< Inclusive lower bound.
    int hi;  ///< Inclusive upper bound.
};

/// Clamp parsed value to be at least @c lo; no upper bound.
struct MinInt
{
    int lo;  ///< Inclusive lower bound.
};

/// Accept the parsed value unchanged.
struct NoClamping
{
};

/// Tag list of all supported validation rules.
using Validation = std::variant<ClampFloat, MinFloat, ClampInt, MinInt, NoClamping>;

// --- Setting entry ----------------------------------------------------------

struct SettingEntry
{
    std::string_view key;    ///< Canonical key name (e.g. "FadeStartDistance")
    std::string_view alias;  ///< Optional alias (empty = none)
    std::variant<float*, bool*, int*, std::string*> target;
    std::variant<float, bool, int, std::string> defaultValue;
    Validation validation;
};

}  // namespace Settings
