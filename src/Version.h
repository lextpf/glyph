#pragma once

/// Major version number.
#define GLYPH_VERSION_MAJOR 0
/// Minor version number.
#define GLYPH_VERSION_MINOR 1
/// Patch version number.
#define GLYPH_VERSION_PATCH 0
/// Release version number.
#define GLYPH_VERSION_RELEASE 0

/**
 * Stringify version components.
 *
 * @param major Major version.
 * @param minor Minor version.
 * @param patch Patch version.
 * @param release Release version.
 *
 * @see GLYPH_VERSION
 */
#define GLYPH_VERSION_STRINGIFY(major, minor, patch, release) \
	#major "." #minor "." #patch "." #release

/// Complete version string (e.g., "0.1.0.0").
#define GLYPH_VERSION \
	GLYPH_VERSION_STRINGIFY( \
		GLYPH_VERSION_MAJOR, \
		GLYPH_VERSION_MINOR, \
		GLYPH_VERSION_PATCH, \
		GLYPH_VERSION_RELEASE)

