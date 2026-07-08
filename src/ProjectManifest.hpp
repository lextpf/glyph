#pragma once

#include <string>
#include <vector>

/**
 * @namespace ProjectManifest
 * @brief Asset manifest (`glyph.project.json`) resolving obfuscated GUID assets.
 * @author Alex (https://github.com/lextpf)
 *
 * The plugin's custom assets (fonts, tier emblem badges, particle sprites) ship
 * under GUID-obfuscated file names.  `glyph.project.json` sits next to the DLL
 * in `Data/SKSE/Plugins/` and maps their logical roles/tokens to those files so
 * the loaders know where to pull what.  Paths in the manifest are relative to
 * the manifest's own folder and are resolved to full
 * `Data/SKSE/Plugins/glyph/...` paths here.
 *
 * When the manifest is missing or malformed, every accessor returns empty and
 * each loader falls back to its built-in default (Font Awesome tier icons,
 * procedural particle sprites, INI font paths) -- the plugin never hard-fails
 * on a bad manifest.
 */
namespace ProjectManifest
{
/// Load and parse the manifest.  Call once at startup, before any asset load.
/// @return true if the manifest parsed; false (accessors empty) otherwise.
bool Load(const std::string& path = "Data/SKSE/Plugins/glyph.project.json");

/// True after a successful Load().
bool IsLoaded();

/// @name Font paths by role (full path, or empty when not mapped).
/// @{
const std::string& FontName();
const std::string& FontLevel();
const std::string& FontTitle();
const std::string& FontOrnament();
/// @}

/// Tier emblem badge paths in rank order (index 0 = lowest rank).  Empty when
/// no emblems are mapped.
const std::vector<std::string>& TierBadges();

/// Particle sprite variant paths for a style token (e.g. "smoke").  Returns a
/// stable empty vector when the token has no mapped sprites.
const std::vector<std::string>& ParticleVariants(const std::string& token);

/// End-of-life bubble pop sprite path (full path, or empty when not mapped).
const std::string& BubblePop();
}  // namespace ProjectManifest
