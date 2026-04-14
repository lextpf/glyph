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
 * Loads hand-made PNG sprites per particle style and creates D3D11 shader
 * resource views for use with ImGui textured quads. The per-particle texture
 * selection is deterministic to avoid flickering.
 *
 * ## :material-folder-image: Sprite Files
 *
 * Sprites live in `Data/SKSE/Plugins/glyph/particles/` and are discovered by
 * a scan driven by `Settings::kParticleStyleTokens` (the token doubles as the
 * filename base). For every style the loader probes the base name plus
 * numbered variants, preferring an animated flipbook strip over the
 * same-named static:
 *
 * | Candidate           | Meaning                                    |
 * |---------------------|--------------------------------------------|
 * | `<token>.png`       | Static 16x16 sprite                        |
 * | `<token>_strip.png` | 4-frame 64x16 flipbook, supersedes static  |
 * | `<token>2.png` ...  | Numbered variants, same strip-over-static  |
 * | `bubblepop.png`     | Bubble pop sprite, outside the rotation    |
 *
 * All loaded variants of a style enter the per-particle hash rotation, so a
 * style with N variants spawns each on ~1/N of its particles, stably across
 * frames.
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
 * Each particle is assigned a texture deterministically -- a stratified
 * round-robin over the style's variants with a per-style hash offset -- so a
 * style with N variants puts each on an equal share of its particles (within
 * one) and never flickers across frames:
 *
 * $$\text{texture} = (\text{particleIndex} + \text{hash}(\text{style})) \bmod \text{textureCount}$$
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

/// Art-aware visibility compensation for one particle style. The motion
/// renderer still owns its physical size; this only compensates for how much
/// of the 16x16 source frame is actually painted.
struct StyleVisibilityTuning
{
    float coreSizeScale = 1.0f;   ///< Crisp sprite only; does not enlarge the halo
    float alphaGamma = 1.0f;      ///< Source-alpha curve; lower is more opaque
    float haloAlphaScale = 1.0f;  ///< Per-style multiplier for the shared halo
};

/// Return the measured/art-directed visibility tuning for a style ordinal.
/// Invalid ordinals return neutral tuning.
const StyleVisibilityTuning& GetStyleVisibilityTuning(int style);

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
 * @param frame Flipbook frame for animated strips (wrapped into range;
 *              ignored for 1-frame statics)
 */
void DrawSpriteWithIndex(ImDrawList* list,
                         const ImVec2& center,
                         float size,
                         int style,
                         int particleIndex,
                         ImU32 color,
                         BlendMode blendMode = BlendMode::Alpha,
                         float rotation = .0f,
                         int frame = 0);

/**
 * Flipbook frame count of the texture the given particle hash-selects.
 * @return >= 1; 1 when the selected sprite is a static (or nothing loaded)
 */
int GetFrameCountForIndex(int style, int particleIndex);

/**
 * Whether the style has an end-of-life pop sprite loaded (e.g. Bubble's
 * `bubblepop_strip.png` / `bubblepop.png`). Pop sprites live outside the
 * hash rotation.
 */
bool HasPopSprite(int style);

/**
 * Flipbook frame count of the style's pop sprite.
 * @return >= 1; 1 when the pop is a static (or nothing loaded)
 */
int GetPopFrameCount(int style);

/**
 * Draw the style's pop sprite (see HasPopSprite). No-op when absent.
 * @param frame Flipbook frame for an animated pop strip -- play it ONCE
 *              across the pop window (wrapped into range; 0 for statics)
 */
void DrawPopSprite(ImDrawList* list,
                   const ImVec2& center,
                   float size,
                   int style,
                   ImU32 color,
                   BlendMode blendMode = BlendMode::Alpha,
                   float rotation = .0f,
                   int frame = 0);

/**
 * Draw the shared soft light disc (procedural Gaussian falloff, additive).
 * Used as the glow-halo / glint layer behind and over crisp sprites -- a
 * featureless light so it never reads as a duplicate of the particle art.
 * No-op until Initialize has generated the disc.
 *
 * @param list ImGui draw list
 * @param center Center position
 * @param size Quad edge length in pixels (visible glow radius is ~1/3 of it)
 * @param color Tint color; alpha scales the glow strength
 */
void DrawSoftGlow(ImDrawList* list, const ImVec2& center, float size, ImU32 color);

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
