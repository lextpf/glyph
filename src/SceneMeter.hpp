#pragma once

#include <cstdint>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace SceneMeter
 * @brief Candlelight Metering -- image-space scene sampling for adaptive ink.
 * @ingroup TextPostProcess
 *
 * Meters the scene like a film exposure meter so nameplate ink can sit in
 * the scene's light instead of floating at fixed sRGB white: over a bright
 * snowfield the ink dims a touch, in a torchlit crypt it lifts a few
 * percent and picks up a whisper of the flame's warmth.
 *
 * ## Pipeline (one frame of latency, never stalls)
 *
 * ```
 * frame N   : capture callback (before any glyph draws)
 *             backbuffer -> mip chain -> tiny mip -> staging ring
 * frame N+1 : CollectResults() maps the ready staging texture (DO_NOT_WAIT)
 *             into a CPU grid; plates bilinear-sample it during layout
 * ```
 *
 * The capture runs before glyph's own draws so the meter never reads the
 * overlay's own text back (no feedback loop).  Any failure -- exotic
 * backbuffer format, MSAA, no mip autogen -- disables the feature with one
 * log line and zero visual change.
 */
namespace SceneMeter
{
/// Create device references.  Resources themselves are (re)created lazily on
/// first capture so backbuffer format changes (ENB, upscalers) are handled.
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/// True when the meter is usable (initialized and not failed).
bool IsInitialized();

/// Release all GPU resources.  Safe to call repeatedly.
void Shutdown();

/// Drop size-dependent resources; they rebuild on the next capture.
void OnResize(uint32_t width, uint32_t height);

/// ImDrawCallback: downsample the current backbuffer into the staging ring.
/// Add as the FIRST callback of the overlay draw list, before any glyph
/// content, so the sample excludes the overlay itself.  Alters no pipeline
/// state (copies + GenerateMips only) -- no ResetRenderState needed.
void CaptureCallback(const ImDrawList* dl, const ImDrawCmd* cmd);

/// Map the oldest pending staging texture into the CPU grid (non-blocking).
/// Call once per frame from the render thread before plates sample.
void CollectResults();

/// Bilinear-sample the metered scene at normalized screen coords [0,1].
/// @param[out] outLum  Rec.709 luminance of the sampled region [0,1].
/// @param[out] outRGB  Average color of the sampled region (clamped [0,1]).
/// @return false while no results are available yet (feature warms up).
bool Sample(float x01, float y01, float& outLum, float outRGB[3]);
}  // namespace SceneMeter
