#pragma once

/**
 * @brief Version values shared with documentation tooling.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 *
 * `_clean_docs.py` reads the major, minor, and patch macros. `SKSEPluginInfo` in main.cpp
 * uses a separate `REL::Version` literal; update both when changing the version.
 * PCH.hpp includes this header; no C++ call site reads these macros.
 */

#define GLYPH_VERSION_MAJOR 0
#define GLYPH_VERSION_MINOR 1
#define GLYPH_VERSION_PATCH 0
#define GLYPH_VERSION_RELEASE 0

/**
 * @brief Stringify literal components without macro expansion.
 * @author Alex (<https://github.com/lextpf>)
 *
 * `#` preserves an operand's macro name instead of expanding its value.
 */
#define GLYPH_VERSION_STRINGIFY(major, minor, patch, release) \
    #major "." #minor "." #patch "." #release

/**
 * @brief String of component macro names, without numeric expansion.
 * @author Alex (<https://github.com/lextpf>)
 *
 * `GLYPH_VERSION_STRINGIFY` produces names such as "GLYPH_VERSION_MAJOR" here.
 * This macro has no call site.
 */
#define GLYPH_VERSION        \
    GLYPH_VERSION_STRINGIFY( \
        GLYPH_VERSION_MAJOR, GLYPH_VERSION_MINOR, GLYPH_VERSION_PATCH, GLYPH_VERSION_RELEASE)
