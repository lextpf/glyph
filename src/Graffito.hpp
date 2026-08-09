#pragma once

#include <RE/N/NiPoint3.h>

#include <array>

#include <d3d11.h>
#include <imgui.h>

/**
 * @namespace Graffito
 * @brief Perspective-correct ImGui geometry attached to a world-space plane.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Rendering
 *
 * Project source positions onto world planes with homogeneous W for perspective-correct
 * UVs. The vertex shader also applies InkMaterial.
 *
 * Render-thread only, on one immediate context. Frame-building calls precede callback
 * execution. BuildProjection reads the null-guarded world root camera.
 *
 * Callbacks save and restore only the vertex shader and VS constant-buffer slot 0.
 * All plates share one buffer; restore rebinds its pointer without restoring contents.
 * Do not nest brackets. Close each pair or isolate it in a splitter channel.
 */
namespace Graffito
{
/**
 * @brief Maximum number of chord strips in one plate.
 *
 * Keep this value, ShaderContract::MAX_SEGMENTS and the HLSL segments array in sync.
 * A static assertion checks only the C++ pair; HLSL uses SEGMENT_FLOAT4S * MAX_SEGMENTS.
 */
inline constexpr std::size_t MAX_SEGMENTS = 12;

/**
 * @struct CylinderWrap
 * @brief Cylindrical source-space wrap applied to an inscription.
 * @author Alex (<https://github.com/lextpf>)
 *
 * One segment or zero curvature preserves the flat projection. Front radius must equal
 * worldUnitsPerPixel / radiansPerPixel; concentric layers add their normal offset.
 * The curved horizontal offset is radius * sin(theta).
 */
struct CylinderWrap
{
    float sourceCenterX = .0f;  ///< Absolute ImDraw X of the arc apex.
    /// Curvature <= 1e-9 stays flat; negative values fail BuildProjection.
    float radiansPerPixel = .0f;
    float surfaceRadius = .0f;  ///< Radius in world units; zero stays flat, negative values fail.
    /// Positive chord count, clamped to MAX_SEGMENTS.
    int segmentCount = 1;
};

/**
 * @struct FisheyeWarp
 * @brief Row-local barrel magnification layered over the cylindrical surface.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Horizontal endpoints stay fixed. At full strength, vertical scale ranges from 1.18
 * at the midpoint to .74 at the endpoints. Zero strength or reciprocal half-width
 * disables warping; only the CPU mirror returns a bit-exact identity.
 */
struct FisheyeWarp
{
    ImVec2 center{};           ///< Absolute source-space center of this typography row.
    float invHalfWidth = .0f;  ///< Reciprocal row half-width; zero disables the warp.
    /// Strength from 0 to 1; out-of-range values make MakeCallbackParams return nullptr.
    float strength = .0f;
};

/**
 * @struct WorldPlane
 * @brief Plane placement expressed in game-world units.
 * @author Alex (<https://github.com/lextpf>)
 *
 * BuildProjection normalizes right, then orthogonalizes up against it.
 */
struct WorldPlane
{
    RE::NiPoint3 origin{};           ///< World point corresponding to sourceAnchor.
    RE::NiPoint3 right{};            ///< Page-right axis when viewed from the readable side.
    RE::NiPoint3 up{};               ///< Page-up axis.
    float worldUnitsPerPixel = .0f;  ///< World units per source pixel; must be above 0.
    CylinderWrap wrap{};             ///< Optional cylindrical wrap; zero curvature stays flat.
};

/**
 * @struct SourceBounds
 * @brief Absolute ImDraw coordinate bounds that the plane must contain.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Both extents must be positive; degenerate bounds fail BuildProjection.
 */
struct SourceBounds
{
    ImVec2 min{};  ///< Top-left corner, in absolute ImDraw coordinates.
    ImVec2 max{};  ///< Bottom-right corner; must be strictly greater than min on both axes.
};

/**
 * @struct InkMaterial
 * @brief View-dependent color treatment applied uniformly to all projected ink.
 * @author Alex (<https://github.com/lextpf>)
 *
 * MakeCallbackParams rejects desaturation, opacity or edgeSheen outside [0, 1],
 * and negative brightness. Brightness above 1 intentionally overbrightens.
 */
struct InkMaterial
{
    float desaturation = .0f;  ///< Blend RGB toward luminance [0, 1].
    float brightness = 1.0f;   ///< RGB multiplier after desaturation.
    float opacity = 1.0f;      ///< Uniform alpha multiplier [0, 1].
    /// White wing highlight from 0 to 1; flat plates have zero band width and no sheen.
    float edgeSheen = .0f;
};

/**
 * @struct DepthPlane
 * @brief Viewport-depth equation over normalized top-left screen coordinates.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Depth = xSlope*x01 + ySlope*y01 + constant. DepthClip uses this equation for
 * per-pixel occlusion of an angled plane.
 */
struct DepthPlane
{
    float xSlope = .0f;     ///< Depth change per unit of normalized screen x.
    float ySlope = .0f;     ///< Depth change per unit of normalized screen y, y downward.
    float constant = 1.0f;  ///< Depth at the top-left screen corner.

    /**
     * @fn float Sample(float x01, float y01) const
     * @brief Evaluate the plane at normalized screen coordinates.
     * @author Alex (<https://github.com/lextpf>)
     *
     * @param x01 Normalized screen X coordinate.
     * @param y01  Normalized screen Y coordinate.
     * @return     The plane value at the specified point.
     */
    float Sample(float x01, float y01) const { return xSlope * x01 + ySlope * y01 + constant; }
};

/**
 * @struct Projection
 * @brief Fully resolved per-plate projection.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Source coordinates are absolute ImDraw pixels. The shader subtracts sourceAnchor
 * before applying the selected homography. MakeCallbackParams copies the projection.
 */
struct Projection
{
    /**
     * @struct Segment
     * @brief Exact homography and depth plane for one chord strip.
     * @author Alex (<https://github.com/lextpf>)
     */
    struct Segment
    {
        /// Row-major local-pixel -> NDC homogeneous homography.
        std::array<float, 9> homography{1.0f, .0f, .0f, .0f, 1.0f, .0f, .0f, .0f, 1.0f};
        DepthPlane depth{};  ///< Exact viewport depth over this chord.
    };

    /// Ascending source-X order; entries at or beyond segmentCount are unused.
    std::array<Segment, MAX_SEGMENTS> segments{};
    ImVec2 sourceAnchor{};       ///< Absolute ImDrawVert position treated as plane-local (0,0).
    int segmentCount = 1;        ///< Number of valid chord projections.
    float firstBoundaryX = .0f;  ///< Absolute source X at the first chord boundary.
    float invStride = .0f;       ///< Reciprocal source width of one chord; 0 on a flat plate.
    float wingCenterX = .0f;     ///< Absolute source X of the sheen/wrap midpoint.
    /// Reciprocal half-width; zero disables wing sheen, including on flat plates.
    float wingInvHalfWidth = .0f;
    /// Caller assigns after BuildProjection resets it; warped positions are outside preflight.
    FisheyeWarp fisheye{};
    /// Exact flat depth or least-squares wrapped fit; wrapped residual is discarded.
    DepthPlane plateDepth{};
    bool valid = false;  ///< True only after BuildProjection succeeded.
};

/**
 * @fn bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
 * @brief Compile the plane vertex shader and allocate its constant buffer.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Compile vs_5_0 at runtime. Success retains device and context references until
 * Shutdown. Repeated calls return true without accepting a new device; call Shutdown
 * first on device changes. Failure releases partial resources and permits retry.
 *
 * @return True when the shader and the constant buffer exist.
 */
bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * @fn void Shutdown()
 * @brief Release all GPU resources and pending callback state.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Repeatable. Invalidates callback parameters; queued callbacks stay inert while
 * no context exists.
 */
void Shutdown();

/**
 * @fn bool IsInitialized()
 * @brief Report whether the plane shader is ready.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True after successful Initialize and before Shutdown.
 */
bool IsInitialized();

/**
 * @fn void BeginFrame()
 * @brief Clear the callback-parameter arena and the saved-state stack.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Call before building each frame. Clears callback parameters and any unmatched
 * saved-state entries; safe before Initialize.
 */
void BeginFrame();

/**
 * @fn bool BuildProjection(const WorldPlane& plane, const ImVec2& sourceAnchor, const SourceBounds&
 *     sourceBounds, Projection& out)
 * @brief Resolve a world plane against the current world camera.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Flat bounds use one homography; wrapped bounds use vertical chord strips. Invalid
 * inputs, missing camera, degenerate geometry, camera crossings or non-finite solves
 * leave out invalid. Source +Y maps toward -plane.up.
 *
 * Wrapping requires curvature > 1e-9, positive radius and at least two strips.
 * Try the requested count, half that count if still >= 2, then the flat path.
 *
 * Preflight compares the CPU shader mirror against camera projection at the flat anchor,
 * or the first, anchor-owning and last chord midpoints. Tolerances are 2e-4 NDC position
 * and 5e-4 viewport depth. Opposite strip winding rejects a folded wrap.
 *
 * ```mermaid
 * flowchart TD
 *     A[Clear output and validate inputs] --> V{Inputs, basis, and camera valid?}
 *     V -- No --> X[Return false; output stays invalid]
 *     V -- Yes --> W{Wrapped request with at least two strips?}
 *     W -- No --> F[Try the exact flat solve]
 *     W -- Yes --> R[Try the requested strip count]
 *     R --> P{Projection and preflight pass?}
 *     P -- Yes --> S[Return the valid projection]
 *     P -- No --> H{Half the count is at least two?}
 *     H -- Yes --> T[Retry once at half the count]
 *     H -- No --> F
 *     T --> Q{Projection and preflight pass?}
 *     Q -- Yes --> S
 *     Q -- No --> F
 *     F --> G{Flat projection and preflight pass?}
 *     G -- Yes --> S
 *     G -- No --> X
 * ```
 *
 * @param plane         World placement, physical scale, and optional cylindrical wrap for this
 * surface.
 * @param sourceAnchor  Absolute ImDraw position treated as plane-local (0, 0).
 * @param sourceBounds  Absolute ImDraw bounds the projection must cover.
 * @param out           Receives the resolved projection. It is cleared first, and the fisheye field
 * is left at {} for the caller to assign.
 * @return True when a flat or wrapped solve succeeded; false leaves @p out invalid.
 * @pre `plane`.right and `plane`.up need not be normalized or exactly orthogonal. `right` is
 * normalized first, then `up` is re-orthogonalized against it, so `right` wins any disagreement.
 * The readable normal is `right` cross `up`.
 * @pre `plane`.worldUnitsPerPixel must be greater than 0, so a default-constructed WorldPlane
 * always fails.
 * @post On success `out.segmentCount` can be lower than the requested strip count, because of the
 * retry or the flat fallback. Compare the two when several surfaces must share one strip layout.
 */
bool BuildProjection(const WorldPlane& plane,
                     const ImVec2& sourceAnchor,
                     const SourceBounds& sourceBounds,
                     Projection& out);

/**
 * @fn void* MakeCallbackParams(const Projection& projection, const InkMaterial& material)
 * @brief Copy projection/material constants into stable callback storage.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The returned frame-owned pointer remains valid until BeginFrame or Shutdown.
 * Pass it to ApplyCallback as ImDrawList::AddCallback user data.
 *
 * @param projection  Resolved plate projection to copy.
 * @param material    Ink treatment to copy.
 * @return Pointer suitable for ImDrawList::AddCallback, or nullptr when Graffito is not initialized
 * or the projection or material fails validation.
 */
void* MakeCallbackParams(const Projection& projection, const InkMaterial& material);

/**
 * @fn void ApplyCallback(const ImDrawList* drawList, const ImDrawCmd* command)
 * @brief Bind the Graffito VS for subsequent commands, preserving the previous VS state.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Write constants with a discard map, then save and replace the VS and CB slot 0.
 * Recoverable failures leave the prior shader bound, so geometry can still draw flat;
 * push an inactive marker to keep RestoreCallback paired. Without a context, both
 * callbacks return without a marker.
 *
 * @param command   Draw command whose UserCallbackData holds the params from MakeCallbackParams.
 */
void ApplyCallback(const ImDrawList* drawList, const ImDrawCmd* command);

/**
 * @fn void RestoreCallback(const ImDrawList* drawList, const ImDrawCmd* command)
 * @brief Restore the vertex shader and VS constant-buffer slot 0 of one bracket.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Pop one marker; inactive markers and an empty stack change no state.
 * Restore only the buffer binding, not its contents.
 *
 * @param command   Draw command. It is not used.
 */
void RestoreCallback(const ImDrawList* drawList, const ImDrawCmd* command);
}  // namespace Graffito
