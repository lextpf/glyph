#pragma once

#include <cstdint>

/**
 * @namespace RasterQuality
 * @brief Source raster quality independent of geometry and backbuffer size.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Changing font density requires a full atlas rebuild.
 */
namespace RasterQuality
{
inline constexpr float FONT_DENSITY = 1.5f;  ///< Preferred source density for all fonts.
inline constexpr float FONT_FALLBACK_DENSITY =
    1.0f;  ///< Source density used when the preferred atlas does not fit.
inline constexpr int FONT_GLYPH_PADDING = 8;          ///< Source pixels between packed glyphs.
inline constexpr int FONT_MIP_LIMIT = 3;              ///< Highest mip level used for font sampling.
inline constexpr int STATUS_ICON_TEXTURE_SIZE = 256;  ///< Square SVG raster size in pixels.
inline constexpr int RANK_EMBLEM_TEXTURE_SIZE = 512;  ///< Square emblem raster size in pixels.

/**
 * @fn bool IsPowerOfTwo(std::uint32_t value)
 * @brief Check whether an integer contains exactly one set bit.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True for positive powers of two; false for zero.
 */
constexpr bool IsPowerOfTwo(std::uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/**
 * @fn bool FontPaddingSupportsMipLimit()
 * @brief Require at least one padding texel at the highest font mip.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Each mip halves padding; mip 3 needs eight source pixels.
 *
 * @return True when padding leaves at least one texel at the highest permitted font mip.
 */
constexpr bool FontPaddingSupportsMipLimit()
{
    return FONT_MIP_LIMIT >= 0 && FONT_MIP_LIMIT < 31 &&
           FONT_GLYPH_PADDING >= (1 << FONT_MIP_LIMIT);
}

static_assert(IsPowerOfTwo(STATUS_ICON_TEXTURE_SIZE));
static_assert(IsPowerOfTwo(RANK_EMBLEM_TEXTURE_SIZE));
static_assert(FontPaddingSupportsMipLimit());
}  // namespace RasterQuality
