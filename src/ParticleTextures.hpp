#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <string>

/**
 * @namespace ParticleTextures
 * @brief Particle texture management for sprite-based effects.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup ParticleTextures
 *
 * Render-thread only. Initialize and Shutdown share a mutex; queries and draws are
 * unlocked. Shutdown on another thread races them.
 *
 * Resolve sprites through ProjectManifest::ParticleVariants and BubblePop; no directory
 * scan or filename convention. Styles with no decoded texture use procedural fallbacks,
 * including when COM or WIC is unavailable.
 *
 * Horizontal strips require width to be an exact multiple of height with quotient > 1.
 * Other dimensions mean one frame. Frames use the full V range and the U window below.
 *
 * Variants use a stable round-robin with a per-style hash offset. The pop sprite is excluded.
 *
 * Procedural 256x256 sprites use 4x rotated-grid supersampling, noise dithering and CPU
 * mips. Mapped images above 64 px on either axis use GPU mips. Sampling is point when
 * a frame is at most 64 px on both axes, otherwise linear.
 *
 * | Mapped image | Meaning                                           |
 * |--------------|---------------------------------------------------|
 * | `16x16`      | Single frame                                      |
 * | `Nx16`       | Horizontal flipbook; the shipped strips are 64x16 |
 * $$\text{frames} = \frac{\text{width}}{\text{height}}$$
 *
 * $$u_0 = \frac{f}{\text{frames}}, \qquad u_1 = \frac{f + 1}{\text{frames}}$$
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
 *     A[Manifest PNG paths]:::io --> B[WIC decode, alphaGamma bake]:::process
 *     A2[Style left with no texture]:::io --> P[Procedural generator set]:::process
 *     B --> C[ID3D11Texture2D]:::process
 *     P --> C
 *     C --> D[Shader Resource View]:::process
 *     D --> E[ImGui Textured Quad]:::render
 *     D -.->|zero textures in total| Z[Release all, stay uninitialized]:::process
 * ```
 *
 * $$\text{texture} = (\text{particleIndex} + \text{hash}(\text{style})) \bmod \text{textureCount}$$
 */
namespace ParticleTextures
{
/**
 * @enum BlendMode
 * @brief Blend state a particle sprite draw selects.
 * @author Alex (<https://github.com/lextpf>)
 *
 * INI values use 0=Additive, 1=screen, 2=alpha. DrawParticleAura maps them to this enum.
 */
enum class BlendMode
{
    /// Retain the bound ImGui blend state; no blend callbacks.
    Alpha = 0,
    Additive = 1,  ///< Dst + src*src_alpha; for bright, glowing particles
    Screen = 2     ///< Dst*(1-src_color) + src*src_alpha; softer luminous sprites
};

/**
 * @struct StyleVisibilityTuning
 * @brief Visibility compensation for the art of one particle style.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Compensates painted coverage within a source frame; the motion renderer owns size.
 * Initialize applies pow(alpha, gamma) to decoded manifest sprites only when alphaGamma < 1.
 * Procedural and already-loaded textures keep their alpha; reload to apply tuning changes.
 */
struct StyleVisibilityTuning
{
    float coreSizeScale = 1.0f;  ///< Crisp sprite only; does not enlarge the halo

    float alphaGamma = 1.0f;  ///< Alpha exponent for decoded manifest sprites.

    float haloAlphaScale = 1.0f;  ///< Per-style multiplier for the shared halo
};

/**
 * @fn const StyleVisibilityTuning& GetStyleVisibilityTuning(int style)
 * @brief Get visibility tuning for a particle-style ordinal.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param style  Particle-style ordinal.
 * @return       The style tuning, or neutral tuning when the ordinal is invalid.
 */
const StyleVisibilityTuning& GetStyleVisibilityTuning(int style);

/**
 * @fn bool Initialize(ID3D11Device* device)
 * @brief Initialize particle textures using the D3D11 device.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Load mapped variants, generate missing styles and the shared glow disc. Call after
 * D3D11 and ImGui setup. Success retains resources and repeated calls return true;
 * call Shutdown before changing devices. Failure permits retry. Null device changes nothing.
 *
 * If no texture loads, release all resources; initialization and glow availability stay
 * false, and draws do nothing.
 *
 * @param device Device retained by COM reference; null leaves existing state unchanged.
 * @return True if at least one texture loaded successfully.
 */
bool Initialize(ID3D11Device* device);

/**
 * @fn bool IsInitialized()
 * @brief Check if particle textures have been loaded.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True if textures are available for use
 */
bool IsInitialized();

/**
 * @fn void Shutdown()
 * @brief Release all particle texture resources.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Release textures, samplers, blend states and device references; clear callback stacks.
 * All returned ImTextureID values become invalid. Repeatable; serialized with Initialize.
 */
void Shutdown();

/**
 * @fn int GetTextureCount(int style)
 * @brief Get the number of loaded texture variants for a particle style.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Includes procedural variants; excludes pop and shared glow textures.
 *
 * @param style ParticleStyle ordinal
 * @return Number of variants available; 0 when the ordinal is out of range or the style loaded
 * nothing
 */
int GetTextureCount(int style);

/**
 * @fn ImTextureID GetRandomTexture(int style, int particleIndex)
 * @brief Get the texture that a particle index selects.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Selection is deterministic. The namespace owns the SRV; Shutdown invalidates it.
 * Do not cache the returned ID across frames.
 *
 * @param style ParticleStyle ordinal
 * @param particleIndex Caller-assigned particle index. It must stay the same across frames for the
 * same particle, or the sprite flickers.
 * @return The selected texture, or an empty ImTextureID when the ordinal is out of range or the
 * style loaded nothing
 */
ImTextureID GetRandomTexture(int style, int particleIndex);

/**
 * @fn void DrawSpriteWithIndex(ImDrawList* list, const ImVec2& center, float size, int style, int
 *     particleIndex, ImU32 color, BlendMode blendMode = BlendMode::Alpha, float rotation = .0f, int
 *     frame = 0)
 * @brief Draw a particle sprite with stable variant selection.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Invalid or empty styles draw nothing. Sampler and blend callbacks save only the state
 * they change, preserving enclosing graffito and DepthClip shaders.
 *
 * @param list Current frame draw list; null draws nothing. Callbacks execute during playback.
 * @param center Center position of the sprite
 * @param size Requested quad edge in pixels. A source frame larger than 1200 px is scaled down by
 * clamp(1200 / maxFrameDim, 0.45, 1.0) so high-resolution art does not draw larger than the
 * pixel-art sprites. Every shipped sprite gives a factor of 1.0, so `size` is then the on-screen
 * edge. Nothing is drawn when the scaled edge falls to 0.02 px or below.
 * @param style ParticleStyle ordinal; selects the style's loaded variant set
 * @param particleIndex Caller-assigned particle index; it picks the variant and must stay the same
 * across frames for the same particle
 * @param color Tint color (white = no tint)
 * @param blendMode Blend state to use while drawing this sprite
 * @param rotation Rotation angle in radians. Positive turns clockwise on screen, because ImGui's y
 * axis points down. Exactly 0 takes an axis-aligned fast path.
 * @param frame Flipbook frame for animated strips (wrapped into range; ignored for 1-frame statics)
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
 * @fn int GetFrameCountForIndex(int style, int particleIndex)
 * @brief Flipbook frame count of the texture the given particle selects.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param style ParticleStyle ordinal
 * @param particleIndex The same index the caller passes to DrawSpriteWithIndex; it picks the
 * variant whose frame count is returned
 * @return >= 1; 1 when the selected sprite is a single frame (or nothing loaded, or the image size
 * does not match the flipbook rule)
 */
int GetFrameCountForIndex(int style, int particleIndex);

/**
 * @fn bool HasPopSprite(int style)
 * @brief Whether the style has an end-of-life pop sprite loaded.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Only bubble uses the manifest bubblePop entry. It stays outside variant rotation.
 *
 * @param style ParticleStyle ordinal
 * @return True when a pop sprite is loaded for the style
 */
bool HasPopSprite(int style);

/**
 * @fn int GetPopFrameCount(int style)
 * @brief Flipbook frame count of the style's pop sprite.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param style ParticleStyle ordinal
 * @return >= 1; 1 when the pop sprite is a single frame, when the style has no pop sprite, or when
 * the ordinal is out of range
 */
int GetPopFrameCount(int style);

/**
 * @fn void DrawPopSprite(ImDrawList* list, const ImVec2& center, float size, int style, ImU32
 *     color, BlendMode blendMode = BlendMode::Alpha, float rotation = .0f, int frame = 0)
 * @brief Draw the pop sprite when the selected style provides one.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Drawing parameters follow DrawSpriteWithIndex.
 *
 * @param list Current frame draw list; null draws nothing. Callbacks execute during playback.
 * @param center     Sprite center, in screen pixels.
 * @param size       Sprite edge length, in pixels.
 * @param style      Particle-style ordinal.
 * @param color      Packed sprite tint.
 * @param blendMode  Sprite blend mode.
 * @param rotation   Clockwise rotation, in radians.
 * @param frame      Flipbook frame for an animated pop strip. The value wraps into range. Use zero
 * for a static sprite.
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
 * @fn void DrawSoftGlow(ImDrawList* list, const ImVec2& center, float size, ImU32 color)
 * @brief Draw the shared soft light disc, additively.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The procedural gaussian disc supplies halo and glint without repeating sprite art.
 * No-op until the disc exists.
 *
 * @param list Current frame draw list; null draws nothing. Callbacks execute during playback.
 * @param center Center position
 * @param size Quad edge length in pixels. The disc is 256x256, below the 1200 px normalization
 * threshold, so this is the on-screen edge. The visible glow radius is about a third of it.
 * @param color Tint color; alpha scales the glow strength
 */
void DrawSoftGlow(ImDrawList* list, const ImVec2& center, float size, ImU32 color);

/**
 * @fn bool HasSoftGlow()
 * @brief Whether the shared soft light disc is currently drawable.
 * @author Alex (<https://github.com/lextpf>)
 *
 * False before initialization, after release, or if disc creation failed. Callers should
 * supply a fallback when glow is unavailable.
 *
 * @return True when the shared soft-light disc can be drawn.
 */
bool HasSoftGlow();

/**
 * @fn void PushAdditiveBlend(ImDrawList* dl)
 * @brief Queue additive blending for subsequent draw commands.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Subsequent draws use dst + src*src_alpha until PopBlendState. Callbacks execute at
 * ImGui render time; keep pairs ordered in the same frame. Failed state creation still
 * pushes an inactive entry that the pop must remove.
 */
void PushAdditiveBlend(ImDrawList* dl);

/**
 * @fn void PushScreenBlend(ImDrawList* dl)
 * @brief Queue screen blending for subsequent draw commands.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Subsequent draws use src*src_alpha + dst*(1-src_color). Source alpha prevents
 * transparent borders from brightening the background. Pairing follows PushAdditiveBlend.
 */
void PushScreenBlend(ImDrawList* dl);

/**
 * @fn void PopBlendState(ImDrawList* dl)
 * @brief Queue restoration of the matching particle blend state.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Restore blend state, factor and sample mask at ImGui render time. No matching push
 * means no-op. A full ImGui reset would discard enclosing graffito or DepthClip shaders.
 */
void PopBlendState(ImDrawList* dl);
}  // namespace ParticleTextures
