#pragma once

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace RenderSampling
 * @brief Balanced ImGui callbacks for pixel-shader sampler slot 0.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Push and pop enqueue callbacks; sampler state changes when ImGui executes them.
 * Match each push with one pop in the same draw order. Nested pairs restore in reverse order.
 * A null draw list or unavailable samplers queues nothing.
 *
 * Use all entry points on the render thread. Finish queued callbacks before Shutdown or
 * device replacement: their user data borrows the sampler pointers.
 */
namespace RenderSampling
{
/**
 * @fn bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
 * @brief Create quality font and badge samplers once per device.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Retain device and context references until Shutdown or device replacement. A different
 * device replaces the resources. Calls with the same device reuse the first result and
 * context, including a failed result; call Shutdown before retrying that device.
 *
 * @param device   Device that owns the samplers; null returns false.
 * @param context  Immediate context used by draw callbacks; null returns false.
 * @return True when both samplers are ready. Failure keeps ImGui's sampler in use.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * @fn void Shutdown()
 * @brief Release sampler resources and discard saved callback state.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Release retained device and context references. Clear saved states without restoring
 * the currently bound sampler; call only after all queued push/pop pairs have executed.
 */
void Shutdown();

/**
 * @fn void PushFontSampler(ImDrawList* drawList)
 * @brief Add a callback that pushes the trilinear font sampler with a limited mip range.
 * @author Alex (<https://github.com/lextpf>)
 */
void PushFontSampler(ImDrawList* drawList);

/**
 * @fn void PushBadgeSampler(ImDrawList* drawList)
 * @brief Add a callback that pushes the trilinear badge sampler with the full mip range.
 * @author Alex (<https://github.com/lextpf>)
 */
void PushBadgeSampler(ImDrawList* drawList);

/**
 * @fn void PopSampler(ImDrawList* drawList)
 * @brief Add a callback that restores the sampler saved by the matching push callback.
 * @author Alex (<https://github.com/lextpf>)
 */
void PopSampler(ImDrawList* drawList);
}  // namespace RenderSampling
