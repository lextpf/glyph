#pragma once

#include <SKSE/SKSE.h>

/**
 * @namespace Occlusion
 * @brief Actor visibility and occlusion culling.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Occlusion
 *
 * Hides a plate after a successful negative line-of-sight result or a behind-camera test.
 * Missing inputs and failed engine queries leave the plate visible. This avoids hiding
 * actors when a visibility check cannot run.
 *
 * ### :material-arrow-decision: Thread affinity
 *
 * Actor and player queries run on the game thread. The snapshot carries their result
 * to the render thread. `GetCameraInfo` also permits the renderer's camera-only read
 * exception; consumers accept a one-frame mismatch between camera and actor samples.
 * `IsBehindCamera` uses only the supplied values and reads no engine state.
 *
 * ### :material-eye-off-outline: Occlusion flow
 *
 * ```mermaid
 * flowchart LR
 *     Inputs{Enabled and inputs available?} -->|No| Visible[Visible]
 *     Inputs -->|Yes| Near{Anchor within 100 units?}
 *     Near -->|Yes| Visible
 *     Near -->|No| Behind{Behind camera?}
 *     Behind -->|Yes| Hidden[Hidden]
 *     Behind -->|No| LOS{Successful negative LOS query?}
 *     LOS -->|Yes| Hidden
 *     LOS -->|No| Visible
 * ```
 *
 * The camera-to-anchor direction is normalized before its dot product with camera
 * forward is compared to the threshold. A strict dot < -0.2 test permits about
 * 11.5 degrees beyond the forward hemisphere and reduces flicker at screen edges.
 * Line of sight starts at the player; distance and facing use the camera.
 *
 * | Test                | Boundary                                  |
 * |---------------------|-------------------------------------------|
 * | Close anchor        | Distance < 100 world units stays visible. |
 * | Behind camera       | Dot < -0.2, about 101.5 degrees off-axis. |
 * | Undefined direction | Distance < 0.001 never counts as behind.  |
 */
namespace Occlusion
{
/**
 * @namespace Occlusion::Constants
 * @brief Constants for occlusion calculations.
 * @author Alex (<https://github.com/lextpf>)
 */
namespace Constants
{
inline constexpr float CLOSE_DISTANCE_THRESHOLD =
    100.0f;  ///< Visible when $\|p_{actor} - p_{cam}\| < 100$ game units
inline constexpr float BEHIND_CAMERA_DOT_THRESHOLD =
    -.2f;  ///< Behind camera when $\hat{f} \cdot \hat{d} < -0.2$ (~101.5 deg)
}  // namespace Constants

/**
 * @fn bool HasLineOfSightToActor(RE::Actor* actor)
 * @brief Query player-to-actor line of sight through world geometry.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True for visible actors, null actors, absent player, or failed engine queries.
 * @pre Game thread; dereferences actor and player.
 */
bool HasLineOfSightToActor(RE::Actor* actor);

/**
 * @fn bool IsActorOccluded(RE::Actor* actor, RE::Actor* player, const RE::NiPoint3& actorWorldPos,
 *     bool occlusionEnabled)
 * @brief Combine camera distance, facing, and line of sight.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param player Liveness guard only; LOS always starts from the player singleton.
 * @param actorWorldPos Nameplate anchor in world units; tests measure from the camera.
 * @return True to hide. False also covers disabled culling, null actors/player,
 * or unavailable camera data; it does not prove visibility.
 * @pre Game thread; publish the result to the render snapshot.
 */
bool IsActorOccluded(RE::Actor* actor,
                     RE::Actor* player,
                     const RE::NiPoint3& actorWorldPos,
                     bool occlusionEnabled);

/**
 * @fn bool GetCameraInfo(RE::NiPoint3& outPos, RE::NiPoint3& outForward)
 * @brief Read camera position and its world-space unit forward axis.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @param[out] outPos Camera position, in world units.
 * @param[out] outForward Rotation column 1, independent of player facing.
 * @return False if unavailable; neither output is written on failure.
 * @note Game-thread callers and render-thread camera consumers may call this function.
 * The camera read has no engine lock; this exception permits no actor or cell reads.
 */
bool GetCameraInfo(RE::NiPoint3& outPos, RE::NiPoint3& outForward);

/**
 * @fn bool IsBehindCamera(const RE::NiPoint3& worldPos, const RE::NiPoint3& cameraPos, const
 *     RE::NiPoint3& cameraForward)
 * @brief Test whether a position lies past the camera visibility cone.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The threshold is dot < -0.2, about 101.5 degrees, to avoid popping at screen edges.
 * @return False within 0.001 game units, where direction is undefined.
 * @pre `cameraForward` is normalized; only the camera-to-position vector is normalized here.
 */
bool IsBehindCamera(const RE::NiPoint3& worldPos,
                    const RE::NiPoint3& cameraPos,
                    const RE::NiPoint3& cameraForward);

}  // namespace Occlusion
