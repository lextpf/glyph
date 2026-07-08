#pragma once

#include <cstdint>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace DepthClip
 * @brief Cut by the World -- per-pixel depth occlusion for nameplates.
 * @ingroup TextPostProcess
 *
 * The single biggest "pasted-on sticker" tell in any nameplate overlay is
 * binary occlusion: the whole plate pops when line of sight breaks.  This
 * module gives every plate a real per-pixel depth test against the game's
 * own depth buffer, so a name is physically sliced by a doorframe as an
 * actor walks behind it -- the lower half disappearing behind a market
 * stall while the top line still reads -- with a soft feather at the
 * intersection edge.
 *
 * ## How
 *
 * A custom pixel shader (ImGui's shader + a scene-depth compare) is bound
 * around each plate's draws via ImGui draw callbacks.  The plate's depth is
 * the NDC z that `WorldToScreen` already computes -- the game's own
 * projection, so it matches what the rasterizer wrote into the depth buffer
 * by construction.  Depth-convention polarity is derived each frame from
 * two projected probe points (no readback), and the scene depth SRV comes
 * from the game's renderer data (`kPOST_ZPREPASS_COPY`, falling back to
 * `kMAIN`).
 *
 * ## Fallback contract
 *
 * Anything unexpected -- no depth SRV (some ENB/upscaler stacks), shader
 * compile failure, indeterminate polarity -- leaves the frame unclipped and
 * the existing line-of-sight culling as the only occlusion, exactly as
 * before this feature existed.  The coarse LOS gate stays on regardless;
 * depth clipping is the fine, sub-plate truth layered on top.
 */
namespace DepthClip
{
/// Compile the depth-clip pixel shader and create the constant buffer.
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/// True when shader + buffers exist (frame usability is BeginFrame's call).
bool IsInitialized();

/// Release all GPU resources.  Safe to call repeatedly.
void Shutdown();

/// Per-frame setup: locate the game's depth SRV and latch feather/polarity.
/// @param featherPx  Feather radius at the occlusion edge, in pixels.
/// @param polarity   +1 standard z (larger = farther), -1 reversed.
/// @return false when depth clipping cannot run this frame (no SRV).
bool BeginFrame(float featherPx, float polarity);

/// Allocate per-plate callback params (valid until the next BeginFrame).
/// @param plateDepthNDC  The plate's viewport-space depth from WorldToScreen.
void* MakePlateParams(float plateDepthNDC);

/// Params that disable clipping for the following draws (used by exit/death
/// ghosts whose reprojection failed) while keeping the bracket structure.
void* MakeNeutralParams();

/// ImDrawCallback: bind the depth-clip shader + this plate's constants for
/// subsequent draws in the current splitter channel.  Reset at end of frame
/// with ImDrawCallback_ResetRenderState.
void ApplyCallback(const ImDrawList* dl, const ImDrawCmd* cmd);
}  // namespace DepthClip
