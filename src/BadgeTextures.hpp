#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <string>
#include <vector>

/**
 * @namespace BadgeTextures
 * @brief Status badge icon textures rasterized from duotone SVGs.
 * @author Alex (https://github.com/lextpf)
 * @ingroup BadgeTextures
 *
 * Loads the status badge icons referenced by the `[Icons]` settings from a
 * folder of Font Awesome duotone SVGs, rasterizes them via nanosvg, and
 * creates mipmapped D3D11 shader resource views for ImGui textured quads.
 *
 * ## :material-layers: Duotone Pipeline
 *
 * Each Font Awesome duotone SVG carries two `currentColor` paths: a
 * secondary layer at `opacity=".4"` and a fully opaque primary layer. Both
 * are rasterized together so the per-pixel alpha already encodes the
 * two-tone look; the badge's semantic color is applied as a vertex tint at
 * draw time. Icons whose strongest layer is translucent (some glyphs keep
 * all content in the secondary layer) are normalized so their peak alpha is
 * fully opaque.
 *
 * Texels are white with computed alpha; fully transparent texels carry
 * black RGB so screen-style blending cannot lift hidden white into the
 * framebuffer. A full CPU-built mip chain keeps minified badges stable.
 */
namespace BadgeTextures
{
/**
 * Rasterize the given icon names from `folder` and create their textures.
 * Names resolve to `<folder>/<name>.svg`; empty names are skipped, missing
 * or unparsable files are logged once and skipped.
 *
 * Call on the render thread once the D3D11 device exists. Safe to call
 * again after Shutdown() (settings hot reload).
 *
 * @param device D3D11 device to create textures on
 * @param folder SVG folder path (relative to the game directory or absolute)
 * @param names  Icon names to load (duplicates are loaded once)
 * @return true if at least one icon loaded successfully
 */
bool Initialize(ID3D11Device* device,
                const std::string& folder,
                const std::vector<std::string>& names);

/// True after Initialize() ran (even if no icon loaded -- prevents retry storms).
bool IsInitialized();

/// Release all badge texture resources. Safe to call multiple times.
void Shutdown();

/**
 * Look up a loaded badge texture by icon name.
 * @param name Icon name as configured (e.g. "shield-halved")
 * @return ImTextureID of the icon, or 0 when the icon is not loaded
 */
ImTextureID Get(const std::string& name);

/**
 * Load the full-color prestige emblem PNGs used by the player tier badge.
 *
 * Unlike the duotone SVG icons above (alpha masks tinted at draw time), these
 * are true-color images rendered untinted.  `paths` lists the emblem files in
 * rank order (index 0 = lowest rank), resolved from the obfuscated asset
 * manifest.  A file that fails to load keeps its rank slot (rendered blank) so
 * the emblem-to-rank alignment is preserved; if *none* load, the set is cleared
 * so the tier badge falls back to the Font Awesome medal/gem/crown icons.
 *
 * Each image is trimmed to its opaque content and resampled into a centered,
 * mipmapped square so all emblems read at a uniform on-screen size.
 *
 * Call on the render thread once the D3D11 device exists.  An empty `paths`
 * clears any loaded emblems.  Safe to call again (settings hot reload).
 *
 * @param device D3D11 device to create textures on
 * @param paths  Emblem file paths in rank order (empty clears the set)
 * @return number of emblem images that loaded successfully
 */
int InitializeTierImages(ID3D11Device* device, const std::vector<std::string>& paths);

/// Texture for tier emblem `index` (0-based), or 0 when not loaded.
ImTextureID GetTierImage(int index);

/// Number of loaded tier emblem images (lock-free; 0 when none/disabled).
int TierImageCount();
}  // namespace BadgeTextures
