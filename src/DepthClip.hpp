#pragma once

#include <cstdint>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace DepthClip
 * @brief Per-pixel depth occlusion for nameplates.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup TextPostProcess
 *
 * Compare plate viewport depth directly with scene depth and feather intersections.
 * Projected probes determine depth polarity each frame without readback.
 *
 * Sample only kPOST_ZPREPASS_COPY. kMAIN can remain bound as a DSV; D3D11 nulls a
 * conflicting SRV, which would hide every plate. Missing depth, shader failure or
 * unknown polarity leaves the frame unclipped. Line-of-sight culling remains active.
 *
 * All entry points are render-thread only; state has no synchronization. Parameter pointers
 * remain valid until BeginFrame or Shutdown. Execute queued callbacks before either call.
 * Brackets share one constant buffer, so use sequential pairs within each splitter channel;
 * restoring a binding does not restore overwritten constant-buffer contents.
 *
 * ```mermaid
 * sequenceDiagram
 *     participant R as Renderer
 *     participant L as ImDrawList
 *     participant D as DepthClip
 *     participant G as D3D11 state
 *     R->>D: BeginFrame
 *     R->>D: Allocate plate parameters
 *     D-->>R: Frame-owned parameter pointer
 *     R->>L: Queue ApplyCallback, plate draws, RestoreCallback
 *     Note over L,G: ImGui executes queued commands after frame construction
 *     L->>D: ApplyCallback
 *     D->>D: Push active or inactive state entry
 *     opt Setup succeeds
 *         D->>G: Save bindings, then bind depth shader and plate constants
 *     end
 *     L->>G: Draw plate geometry
 *     L->>D: RestoreCallback
 *     opt Saved entry is active
 *         D->>G: Restore pixel shader, CB0, and SRV1
 *     end
 *     Note over D: No context makes both callbacks inert
 * ```
 */
namespace DepthClip
{
/**
 * @fn bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
 * @brief Compile the depth-clip shader and create its constant buffer.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Retain device and context references on success. Call Shutdown before reinitializing;
 * initialization does not clear pending callback parameters or saved states.
 *
 * @return True when shader and buffer creation succeeds; false for null arguments or GPU failure.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * @fn bool IsInitialized()
 * @brief Report whether initialization succeeded.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True after successful Initialize and before Shutdown; BeginFrame may still fail.
 */
bool IsInitialized();

/**
 * @fn void Shutdown()
 * @brief Release all GPU resources.
 * @author Alex (<https://github.com/lextpf>)
 */
void Shutdown();

/**
 * @fn bool BeginFrame(float featherPx, float polarity)
 * @brief Per-frame setup: find the scene depth SRV and latch feather and polarity.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clear the parameter arena and saved-state stack first, even when frame setup fails.
 *
 * @param featherPx  Feather radius at the occlusion edge, in pixels.  Clamped to [0, 8].
 * @param polarity   +1 standard z (larger = farther), -1 reversed.
 * @return False when depth clipping cannot run this frame: not initialized, polarity 0
 * (indeterminate), no renderer singleton, or no depth SRV.
 */
bool BeginFrame(float featherPx, float polarity);

/**
 * @fn void* MakePlateParams(float plateDepthNDC)
 * @brief Allocate callback parameters for a constant plate depth.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param plateDepthNDC  The plate's viewport depth from WorldToScreen, used unchanged as a constant
 * depth over the whole plate.
 * @return Opaque params pointer for ApplyCallback's user data.
 */
void* MakePlateParams(float plateDepthNDC);

/**
 * @fn void* MakePlaneParams(float depthX, float depthY, float depthConstant)
 * @brief Allocate params for a projected world plane.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Evaluate depthX * u + depthY * v + depthConstant at normalized screen coordinates (u, v).
 * This is exact for a perspective-projected plane.
 *
 * @param depthX         Depth gradient along normalized screen x.
 * @param depthY         Depth gradient along normalized screen y.
 * @param depthConstant  Depth at the screen origin.
 * @return Opaque params pointer for ApplyCallback's user data.
 */
void* MakePlaneParams(float depthX, float depthY, float depthConstant);

/**
 * @fn void* MakeNeutralParams()
 * @brief Allocate params that disable clipping for the following draws.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Keeps exit/death ghost brackets balanced when reprojection fails.
 *
 * @return Opaque params pointer for ApplyCallback's user data.
 */
void* MakeNeutralParams();

/**
 * @fn void ApplyCallback(const ImDrawList* dl, const ImDrawCmd* cmd)
 * @brief ImDrawCallback: bind the depth-clip shader and this plate's constants.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Save PS, PS constant-buffer slot 0 and PS resource slot 1 in the current splitter channel.
 *
 * @param cmd  Draw command whose user data comes from a MakePlateParams, MakePlaneParams,
 *             or MakeNeutralParams call in the current frame.
 * @post With a context, push one active or inactive state entry even if setup fails.
 * Without a context, both callbacks return without a state entry. Match every ApplyCallback
 * with one RestoreCallback in the same splitter channel.
 */
void ApplyCallback(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @fn void RestoreCallback(const ImDrawList* dl, const ImDrawCmd* cmd)
 * @brief Restore the pixel-shader state captured by the matching ApplyCallback.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Pop one entry. An empty stack or inactive marker changes no pipeline state.
 *
 * @param cmd  Draw command that carries the callback (unused).
 */
void RestoreCallback(const ImDrawList* dl, const ImDrawCmd* cmd);
}  // namespace DepthClip
