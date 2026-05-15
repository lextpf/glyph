#pragma once

#include <array>
#include <string>
#include <string_view>
#include <variant>

/**
 * @brief Declarative descriptor table for Settings scalars.
 *
 * Each SettingEntry describes a single INI key: its canonical name,
 * optional alias, pointer to the backing variable, default value,
 * and validation rule.  The table is walked by ResetToDefaults(),
 * ClampAndValidate(), and the Load() INI parser, eliminating the
 * previous 5-way redundancy.
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

struct ClampFloat
{
    float lo, hi;
};
struct MinFloat
{
    float lo;
};
struct ClampInt
{
    int lo, hi;
};
struct MinInt
{
    int lo;
};
struct NoClamping
{
};

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
