#pragma once

#include <cstdint>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace SceneMeter
 * @brief Image-space scene sampling that adapts nameplate ink to the scene's light.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup TextPostProcess
 *
 * Capture before all glyph draws to exclude the overlay. A three-slot staging ring
 * provides delayed CPU samples without waiting for the GPU. All entry points run on
 * the render thread; the grid and GPU state have no synchronization.
 *
 * Unsupported formats, MSAA or missing mip autogeneration latch a failure. IsInitialized
 * then returns false until Initialize or Shutdown clears the latch; resize alone cannot
 * restore the feature.
 *
 * @verbatim
 * frame N   : capture callback (before any glyph draws)
 *             backbuffer -> mip chain -> tiny mip -> staging ring slot
 * frame N+2 : CollectResults() maps the oldest pending slot (DO_NOT_WAIT)
 *             into a CPU grid; plates bilinear-sample it during layout
 * @endverbatim
 *
 * @verbatim
 * for capture counter c:
 *
 * phase                  slot          stored capture
 * draw-list build now    (c + 1) % 3    c - 2
 * GPU may still own      (c + 2) % 3    c - 1
 * ImGui execution later  c % 3          c
 *
 * roles rotate by one slot after each capture.
 * @endverbatim
 */
namespace SceneMeter
{
/**
 * @fn bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
 * @brief Store the device references and clear the latched failure flag.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Retain device and context references. Create resources lazily at capture time, using
 * the actual backbuffer format. Call Shutdown before replacing the device so cached
 * textures cannot survive onto a different device.
 *
 * @return True when both pointers are valid.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * @fn bool IsInitialized()
 * @brief Report whether scene capture can be attempted.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True after initialization when no failure is latched; samples may still be unavailable.
 */
bool IsInitialized();

/**
 * @fn void Shutdown()
 * @brief Release all GPU resources.
 * @author Alex (<https://github.com/lextpf>)
 */
void Shutdown();

/**
 * @fn void OnResize(uint32_t width, uint32_t height)
 * @brief Invalidate the cached resources after a backbuffer size change.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The next capture replaces resources. Format-only changes are detected during capture.
 *
 * @param width   New backbuffer width in pixels.
 * @param height  New backbuffer height in pixels.
 */
void OnResize(uint32_t width, uint32_t height);

/**
 * @fn void CaptureCallback(const ImDrawList* dl, const ImDrawCmd* cmd)
 * @brief ImDrawCallback: downsample the current backbuffer into the staging ring.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Queue first, before any glyph content. Copies and GenerateMips preserve pipeline state.
 */
void CaptureCallback(const ImDrawList* dl, const ImDrawCmd* cmd);

/**
 * @fn void CollectResults()
 * @brief Map the oldest pending staging texture into the CPU grid, without blocking.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Call once per frame on the render thread before sampling. Keep the previous grid
 * when the GPU still owns the pending slot.
 */
void CollectResults();

/**
 * @fn bool Sample(float x01, float y01, float& outLum, float* outRGB)
 * @brief Bilinear-sample the metered scene at normalized screen coordinates.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param x01          Normalized viewport x in [0, 1]; outside values clamp.
 * @param y01          Normalized viewport y in [0, 1]; outside values clamp.
 * @param[out] outLum  Rec.709 luminance of the sampled region [0,1].
 * @param[out] outRGB  Average color in [0, 1]; points to three writable floats.
 * @return False while no results are available yet (feature warms up).  Both outputs stay untouched
 * in that case.
 * @pre Coordinates are finite and the RGB output pointer is not null.
 */
bool Sample(float x01, float y01, float& outLum, float outRGB[3]);
}  // namespace SceneMeter
