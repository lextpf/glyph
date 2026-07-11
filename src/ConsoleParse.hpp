#pragma once

#include "Settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @namespace ConsoleParse
 * @brief Plain-value parsing for console commands and direct tests.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 *
 * Character classification and case conversion use unsigned bytes and the current C locale.
 * These helpers do not decode Unicode. Returned strings own their storage.
 */
namespace ConsoleParse
{
/**
 * @fn std::vector<std::string> Tokenize(std::string_view text)
 * @brief Split nonempty tokens on whitespace.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Quotes are ordinary bytes; use `RestAfterTokens` for multi-word arguments.
 */
inline std::vector<std::string> Tokenize(std::string_view text)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0)
        {
            ++i;
        }
        if (i >= text.size())
        {
            break;
        }
        const std::size_t start = i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0)
        {
            ++i;
        }
        tokens.emplace_back(text.substr(start, i - start));
    }
    return tokens;
}

/**
 * @fn std::string ToLowerAscii(std::string s)
 * @brief Fold letter case byte by byte with the current C locale.
 * @author Alex (<https://github.com/lextpf>)
 */
inline std::string ToLowerAscii(std::string s)
{
    std::ranges::transform(
        s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/**
 * @fn bool IsPrefixOf(std::string_view typed, std::string_view full)
 * @brief Match a nonempty leading prefix of a command word.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The comparison is case-sensitive; fold command tokens before this call.
 */
inline bool IsPrefixOf(std::string_view typed, std::string_view full)
{
    return !typed.empty() && full.starts_with(typed);
}

/**
 * @enum TriState
 * @brief Result of parsing an on/off/toggle argument.
 * @author Alex (<https://github.com/lextpf>)
 */
enum class TriState : std::uint8_t
{
    On,     ///< `on`, `1`, `true` or `yes`
    Off,    ///< `off`, `0`, `false` or `no`
    Toggle  ///< Any other word, or no word
};

/**
 * @fn TriState ParseTriState(const std::string& arg)
 * @brief Parse an on/off/toggle argument; the comparison is case-insensitive.
 * @author Alex (<https://github.com/lextpf>)
 */
inline TriState ParseTriState(const std::string& arg)
{
    const auto lower = ToLowerAscii(arg);
    if (lower == "on" || lower == "1" || lower == "true" || lower == "yes")
    {
        return TriState::On;
    }
    if (lower == "off" || lower == "0" || lower == "false" || lower == "no")
    {
        return TriState::Off;
    }
    return TriState::Toggle;
}

/**
 * @fn std::string Trim(std::string text)
 * @brief Remove leading and trailing whitespace while keeping interior spacing.
 * @author Alex (<https://github.com/lextpf>)
 */
inline std::string Trim(std::string text)
{
    std::size_t end = text.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
    {
        --end;
    }
    std::size_t start = 0;
    while (start < end && std::isspace(static_cast<unsigned char>(text[start])) != 0)
    {
        ++start;
    }
    return text.substr(start, end - start);
}

/**
 * @fn std::string RestAfterTokens(std::string_view line, std::size_t count)
 * @brief Skip leading tokens and trim the remaining text.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Interior spacing and quotes are preserved in the returned copy.
 * @return Empty when the line has at most `count` tokens.
 */
inline std::string RestAfterTokens(std::string_view line, std::size_t count)
{
    std::size_t i = 0;
    for (std::size_t skipped = 0; skipped < count; ++skipped)
    {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) != 0)
        {
            ++i;
        }
        if (i >= line.size())
        {
            return "";
        }
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])) == 0)
        {
            ++i;
        }
    }
    return Trim(std::string(line.substr(i)));
}

/**
 * @fn std::string StripSurroundingQuotes(std::string text)
 * @brief Strip one matched outer quote pair; leave other quotes unchanged.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Only double quotes match. This helper does not trim whitespace or decode escapes.
 */
inline std::string StripSurroundingQuotes(std::string text)
{
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

/**
 * @fn std::optional<Settings::Color3> ParseColorTriplet(std::string_view text)
 * @brief Parse three finite RGB channels, ignoring whitespace.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Require exactly three comma-separated numbers and consume each complete number.
 * Remove whitespace before parsing, including whitespace inside a number.
 *
 * @return Color clamped to [0, 1], or empty for any other input shape.
 */
inline std::optional<Settings::Color3> ParseColorTriplet(std::string_view text)
{
    std::string compact;
    compact.reserve(text.size());
    for (const char c : text)
    {
        if (std::isspace(static_cast<unsigned char>(c)) == 0)
        {
            compact.push_back(c);
        }
    }

    float channels[3] = {};
    std::size_t index = 0;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t comma = compact.find(',', start);
        const std::string part = compact.substr(start, comma - start);
        if (index >= 3 || part.empty())
        {
            return std::nullopt;
        }
        char* end = nullptr;
        const float value = std::strtof(part.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(value))
        {
            return std::nullopt;
        }
        channels[index++] = value;
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    if (index != 3)
    {
        return std::nullopt;
    }
    return Settings::Color3(channels[0], channels[1], channels[2]).clamp01();
}

/**
 * @fn bool IsSafeIconName(std::string_view name)
 * @brief Accept a nonempty icon name without path syntax.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Accept alphanumeric bytes in the current C locale, plus `-` and `_`.
 * This checks the name syntax only; the caller checks whether the icon file exists.
 */
inline bool IsSafeIconName(std::string_view name)
{
    if (name.empty())
    {
        return false;
    }
    return std::ranges::all_of(
        name, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_'; });
}
}  // namespace ConsoleParse
