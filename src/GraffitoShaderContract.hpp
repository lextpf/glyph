#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>

/**
 * @namespace Graffito::ShaderContract
 * @brief Shared CPU/HLSL ABI and reference transform for Graffito vertices.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Packing and arithmetic mirror kPlaneVS in Graffito.cpp. Update both sides together;
 * static assertions and tests check CPU packing only. HLSL row order, Y flip, depth
 * product, field order, array size and the hard-coded stride have no cross-check.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef input fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef step fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef output fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     K[PackSegments]:::step --> C[Constants]:::input
 *     P[Absolute ImDraw position]:::input --> F[EvaluateFisheyePosition]:::step
 *     C --> F
 *     F --> S[EvaluateSegmentSlot<br/>from warped X]:::step
 *     C --> S
 *     F --> A[Subtract sourceAnchor]:::step
 *     C --> A
 *     S --> H[Selected homography<br/>and depth rows]:::step
 *     C --> H
 *     A --> H
 *     H --> V[ClipPosition<br/>before the divide]:::output
 *     P --> W[EvaluateWingWeight<br/>from unwarped X]:::step
 *     C --> W
 *     I[ImDraw vertex color]:::input --> E[EvaluateColor]:::step
 *     C --> E
 *     W --> E
 *     E --> O[Treated vertex color]:::output
 * ```
 */
namespace Graffito::ShaderContract
{
/**
 * @brief Maximum number of chord strips in one plate.
 *
 * Must match Graffito::MAX_SEGMENTS and the HLSL array capacity.
 */
inline constexpr std::size_t MAX_SEGMENTS = 12;
/**
 * @brief Number of float4 rows stored for each chord.
 *
 * Three homography rows plus one depth row. kPlaneVS encodes this stride as a shift by two.
 */
inline constexpr std::size_t SEGMENT_FLOAT4S = 4;

/**
 * @struct SegmentData
 * @brief Shader payload for one chord strip before float4 packing.
 * @author Alex (<https://github.com/lextpf>)
 */
struct SegmentData
{
    std::array<float, 9> homography{};  ///< Row-major local-pixel -> NDC homogeneous map.
    std::array<float, 3> depthPlane{};  ///< xSlope, ySlope, constant over normalized screen.
};

/**
 * @struct Constants
 * @brief CPU image of the GraffitoCB buffer bound at vertex-shader slot b0.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Chord i starts at byte 64 + 64 * i. Unused rows stay zero; float4 groups satisfy
 * D3D11 constant-buffer alignment.
 *
 * @verbatim
 * byte  0  sourceAnchor    = anchorX, anchorY, wingCenterX, wingInvHalfWidth
 * byte 16  segmentParams   = firstBoundaryX, invStride, maxSlot, unused
 * byte 32  materialParams  = desaturation, brightness, opacity, edgeSheen
 * byte 48  fisheyeParams   = centerX, centerY, invHalfWidth, strength
 * byte 64  segments[4i+0]  = homography row 0 in xyz, w unused
 *          segments[4i+1]  = homography row 1 in xyz, w unused
 *          segments[4i+2]  = homography row 2 in xyz, w unused
 *          segments[4i+3]  = xSlope, ySlope, constant, w unused
 * @endverbatim
 */
struct alignas(16) Constants
{
    std::array<float, 4> sourceAnchor{};

    std::array<float, 4> segmentParams{};

    std::array<float, 4> materialParams{};
    std::array<float, 4> fisheyeParams{};  ///< Lanes: centerX, centerY, invHalfWidth, strength.

    std::array<std::array<float, 4>, SEGMENT_FLOAT4S * MAX_SEGMENTS> segments{};
};

static_assert(std::is_standard_layout_v<Constants>);
static_assert(std::is_trivially_copyable_v<Constants>);
static_assert(alignof(Constants) == 16);
static_assert(sizeof(Constants) == 832);
static_assert(sizeof(Constants) == 64 + 64 * MAX_SEGMENTS);
static_assert(offsetof(Constants, sourceAnchor) == 0);
static_assert(offsetof(Constants, segmentParams) == 16);
static_assert(offsetof(Constants, materialParams) == 32);
static_assert(offsetof(Constants, fisheyeParams) == 48);
static_assert(offsetof(Constants, segments) == 64);

/**
 * @struct ClipPosition
 * @brief Homogeneous clip position before the perspective divide.
 * @author Alex (<https://github.com/lextpf>)
 */
struct ClipPosition
{
    float x = .0f;
    float y = .0f;
    float z = .0f;
    float w = 1.0f;
};

/**
 * @struct VertexColor
 * @brief Vertex color channels from zero through one.
 * @author Alex (<https://github.com/lextpf>)
 */
struct VertexColor
{
    float r = .0f;
    float g = .0f;
    float b = .0f;
    float a = .0f;
};

/**
 * @fn Constants PackSegments(const std::array<SegmentData, MAX_SEGMENTS>& segments, std::size_t
 *     segmentCount, float firstBoundaryX, float invStride, const std::array<float, 2>&
 *     sourceAnchor, const std::array<float, 2>& wingBand, const std::array<float, 4>& material,
 *     const std::array<float, 4>& fisheye = {}) noexcept
 * @brief Pack a segmented projection into the constant-buffer layout kPlaneVS reads.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Unused chord rows stay zero; segmentParams lane 2 is the maximum slot, not a count.
 *
 * @param segments       Per-segment homography and depth plane, in slot order.
 * @param segmentCount   Number of valid segments. It is clamped to [1, MAX_SEGMENTS] before use.
 * @param firstBoundaryX Absolute source X at the first chord boundary.
 * @param invStride      Reciprocal source width of one chord.
 * @param sourceAnchor   Absolute source position treated as plane-local (0, 0).
 * @param wingBand       Wing center X and reciprocal half-width.
 * @param material       Desaturation, brightness, opacity, and edge sheen.
 * @param fisheye        Center X, center Y, reciprocal half-width, and strength. The default
 * all-zero value disables the warp.
 * @return The packed constants.
 */
inline Constants PackSegments(const std::array<SegmentData, MAX_SEGMENTS>& segments,
                              std::size_t segmentCount,
                              float firstBoundaryX,
                              float invStride,
                              const std::array<float, 2>& sourceAnchor,
                              const std::array<float, 2>& wingBand,
                              const std::array<float, 4>& material,
                              const std::array<float, 4>& fisheye = {}) noexcept
{
    Constants out{};
    segmentCount = std::clamp<std::size_t>(segmentCount, 1, MAX_SEGMENTS);
    out.sourceAnchor = {sourceAnchor[0], sourceAnchor[1], wingBand[0], wingBand[1]};
    out.segmentParams = {firstBoundaryX, invStride, static_cast<float>(segmentCount - 1), .0f};
    out.materialParams = {material[0], material[1], material[2], material[3]};
    out.fisheyeParams = fisheye;
    for (std::size_t i = 0; i < segmentCount; ++i)
    {
        const std::size_t base = i * SEGMENT_FLOAT4S;
        const auto& homography = segments[i].homography;
        const auto& depth = segments[i].depthPlane;
        out.segments[base] = {homography[0], homography[1], homography[2], .0f};
        out.segments[base + 1] = {homography[3], homography[4], homography[5], .0f};
        out.segments[base + 2] = {homography[6], homography[7], homography[8], .0f};
        out.segments[base + 3] = {depth[0], depth[1], depth[2], .0f};
    }
    return out;
}

/**
 * @fn std::array<float, 2> EvaluateFisheyePosition( const Constants& constants, const
 *     std::array<float, 2>& absolutePosition) noexcept
 * @brief Mirror of the row-local barrel magnification in kPlaneVS.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Horizontal endpoints stay fixed; vertical scale ranges from 1.18 at the center to .74
 * at the ends. The CPU zero-strength path returns an exact identity. HLSL evaluates
 * c + (y - c) * 1.0, which can round for large centers; preserve this difference.
 *
 * @param constants         Packed constants. Only fisheyeParams is read.
 * @param absolutePosition  Unwarped vertex position, in absolute ImDraw pixels.
 * @return The warped position. It feeds EvaluateSegmentSlot and the homography; the wing weight
 * keeps using the unwarped X, as the shader does.
 */
inline std::array<float, 2> EvaluateFisheyePosition(
    const Constants& constants, const std::array<float, 2>& absolutePosition) noexcept
{
    constexpr float HORIZONTAL_BARREL = .24f;
    constexpr float CENTER_VERTICAL_SCALE = 1.18f;
    constexpr float EDGE_VERTICAL_SCALE = .74f;

    const float invHalfWidth = std::max(constants.fisheyeParams[2], .0f);
    const float strength =
        invHalfWidth > .0f ? std::clamp(constants.fisheyeParams[3], .0f, 1.0f) : .0f;
    if (strength <= .0f)
    {
        return absolutePosition;
    }

    const float halfWidth = 1.0f / invHalfWidth;
    const float normalized = (absolutePosition[0] - constants.fisheyeParams[0]) * invHalfWidth;
    const float t = std::clamp(normalized, -1.0f, 1.0f);
    const float edge = std::abs(t);
    const float edgeWeight = edge * edge * (3.0f - 2.0f * edge);
    const float verticalScale =
        1.0f + strength * ((CENTER_VERTICAL_SCALE - 1.0f) * (1.0f - edgeWeight) +
                           (EDGE_VERTICAL_SCALE - 1.0f) * edgeWeight);

    return {absolutePosition[0] + halfWidth * HORIZONTAL_BARREL * strength * t * (1.0f - t * t),
            constants.fisheyeParams[1] +
                (absolutePosition[1] - constants.fisheyeParams[1]) * verticalScale};
}

/**
 * @fn Constants Pack(const std::array<float, 9>& homography, const std::array<float, 3>&
 *     depthPlane, const std::array<float, 2>& sourceAnchor, const std::array<float, 3>& material)
 *     noexcept
 * @brief Pack a planar projection while retaining the segmented shader ABI.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Pack one chord with zero stride, wing band and fisheye. Zero stride selects chord 0
 * for every X. Force edge sheen to zero.
 *
 * @param homography    Row-major local-pixel to NDC homogeneous map.
 * @param depthPlane    xSlope, ySlope and constant over normalized screen.
 * @param sourceAnchor  Absolute source position treated as plane-local (0, 0).
 * @param material      Desaturation, brightness and opacity.
 * @return The packed constants.
 */
inline Constants Pack(const std::array<float, 9>& homography,
                      const std::array<float, 3>& depthPlane,
                      const std::array<float, 2>& sourceAnchor,
                      const std::array<float, 3>& material) noexcept
{
    std::array<SegmentData, MAX_SEGMENTS> segments{};
    segments[0] = {homography, depthPlane};
    return PackSegments(segments,
                        1,
                        .0f,
                        .0f,
                        sourceAnchor,
                        {.0f, .0f},
                        {material[0], material[1], material[2], .0f});
}

/**
 * @fn float EvaluateWingWeight(const Constants& constants, const std::array<float, 2>&
 *     absolutePosition) noexcept
 * @brief Smoothstep weight of the wing sheen at a source position.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Use unwarped X. Zero reciprocal half-width disables sheen, including on flat plates.
 *
 * @param constants         Packed constants. Only sourceAnchor lanes 2 and 3 are read.
 * @param absolutePosition  Unwarped vertex position, in absolute ImDraw pixels.
 * @return Band weight from 0 at the wing center to 1 at or past the half-width.
 */
inline float EvaluateWingWeight(const Constants& constants,
                                const std::array<float, 2>& absolutePosition) noexcept
{
    const float normalizedDistance =
        std::clamp(std::abs(absolutePosition[0] - constants.sourceAnchor[2]) *
                       std::max(constants.sourceAnchor[3], .0f),
                   .0f,
                   1.0f);
    return normalizedDistance * normalizedDistance * (3.0f - 2.0f * normalizedDistance);
}

/**
 * @fn std::size_t EvaluateSegmentSlot(const Constants& constants, const std::array<float, 2>&
 *     absolutePosition) noexcept
 * @brief Select the chord slot for a source position.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Use fisheye-warped X. The non-finite guard and MAX_SEGMENTS clamp are CPU-only;
 * HLSL clamps against segmentParams lane 2.
 *
 * @param constants         Packed constants. Only segmentParams is read.
 * @param absolutePosition  Fisheye-warped position, in absolute ImDraw pixels.
 * @return Slot index. Multiply it by SEGMENT_FLOAT4S to reach the chord's rows.
 */
inline std::size_t EvaluateSegmentSlot(const Constants& constants,
                                       const std::array<float, 2>& absolutePosition) noexcept
{
    const float raw =
        std::floor((absolutePosition[0] - constants.segmentParams[0]) * constants.segmentParams[1]);
    if (!std::isfinite(raw))
    {
        return 0;
    }
    const float maxSlot =
        std::clamp(constants.segmentParams[2], .0f, static_cast<float>(MAX_SEGMENTS - 1));
    return static_cast<std::size_t>(std::clamp(raw, .0f, maxSlot));
}

/**
 * @fn ClipPosition EvaluateVertex(const Constants& constants, const std::array<float, 2>&
 *     absolutePosition) noexcept
 * @brief Mirror of the position arithmetic in kPlaneVS.
 * @author Alex (<https://github.com/lextpf>)
 *
 * EvaluateFisheyePosition and EvaluateSegmentSlot document CPU-only differences.
 * In the equations, s is the warped source offset, h_0..h_2 are the selected homography
 * rows, and d_x, d_y, d_c are its depth plane.
 *
 * X/W and Y/W are NDC; u-tilde/W and v-tilde/W are normalized top-left screen
 * coordinates; Z/W is viewport depth. Uniform homography scaling preserves all quotients.
 *
 * $$
 * \begin{aligned}
 * (X, Y, W) &= (h_0 \cdot s,\; h_1 \cdot s,\; h_2 \cdot s), \qquad s = (x, y, 1) \\
 * \tilde{u} &= \tfrac{1}{2}(X + W), \qquad \tilde{v} = \tfrac{1}{2}(W - Y) \\
 * Z &= d_x \tilde{u} + d_y \tilde{v} + d_c W
 * \end{aligned}
 * $$
 *
 * @param constants         Packed constants for this plate.
 * @param absolutePosition  Unwarped vertex position, in absolute ImDraw pixels.
 * @return The homogeneous clip position, before the perspective divide.
 */
inline ClipPosition EvaluateVertex(const Constants& constants,
                                   const std::array<float, 2>& absolutePosition) noexcept
{
    const auto warpedPosition = EvaluateFisheyePosition(constants, absolutePosition);
    const float sourceX = warpedPosition[0] - constants.sourceAnchor[0];
    const float sourceY = warpedPosition[1] - constants.sourceAnchor[1];
    const std::size_t base = EvaluateSegmentSlot(constants, warpedPosition) * SEGMENT_FLOAT4S;
    const auto& row0 = constants.segments[base];
    const auto& row1 = constants.segments[base + 1];
    const auto& row2 = constants.segments[base + 2];
    const auto& depth = constants.segments[base + 3];
    const std::array<float, 3> projected{row0[0] * sourceX + row0[1] * sourceY + row0[2],
                                         row1[0] * sourceX + row1[1] * sourceY + row1[2],
                                         row2[0] * sourceX + row2[1] * sourceY + row2[2]};
    const float x01Numerator = .5f * (projected[0] + projected[2]);
    const float y01Numerator = .5f * (projected[2] - projected[1]);
    const float depthNumerator =
        depth[0] * x01Numerator + depth[1] * y01Numerator + depth[2] * projected[2];

    ClipPosition out{};
    out.x = projected[0];
    out.y = projected[1];
    out.z = depthNumerator;
    out.w = projected[2];
    return out;
}

/**
 * @fn VertexColor EvaluateColor(const Constants& constants, const VertexColor& input, float
 *     wingWeight = .0f) noexcept
 * @brief Reference implementation of the color arithmetic in kPlaneVS.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Desaturation, opacity and sheen clamp to [0, 1]. Brightness clamps only below zero,
 * so RGB can exceed 1, matching HLSL.
 *
 * @param constants   Packed constants. Only materialParams is read.
 * @param input       Vertex color taken from the ImGui draw vertex.
 * @param wingWeight  Band weight from EvaluateWingWeight. The default 0 removes the sheen term,
 * which matches every flat plate.
 * @return The treated color. Alpha is scaled by the opacity lane only.
 */
inline VertexColor EvaluateColor(const Constants& constants,
                                 const VertexColor& input,
                                 float wingWeight = .0f) noexcept
{
    const float desaturation = std::clamp(constants.materialParams[0], .0f, 1.0f);
    const float brightness = std::max(constants.materialParams[1], .0f);
    const float luminance = input.r * .2126f + input.g * .7152f + input.b * .0722f;
    const float sheen =
        std::clamp(constants.materialParams[3], .0f, 1.0f) * std::clamp(wingWeight, .0f, 1.0f);
    const float r = (input.r + (luminance - input.r) * desaturation) * brightness;
    const float g = (input.g + (luminance - input.g) * desaturation) * brightness;
    const float b = (input.b + (luminance - input.b) * desaturation) * brightness;
    return {r + (1.0f - r) * sheen,
            g + (1.0f - g) * sheen,
            b + (1.0f - b) * sheen,
            input.a * std::clamp(constants.materialParams[2], .0f, 1.0f)};
}
}  // namespace Graffito::ShaderContract
