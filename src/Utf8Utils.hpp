#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
 * @namespace Utf8Utils
 * @brief UTF-8 iteration with defined malformed-input recovery.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 *
 * Do not mix recovery families on malformed text: a surrogate half counts as three
 * characters for Utf8CharLen, one for Utf8Next. Counts refer to decoded units, not visible
 * grapheme clusters; combining marks count separately.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart TD
 *     I[Read the next input bytes] --> K{Sequence class}
 *     K -- Valid scalar value --> V[Both families consume the complete sequence]
 *     V --> VR[Utf8CharLen and Utf8ToChars<br/>keep the encoded bytes]
 *     V --> VN[Utf8Next decodes the scalar;<br/>count and truncate advance with it]
 *     K -- Malformed byte structure --> M[Both families advance one byte]
 *     M --> MR[Utf8CharLen and Utf8ToChars<br/>keep that raw byte]
 *     M --> MN[Utf8Next yields U+FFFD;<br/>count and truncate advance one byte]
 *     K -- Invalid scalar value --> C[Utf8CharLen and Utf8ToChars<br/>keep one raw byte]
 *     K -- Invalid scalar value --> N[Next family consumes all bytes;<br/>Utf8Next yields U+FFFD]
 *     MN -.-> T[Utf8Truncate copies the original prefix;<br/>it never writes U+FFFD]
 *     N -.-> T
 * ```
 */
namespace Utf8Utils
{
/**
 * @fn bool IsUtf8Continuation(unsigned char c)
 * @brief Test the UTF-8 continuation-byte pattern.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Test the UTF-8 continuation-byte pattern 10xxxxxx.
 */
inline bool IsUtf8Continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/**
 * @fn size_t Utf8CharLen(const char* s)
 * @brief Measure one UTF-8 character with byte-wise error recovery.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Invalid leads, overlong encodings, surrogates and values above U+10FFFF consume one byte.
 *
 * @param s Null-terminated input; may be null.
 * @return 1..4 bytes, or zero at null/terminator. Zero means stop.
 */
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

/**
 * @fn const char* Utf8Next(const char* s, unsigned int& out)
 * @brief Decode one character with sequence-wise scalar-error recovery.
 * @author Alex (<https://github.com/lextpf>)
 *
 * | Input                       | Output | Advance        |
 * |-----------------------------|--------|----------------|
 * | Null or terminator          | 0      | None           |
 * | Valid scalar                | Scalar | Whole sequence |
 * | Malformed structure         | U+FFFD | One byte       |
 * | Invalid scalar in 3/4 bytes | U+FFFD | Whole sequence |
 *
 * Invalid two-byte leads consume one byte. Every nonterminal path advances.
 *
 * @param s Null-terminated input; may be null.
 * @param out Decoded value or replacement character.
 * @return Next input pointer; unchanged at null/terminator.
 */
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

/**
 * @fn size_t Utf8CharCount(const char* s)
 * @brief Count with Utf8Next recovery rules; null returns zero.
 * @author Alex (<https://github.com/lextpf>)
 */
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

/**
 * @fn std::string Utf8Truncate(const char* s, size_t maxChars)
 * @brief Keep at most maxChars codepoints using Utf8Next recovery.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Copies the byte-exact prefix without repairing malformed input.
 *
 * @param s Null-terminated input; may be null.
 * @return Empty when s is null or maxChars is zero.
 */
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

/**
 * @fn std::vector<std::string> Utf8ToChars(const std::string& str)
 * @brief Split encoded characters with Utf8CharLen recovery.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Malformed bytes remain unchanged and separate. Stops at an embedded null.
 */
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
