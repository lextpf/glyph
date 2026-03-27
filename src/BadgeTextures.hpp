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
}  // namespace BadgeTextures
