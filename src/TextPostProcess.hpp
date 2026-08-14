#pragma once

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace TextPostProcess
 * @brief GPU glow and color divide passes for nameplate text.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup TextPostProcess
 *
 * Render-thread only; no game-state access. Initialize and Shutdown share a mutex;
 * other calls use unsynchronized state. Callbacks consume parameters stored during
 * frame construction.
 *
 * Glow captures at half resolution, blurs horizontally then vertically, and uses a
 * screen blend. In the equations, c_rgb and c_a are the blurred color and alpha,
 * I is intensity, and sat clamps to [0, 1]. The blend writes
 * src_rgb * src_a + dst_rgb * (1 - src_rgb). Unavailable GPU setup selects CPU glow.
 *
 * Divide composites from a backbuffer snapshot with blending disabled. Captured alpha
 * below .001 is discarded, preserving destination pixels without text.
 *
 * Glyph.ini scalar keys match by name. GlowSamples affects only CPU glow. The GPU uses
 * 33 blur taps; sigma = max(1, radius * .48) half-resolution texels, capped at 16/3.
 * Radii outside roughly [2.1, 11.1] have no further effect. EnableGlow gates the bracket;
 * tier gating can leave an empty capture that still incurs blur work.
 *
 * Divide strength is independent of EnableGlow and scales by smoothstep(.10, .25, luma),
 * so dark outlines retain normal compositing.
 *
 * Append ImDrawCallback_ResetRenderState after each end callback. The trace shows
 * the renderer ordering when both passes are enabled.
 *
 * @verbatim
 * AddText (single call) -> half-res offscreen RT -> Gaussian blur (H then V)
 *                       -> screen-style composite onto the saved render target
 * @endverbatim
 *
 * $$src_{rgb} = \mathrm{sat}\bigl(c_{rgb}\,(0.46 + 0.34\,I)\bigr), \qquad
 * a_{veil} = \mathrm{sat}\bigl(c_a^{\,0.72}\,(0.14 + 0.16\,I)\bigr)$$
 *
 * $$a_{core} = \mathrm{sat}\bigl(c_a^{\,1.18 + 0.60\,I}\,(0.34 + 0.18\,I)\bigr), \qquad
 * src_a = \mathrm{sat}\bigl(a_{veil} + 0.55\,a_{core}\bigr)$$
 *
 * @verbatim
 * Snapshot the bound render target -> nametag draws to offscreen RT
 *                                  -> Photoshop-style color divide composite
 * @endverbatim
 *
 * | Setting            | Type  | Default | Description                                    |
 * |--------------------|-------|---------|------------------------------------------------|
 * | EnableGlow         | bool  | false   | Master toggle for glow (GPU pass and CPU path) |
 * | GlowRadius         | float | 4.0     | Blur radius in pixels (gaussian sigma proxy)   |
 * | GlowIntensity      | float | 0.5     | Composite intensity, clamped to [0, 1]         |
 * | GlowSamples        | int   | 8       | CPU fallback only; unused by the GPU blur      |
 * | GlowDivideStrength | float | 0.0     | Divide blend strength in [0, 1]; 0 disables    |
 * @code{.cpp}
 * TextPostProcess::SetGlowParams(radius, intensity);
 * drawList->AddCallback(TextPostProcess::BeginGlowCapture, nullptr);
 *     // ... AddText / AddTextHorizontalGradient / etc. for glow targets ...
 * drawList->AddCallback(TextPostProcess::EndGlowAndComposite, nullptr);
 * drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
 * @endcode
 *
 * @verbatim
 * channel 0 : BeginGlowCapture      clear + bind half-res glow RT, half-res
 *                                   viewport
 *             BeginDivideCapture    snapshot the bound RT (the glow RT here),
 *                                   then clear + bind the full-res divide RT
 *             ... glow-target draws ...
 *             EndGlowAndComposite   blur the glow RT, rebind the RT saved by
 *                                   BeginGlowCapture, then composite
 *             ImDrawCallback_ResetRenderState
 * channel 1 : particle draws
 * channel 2 : text, shadow and outline draws
 * (merge)
 *             EndDivideAndComposite rebind the RT saved by BeginDivideCapture,
 *                                   then composite the divide RT
 *             ImDrawCallback_ResetRenderState
 * @endverbatim
 *
 * @warning With both passes active, divide capture binds inside glow capture. The divide
 * snapshot requires a full-resolution backbuffer, but receives the half-resolution glow
 * target, so CopyResource is invalid and keeps stale content. Subsequent draws enter the
 * divide target and leave the glow capture empty. The passes cannot compose in this order.
 */
namespace TextPostProcess
{
/**
 * @fn bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
 * @brief Compile the shaders and create the constant buffers, sampler and pipeline states.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Retain device/context references until Shutdown. OnResize creates targets separately.
 * Prefer R16G16B16A16_FLOAT when supported for rendering and sampling; otherwise use
 * R8G8B8A8_UNORM. Failure releases partial resources and permits retry.
 *
 * @return True when initialization succeeded, or when the module is already initialized. False when
 * either argument is null, or when a resource creation step failed.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * @fn bool IsInitialized()
 * @brief Report whether the shader-side resources exist.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True when the shaders, constant buffers and pipeline states exist.  Render targets come
 * from OnResize; without them both bracket pairs are silent no-ops, so a true result alone does not
 * guarantee a working pass.
 */
bool IsInitialized();

/**
 * @fn void Shutdown()
 * @brief Release all GPU resources.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Repeatable; release GPU resources and device/context references. Queued callbacks
 * become inert. Shutdown between begin/end leaves the capture target bound because
 * the end callback can no longer restore it.
 */
void Shutdown();

/**
 * @fn void OnResize(uint32_t width, uint32_t height)
 * @brief Create the render targets, or recreate them after a size change.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Create half-resolution glow targets, full-resolution divide target and snapshot.
 * No-op before initialization, for zero dimensions, or when dimensions are unchanged.
 *
 * The game backbuffer must be bound so the snapshot gets a compatible CopyResource
 * format; an absent target selects R8G8B8A8_UNORM. Size is recorded before creation,
 * so failure retries only after another resize. Glow and divide failures are independent;
 * a bracket with missing targets is inert.
 *
 * @param width   Backbuffer width in pixels.
 * @param height  Backbuffer height in pixels.
 */
void OnResize(uint32_t width, uint32_t height);

/**
 * @fn void SetGlowParams(float radius, float intensity)
 * @brief Set glow parameters for the current frame.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Call before queuing the glow bracket. Values are stored without clamping and read
 * later by EndGlowAndComposite.
 *
 * @param radius     Blur radius in pixels (maps to gaussian sigma).  Values outside roughly [2.1,
 * 11.1] have no further effect, because sigma is max(1.0, radius * 0.48) half-res texels and the
 * shader clamps it to 16/3.  One half-res texel spans two backbuffer pixels.
 * @param intensity  Composite intensity.  The composite constants assume [0, 1]; see the glow
 * pipeline section for the mapping.  Higher values extrapolate.
 */
void SetGlowParams(float radius, float intensity);

/**
 * @fn void BeginGlowCapture(const ImDrawList* dl, const ImDrawCmd* cmd)
 * @brief ImDrawCallback: bind the half-res glow capture RT and clear it.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Save RT, DSV and viewport, then bind the cleared glow target without depth at half
 * resolution. EndGlowAndComposite restores them. Missing context or target makes both
 * callbacks inert.
 */
void BeginGlowCapture(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @fn void EndGlowAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd)
 * @brief ImDrawCallback: blur the glow capture and composite it onto the saved target.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Blur horizontally then vertically, restore saved RT and viewport, then screen-composite.
 * Consume saved state; repeated calls are inert. An empty capture still runs three
 * fullscreen passes.
 *
 * @post The caller must append ImDrawCallback_ResetRenderState immediately after this callback; it
 * restores ImGui's own shader, blend state, viewport and sampler.
 */
void EndGlowAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @fn void SetDivideParams(float strength)
 * @brief Set color divide parameters for the current frame.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Call before queuing the divide bracket; EndDivideAndComposite reads the stored value.
 *
 * @param strength  Blend strength [0, 1].  The shader multiplies it by smoothstep(0.10, 0.25, luma)
 * per text pixel, so dark pixels keep the normal alpha composite.
 */
void SetDivideParams(float strength);

/**
 * @fn void BeginDivideCapture(const ImDrawList* dl, const ImDrawCmd* cmd)
 * @brief ImDrawCallback: snapshot the bound RT, then bind and clear the divide RT.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Save RT, DSV and viewport. Snapshot the bound target, then clear and bind the full-size
 * divide target. CopyResource requires matching dimensions and format: the backbuffer
 * must be bound. Missing context, divide target or snapshot makes both callbacks inert.
 */
void BeginDivideCapture(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @fn void EndDivideAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd)
 * @brief ImDrawCallback: composite the captured nametag RT onto the saved render target.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Queue after all nameplate draws and the splitter merge. Restore saved RT and viewport;
 * write snapshot/text division with blending disabled. Discard captured alpha below .001;
 * other pixels replace the destination opaquely. Consume saved state so repeat calls are inert.
 *
 * @post The caller must append ImDrawCallback_ResetRenderState immediately after this callback.
 */
void EndDivideAndComposite(const ImDrawList* dl, const ImDrawCmd* cmd);
}  // namespace TextPostProcess
