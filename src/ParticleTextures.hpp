#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <string>

/**
 * @namespace ParticleTextures
 * @brief Particle texture management for sprite-based effects.
 * @author Alex (https://github.com/lextpf)
 * @ingroup ParticleTextures
 *
 * Loads one hand-made PNG sprite per weather type and creates D3D11 shader
 * resource views for use with ImGui textured quads. The per-particle texture
 * selection is deterministic to avoid flickering.
 *
 * ## :material-folder-image: Sprite Files
 *
 * One PNG per type is loaded from `Data/SKSE/Plugins/glyph/particles/`. The
 * style index matches `Settings::ParticleStyle`:
 *
 * | File                | Style Index | Type           |
 * |---------------------|:-----------:|----------------|
 * | `firefly.png`       | 0           | Firefly        |
 * | `rain.png`          | 1           | Rain           |
 * | `snow.png`          | 2           | Snow           |
 * | `smoke.png`         | 3           | Smoke          |
 * | `spark.png`         | 4           | Spark          |
 * | `wisp.png`          | 5           | Wisp           |
 * | `leaf.png`          | 6           | Leaf           |
 * | `aurora.png`        | 7           | Aurora         |
 * | `cherryblossom.png` | 8           | Cherry blossom |
 * | `dust.png`          | 9           | Dust           |
 * | `mote.png`          | 10          | Mote           |
 *
 * ## :material-image-filter-hdr: Texture Pipeline
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef io fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef process fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef render fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[PNG Files]:::io --> B[WIC Load]:::process
 *     B --> C[ID3D11Texture2D]:::process
 *     C --> D[Shader Resource View]:::process
 *     D --> E[ImGui Textured Quad]:::render
 * ```
 *
 * ## :material-dice-multiple-outline: Texture Selection
 *
 * Each particle is assigned a texture deterministically using a hash of its
 * index and style to avoid random flickering across frames. The hash uses
 * prime multipliers for good distribution:
 *
 * $$\text{texture} = \text{hash}(\text{particleIndex},\; \text{style}) \bmod \text{textureCount}$$
 *
 * ## :material-auto-fix: Quality Pipeline
 *
 * Styles without user textures fall back to procedurally generated
 * 256x256 white-on-transparent sprites. Every generated sprite runs
 * through a quality pipeline: 4x rotated-grid supersampling (anti-aliased
 * line work), interleaved-gradient-noise dithering at 8-bit quantization
 * (no banding in soft glows), and a full CPU-built mip chain (no shimmer
 * under minification). User-provided textures larger than 64px receive a
 * GPU-generated mip chain at load time for the same reason.
 */
namespace ParticleTextures
{
/// Blend modes for particle sprite rendering.
///
/// @note These enum values differ from the INI integer convention
/// (Settings: 0=Additive, 1=Screen, 2=Alpha).  DrawParticleAura
/// remaps the INI integer to this enum before calling DrawSpriteWithIndex.
enum class BlendMode
{
    Alpha = 0,     ///< Standard alpha blending
    Additive = 1,  ///< Additive blending for bright, glowing particles
    Screen = 2     ///< Screen-like blend for softer luminous sprites
};

/**
 * Initialize particle textures using the D3D11 device.
 * Scans subfolders and loads all PNG textures.
 * Call after D3D11 device is created and ImGui is initialized.
 *
 * @param device D3D11 device to create textures on
 * @return true if at least one texture loaded successfully
 */
bool Initialize(ID3D11Device* device);

/**
 * Check if particle textures have been loaded.
 * @return true if textures are
 * available for use
 */
bool IsInitialized();

/**
 * Release all particle texture resources.
 * Safe to call multiple times.
 */
void Shutdown();

/**
 * Get the number of loaded textures for a particle type.
 * @param style Particle style index
 * @return Number of textures available (0 if none)
 */
int GetTextureCount(int style);

/**
 * Get a texture based on particle index.
 * Despite the name, selection is deterministic (hash-based, not random)
 * to avoid flickering across frames.
 * @param style Particle style
 * @param particleIndex Index of the particle
 * @return ImTextureID or empty if not loaded
 */
ImTextureID GetRandomTexture(int style, int particleIndex);

/**
 * Draw a textured particle sprite with specific particle index.
 * Selects texture based on particle index for variety.
 *
 * @param list ImGui draw list
 * @param center Center position of the sprite
 * @param size Size of the sprite (width and height)
 * @param style Particle style (determines which texture folder)
 * @param particleIndex Index of the particle
 * @param color Tint color (white = no tint)
 * @param blendMode Blend state to use while drawing this sprite
 * @param rotation Rotation angle in radians
 */
void DrawSpriteWithIndex(ImDrawList* list,
                         const ImVec2& center,
                         float size,
                         int style,
                         int particleIndex,
                         ImU32 color,
                         BlendMode blendMode = BlendMode::Alpha,
                         float rotation = .0f);

/**
 * Push additive blend state onto the draw list via callback.
 * All subsequent draws will use additive blending until PopBlendState.
 * @param dl ImGui draw list
 */
void PushAdditiveBlend(ImDrawList* dl);

/**
 * Push screen blend state onto the draw list via callback.
 * Screen blend: src + dst*(1-src_color). Brightens the background
 * through colored pixels; black pixels are invisible.
 * @param dl ImGui draw list
 */
void PushScreenBlend(ImDrawList* dl);

/**
 * Reset blend state to ImGui defaults.
 * @param dl ImGui draw list
 */
void PopBlendState(ImDrawList* dl);
}  // namespace ParticleTextures
