#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
 * @namespace Utf8Utils
 * @brief Validated UTF-8 string utilities.
 *
 * Shared helpers for iterating, counting, and truncating UTF-8 text.
 * All functions gracefully handle malformed sequences by substituting
 * U+FFFD or consuming a single byte.
 */
namespace Utf8Utils
{
/// Check if a byte is a UTF-8 continuation byte (10xxxxxx).
inline bool IsUtf8Continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/// Get byte length of the UTF-8 character at @p s.
///
/// Returns 1 for invalid lead bytes, overlong encodings,
/// surrogate halves, and codepoints above U+10FFFF.
inline size_t Utf8CharLen(const char* s)
{
    if (!s || !*s)
    {
        return 0;
    }
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80)
    {
        return 1;
    }

    // Invalid lead byte (continuation/overlong starter), consume one byte.
    if (c0 < 0xC2)
    {
        return 1;
    }

    if (c0 < 0xE0)
    {
        if (!s[1])
        {
            return 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        return IsUtf8Continuation(c1) ? 2 : 1;
    }

    if (c0 < 0xF0)
    {
        if (!s[1] || !s[2])
        {
            return 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        if (!IsUtf8Continuation(c1) || !IsUtf8Continuation(c2))
        {
            return 1;
        }
        // Reject overlong + surrogate encodings.
        if ((c0 == 0xE0 && c1 < 0xA0) || (c0 == 0xED && c1 >= 0xA0))
        {
            return 1;
        }
        return 3;
    }

    if (c0 < 0xF5)
    {
        if (!s[1] || !s[2] || !s[3])
        {
            return 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        const unsigned char c3 = static_cast<unsigned char>(s[3]);
        if (!IsUtf8Continuation(c1) || !IsUtf8Continuation(c2) || !IsUtf8Continuation(c3))
        {
            return 1;
        }
        // Reject overlong + values above U+10FFFF.
        if ((c0 == 0xF0 && c1 < 0x90) || (c0 == 0xF4 && c1 >= 0x90))
        {
            return 1;
        }
        return 4;
    }

    return 1;
}

/// Decode one UTF-8 character and advance.
///
/// This is the core iterator helper used by counting/truncation code:
/// pass a byte pointer @p s, get the decoded Unicode codepoint in @p out,
/// and receive the next byte position as the return value.
///
/// Contract:
/// - If @p s is null or points at '\\0', returns @p s unchanged and sets @p out to 0.
/// - For valid UTF-8, @p out is the decoded codepoint and the return value is @p s + 1/2/3/4.
/// - For invalid or truncated UTF-8, @p out is set to U+FFFD and one byte is consumed
///   (return @p s + 1) so callers always make forward progress.
inline const char* Utf8Next(const char* s, unsigned int& out)
{
    out = 0;
    if (!s || !*s)
    {
        return s;
    }

    const unsigned char c = static_cast<unsigned char>(s[0]);

    // Single-byte ASCII character (0x00-0x7F)
    if (c < 0x80)
    {
        out = c;
        return s + 1;
    }

    // Reject continuation bytes (0x80-0xBF) appearing as start bytes
    if (c < 0xC0)
    {
        out = 0xFFFD;
        return s + 1;
    }

    // Reject overlong 2-byte starters (0xC0-0xC1 encode 0x00-0x7F)
    if (c < 0xC2)
    {
        out = 0xFFFD;
        return s + 1;
    }

    // 2-byte sequence (0xC2-0xDF): 110xxxxx 10xxxxxx
    if (c < 0xE0)
    {
        if (!s[1])
        {
            out = 0xFFFD;
            return s + 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        if ((c1 & 0xC0) != 0x80)
        {
            out = 0xFFFD;
            return s + 1;
        }
        out = ((c & 0x1F) << 6) | (c1 & 0x3F);
        return s + 2;
    }

    // 3-byte sequence (0xE0-0xEF): 1110xxxx 10xxxxxx 10xxxxxx
    if (c < 0xF0)
    {
        if (!s[1] || !s[2])
        {
            out = 0xFFFD;
            return s + 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80)
        {
            out = 0xFFFD;
            return s + 1;
        }
        out = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
        // Reject overlong (< 0x800) and surrogate halves (U+D800-U+DFFF)
        if (out < 0x800 || (out >= 0xD800 && out <= 0xDFFF))
        {
            out = 0xFFFD;
        }
        return s + 3;
    }

    // 4-byte sequence (0xF0-0xF7): 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
    if (c < 0xF8)
    {
        if (!s[1] || !s[2] || !s[3])
        {
            out = 0xFFFD;
            return s + 1;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[1]);
        const unsigned char c2 = static_cast<unsigned char>(s[2]);
        const unsigned char c3 = static_cast<unsigned char>(s[3]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
        {
            out = 0xFFFD;
            return s + 1;
        }
        out = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        // Reject overlong (< 0x10000) and codepoints above U+10FFFF
        if (out < 0x10000 || out > 0x10FFFF)
        {
            out = 0xFFFD;
        }
        return s + 4;
    }

    // Invalid UTF-8 sequence
    out = 0xFFFD;
    return s + 1;
}

/// Count the number of UTF-8 codepoints in a null-terminated string.
inline size_t Utf8CharCount(const char* s)
{
    size_t count = 0;
    if (!s)
    {
        return 0;
    }

    while (*s)
    {
        unsigned int cp = 0;
        const char* next = Utf8Next(s, cp);
        if (!next || next <= s)
        {
            ++s;
            continue;
        }
        s = next;
        count++;
    }
    return count;
}

/// Truncate a UTF-8 string to at most @p maxChars codepoints.
inline std::string Utf8Truncate(const char* s, size_t maxChars)
{
    if (!s || maxChars == 0)
    {
        return "";
    }

    const char* start = s;
    size_t count = 0;

    while (*s && count < maxChars)
    {
        unsigned int cp = 0;
        const char* next = Utf8Next(s, cp);
        if (!next || next <= s)
        {
            ++s;
            continue;
        }
        s = next;
        count++;
    }

    return std::string(start, s - start);
}

/// Split a UTF-8 string into a vector of individual characters.
inline std::vector<std::string> Utf8ToChars(const std::string& str)
{
    std::vector<std::string> chars;
    const char* s = str.c_str();
    while (*s)
    {
        size_t len = Utf8CharLen(s);
        if (len == 0)
        {
            break;
        }
        chars.emplace_back(s, len);
        s += len;
    }
    return chars;
}

}  // namespace Utf8Utils
