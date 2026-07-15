#pragma once

#include <array>
#include <string>
#include <string_view>
#include <variant>

/**
 * @namespace Settings
 * @brief Descriptor table for scalar defaults, parsing, and validation.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Core
 *
 * `Load()` resets `kSettings` defaults, parses values, then validates once after the file.
 * Keys and aliases match case-insensitively in any section; a duplicate keeps the last row.
 * Indexed fields and `Format` / `InfoFormat` take precedence, as shown below.
 *
 * Invalid numbers become zero before clamping. Bools accept true, 1, yes, on, or enabled
 * case-insensitively; other text is false. Strings retain quotes after trimming and
 * comment removal. Targets require the unique `Settings::Mutex()` lock held by `Load()`.
 *
 * Float rules apply only to `float*`; integer rules apply only to `int*`. Mismatches and
 * rules on bool/string targets are silently ignored; use `NoClamping` for those targets.
 * Clamp rules use inclusive bounds; min rules have no upper bound.
 *
 * ```mermaid
 * flowchart TD
 *     K[One INI key = value line] --> A{Indexed section active<br/>and field name known?}
 *     A -- Yes --> S[Section parser writes the indexed entry]
 *     A -- No --> F{Key is Format or InfoFormat?}
 *     F -- Yes --> P[Quoted-segment parser]
 *     F -- No --> M{Lowercased key in the kSettings map?}
 *     M -- Yes --> T[ApplySettingValue writes the row target]
 *     M -- No --> W[Counted as an unknown key and warned]
 * ```
 *
 * @code{.cpp}
 * SettingEntry{
 *     "FadeStartDistance",            // Canonical key
 *     "",                             // No alias
 *     &Distance().FadeStartDistance,  // Backing variable
 *     200.0f,                         // Default
 *     MinFloat{.0f},                  // Validation
 * }
 * @endcode
 */

namespace Settings
{

/**
 * @struct overloaded
 * @brief Callable overload set for variant dispatch.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @tparam Ts Callable base types.
 */
template <class... Ts>
struct overloaded : Ts...
{
    using Ts::operator()...;
};

/**
 * @struct ClampFloat
 * @brief Clamp a parsed float to the closed interval from lo to hi.
 * @author Alex (<https://github.com/lextpf>)
 */
struct ClampFloat
{
    float lo;  ///< Inclusive lower bound.
    float hi;  ///< Inclusive upper bound.
};

/**
 * @struct MinFloat
 * @brief Float lower bound with no upper limit.
 * @author Alex (<https://github.com/lextpf>)
 */
struct MinFloat
{
    float lo;  ///< Inclusive lower bound.
};

/**
 * @struct ClampInt
 * @brief Clamp a parsed integer to the closed interval from lo to hi.
 * @author Alex (<https://github.com/lextpf>)
 */
struct ClampInt
{
    int lo;  ///< Inclusive lower bound.
    int hi;  ///< Inclusive upper bound.
};

/**
 * @struct MinInt
 * @brief Integer lower bound with no upper limit.
 * @author Alex (<https://github.com/lextpf>)
 */
struct MinInt
{
    int lo;  ///< Inclusive lower bound.
};

/**
 * @struct NoClamping
 * @brief Accept a parsed value without numeric clamping.
 * @author Alex (<https://github.com/lextpf>)
 */
struct NoClamping
{
};

/// Variant of all supported validation rules.
using Validation = std::variant<ClampFloat, MinFloat, ClampInt, MinInt, NoClamping>;

/**
 * @struct SettingEntry
 * @brief One scalar binding in kSettings.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Core
 *
 * `key` and `alias` must outlive the table.
 * @warning `defaultValue` must match the target's pointee type: use `0.0f` for a float
 * and `std::string("x")` for a string. A mismatch throws uncaught `std::bad_variant_access`
 * during the first default reset and aborts plugin loading.
 */
struct SettingEntry
{
    std::string_view key;  ///< Canonical key name; INI lookup is case-insensitive
    /// Equivalent key; empty means no alias.
    std::string_view alias;
    std::variant<float*, bool*, int*, std::string*> target;    ///< Backing variable
    std::variant<float, bool, int, std::string> defaultValue;  ///< Applied by ResetToDefaults()
    /// Applied after parsing; ignored if target and rule types do not match.
    Validation validation;
};

}  // namespace Settings
