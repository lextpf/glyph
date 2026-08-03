#pragma once

#include <string>
#include <vector>

/**
 * @namespace ProjectManifest
 * @brief Manifest lookup for fonts, badges, and particles.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Utilities
 *
 * Relative paths resolve against the manifest directory; absolute paths pass through.
 * Missing or malformed manifests leave accessors empty so loaders use built-in fallbacks.
 * Wrong JSON section types are silently skipped; a parsed manifest still reports success.
 * Unmapped fonts return stable empty strings and use the corresponding INI font paths.
 * Accessors borrow storage until the next Load call. Copy paths that must survive a reload.
 */
namespace ProjectManifest
{
/**
 * @fn bool Load(const std::string& path = "Data/SKSE/Plugins/glyph.project.json")
 * @brief Replace all resolved manifest state.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clears prior state before opening the file, including on failure.
 * Paths are resolved without checking whether the asset files exist; each loader checks its files.
 *
 * @param path Manifest file, relative to the game directory or absolute.
 * @return True after parsing and extraction; false with empty accessors on failure.
 * @pre Call at startup before asset loads. State has no mutex; loading must not overlap
 * render-thread accessor calls.
 */
bool Load(const std::string& path = "Data/SKSE/Plugins/glyph.project.json");

/**
 * @fn bool IsLoaded()
 * @brief Report whether manifest parsing succeeded.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True even when no usable asset entries were found.
 */
bool IsLoaded();

/**
 * @fn const std::string& FontName()
 * @brief Return the actor-name font path for the manifest role "name".
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Borrowed path; empty selects the matching INI font path.
 */
const std::string& FontName();

/**
 * @fn const std::string& FontLevel()
 * @brief Return the level-number font path for the manifest role "level".
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Borrowed path; empty selects the matching INI font path.
 */
const std::string& FontLevel();

/**
 * @fn const std::string& FontTitle()
 * @brief Return the title and honorific font path for the manifest role "title".
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Borrowed path; empty selects the matching INI font path.
 */
const std::string& FontTitle();

/**
 * @fn const std::string& FontOrnament()
 * @brief Return the ornament glyph font path for the manifest role "ornament".
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Borrowed path; empty selects the matching INI font path.
 */
const std::string& FontOrnament();

/**
 * @fn const std::vector<std::string>& TierBadges()
 * @brief Emblem paths in rank order, lowest first.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Borrowed paths in manifest order; empty when no emblems are mapped.
 */
const std::vector<std::string>& TierBadges();

/**
 * @fn const std::vector<std::string>& ParticleVariants(const std::string& token)
 * @brief Sprite variants mapped to a particle style token.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param token Exact, case-sensitive manifest key; no case folding is applied.
 * @return Borrowed paths, or a stable empty vector when unmapped.
 */
const std::vector<std::string>& ParticleVariants(const std::string& token);

/**
 * @fn const std::string& BubblePop()
 * @brief Return the bubble-pop sprite path, or an empty string when it is not mapped.
 * @author Alex (<https://github.com/lextpf>)
 */
const std::string& BubblePop();
}  // namespace ProjectManifest
