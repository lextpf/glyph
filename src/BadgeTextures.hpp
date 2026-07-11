#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <string>
#include <vector>

/**
 * @namespace BadgeTextures
 * @brief Mipmapped status icons and full-color rank emblems.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup BadgeTextures
 *
 * Duotone SVGs become white alpha masks with a 0.80 opacity floor and normalized peak
 * alpha; the renderer applies semantic vertex tint. Transparent texels have black RGB
 * to prevent hidden color from leaking into screen blending. CPU mip chains stabilize
 * minification. Status icons use 256-pixel source squares; tier emblems use 512 pixels.
 *
 * Returned texture IDs borrow the stored SRVs; lookups do not extend resource lifetimes.
 * Finish draw-list playback before replacing or releasing textures. The map mutex protects
 * lookup and mutation, but it cannot protect an ID after the lookup returns.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart TB
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef step fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef output fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     subgraph D[Duotone status icon]
 *         direction LR
 *         S[SVG paths]:::input --> A[Raise low opacity to 0.80]:::step
 *         A --> R[nanosvg raster]:::step
 *         R --> N[Normalize peak alpha when needed]:::step
 *         N --> W[White RGB and computed alpha]:::step
 *         W --> M[CPU mip chain and SRV]:::step
 *         M --> T[ImGui quad with vertex tint]:::output
 *     end
 *
 *     subgraph P[Full-color tier emblem]
 *         direction LR
 *         G[Rank-ordered PNG paths]:::input --> I[WIC decode each file]:::step
 *         I --> C[Trim opaque bounds]:::step
 *         C --> Q[Center and resample to a square]:::step
 *         Q --> U[Mip chain and SRV]:::step
 *         U --> O[ImGui quad with white tint]:::output
 *         I -. one file fails .-> B[Keep its rank slot blank]:::step
 *         I -. no file loads .-> F[Use duotone rank icons]:::output
 *     end
 * ```
 */
namespace BadgeTextures
{
/**
 * @fn bool Initialize(ID3D11Device* device, const std::string& folder, const
 *     std::vector<std::string>& names)
 * @brief Replace the status icon texture set.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clears existing icons even if loading fails. Empty names are skipped; missing or invalid
 * `<folder>/<name>.svg` files are logged and skipped. Duplicate names load once.
 * @param device Device used to create textures; null loads nothing.
 * @param folder Absolute or relative to the game directory.
 * @param names SVG filename stems, matched exactly and without case folding.
 * @return True if at least one icon loaded.
 * @pre Render thread; a D3D11 device exists.
 */
bool Initialize(ID3D11Device* device,
                const std::string& folder,
                const std::vector<std::string>& names);

/**
 * @fn int AddIcons(ID3D11Device* device, const std::string& folder, const std::vector<std::string>&
 *     names)
 * @brief Add textures without replacing loaded icons.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Skips empty, loaded, missing, and invalid names; failures are logged. Leaves the
 * initialized flag unchanged, so pre-initialization textures remain hidden from layout.
 * @param device Device used to create textures; null loads nothing.
 * @param folder Absolute or relative to the game directory.
 * @param names SVG filename stems, matched exactly and without case folding.
 * @return Number loaded.
 * @pre Render thread; a D3D11 device exists; call before the frame's first `Get`.
 */
int AddIcons(ID3D11Device* device,
             const std::string& folder,
             const std::vector<std::string>& names);

/**
 * @fn bool IsInitialized()
 * @brief Report whether status icon initialization has completed.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True after Initialize, including an empty result; false after Shutdown.
 */
bool IsInitialized();

/**
 * @fn void Shutdown()
 * @brief Release all badge texture resources.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clears icons, rank emblems, and availability flags. All returned texture IDs become invalid.
 */
void Shutdown();

/**
 * @fn ImTextureID Get(const std::string& name)
 * @brief Texture lookup by configured icon name.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Borrowed texture ID, or zero when the entry is unavailable.
 */
ImTextureID Get(const std::string& name);

/**
 * @fn int InitializeTierImages(ID3D11Device* device, const std::vector<std::string>& paths)
 * @brief Replace rank emblems with centered, mipmapped PNG images.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Trim opaque bounds and resample to 512-pixel squares. Failed files retain blank rank
 * slots; if none load, clear the set to restore Font Awesome rank icons.
 * @param device Device used to create textures; null clears the set without loading.
 * @param paths Rank order, lowest first; empty clears the set.
 * @return Number loaded, or zero if COM is unavailable, with a warning.
 * @pre Render thread with a D3D11 device and initialized COM for WIC decoding.
 */
int InitializeTierImages(ID3D11Device* device, const std::vector<std::string>& paths);

/**
 * @fn ImTextureID GetTierImage(int index)
 * @brief Rank texture at a zero-based index.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Borrowed texture ID, or zero when the entry is unavailable.
 */
ImTextureID GetTierImage(int index);

/**
 * @fn int TierImageCount()
 * @brief Rank-slot count, including failed image slots.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return Zero when disabled; this query is lock-free.
 */
int TierImageCount();
}  // namespace BadgeTextures
