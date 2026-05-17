#pragma once

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace TextPostProcess
 * @brief GPU-accelerated text post-processing for glow, outline, and shadow.
 * @author Alex (https://github.com/lextpf)
 * @ingroup TextPostProcess
 *
 * Provides render-to-texture passes that replace CPU-based multi-copy
 * text effects with GPU shader passes.  Uses ImGui draw callbacks
 * (same pattern as ParticleTextures blend states) to bracket draws
 * with render target switches and shader dispatch.
 *
 * ## :material-blur: Glow Pipeline
 *
 * ```
 * AddText (single call) -> offscreen RT -> Gaussian blur (H+V) -> additive composite
 * ```
 *
 * Replaces the previous 24-call AddTextGlow with a proper Gaussian blur.
 *
 * ## :material-divide: Color Divide Pipeline
 *
 * ```
 * Snapshot backbuffer -> nametag draws to offscreen RT -> Photoshop-style Color Divide composite
 * ```
 *
 * ## :material-cogs: Settings
 *
 * Configure in glyph.ini under `[Effects]`:
 *
 * |            Setting | Type  | Default | Description                                          |
 * |--------------------|-------|---------|------------------------------------------------------|
 * |         EnableGlow | bool  | false   | Master toggle for the GPU glow pipeline              |
 * |         GlowRadius | float | 4.0     | Gaussian blur radius in pixels (sigma proxy)         |
 * |      GlowIntensity | float | 0.5     | Brightness multiplier on the additive composite      |
 * |        GlowSamples | int   | 8       | Blur sample count (1-64, higher = smoother / slower) |
 * | GlowDivideStrength | float | 0.0     | Color Divide blend: 0 = additive glow, 1 = full divide |
 *
 * ## :material-code-tags: Callback Bracket Pattern
 *
 * Each pass is a pair of ImDrawCallbacks placed around the affected draws.
 * The render code adds them through `ImDrawList::AddCallback`:
 *
 * ```cpp
 * TextPostProcess::SetGlowParams(radius, intensity);
 * drawList->AddCallback(TextPostProcess::BeginGlowCapture, nullptr);
 *     // ... AddText / AddTextHorizontalGradient / etc. for glow targets ...
 * drawList->AddCallback(TextPostProcess::EndGlowAndComposite, nullptr);
 * ```
 *
 * The Color Divide pass uses the same shape with
 * `BeginDivideCapture` / `EndDivideAndComposite`, and brackets ALL
 * nametag draws (outermost).
 */
namespace TextPostProcess
{
/**
 * Initialize GPU resources (render targets, shaders, buffers).
 * Call after D3D11 device is available and ImGui is initialized.
 *
 * @param device D3D11 device for resource creation
 * @param context D3D11 immediate context for shader dispatch
 * @return true if initialization succeeded
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * Check if GPU post-processing is available.
 * @return true if initialized and ready
 */
bool IsInitialized();

/**
 * Release all GPU resources.
 * Safe to call multiple times or when not initialized.
 */
void Shutdown();

/**
 * Recreate render targets if viewport dimensions changed.
 * No-op if size is unchanged.
 *
 * @param width  Viewport width in pixels
 * @param height Viewport height in pixels
 */
void OnResize(uint32_t width, uint32_t height);

/**
 * Set glow parameters for the current frame.
 * Call before rendering begins.
 *
 * @param radius    Blur radius in pixels (maps to Gaussian sigma)
 * @param intensity Glow brightness multiplier [0, ...]
 */
void SetGlowParams(float radius, float intensity);

/**
 * ImDrawCallback: switch render target to glow capture RT and clear it.
 * Add to draw list before glow text draws.
 */
void BeginGlowCapture(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * ImDrawCallback: blur the captured glow texture and composite
 * it back onto the main scene with additive blending.
 * Add to draw list after all glow text draws.
 */
void EndGlowAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * Set Color Divide parameters for the current frame.
 * Call before rendering begins.
 *
 * @param strength Blend strength [0, 1] (0=normal composite, 1=full divide)
 */
void SetDivideParams(float strength);

/**
 * ImDrawCallback: snapshot the backbuffer, then switch render target to
 * the divide capture RT and clear it.
 * Add to draw list before all nametag draws (outermost bracket).
 */
void BeginDivideCapture(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * ImDrawCallback: composite the captured nametag RT onto the backbuffer
 * using Photoshop-style Color Divide (background / foreground).
 * Add to draw list after all nametag draws.
 */
void EndDivideAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd);
}  // namespace TextPostProcess
