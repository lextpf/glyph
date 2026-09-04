// Render-thread frame orchestration. Cached plates follow this lifecycle:
//
//   in the snapshot, alive   -> DrawLabel: seed the cache entry, run the entrance
//                               and the typewriter, smooth, project, draw
//   in the snapshot, dead    -> DrawDyingLabel: one-shot rite of hold, ink drain and
//                               creature farewell. Needs DeathRiteEnabled and a plate
//                               this overlay saw alive; it plays at most once
//   left the snapshot        -> DrawExitingLabel: fade and sink over ExitDuration.
//                               Needs EnableExitAnimation and a completed entrance
//   idle                     -> PruneCacheToSnapshot: erase the entry after
//                               CACHE_GRACE_FRAMES idle frames, or 3x that while an
//                               exit is still playing
//
// Rites and exits replay lastDrawData; finishing a rite disables the exit.
// Render-thread RE reads are limited to camera and BSGraphics singletons. Actor facts
// arrive only through ActorDrawData.

#include "Renderer.hpp"

#include "RendererInternal.hpp"

#include "BadgeTextures.hpp"
#include "Deck.hpp"
#include "DepthClip.hpp"
#include "GameState.hpp"
#include "Graffito.hpp"
#include "GraffitoMath.hpp"
#include "Occlusion.hpp"
#include "ParticleTextures.hpp"
#include "RenderSampling.hpp"
#include "SceneMeter.hpp"
#include "TextPostProcess.hpp"
#include "TierEmblem.hpp"

#include <SKSE/SKSE.h>

#include <cctype>
#include <chrono>
#include <cmath>

namespace Renderer
{

RendererState& GetState()
{
    static RendererState instance;
    return instance;
}

std::unordered_map<uint32_t, OcclusionCacheEntry>& GetOcclusionCache()
{
    static std::unordered_map<uint32_t, OcclusionCacheEntry> instance;
    return instance;
}

std::unordered_map<uint32_t, float>& OverlapOffsets()
{
    static std::unordered_map<uint32_t, float> offsets;
    return offsets;
}

bool IsOverlayAllowedRT()
{
    return GetState().manualEnabled.load(std::memory_order_acquire) &&
           GetState().allowOverlay.load(std::memory_order_acquire);
}

bool ToggleEnabled()
{
    bool expected = GetState().manualEnabled.load(std::memory_order_relaxed);
    while (!GetState().manualEnabled.compare_exchange_weak(
        expected, !expected, std::memory_order_acq_rel, std::memory_order_relaxed))
    {
    }
    return !expected;
}

void SetEnabled(bool enabled)
{
    GetState().manualEnabled.store(enabled, std::memory_order_release);
}

bool IsEnabled()
{
    return GetState().manualEnabled.load(std::memory_order_acquire);
}

void RequestIdentityRefresh()
{
    GetState().pendingIdentityRefresh.store(true, std::memory_order_release);
}

ImFont* GetFontAt(int index)
{
    auto& io = ImGui::GetIO();
    if (!io.Fonts || io.Fonts->Fonts.Size <= 0)
    {
        return nullptr;
    }
    if (index >= 0 && index < io.Fonts->Fonts.Size)
    {
        if (auto* font = io.Fonts->Fonts[index])
        {
            return font;
        }
    }
    return io.Fonts->Fonts[0];
}

void PruneCacheToSnapshot(const std::vector<ActorDrawData>& snap)
{
    // Grace period prevents jitter when actors briefly leave the snapshot
    constexpr uint32_t CACHE_GRACE_FRAMES = RenderConstants::CACHE_GRACE_FRAMES;
    std::unordered_set<uint32_t> visibleFormIDs;
    visibleFormIDs.reserve(snap.size());
    for (const auto& d : snap)
    {
        visibleFormIDs.insert(d.formID);
    }

    for (auto it = GetState().cache.begin(); it != GetState().cache.end();)
    {
        const bool inSnapshot = visibleFormIDs.find(it->first) != visibleFormIDs.end();
        if (inSnapshot)
        {
            it->second.lastSeenFrame = GetState().frame;
        }

        if (!inSnapshot)
        {
            // Bound exit retention even when exit animation is disabled mid-transition.
            const uint32_t framesSinceLastSeen = GetState().frame - it->second.lastSeenFrame;
            const bool midExit = it->second.exitPhase > .0f && it->second.exitPhase < 1.0f;
            if (framesSinceLastSeen > CACHE_GRACE_FRAMES &&
                (!midExit || framesSinceLastSeen > CACHE_GRACE_FRAMES * 3))
            {
                GetState().lastDrawData.erase(it->first);
                it = GetState().cache.erase(it);
                continue;
            }
        }
        ++it;
    }
}

float ExpApproachAlpha(float dt, float settleTime, float epsilon)
{
    dt = std::max(.0f, dt);
    settleTime = std::max(1e-5f, settleTime);
    return std::clamp(1.0f - std::pow(epsilon, dt / settleTime), .0f, 1.0f);
}

/**
 * @fn static void ResetTrailHistory(ActorCache& entry, const RE::NiPoint3* seedWorldPos = nullptr)
 * @brief Clear the world-position trail and optionally seed its first sample.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Seeded resets leave sample 0 valid; the next push can draw a trail.
 */
static void ResetTrailHistory(ActorCache& entry, const RE::NiPoint3* seedWorldPos = nullptr)
{
    const RE::NiPoint3 initPos = seedWorldPos ? *seedWorldPos : RE::NiPoint3{};
    for (int i = 0; i < ActorCache::TRAIL_HISTORY_SIZE; ++i)
    {
        entry.trailHistory[i] = initPos;
    }
    entry.trailIndex = seedWorldPos ? 1 : 0;
    entry.trailFilled = false;
}

/**
 * @fn static DistanceFactors ComputeDistanceFactors(const ActorDrawData& d, const
 *     RenderSettingsSnapshot& snap)
 * @brief Resolve distance fade, text scale, and effect detail factors.
 * @author Alex (<https://github.com/lextpf>)
 */
static DistanceFactors ComputeDistanceFactors(const ActorDrawData& d,
                                              const RenderSettingsSnapshot& snap)
{
    DistanceFactors df{};
    const float dist = d.distToPlayer;

    const float regFade = GetState().regFadeMul;
    const float fadeStart = snap.fadeStartDistance * regFade;
    const float fadeEnd = snap.fadeEndDistance * regFade;

    // Alpha fade: the complement of a smoothstep, then squared, so the far tail
    // thins faster than a plain smoothstep. Distances are Skyrim world units.
    const float fadeRange = std::max(1.0f, fadeEnd - fadeStart);
    float fadeT = TextEffects::SmoothStep((dist - fadeStart) / fadeRange);
    df.alphaTarget = 1.0f - fadeT;
    df.alphaTarget = df.alphaTarget * df.alphaTarget;

    df.lodTitleFactor = 1.0f;
    df.lodEffectsFactor = 1.0f;

    if (snap.visual.EnableLOD)
    {
        float transRange = std::max(1.0f, snap.visual.LODTransitionRange);
        float titleFadeT = TextEffects::Saturate((dist - snap.visual.LODFarDistance) / transRange);
        df.lodTitleFactor = 1.0f - TextEffects::SmoothStep(titleFadeT);
        float effectsFadeT =
            TextEffects::Saturate((dist - snap.visual.LODMidDistance) / transRange);
        df.lodEffectsFactor = 1.0f - TextEffects::SmoothStep(effectsFadeT);
    }

    const float scaleRange = std::max(1.0f, snap.scaleEndDistance - snap.scaleStartDistance);
    float scaleT = TextEffects::Saturate((dist - snap.scaleStartDistance) / scaleRange);
    constexpr float SCALE_GAMMA = .5f;
    scaleT = std::pow(scaleT, SCALE_GAMMA);
    df.textScaleTarget = 1.0f + (snap.minimumScale - 1.0f) * scaleT;

    if (auto pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot)
    {
        RE::NiPoint3 cameraPos = pc->cameraRoot->world.translate;
        const float dx = d.worldPos.x - cameraPos.x;
        const float dy = d.worldPos.y - cameraPos.y;
        const float dz = d.worldPos.z - cameraPos.z;
        float camDist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float camScaleT = TextEffects::Saturate((camDist - snap.scaleStartDistance) / scaleRange);
        camScaleT = std::pow(camScaleT, SCALE_GAMMA);
        float camTextScale = 1.0f + (snap.minimumScale - 1.0f) * camScaleT;
        df.textScaleTarget = std::min(df.textScaleTarget, camTextScale);
    }

    if (snap.visual.MinimumPixelHeight > .0f)
    {
        float minScale = snap.visual.MinimumPixelHeight / snap.nameFontSize;
        df.textScaleTarget = std::max(df.textScaleTarget, minScale);
    }

    return df;
}

bool WorldToScreen(const RE::NiPoint3& worldPos,
                   RE::NiPoint3& screenPos,
                   RE::NiPoint3* cameraPosOut)
{
    auto* cam = RE::Main::WorldRootCamera();
    if (!cam)
    {
        return false;
    }

    const auto& rt = cam->GetRuntimeData();
    const auto& rt2 = cam->GetRuntimeData2();

    if (cameraPosOut)
    {
        *cameraPosOut = cam->world.translate;
    }

    float x = .0f, y = .0f, z = .0f;
    if (!RE::NiCamera::WorldPtToScreenPt3(rt.worldToCam, rt2.port, worldPos, x, y, z, 1e-5f))
    {
        return false;
    }

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return false;
    }
    const auto ss = renderer->GetScreenSize();
    const float w = static_cast<float>(ss.width);
    const float h = static_cast<float>(ss.height);

    screenPos.x = x * w;
    screenPos.y = (1.0f - y) * h;
    screenPos.z = z;
    return true;
}

/**
 * @fn static float QuietTarget(float degPerSec, float lo, float hi)
 * @brief Map camera angular speed to a bounded quieting target.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Camera speed in deg/s maps to [0,1]. Mirrored in tests/test_utils.cpp; keep in sync.
 */
static float QuietTarget(float degPerSec, float lo, float hi)
{
    if (hi <= lo)
    {
        return degPerSec >= hi ? 1.0f : .0f;
    }
    return TextEffects::SmoothStep(TextEffects::Saturate((degPerSec - lo) / (hi - lo)));
}

/**
 * @fn static void UpdateQuietFrame(const RenderSettingsSnapshot& snap, float dt)
 * @brief Advance the camera-motion envelope for secondary plate elements.
 * @author Alex (<https://github.com/lextpf>)
 *
 * quietSub folds title/badges on live and exit plates, info/badges during rites.
 * Attack is faster than release. quietName advances but has no draw consumer.
 */
static void UpdateQuietFrame(const RenderSettingsSnapshot& snap, float dt)
{
    auto& st = GetState();
    if (!snap.quiet.Enabled)
    {
        st.quietName = .0f;
        st.quietSub = .0f;
        st.prevCamValid = false;
        return;
    }

    float target = .0f;
    RE::NiPoint3 camPos{};
    RE::NiPoint3 camFwd{};
    if (Occlusion::GetCameraInfo(camPos, camFwd) && dt > 1e-5f)
    {
        if (st.prevCamValid)
        {
            constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
            const float dot =
                std::clamp(camFwd.x * st.prevCamForward.x + camFwd.y * st.prevCamForward.y +
                               camFwd.z * st.prevCamForward.z,
                           -1.0f,
                           1.0f);
            const float degPerSec = std::acos(dot) * kRadToDeg / dt;
            target = QuietTarget(degPerSec, snap.quiet.PanThresholdLo, snap.quiet.PanThresholdHi);
        }
        st.prevCamForward = camFwd;
        st.prevCamValid = true;
    }
    else
    {
        st.prevCamValid = false;
    }

    const float nameSettle =
        target > st.quietName ? snap.quiet.AttackTime : snap.quiet.NameReleaseTime;
    const float subSettle =
        target > st.quietSub ? snap.quiet.AttackTime : snap.quiet.SubReleaseTime;
    st.quietName += (target - st.quietName) * ExpApproachAlpha(dt, nameSettle);
    st.quietSub += (target - st.quietSub) * ExpApproachAlpha(dt, subSettle);
}

/**
 * @fn static float ComputeDepthPolarity()
 * @brief Resolve standard or reversed depth from two camera-space probes.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Compare two projected depths: +1 standard, -1 reversed, 0 indeterminate.
 * Indeterminate depth skips clipping for this frame.
 */
static float ComputeDepthPolarity()
{
    RE::NiPoint3 camPos{};
    RE::NiPoint3 camFwd{};
    if (!Occlusion::GetCameraInfo(camPos, camFwd))
    {
        return .0f;
    }

    const RE::NiPoint3 nearPt{
        camPos.x + camFwd.x * 50.0f, camPos.y + camFwd.y * 50.0f, camPos.z + camFwd.z * 50.0f};
    const RE::NiPoint3 farPt{
        camPos.x + camFwd.x * 500.0f, camPos.y + camFwd.y * 500.0f, camPos.z + camFwd.z * 500.0f};
    RE::NiPoint3 sNear{};
    RE::NiPoint3 sFar{};
    if (!WorldToScreen(nearPt, sNear) || !WorldToScreen(farPt, sFar))
    {
        return .0f;
    }
    const float delta = sFar.z - sNear.z;
    if (std::abs(delta) < 1e-7f)
    {
        return .0f;
    }
    return delta > .0f ? 1.0f : -1.0f;
}

/**
 * @fn static void BracketPlateDepthClip(ImDrawList* drawList, ImDrawListSplitter* splitter, void*
 *     params, const RenderSettingsSnapshot& snap)
 * @brief Start plate depth clipping on each active draw channel.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Each channel needs its own depth shader and plate constants after splitting.
 */
static void BracketPlateDepthClip(ImDrawList* drawList,
                                  ImDrawListSplitter* splitter,
                                  void* params,
                                  const RenderSettingsSnapshot& snap)
{
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int channelCount = gpuGlow ? 3 : 2;
    for (int c = 0; c < channelCount; ++c)
    {
        splitter->SetCurrentChannel(drawList, c);
        drawList->AddCallback(DepthClip::ApplyCallback, params);
    }
}

/**
 * @fn static void EndPlateDepthClip(ImDrawList* drawList, ImDrawListSplitter* splitter, const
 *     RenderSettingsSnapshot& snap)
 * @brief Restore neutral depth clipping on each active draw channel.
 * @author Alex (<https://github.com/lextpf>)
 */
static void EndPlateDepthClip(ImDrawList* drawList,
                              ImDrawListSplitter* splitter,
                              const RenderSettingsSnapshot& snap)
{
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int channelCount = gpuGlow ? 3 : 2;
    for (int c = 0; c < channelCount; ++c)
    {
        splitter->SetCurrentChannel(drawList, c);
        drawList->AddCallback(DepthClip::RestoreCallback, nullptr);
    }
}

// Font design pixels to Skyrim world units (roughly 70 units per metre).
inline constexpr float GRAFFITO_WORLD_UNITS_PER_PIXEL = .10f;
inline constexpr std::size_t GRAFFITO_RELIEF_LAYERS = Graffito::Math::FOLIO_RELIEF_STEPS.size();
// Reduce player wrap and lens strength because the player always faces the camera.
inline constexpr float PLAYER_GRAFFITO_DIMENSIONAL_SCALE = .12f;

/**
 * @struct GraffitoSurface
 * @brief One projected folio face with matching source and depth parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
struct GraffitoSurface
{
    bool active = false;
    Graffito::WorldPlane worldPlane{};
    Graffito::InkMaterial material{};
    ImVec2 sourceAnchor{};
    Graffito::SourceBounds sourceBounds{};
    Graffito::Projection projection{};
    void* callbackParams = nullptr;
};

/**
 * @struct GraffitoPlate
 * @brief World anchors and measured surfaces for one projected plate.
 * @author Alex (<https://github.com/lextpf>)
 */
struct GraffitoPlate
{
    bool requested = false;  // Setting applies to this actor.
    bool active = false;     // A world anchor is ready, then any finalized face is ready.
    float visibility = 1.0f;
    RE::NiPoint3 screenPos{};
    RE::NiPoint3 headScreenPos{};
    ImVec2 sourceAnchor{};
    ImVec2 headSourceAnchor{};
    Graffito::Math::PlanePose pose{};
    Graffito::Math::Vec3 cameraPosition{};
    Graffito::Math::Vec3 headPosition{};
    Graffito::Math::FacingMaterial legacyMaterial{};
    float worldUnitsPerPixel = .0f;
    Graffito::SourceBounds contentBounds{};
    Graffito::SourceBounds facetDrawBounds{};
    float facetRankSize = .0f;
    GraffitoSurface front{};
    GraffitoSurface recessedInk{};
    GraffitoSurface recessedInfo{};
    GraffitoSurface raisedMarks{};
    GraffitoSurface raisedEmblem{};
    std::array<GraffitoSurface, GRAFFITO_RELIEF_LAYERS> relief{};
    GraffitoSurface facet{};

    // Use a provisional zero hinge until layout supplies the inscription top edge.
    float epitaphProgress = .0f;
    double poseYaw = .0;
    Graffito::Math::Vec3 uprightOrigin{};
    Graffito::Math::Vec3 groundHinge{};
};

/**
 * @fn static Graffito::Math::Vec3 ToGraffitoMath(const RE::NiPoint3& point)
 * @brief Copy world coordinates into the double-precision geometry type.
 * @author Alex (<https://github.com/lextpf>)
 */
static Graffito::Math::Vec3 ToGraffitoMath(const RE::NiPoint3& point)
{
    return {
        static_cast<double>(point.x), static_cast<double>(point.y), static_cast<double>(point.z)};
}

/**
 * @fn static RE::NiPoint3 ToEnginePoint(const Graffito::Math::Vec3& point)
 * @brief Convert geometry coordinates to the engine float value type.
 * @author Alex (<https://github.com/lextpf>)
 */
static RE::NiPoint3 ToEnginePoint(const Graffito::Math::Vec3& point)
{
    return {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)};
}

/**
 * @fn static double PoseClockSeconds()
 * @brief Read steady-clock seconds shared by pose capture and prediction.
 * @author Alex (<https://github.com/lextpf>)
 */
static double PoseClockSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/**
 * @fn static void ResolvePlayerRenderPose(ActorCache& entry, ActorDrawData& d)
 * @brief Extrapolate the player pose between game-thread samples.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Uses steady-clock sample age, independent of the ImGui animation clock. The predicted
 * head offset also moves the feet so a plate keeps its shape. New samples after a gap
 * above 0.25 seconds or a jump above 256 world units reset the velocity estimate.
 */
static void ResolvePlayerRenderPose(ActorCache& entry, ActorDrawData& d)
{
    if (!d.isPlayer || !std::isfinite(d.poseSampleTime) || d.poseSampleTime <= .0)
    {
        return;
    }

    // Sample times and gaps are steady-clock seconds; velocity response is unitless.
    // A gap > 0.25 s or jump > 256 world units reseeds. Cadence clamps to 20-1000 Hz.
    constexpr double SAMPLE_EPSILON = 1e-6;
    constexpr double MAX_SAMPLE_GAP = .25;
    constexpr double VELOCITY_RESPONSE = .82;
    constexpr double TELEPORT_DISTANCE = 256.0;

    const auto sample = ToGraffitoMath(d.worldPos);
    const auto resetTracker = [&]()
    {
        entry.playerPoseSample = d.worldPos;
        entry.playerPoseVelocity = {};
        entry.playerPoseSampleTime = d.poseSampleTime;
        entry.playerPoseInterval = 1.0 / 60.0;
        entry.playerPoseInitialized = true;
    };

    if (!Graffito::Math::IsFinite(sample))
    {
        return;
    }
    if (!entry.playerPoseInitialized ||
        d.poseSampleTime + SAMPLE_EPSILON < entry.playerPoseSampleTime)
    {
        resetTracker();
    }
    else if (d.poseSampleTime > entry.playerPoseSampleTime + SAMPLE_EPSILON)
    {
        const double sampleDelta = d.poseSampleTime - entry.playerPoseSampleTime;
        const auto previousSample = ToGraffitoMath(entry.playerPoseSample);
        const auto delta = sample - previousSample;
        const double distanceSquared = Graffito::Math::LengthSquared(delta);
        if (!std::isfinite(sampleDelta) || sampleDelta > MAX_SAMPLE_GAP ||
            !std::isfinite(distanceSquared) ||
            distanceSquared > TELEPORT_DISTANCE * TELEPORT_DISTANCE)
        {
            resetTracker();
        }
        else
        {
            const auto velocity =
                Graffito::Math::BlendMotionVelocity(ToGraffitoMath(entry.playerPoseVelocity),
                                                    previousSample,
                                                    sample,
                                                    sampleDelta,
                                                    VELOCITY_RESPONSE);
            entry.playerPoseSample = d.worldPos;
            entry.playerPoseVelocity = ToEnginePoint(velocity);
            entry.playerPoseSampleTime = d.poseSampleTime;
            entry.playerPoseInterval = std::clamp(sampleDelta, .001, .050);
        }
    }

    const double sampleAge = std::max(.0, PoseClockSeconds() - entry.playerPoseSampleTime);
    const auto rawSample = ToGraffitoMath(entry.playerPoseSample);
    const auto predicted = Graffito::Math::PredictMotionPosition(
        rawSample, ToGraffitoMath(entry.playerPoseVelocity), sampleAge, entry.playerPoseInterval);
    const auto offset = predicted - rawSample;
    d.worldPos = ToEnginePoint(predicted);
    d.feetPos += ToEnginePoint(offset);
}

/**
 * @fn static GraffitoPlate PrepareGraffitoPlate(const ActorDrawData& d, ActorCache& entry, const
 *     RenderSettingsSnapshot& snap, float dt, float epitaphProgress = .0f, bool forceReadableFront
 *     = false)
 * @brief Resolve a world anchor and visibility before plate measurement.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Prepare yaw, anchor and visibility before layout. Inactive Graffito falls back
 * to billboards only when disabled or uninitialized; projection/visibility failures
 * skip the actor. FinalizeGraffitoPlate builds faces after measurement.
 */
static GraffitoPlate PrepareGraffitoPlate(const ActorDrawData& d,
                                          ActorCache& entry,
                                          const RenderSettingsSnapshot& snap,
                                          float dt,
                                          float epitaphProgress = .0f,
                                          bool forceReadableFront = false)
{
    GraffitoPlate plate;
    plate.requested = snap.graffito.Enabled;
    if (!plate.requested || !Graffito::IsInitialized())
    {
        return plate;  // Unsupported GPU setup: preserve the billboard fallback.
    }

    plate.headPosition = ToGraffitoMath(d.worldPos);
    RE::NiPoint3 cameraPos{};
    if (!WorldToScreen(d.worldPos, plate.headScreenPos, &cameraPos))
    {
        return plate;
    }
    plate.cameraPosition = ToGraffitoMath(cameraPos);
    plate.headSourceAnchor = ImVec2(plate.headScreenPos.x, plate.headScreenPos.y);

    // Make the player and aimed actor readable without waiting for actor yaw.
    forceReadableFront = forceReadableFront || d.isPlayer;
    float targetYaw = d.headingRadians;
    if (forceReadableFront)
    {
        targetYaw = static_cast<float>(
            Graffito::Math::YawFacingPoint(plate.headPosition, plate.cameraPosition, targetYaw));
    }

    if (forceReadableFront || !entry.graffitoYawInitialized)
    {
        entry.graffitoYaw = targetYaw;
        entry.graffitoYawInitialized = true;
    }
    else
    {
        entry.graffitoYaw = static_cast<float>(Graffito::Math::SmoothAngle(
            entry.graffitoYaw, targetYaw, dt, snap.graffito.OrientationSettleTime));
    }

    const auto upright = Graffito::Math::BuildUprightBasis(entry.graffitoYaw);
    const RE::NiPoint3 forward = ToEnginePoint(upright.forward);
    const float actorScale = d.isPlayer ? snap.graffito.PlayerScale : 1.0f;
    const double worldUnitsPerPixel =
        static_cast<double>(GRAFFITO_WORLD_UNITS_PER_PIXEL * snap.graffito.Scale * actorScale);
    plate.epitaphProgress = std::clamp(epitaphProgress, .0f, 1.0f);
    plate.poseYaw = entry.graffitoYaw;
    plate.uprightOrigin = ToGraffitoMath(d.worldPos + forward * snap.graffito.ForwardOffset);
    RE::NiPoint3 ground = d.feetPos + forward * snap.graffito.ForwardOffset;
    ground.z += snap.graffito.EpitaphGroundLift;
    plate.groundHinge = ToGraffitoMath(ground);

    plate.pose = Graffito::Math::BuildFallenEpitaphPose(plate.poseYaw,
                                                        plate.epitaphProgress,
                                                        plate.uprightOrigin,
                                                        plate.groundHinge,
                                                        {},
                                                        worldUnitsPerPixel);
    plate.worldUnitsPerPixel = static_cast<float>(worldUnitsPerPixel);

    if (!WorldToScreen(ToEnginePoint(plate.pose.origin), plate.screenPos))
    {
        return plate;
    }
    const float range =
        snap.graffito.MaxDistance <= .0f
            ? 1.0f
            : static_cast<float>(Graffito::Math::RangeFade(
                  d.distToPlayer, snap.graffito.MaxDistance * .8f, snap.graffito.MaxDistance));
    plate.visibility = range;
    if (!snap.graffito.FolioEnabled)
    {
        Graffito::Math::Vec3 view{};
        if (!Graffito::Math::Normalize(plate.cameraPosition - plate.pose.origin, view))
        {
            return plate;
        }
        plate.legacyMaterial =
            Graffito::Math::EvaluateFacingMaterial(Graffito::Math::Dot(plate.pose.normal, view),
                                                   snap.graffito.FacingFadeDegrees,
                                                   snap.graffito.BacksideBleedAlpha,
                                                   snap.graffito.EdgeSeamAlpha);
        plate.visibility *= static_cast<float>(plate.legacyMaterial.opacity);
    }
    if (plate.visibility <= .001f)
    {
        return plate;
    }

    plate.sourceAnchor = ImVec2(plate.screenPos.x, plate.screenPos.y);
    plate.active = true;
    return plate;
}

/**
 * @fn static Graffito::SourceBounds ComputeGraffitoContentBounds(const LabelLayout& layout)
 * @brief Enclose the text and nameplate geometry before effect padding.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Screen-pixel union of text and nameplate bounds, before effect padding.
 */
static Graffito::SourceBounds ComputeGraffitoContentBounds(const LabelLayout& layout)
{
    const ImVec2 textMin = layout.startPos + layout.textBoundsMin;
    const ImVec2 textMax = layout.startPos + layout.textBoundsMax;
    return {
        {std::min(textMin.x, layout.nameplateLeft), std::min(textMin.y, layout.nameplateTop)},
        {std::max(textMax.x, layout.nameplateRight), std::max(textMax.y, layout.nameplateBottom)}};
}

/**
 * @fn static Graffito::SourceBounds ComputeGraffitoSourceBounds(const LabelLayout& layout, const
 *     LabelStyle& style, const RenderSettingsSnapshot& snap)
 * @brief Pad plate bounds to retain all projected effect extents.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Include all effect extents so the plane sampler cannot clip them.
 */
static Graffito::SourceBounds ComputeGraffitoSourceBounds(const LabelLayout& layout,
                                                          const LabelStyle& style,
                                                          const RenderSettingsSnapshot& snap)
{
    const float outlineRadius =
        std::max({.0f, style.nameOutlineWidth, style.levelOutlineWidth, style.titleOutlineWidth});
    float effectRadius = outlineRadius;

    if (style.outlineGlowAllowed && snap.enableOutlineGlow && snap.outlineGlowAlpha > .0f &&
        snap.outlineGlowRings > 0)
    {
        const float outerRingScale = snap.outlineGlowRings > 1 ? 1.6f : 1.0f;
        effectRadius =
            std::max(effectRadius, outlineRadius * snap.outlineGlowScale * outerRingScale);
    }

    if (snap.dualOutlineEnabled && snap.innerOutlineAlpha > .0f)
    {
        const float directionalScale =
            snap.fastOutlines ? 1.0f : 1.0f + std::abs(snap.directionalLightBias);
        effectRadius =
            std::max(effectRadius, outlineRadius * snap.innerOutlineScale * directionalScale);
    }

    const bool waveEnabled =
        snap.visual.EnableWave && style.usesTierVisuals && style.tierIdx >= snap.visual.WaveMinTier;
    const float waveRadius = waveEnabled ? std::abs(snap.visual.WaveAmplitude) : .0f;

    float left = effectRadius;
    float right = effectRadius;
    float top = effectRadius + waveRadius;
    float bottom = effectRadius + waveRadius;

    const float spacingScale = layout.fontName && layout.fontName->FontSize > 1e-4f
                                   ? layout.nameFontSize / layout.fontName->FontSize
                                   : 1.0f;
    const auto includeShadow = [&](float offsetX, float offsetY, float softness)
    {
        const float radius = std::max(.0f, softness);
        left = std::max(left, std::max(.0f, radius - offsetX));
        right = std::max(right, std::max(.0f, radius + offsetX));
        top = std::max(top, std::max(.0f, radius - offsetY));
        bottom = std::max(bottom, std::max(.0f, radius + offsetY));
    };

    if (snap.softShadowEnabled && snap.softShadowOpacity > .01f)
    {
        constexpr float DEGREES_TO_RADIANS = .01745329252f;
        const float angle = snap.softShadowAngle * DEGREES_TO_RADIANS;
        const float distance = snap.softShadowDistance * spacingScale;
        includeShadow(std::cos(angle) * distance,
                      std::sin(angle) * distance,
                      snap.softShadowSoftness * spacingScale);
    }
    else
    {
        includeShadow(
            snap.titleShadowOffsetX * spacingScale, snap.titleShadowOffsetY * spacingScale, .0f);
        includeShadow(
            snap.mainShadowOffsetX * spacingScale, snap.mainShadowOffsetY * spacingScale, .0f);
    }

    // Covers font side bearings and float-to-raster rounding.
    constexpr float RASTER_SAFETY_PX = 1.0f;
    left += RASTER_SAFETY_PX;
    right += RASTER_SAFETY_PX;
    top += RASTER_SAFETY_PX;
    bottom += RASTER_SAFETY_PX;

    const auto content = ComputeGraffitoContentBounds(layout);
    return {{content.min.x - left, content.min.y - top},
            {content.max.x + right, content.max.y + bottom}};
}

/**
 * @fn static void SpreadGraffitoRows(LabelLayout& layout, float dimensionalScale)
 * @brief Separate rows around the lens-enlarged name without detaching badges.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Separate rows around the lens-enlarged name; move badges with the upper text.
 */
static void SpreadGraffitoRows(LabelLayout& layout, float dimensionalScale)
{
    const float gap = std::clamp(layout.nameFontSize * .035f, 3.0f, 8.0f) *
                      std::clamp(dimensionalScale, .0f, 1.0f);
    if (gap <= .01f)
    {
        return;
    }
    const bool hasTitle = !layout.titleStr.empty();
    const bool hasUpperGroup = hasTitle || !layout.badges.empty() || layout.tierEmblemShown;

    if (hasTitle)
    {
        layout.titleY -= gap;
        layout.textBoundsMin.y -= gap;
    }
    for (auto& badge : layout.badges)
    {
        badge.pos.y -= gap;
    }
    if (layout.tierEmblemShown)
    {
        layout.tierEmblemPos.y -= gap;
    }
    if (hasUpperGroup)
    {
        layout.nameplateTop -= gap;
    }

    if (!layout.infoSegments.empty())
    {
        layout.infoLineY += gap;
        layout.textBoundsMax.y += gap;
        layout.nameplateBottom += gap;
    }

    layout.nameplateHeight = layout.nameplateBottom - layout.nameplateTop;
    layout.nameplateCenter.y = (layout.nameplateTop + layout.nameplateBottom) * .5f;
}

/**
 * @fn static bool FinalizeGraffitoPlate(GraffitoPlate& plate, LabelLayout& layout, const
 *     LabelStyle& style, const RenderSettingsSnapshot& snap)
 * @brief Build plate faces from measured bounds and advance the epitaph hinge.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Finalize projections after measurement and before drawing. Mutates row spacing
 * and the epitaph hinge. False means no face survived; skip the actor.
 */
static bool FinalizeGraffitoPlate(GraffitoPlate& plate,
                                  LabelLayout& layout,
                                  const LabelStyle& style,
                                  const RenderSettingsSnapshot& snap)
{
    if (!plate.active)
    {
        return false;
    }

    const float dimensionalScale = layout.isPlayer ? PLAYER_GRAFFITO_DIMENSIONAL_SCALE : 1.0f;
    SpreadGraffitoRows(layout, snap.graffito.FisheyeStrength > .0f ? dimensionalScale : .0f);
    const Graffito::SourceBounds bounds = ComputeGraffitoSourceBounds(layout, style, snap);
    plate.contentBounds = ComputeGraffitoContentBounds(layout);
    if (plate.epitaphProgress > .0f)
    {
        // Hinge on text: fading status marks must not leave the pivot in empty space.
        const ImVec2 textMin = layout.startPos + layout.textBoundsMin;
        const ImVec2 textMax = layout.startPos + layout.textBoundsMax;
        const Graffito::Math::Vec2 sourceHinge{
            static_cast<double>((textMin.x + textMax.x) * .5f - plate.sourceAnchor.x),
            static_cast<double>(textMin.y - plate.sourceAnchor.y)};
        plate.pose =
            Graffito::Math::BuildFallenEpitaphPose(plate.poseYaw,
                                                   plate.epitaphProgress,
                                                   plate.uprightOrigin,
                                                   plate.groundHinge,
                                                   sourceHinge,
                                                   static_cast<double>(plate.worldUnitsPerPixel));
    }

    Graffito::Math::Vec3 view{};
    if (!Graffito::Math::Normalize(plate.cameraPosition - plate.pose.origin, view))
    {
        plate.active = false;
        return false;
    }

    const double frontDot = Graffito::Math::Dot(plate.pose.normal, view);
    const double rightDot = Graffito::Math::Dot(plate.pose.right, view);
    const bool allowDimensionalFront = plate.epitaphProgress <= .0f;
    const float arcCenterX = (plate.contentBounds.min.x + plate.contentBounds.max.x) * .5f;
    const float arcHalfWidth = std::max(std::abs(plate.contentBounds.min.x - arcCenterX),
                                        std::abs(plate.contentBounds.max.x - arcCenterX));
    const float cameraDistance = static_cast<float>(
        std::sqrt(Graffito::Math::LengthSquared(plate.cameraPosition - plate.pose.origin)));

    // Narrow the cylinder near side views before an outer chord mirrors. Each chord
    // has true vertex depth; DepthClip approximates the whole plate unless per-fragment.
    constexpr double WRAP_EDGE_ON_GUARD_RADIANS = 80.0 * Graffito::Math::PI / 180.0;
    const double azimuth = std::abs(std::atan2(rightDot, frontDot));
    const double requestedArc =
        allowDimensionalFront ? static_cast<double>(snap.graffitoWrapRadians * dimensionalScale)
                              : .0;
    const double effectiveArc =
        std::clamp(2.0 * (WRAP_EDGE_ON_GUARD_RADIANS - azimuth), .0, requestedArc);
    const double radiansPerPixel = effectiveArc > 1e-9 && arcHalfWidth > 1e-3f
                                       ? effectiveArc / (2.0 * static_cast<double>(arcHalfWidth))
                                       : .0;
    const double frontRadius = radiansPerPixel > 1e-9
                                   ? static_cast<double>(plate.worldUnitsPerPixel) / radiansPerPixel
                                   : .0;

    const float grazing =
        std::sqrt(std::clamp(1.0f - static_cast<float>(std::abs(frontDot)), .0f, 1.0f));
    const float edgeSheen = snap.graffito.EdgeSheen * grazing;

    const auto makeFisheye = [&](const ImVec2& center, float width)
    {
        Graffito::FisheyeWarp warp{};
        const float strength = snap.graffito.FisheyeStrength * dimensionalScale;
        if (allowDimensionalFront && strength > .0f && std::isfinite(width) && width > 2.0f)
        {
            warp.center = center;
            warp.invHalfWidth = 2.0f / width;
            warp.strength = strength;
        }
        return warp;
    };

    const Graffito::FisheyeWarp mainFisheye = makeFisheye(
        {layout.startPos.x, layout.startPos.y + layout.mainLineY + layout.mainLineHeight * .5f},
        layout.mainLineWidth);
    const Graffito::FisheyeWarp titleFisheye = makeFisheye(
        {layout.startPos.x, layout.startPos.y + layout.titleY + layout.titleSize.y * .5f},
        layout.titleSize.x);
    const Graffito::FisheyeWarp infoFisheye = makeFisheye(
        {layout.startPos.x, layout.startPos.y + layout.infoLineY + layout.infoLineHeight * .5f},
        layout.infoLineWidth);

    // Marks share the cylinder but omit the typography lens to preserve proportions.
    constexpr Graffito::FisheyeWarp markFisheye{};

    int targetSegments = 1;
    if (radiansPerPixel > 1e-9 && frontRadius > .0)
    {
        const double halfWidthWorld = static_cast<double>(arcHalfWidth * plate.worldUnitsPerPixel);
        const RE::NiPoint3 leftWorld =
            ToEnginePoint(plate.pose.origin - plate.pose.right * halfWidthWorld);
        const RE::NiPoint3 rightWorld =
            ToEnginePoint(plate.pose.origin + plate.pose.right * halfWidthWorld);
        RE::NiPoint3 leftScreen{};
        RE::NiPoint3 rightScreen{};
        double screenWidth = static_cast<double>(arcHalfWidth) * 2.0;
        if (WorldToScreen(leftWorld, leftScreen) && WorldToScreen(rightWorld, rightScreen))
        {
            screenWidth = std::hypot(static_cast<double>(rightScreen.x - leftScreen.x),
                                     static_cast<double>(rightScreen.y - leftScreen.y));
        }
        // The segment estimate bounds pixel sagitta error:
        //     segments ~ sqrt(k * screenWidth * obliqueness)
        //     k = .32 * arcDegrees / 70
        // .30 floors obliqueness; four segments preserve curvature head-on. A flat plate
        // uses one exact quad. requireExactSegments rejects any count mismatch.
        const double arcDegrees = effectiveArc * 180.0 / Graffito::Math::PI;
        const double errorCoefficient = .32 * (arcDegrees / 70.0);
        const double viewObliqueness = std::sqrt(std::clamp(1.0 - frontDot * frontDot, .0, 1.0));
        const double estimate = std::sqrt(
            std::max(.0, errorCoefficient * screenWidth * std::max(viewObliqueness, .30)));
        targetSegments = std::clamp(
            static_cast<int>(std::ceil(estimate)), 4, static_cast<int>(Graffito::MAX_SEGMENTS));
    }

    const auto setPlane = [&](GraffitoSurface& surface,
                              const Graffito::Math::PlanePose& basePose,
                              double normalOffset,
                              const ImVec2& anchor,
                              const Graffito::SourceBounds& sourceBounds,
                              const Graffito::InkMaterial& material,
                              const Graffito::FisheyeWarp& fisheye,
                              int requestedSegments,
                              bool requireExactSegments)
    {
        surface = {};
        const auto surfacePose = Graffito::Math::OffsetPlanePose(basePose, normalOffset);
        surface.worldPlane.origin = ToEnginePoint(surfacePose.origin);
        surface.worldPlane.right = ToEnginePoint(surfacePose.right);
        surface.worldPlane.up = ToEnginePoint(surfacePose.up);
        surface.worldPlane.worldUnitsPerPixel = plate.worldUnitsPerPixel;
        requestedSegments =
            std::clamp(requestedSegments, 1, static_cast<int>(Graffito::MAX_SEGMENTS));
        if (requestedSegments > 1 && radiansPerPixel > 1e-9 && frontRadius > .0)
        {
            const double surfaceRadius = frontRadius + normalOffset;
            if (!std::isfinite(surfaceRadius) || surfaceRadius < frontRadius * .25)
            {
                return;
            }
            surface.worldPlane.wrap.sourceCenterX = arcCenterX;
            surface.worldPlane.wrap.radiansPerPixel = static_cast<float>(radiansPerPixel);
            surface.worldPlane.wrap.surfaceRadius = static_cast<float>(surfaceRadius);
            surface.worldPlane.wrap.segmentCount = requestedSegments;
        }
        surface.sourceAnchor = anchor;
        surface.sourceBounds = sourceBounds;
        surface.material = material;
        if (material.opacity <= .001f ||
            !Graffito::BuildProjection(
                surface.worldPlane, surface.sourceAnchor, surface.sourceBounds, surface.projection))
        {
            return;
        }
        surface.projection.fisheye = fisheye;
        if (requireExactSegments && surface.projection.segmentCount != requestedSegments)
        {
            surface = {};
            return;
        }
        surface.callbackParams = Graffito::MakeCallbackParams(surface.projection, surface.material);
        surface.active = surface.callbackParams != nullptr;
    };
    // Cap relief to roughly one line height; larger offsets let scene walls
    // clip a title independently of its name.
    constexpr double LAYER_DEPTH_CAP_LINES = 1.0;
    const double layerDepthWorld =
        std::min(static_cast<double>(snap.graffito.LayerDepth) * cameraDistance,
                 static_cast<double>(layout.nameFontSize * plate.worldUnitsPerPixel) *
                     LAYER_DEPTH_CAP_LINES);

    const auto cloneWithFisheye = [&](GraffitoSurface& destination,
                                      const GraffitoSurface& source,
                                      const Graffito::FisheyeWarp& fisheye)
    {
        destination = source;
        if (!source.active)
        {
            destination = {};
            return;
        }
        destination.projection.fisheye = fisheye;
        destination.callbackParams =
            Graffito::MakeCallbackParams(destination.projection, destination.material);
        destination.active = destination.callbackParams != nullptr;
    };

    const auto setFrontLayers = [&](const Graffito::InkMaterial& material, int frontSegments)
    {
        if (!plate.front.active)
        {
            return;
        }

        if (allowDimensionalFront && layerDepthWorld > 1e-5)
        {
            setPlane(plate.recessedInk,
                     plate.pose,
                     -layerDepthWorld,
                     plate.sourceAnchor,
                     bounds,
                     material,
                     titleFisheye,
                     frontSegments,
                     true);
            setPlane(plate.raisedMarks,
                     plate.pose,
                     layerDepthWorld,
                     plate.sourceAnchor,
                     bounds,
                     material,
                     markFisheye,
                     frontSegments,
                     true);
        }

        // Each row needs its own normalized lens band, even with LayerDepth=0.
        if (!plate.recessedInk.active)
        {
            cloneWithFisheye(plate.recessedInk, plate.front, titleFisheye);
        }
        if (!plate.raisedMarks.active)
        {
            cloneWithFisheye(plate.raisedMarks, plate.front, markFisheye);
        }
        cloneWithFisheye(plate.recessedInfo, plate.recessedInk, infoFisheye);
        cloneWithFisheye(plate.raisedEmblem, plate.raisedMarks, markFisheye);
    };

    // The falling inscription is one surface; auxiliary faces would remain upright.
    const bool useFolio = snap.graffito.FolioEnabled && plate.epitaphProgress <= .0f;
    if (!useFolio)
    {
        auto material = plate.legacyMaterial;
        float materialOpacity = 1.0f;  // Single-sheet path already composed opacity on the CPU.
        if (snap.graffito.FolioEnabled)
        {
            material = Graffito::Math::EvaluateFacingMaterial(frontDot,
                                                              snap.graffito.FacingFadeDegrees,
                                                              snap.graffito.BacksideBleedAlpha,
                                                              snap.graffito.EdgeSeamAlpha);
            materialOpacity = static_cast<float>(material.opacity);
        }
        const Graffito::InkMaterial inkMaterial{static_cast<float>(material.desaturation),
                                                static_cast<float>(material.brightness),
                                                materialOpacity,
                                                edgeSheen};
        setPlane(plate.front,
                 plate.pose,
                 .0,
                 plate.sourceAnchor,
                 bounds,
                 inkMaterial,
                 mainFisheye,
                 targetSegments,
                 false);
        const int frontSegments = plate.front.active ? plate.front.projection.segmentCount : 1;
        setFrontLayers(inkMaterial, frontSegments);
        plate.active = plate.front.active;
        return plate.active;
    }

    const auto folio = Graffito::Math::EvaluateFolioWeights(frontDot, rightDot);
    setPlane(plate.front,
             plate.pose,
             .0,
             plate.sourceAnchor,
             bounds,
             {.0f, 1.0f, static_cast<float>(folio.front), edgeSheen},
             mainFisheye,
             targetSegments,
             false);
    const int frontSegments = plate.front.active ? plate.front.projection.segmentCount : 1;

    // Roles follow the wrap arc: title/info behind the name, marks in front.
    const Graffito::InkMaterial frontMaterial{
        .0f, 1.0f, static_cast<float>(folio.front), edgeSheen};
    setFrontLayers(frontMaterial, frontSegments);

    // Three recessed ink copies create letter relief through oblique parallax.
    const auto facetMetrics =
        Graffito::Math::ComputeFolioFacetMetrics(layout.nameFontSize, snap.graffito.FolioDepth);
    const float spacingPixels = static_cast<float>(facetMetrics.reliefSpacing);
    if (spacingPixels > 0.0f)
    {
        const double spacingWorldUnits =
            static_cast<double>(spacingPixels * plate.worldUnitsPerPixel);
        const int reliefSegments =
            frontSegments > 1 ? std::min(frontSegments, std::max(3, frontSegments / 2)) : 1;
        for (std::size_t i = 0; i < plate.relief.size(); ++i)
        {
            const double reliefOffset = -spacingWorldUnits * Graffito::Math::FOLIO_RELIEF_STEPS[i];
            setPlane(plate.relief[i],
                     plate.pose,
                     reliefOffset,
                     plate.sourceAnchor,
                     bounds,
                     {.46f, .58f, static_cast<float>(folio.front * .78)},
                     mainFisheye,
                     reliefSegments,
                     true);
        }
    }

    // One head-pinned mark serves side and rear views with font-derived clearance.
    const float facetHeight = static_cast<float>(facetMetrics.height);
    const float facetWidth = static_cast<float>(facetMetrics.rearWidth);
    const float facetLift = static_cast<float>(facetMetrics.markerLift);
    plate.facetRankSize = static_cast<float>(facetMetrics.RankSize(facetMetrics.rearWidth));
    const float centerX = plate.headSourceAnchor.x;
    const float facetBottom = plate.headSourceAnchor.y - facetLift;
    plate.facetDrawBounds = {{centerX - facetWidth * .5f, facetBottom - facetHeight},
                             {centerX + facetWidth * .5f, facetBottom}};

    // Reserve projection space for the oversized rank halo and shadow.
    constexpr float FACET_PROJECTION_GUARD_PX = 20.0f;
    const Graffito::SourceBounds facetProjectionBounds{
        {plate.facetDrawBounds.min.x - FACET_PROJECTION_GUARD_PX,
         plate.facetDrawBounds.min.y - FACET_PROJECTION_GUARD_PX},
        {plate.facetDrawBounds.max.x + FACET_PROJECTION_GUARD_PX,
         plate.facetDrawBounds.max.y + FACET_PROJECTION_GUARD_PX}};

    Graffito::Math::Vec3 facetView{};
    Graffito::Math::Vec3 facetRight{};
    Graffito::Math::Vec3 facetUp{};
    const bool facetBasisReady =
        Graffito::Math::Normalize(plate.cameraPosition - plate.headPosition, facetView) &&
        Graffito::Math::Normalize(
            Graffito::Math::Cross(Graffito::Math::Vec3{.0, .0, 1.0}, facetView), facetRight) &&
        Graffito::Math::Normalize(Graffito::Math::Cross(facetView, facetRight), facetUp);
    const float facetOpacity = static_cast<float>(folio.spine * snap.graffito.FolioSpineAlpha +
                                                  folio.back * snap.graffito.FolioReverseAlpha);
    if (facetBasisReady)
    {
        const Graffito::Math::PlanePose facetPose{
            plate.headPosition, facetView, facetRight, facetUp};
        setPlane(plate.facet,
                 facetPose,
                 .0,
                 plate.headSourceAnchor,
                 facetProjectionBounds,
                 {.0f, 1.0f, facetOpacity, .0f},
                 {},
                 1,
                 true);
    }

    plate.active = plate.front.active || plate.facet.active;
    return plate.active;
}

/**
 * @fn static int GraffitoInkChannel(const RenderSettingsSnapshot& snap)
 * @brief Select the front draw channel for the active glow pipeline.
 * @author Alex (<https://github.com/lextpf>)
 */
static int GraffitoInkChannel(const RenderSettingsSnapshot& snap)
{
    return snap.enableGlow && TextPostProcess::IsInitialized() ? 2 : 1;
}

/**
 * @fn static void BeginReducedGraffitoSurfaceOnChannel(ImDrawList* drawList, ImDrawListSplitter*
 *     splitter, const GraffitoSurface& surface, int channel)
 * @brief Select a channel and begin its projected depth bracket.
 * @author Alex (<https://github.com/lextpf>)
 */
static void BeginReducedGraffitoSurfaceOnChannel(ImDrawList* drawList,
                                                 ImDrawListSplitter* splitter,
                                                 const GraffitoSurface& surface,
                                                 int channel)
{
    splitter->SetCurrentChannel(drawList, channel);
    drawList->AddCallback(Graffito::ApplyCallback, surface.callbackParams);
    if (GetState().depthClipFrame)
    {
        const auto& depth = surface.projection.plateDepth;
        drawList->AddCallback(
            DepthClip::ApplyCallback,
            DepthClip::MakePlaneParams(depth.xSlope, depth.ySlope, depth.constant));
    }
}

/**
 * @fn static void BeginReducedGraffitoSurface(ImDrawList* drawList, ImDrawListSplitter* splitter,
 *     const GraffitoSurface& surface, const RenderSettingsSnapshot& snap)
 * @brief Begin a projected surface with its own depth parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
static void BeginReducedGraffitoSurface(ImDrawList* drawList,
                                        ImDrawListSplitter* splitter,
                                        const GraffitoSurface& surface,
                                        const RenderSettingsSnapshot& snap)
{
    BeginReducedGraffitoSurfaceOnChannel(drawList, splitter, surface, GraffitoInkChannel(snap));
}

/**
 * @fn static void EndReducedGraffitoSurface(ImDrawList* drawList)
 * @brief Close the projected surface and restore neutral depth parameters.
 * @author Alex (<https://github.com/lextpf>)
 */
static void EndReducedGraffitoSurface(ImDrawList* drawList)
{
    if (GetState().depthClipFrame)
    {
        drawList->AddCallback(DepthClip::RestoreCallback, nullptr);
    }
    drawList->AddCallback(Graffito::RestoreCallback, nullptr);
}

/**
 * @fn static void EndReducedGraffitoSurfaceOnChannel(ImDrawList* drawList, ImDrawListSplitter*
 *     splitter, int channel)
 * @brief Select a channel and close its projected depth bracket.
 * @author Alex (<https://github.com/lextpf>)
 */
static void EndReducedGraffitoSurfaceOnChannel(ImDrawList* drawList,
                                               ImDrawListSplitter* splitter,
                                               int channel)
{
    splitter->SetCurrentChannel(drawList, channel);
    EndReducedGraffitoSurface(drawList);
}

/**
 * @struct FolioRankVisual
 * @brief Borrowed rank texture and its tint mode for a facet marker.
 * @author Alex (<https://github.com/lextpf>)
 */
struct FolioRankVisual
{
    ImTextureID texture = 0;
    Settings::Color3 color{1.0f, 1.0f, 1.0f};
    bool fullColor = false;
};

/**
 * @fn static FolioRankVisual FindFolioRank(const LabelLayout& layout)
 * @brief Find the emblem or rank badge to repeat on the folio facet.
 * @author Alex (<https://github.com/lextpf>)
 */
static FolioRankVisual FindFolioRank(const LabelLayout& layout)
{
    if (layout.tierEmblemShown && layout.tierEmblemTex != 0)
    {
        return {layout.tierEmblemTex, {1.0f, 1.0f, 1.0f}, true};
    }
    for (const auto& badge : layout.badges)
    {
        if (badge.isRank && badge.tex != 0)
        {
            return {badge.tex, badge.color, badge.fullColor};
        }
    }
    return {};
}

/**
 * @fn static void DrawFolioRank(ImDrawList* drawList, const FolioRankVisual& rank, const ImVec2&
 *     center, float size, float alpha)
 * @brief Draw the retained rank texture with the facet opacity.
 * @author Alex (<https://github.com/lextpf>)
 */
static void DrawFolioRank(ImDrawList* drawList,
                          const FolioRankVisual& rank,
                          const ImVec2& center,
                          float size,
                          float alpha)
{
    if (rank.texture == 0 || alpha <= .002f)
    {
        return;
    }
    const ImVec4 tint = rank.fullColor ? ImVec4(1.0f, 1.0f, 1.0f, alpha)
                                       : ImVec4(rank.color.r, rank.color.g, rank.color.b, alpha);
    const ImVec2 half{size * .5f, size * .5f};

    // Shape the shadow from rank alpha so irregular art needs no enclosing disk.
    const float shadowSize = size * 1.16f;
    const ImVec2 shadowHalf{shadowSize * .5f, shadowSize * .5f};
    const ImVec2 shadowCenter{center.x, center.y + size * .045f};
    RenderSampling::PushBadgeSampler(drawList);
    drawList->AddImage(rank.texture,
                       shadowCenter - shadowHalf,
                       shadowCenter + shadowHalf,
                       ImVec2(0, 0),
                       ImVec2(1, 1),
                       ImGui::ColorConvertFloat4ToU32(ImVec4(.0f, .0f, .0f, alpha * .62f)));
    drawList->AddImage(rank.texture,
                       center - half,
                       center + half,
                       ImVec2(0, 0),
                       ImVec2(1, 1),
                       ImGui::ColorConvertFloat4ToU32(tint));
    RenderSampling::PopSampler(drawList);
}

/**
 * @fn static ImU32 ReliefInk(const ImVec4& support, float alpha)
 * @brief Pack a darkened support tint for recessed folio ink.
 * @author Alex (<https://github.com/lextpf>)
 */
static ImU32 ReliefInk(const ImVec4& support, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(.025f + support.x * .16f,
                                                 .022f + support.y * .14f,
                                                 .028f + support.z * .18f,
                                                 std::clamp(alpha, .0f, 1.0f)));
}

/**
 * @fn static void DrawGraffitoRelief(ImDrawList* drawList, ImDrawListSplitter* splitter, const
 *     LabelStyle& style, const LabelLayout& layout, const GraffitoPlate& plate, const
 *     RenderSettingsSnapshot& snap)
 * @brief Draw recessed ink layers that expose parallax around the front text.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The front draw masks aligned ink; recessed copies expose only parallax.
 */
static void DrawGraffitoRelief(ImDrawList* drawList,
                               ImDrawListSplitter* splitter,
                               const LabelStyle& style,
                               const LabelLayout& layout,
                               const GraffitoPlate& plate,
                               const RenderSettingsSnapshot& snap)
{
    if (!layout.fontName)
    {
        return;
    }

    for (const auto& surface : plate.relief)
    {
        if (!surface.active)
        {
            continue;
        }

        BeginReducedGraffitoSurface(drawList, splitter, surface, snap);
        ImVec2 currentPos{layout.startPos.x - layout.totalWidth * .5f +
                              (layout.totalWidth - layout.mainLineWidth) * .5f,
                          layout.startPos.y + layout.mainLineY};
        for (const auto& segment : layout.segments)
        {
            if (!segment.displayText.empty())
            {
                const float verticalOffset = (layout.mainLineHeight - segment.size.y) * .5f;
                const ImVec2 textPos{currentPos.x, currentPos.y + verticalOffset};
                const ImVec4& support = segment.isLevel ? style.supportLevel : style.supportName;
                const float alpha = (segment.isLevel ? style.levelAlpha : style.alpha) * .82f;
                const ImU32 color = ReliefInk(support, alpha);
                const int segmentVertexStart = drawList->VtxBuffer.Size;
                drawList->AddText(segment.font,
                                  segment.fontSize,
                                  textPos + ImVec2(.65f, .35f),
                                  color,
                                  segment.displayText.c_str());
                drawList->AddText(
                    segment.font, segment.fontSize, textPos, color, segment.displayText.c_str());
                ScaleNewVerticesX(drawList, segmentVertexStart, textPos.x, segment.horizontalScale);
            }
            currentPos.x += segment.size.x + layout.segmentPadding;
        }
        EndReducedGraffitoSurface(drawList);
    }
}

/**
 * @fn static void DrawGraffitoFrontInk(ImDrawList* drawList, ImDrawListSplitter* splitter, const
 *     LabelStyle& style, const LabelLayout& layout, const GraffitoPlate& plate, float
 *     lodTitleFactor, float time, const RenderSettingsSnapshot& snap)
 * @brief Draw each text role through its projected folio surface.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Each role has its own projection/depth bracket. Failed auxiliary projections
 * use the principal face so text remains visible.
 */
static void DrawGraffitoFrontInk(ImDrawList* drawList,
                                 ImDrawListSplitter* splitter,
                                 const LabelStyle& style,
                                 const LabelLayout& layout,
                                 const GraffitoPlate& plate,
                                 float lodTitleFactor,
                                 float time,
                                 const RenderSettingsSnapshot& snap)
{
    if (!plate.front.active)
    {
        return;
    }

    const auto drawOnSurface = [&](const GraffitoSurface& preferred, const auto& draw)
    {
        const GraffitoSurface& surface = preferred.active ? preferred : plate.front;
        BeginReducedGraffitoSurface(drawList, splitter, surface, snap);
        draw();
        EndReducedGraffitoSurface(drawList);
    };

    drawOnSurface(
        plate.recessedInk,
        [&]()
        {
            DrawTitleText(
                drawList, style, layout, lodTitleFactor, splitter, snap.fastOutlines, snap);
        });
    drawOnSurface(
        plate.recessedInfo,
        [&]()
        { DrawInfoLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap); });
    drawOnSurface(
        plate.front,
        [&]()
        { DrawMainLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap); });
    drawOnSurface(
        plate.raisedMarks,
        [&]() { DrawBadges(drawList, style, layout, splitter, snap.fastOutlines, snap, true); });
    drawOnSurface(plate.raisedEmblem,
                  [&]() { DrawTierEmblem(drawList, style, layout, time, splitter, snap, true); });
}

/**
 * @fn static void DrawGraffitoDecorations(ImDrawList*, ImDrawListSplitter*, const ActorDrawData&,
 *     const LabelStyle&, const LabelLayout&, const GraffitoPlate&, float, float, const
 *     RenderSettingsSnapshot&)
 * @brief Project particles and ornaments through the marks surface.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Particles and ornaments need separate balanced projection/depth brackets
 * after channel merge. Use the marks surface to preserve authored proportions.
 */
static void DrawGraffitoDecorations(ImDrawList* drawList,
                                    ImDrawListSplitter* splitter,
                                    const ActorDrawData& d,
                                    const LabelStyle& style,
                                    const LabelLayout& layout,
                                    const GraffitoPlate& plate,
                                    float lodEffectsFactor,
                                    float time,
                                    const RenderSettingsSnapshot& snap)
{
    if (!d.isPlayer || lodEffectsFactor <= .01f)
    {
        return;
    }

    const GraffitoSurface& surface = plate.raisedMarks.active ? plate.raisedMarks : plate.front;
    if (!surface.active)
    {
        return;
    }

    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const int particleChannel = gpuGlow ? 1 : 0;
    const int ornamentChannel = GraffitoInkChannel(snap);

    BeginReducedGraffitoSurfaceOnChannel(drawList, splitter, surface, particleChannel);
    if (ornamentChannel != particleChannel)
    {
        BeginReducedGraffitoSurfaceOnChannel(drawList, splitter, surface, ornamentChannel);
    }

    DrawParticlesAndOrnaments(
        drawList, d, style, layout, lodEffectsFactor, time, splitter, snap.fastOutlines, snap);

    EndReducedGraffitoSurfaceOnChannel(drawList, splitter, particleChannel);
    if (ornamentChannel != particleChannel)
    {
        EndReducedGraffitoSurfaceOnChannel(drawList, splitter, ornamentChannel);
    }
}

/**
 * @fn static Settings::Color3 FolioRelationshipColor(const ActorDrawData& d, const
 *     RenderSettingsSnapshot& snap)
 * @brief Resolve the relationship tint for the folio facet.
 * @author Alex (<https://github.com/lextpf>)
 */
static Settings::Color3 FolioRelationshipColor(const ActorDrawData& d,
                                               const RenderSettingsSnapshot& snap)
{
    switch (d.relationship)
    {
        case RelationshipKind::Hostile:
            return snap.icons.colHostile;
        case RelationshipKind::Ally:
            return snap.icons.colAlly;
        case RelationshipKind::Follower:
            return snap.icons.colFollower;
        case RelationshipKind::Neutral:
            return snap.icons.colNeutral;
    }
    return snap.icons.colNeutral;
}

/**
 * @fn static ImVec2 LerpPoint(const ImVec2& from, const ImVec2& to, float t)
 * @brief Interpolate two screen positions without clamping the factor.
 * @author Alex (<https://github.com/lextpf>)
 */
static ImVec2 LerpPoint(const ImVec2& from, const ImVec2& to, float t)
{
    return {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
}

/**
 * @fn static void DrawFadingCornerArm(ImDrawList* drawList, const ImVec2& corner, const ImVec2&
 *     along, const ImVec4& color, float alpha)
 * @brief Fade a facet corner arm before it completes the glass border.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Fade corner arms before they form a complete glass border.
 */
static void DrawFadingCornerArm(ImDrawList* drawList,
                                const ImVec2& corner,
                                const ImVec2& along,
                                const ImVec4& color,
                                float alpha)
{
    constexpr int SEGMENTS = 5;
    constexpr float REACH = .38f;
    for (int i = 0; i < SEGMENTS; ++i)
    {
        const float t0 = REACH * static_cast<float>(i) / static_cast<float>(SEGMENTS);
        const float t1 = REACH * static_cast<float>(i + 1) / static_cast<float>(SEGMENTS);
        const float progress = (static_cast<float>(i) + .5f) / static_cast<float>(SEGMENTS);
        const float fade = std::pow(1.0f - progress, 1.55f);
        const float thickness = 2.0f + 4.2f * (1.0f - progress);
        drawList->AddLine(
            LerpPoint(corner, along, t0),
            LerpPoint(corner, along, t1),
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, alpha * fade)),
            thickness);
    }
}

/**
 * @fn static void DrawFadingTriangleCorners(ImDrawList* drawList, const ImVec2& tip, const ImVec2&
 *     baseA, const ImVec2& baseB, const ImVec4& leftColor, const ImVec4& rightColor, float alpha)
 * @brief Draw separate fading arms at each triangle corner.
 * @author Alex (<https://github.com/lextpf>)
 */
static void DrawFadingTriangleCorners(ImDrawList* drawList,
                                      const ImVec2& tip,
                                      const ImVec2& baseA,
                                      const ImVec2& baseB,
                                      const ImVec4& leftColor,
                                      const ImVec4& rightColor,
                                      float alpha)
{
    DrawFadingCornerArm(drawList, tip, baseA, leftColor, alpha);
    DrawFadingCornerArm(drawList, tip, baseB, rightColor, alpha);
    DrawFadingCornerArm(drawList, baseA, tip, leftColor, alpha);
    DrawFadingCornerArm(drawList, baseA, baseB, leftColor, alpha);
    DrawFadingCornerArm(drawList, baseB, tip, rightColor, alpha);
    DrawFadingCornerArm(drawList, baseB, baseA, rightColor, alpha);
}

/**
 * @fn static void DrawGraffitoFacet(ImDrawList*, ImDrawListSplitter*, const GraffitoSurface&,
 *     const Graffito::SourceBounds&, float, const ActorDrawData&, const LabelStyle&, const
 *     LabelLayout&, const RenderSettingsSnapshot&)
 * @brief Draw the head-anchored pane visible from the side or rear.
 * @author Alex (<https://github.com/lextpf>)
 *
 * One head-pinned pane serves side/rear views; relationship tints its glass.
 */
static void DrawGraffitoFacet(ImDrawList* drawList,
                              ImDrawListSplitter* splitter,
                              const GraffitoSurface& surface,
                              const Graffito::SourceBounds& bounds,
                              float rankSize,
                              const ActorDrawData& d,
                              const LabelStyle& style,
                              const LabelLayout& layout,
                              const RenderSettingsSnapshot& snap)
{
    if (!surface.active)
    {
        return;
    }

    BeginReducedGraffitoSurface(drawList, splitter, surface, snap);
    const float centerX = (bounds.min.x + bounds.max.x) * .5f;
    const ImVec2 tip{centerX, bounds.max.y};
    const ImVec2 baseA{bounds.min.x, bounds.min.y};
    const ImVec2 baseB{bounds.max.x, bounds.min.y};
    const float inkAlpha = std::clamp(style.alpha, .0f, 1.0f);

    const auto relationship = FolioRelationshipColor(d, snap);
    const ImVec4 glass{.025f + relationship.r * .13f,
                       .028f + relationship.g * .12f,
                       .040f + relationship.b * .16f,
                       inkAlpha * .38f};
    drawList->AddTriangleFilled(tip, baseA, baseB, ImGui::ColorConvertFloat4ToU32(glass));

    DrawFadingTriangleCorners(
        drawList, tip, baseA, baseB, style.LcLevel, style.RcLevel, inkAlpha * .96f);

    const float height = bounds.max.y - bounds.min.y;
    const ImVec2 rankCenter{centerX, bounds.min.y + height * .34f};
    const auto rank = FindFolioRank(layout);
    if (rank.texture != 0)
    {
        DrawFolioRank(drawList, rank, rankCenter, rankSize, inkAlpha);
    }
    EndReducedGraffitoSurface(drawList);
}

/**
 * @fn static void DrawGraffitoFacetMarker(ImDrawList* drawList, ImDrawListSplitter* splitter, const
 *     ActorDrawData& d, const LabelStyle& style, const LabelLayout& layout, const GraffitoPlate&
 *     plate, const RenderSettingsSnapshot& snap)
 * @brief Repeat the rank marker on the projected facet.
 * @author Alex (<https://github.com/lextpf>)
 */
static void DrawGraffitoFacetMarker(ImDrawList* drawList,
                                    ImDrawListSplitter* splitter,
                                    const ActorDrawData& d,
                                    const LabelStyle& style,
                                    const LabelLayout& layout,
                                    const GraffitoPlate& plate,
                                    const RenderSettingsSnapshot& snap)
{
    DrawGraffitoFacet(drawList,
                      splitter,
                      plate.facet,
                      plate.facetDrawBounds,
                      plate.facetRankSize,
                      d,
                      style,
                      layout,
                      snap);
}

/**
 * @fn static void TranslateLayout(LabelLayout& layout, const ImVec2& delta)
 * @brief Move plate bounds and absolute badge positions with the text anchor.
 * @author Alex (<https://github.com/lextpf>)
 */
static void TranslateLayout(LabelLayout& layout, const ImVec2& delta)
{
    layout.startPos += delta;
    layout.nameplateCenter += delta;
    layout.nameplateTop += delta.y;
    layout.nameplateBottom += delta.y;
    layout.nameplateLeft += delta.x;
    layout.nameplateRight += delta.x;
    layout.mainLineCenterY += delta.y;
    for (auto& badge : layout.badges)
    {
        badge.pos += delta;
    }
    if (layout.tierEmblemShown)
    {
        layout.tierEmblemPos += delta;
    }
}

/**
 * @fn static void UpdateRegisters(const RenderSettingsSnapshot& snap, float dt)
 * @brief Ease published register values toward their active profile.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Ease published register values toward their targets, or 1/1/1/0 when none matches.
 */
static void UpdateRegisters(const RenderSettingsSnapshot& snap, float dt)
{
    auto& st = GetState();
    float alphaT = 1.0f;
    float fadeT = 1.0f;
    float subT = 1.0f;
    float hideT = .0f;

    const int idx = st.activeRegister.load(std::memory_order_acquire);
    if (snap.registersEnabled && idx >= 0 && idx < static_cast<int>(snap.registers.size()))
    {
        const auto& reg = snap.registers[static_cast<size_t>(idx)];
        alphaT = reg.alphaMul;
        fadeT = reg.fadeMul;
        subT = reg.subLineMul;
        hideT = reg.hideNeutral ? 1.0f : .0f;
    }

    const float k = ExpApproachAlpha(dt, std::max(.05f, snap.registerTransitionTime));
    st.regAlphaMul += (alphaT - st.regAlphaMul) * k;
    st.regFadeMul += (fadeT - st.regFadeMul) * k;
    st.regSubLineMul += (subT - st.regSubLineMul) * k;
    st.regHideNeutral += (hideT - st.regHideNeutral) * k;
}

/**
 * @fn static bool UpdateCacheSmoothing(ActorCache& entry, const ActorDrawData& d, const
 *     DistanceFactors& df, const RE::NiPoint3& screenPos, float dt, const RenderSettingsSnapshot&
 *     snap)
 * @brief Advance position, scale, opacity, and occlusion smoothing for a plate.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Seed first-frame values from targets. Draw while alpha * occlusion > 0.02.
 */
static bool UpdateCacheSmoothing(ActorCache& entry,
                                 const ActorDrawData& d,
                                 const DistanceFactors& df,
                                 const RE::NiPoint3& screenPos,
                                 float dt,
                                 const RenderSettingsSnapshot& snap)
{
    float occlusionTarget = d.isOccluded ? .0f : 1.0f;

    if (!entry.initialized)
    {
        entry.initialized = true;
        entry.alphaSmooth = df.alphaTarget;
        entry.textSizeScale = df.textScaleTarget;
        entry.smooth = ImVec2(screenPos.x, screenPos.y);

        ImVec2 initPos(screenPos.x, screenPos.y);
        for (int i = 0; i < ActorCache::HISTORY_SIZE; i++)
        {
            entry.posHistory[i] = initPos;
        }
        entry.historyIndex = 0;
        entry.historyFilled = true;
        entry.occlusionSmooth = occlusionTarget;
        entry.typewriterTime = .0f;
        entry.typewriterComplete = false;
        ResetTrailHistory(entry, &d.worldPos);
    }
    else
    {
        // 0.015 s avoids adding lag to the already-extrapolated player pose.
        float aLerp = ExpApproachAlpha(dt, snap.alphaSettleTime);
        float sLerp = ExpApproachAlpha(dt, snap.scaleSettleTime);
        float pLerp = d.isPlayer ? ExpApproachAlpha(dt, .015f)
                                 : ExpApproachAlpha(dt, snap.positionSettleTime);
        float oLerp = ExpApproachAlpha(dt, snap.occlusionSettleTime);

        entry.alphaSmooth += (df.alphaTarget - entry.alphaSmooth) * aLerp;
        entry.textSizeScale += (df.textScaleTarget - entry.textSizeScale) * sLerp;
        entry.occlusionSmooth += (occlusionTarget - entry.occlusionSmooth) * oLerp;

        ImVec2 targetPos(screenPos.x, screenPos.y);
        ImVec2 maSmoothed = entry.AddAndGetSmoothed(targetPos);

        ImVec2 expSmoothed;
        expSmoothed.x = entry.smooth.x + (targetPos.x - entry.smooth.x) * pLerp;
        expSmoothed.y = entry.smooth.y + (targetPos.y - entry.smooth.y) * pLerp;

        float blend = snap.visual.PositionSmoothingBlend;
        ImVec2 smoothedPos;
        smoothedPos.x = expSmoothed.x + (maSmoothed.x - expSmoothed.x) * blend;
        smoothedPos.y = expSmoothed.y + (maSmoothed.y - expSmoothed.y) * blend;

        float moveDx = targetPos.x - entry.smooth.x;
        float moveDy = targetPos.y - entry.smooth.y;
        float moveDist = std::sqrt(moveDx * moveDx + moveDy * moveDy);

        if (moveDist > snap.visual.LargeMovementThreshold)
        {
            // Keep world-space trails across camera jumps; only world teleports reseed them.
            entry.smooth.x += (smoothedPos.x - entry.smooth.x) * snap.visual.LargeMovementBlend;
            entry.smooth.y += (smoothedPos.y - entry.smooth.y) * snap.visual.LargeMovementBlend;
        }
        else
        {
            entry.smooth = smoothedPos;
        }

        if (snap.enableTypewriter && !entry.typewriterComplete)
        {
            entry.typewriterTime += dt;
        }
    }

    entry.wasOccluded = d.isOccluded;

    const float alpha = entry.alphaSmooth * entry.occlusionSmooth;
    return alpha > .02f;
}

/**
 * @fn static void DrawLabel(const ActorDrawData& snapshotData, ImDrawList* drawList,
 *     ImDrawListSplitter* splitter, const RenderSettingsSnapshot& snap, uint32_t focusedFormID,
 *     uint32_t aimedFormID)
 * @brief Advance a live actor cache entry and draw its resolved plate.
 * @author Alex (<https://github.com/lextpf>)
 */
static void DrawLabel(const ActorDrawData& snapshotData,
                      ImDrawList* drawList,
                      ImDrawListSplitter* splitter,
                      const RenderSettingsSnapshot& snap,
                      uint32_t focusedFormID,
                      uint32_t aimedFormID)
{
    auto it = GetState().cache.find(snapshotData.formID);
    if (it == GetState().cache.end())
    {
        ActorCache newEntry{};
        newEntry.lastSeenFrame = GetState().frame;
        it = GetState().cache.emplace(snapshotData.formID, newEntry).first;
    }
    auto& entry = it->second;
    ActorDrawData d = snapshotData;
    ResolvePlayerRenderPose(entry, d);
    const uint32_t prevLastSeenFrame = entry.lastSeenFrame;

    if (entry.cachedName != d.name)
    {
        entry.cachedName = d.name;
        entry.cachedNameLower = d.name;
        std::transform(entry.cachedNameLower.begin(),
                       entry.cachedNameLower.end(),
                       entry.cachedNameLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        entry.typewriterTime = .0f;
        entry.typewriterComplete = false;
    }

    // Re-entry threshold counts rendered frames (~0.5 s at 60 fps).
    constexpr uint32_t REENTRY_THRESHOLD = 30;
    if (entry.initialized)
    {
        uint32_t framesSinceLastSeen = GetState().frame - prevLastSeenFrame;
        bool becameVisible = entry.wasOccluded && !d.isOccluded;
        bool reEntered = framesSinceLastSeen >= REENTRY_THRESHOLD || becameVisible;
        if (reEntered)
        {
            if (entry.typewriterComplete)
            {
                entry.typewriterTime = .0f;
                entry.typewriterComplete = false;
            }
            if (entry.entranceDone)
            {
                entry.entrancePhase = .0f;
                entry.entranceDone = false;
                entry.entranceDelay = -1.0f;
            }
            ResetTrailHistory(entry, &d.worldPos);
        }
    }

    entry.lastSeenFrame = GetState().frame;
    entry.sawAlive = true;  // The death rite plays only for actors seen alive

    // Retain the last live facts for exit replay.
    GetState().lastDrawData[d.formID] = d;
    entry.exitPhase = .0f;

    DistanceFactors df = ComputeDistanceFactors(d, snap);

    const float dt = ImGui::GetIO().DeltaTime;
    const bool cameraRayFacing = aimedFormID != 0 && d.formID == aimedFormID;
    const bool heldReadable = cameraRayFacing || d.isPlayer;
    if (entry.graffitoForcedFront && !heldReadable)
    {
        // Restore actor yaw immediately after aim release to avoid lingering front text.
        entry.graffitoYaw = d.headingRadians;
        entry.graffitoYawInitialized = true;
    }
    entry.graffitoForcedFront = heldReadable;
    GraffitoPlate graffito = PrepareGraffitoPlate(d, entry, snap, dt, .0f, cameraRayFacing);
    const bool graffitoMode = graffito.requested && Graffito::IsInitialized();
    if (graffitoMode && !graffito.active)
    {
        // A range or projection failure must not switch a world plate back to a billboard.
        return;
    }
    RE::NiPoint3 screenPos{};
    if (graffito.active)
    {
        screenPos = graffito.screenPos;
    }
    else if (!WorldToScreen(d.worldPos, screenPos))
    {
        return;
    }

    if (!UpdateCacheSmoothing(entry, d, df, screenPos, dt, snap))
    {
        return;
    }

    float entranceAlphaMul = 1.0f;
    float entranceScaleMul = 1.0f;
    float entranceYOffset = .0f;
    if (d.isPlayer || cameraRayFacing)
    {
        // Player and ray targets skip reveal delays unless a console edit rearmed them.
        entry.entranceDone = true;
        entry.entrancePhase = 1.0f;
        entry.entranceDelay = .0f;
        if (cameraRayFacing && !entry.revealArmed)
        {
            entry.typewriterComplete = true;
        }
    }
    else if (snap.enableEntrance && !entry.entranceDone)
    {
        // Stagger slots follow snapshot order: player, ray target, Deck target, distance.
        if (entry.entranceDelay < .0f)
        {
            entry.entranceDelay = std::min(
                static_cast<float>(GetState().entrancesStartedThisFrame) * snap.entranceStaggerStep,
                snap.entranceStaggerMax);
            ++GetState().entrancesStartedThisFrame;
        }
        if (entry.entranceDelay > .0f)
        {
            entry.entranceDelay = std::max(.0f, entry.entranceDelay - dt);
            entry.typewriterTime = .0f;  // Reveal starts with the entrance, not the wait
            return;
        }
        entry.entrancePhase += dt / std::max(snap.entranceDuration, .05f);
        if (entry.entrancePhase >= 1.0f)
        {
            entry.entrancePhase = 1.0f;
            entry.entranceDone = true;
        }
        const float t = entry.entrancePhase;
        // Ease-out without overshoot; only scale differs between entrance styles.
        const float ease = TextEffects::EaseOutCubic(t);

        entranceAlphaMul = ease;

        entranceYOffset = RenderConstants::ENTRANCE_RISE_PX * (1.0f - TextEffects::EaseOutExpo(t));

        if (snap.entranceStyle == 1)  // SlideDown: rise + fade only, no scale
        {
            entranceScaleMul = 1.0f;
        }
        else  // PopIn (0) / expand (2): gentle scale settle, no overshoot
        {
            const float scaleStart = snap.entranceStyle == 2 ? .88f : .90f;
            entranceScaleMul = scaleStart + ease * (1.0f - scaleStart);
        }
    }

    // Focus fades ambient sub-lines; the player always keeps full content.
    const bool isFocused =
        snap.focus.Enabled && (d.formID == focusedFormID || cameraRayFacing) && !d.isPlayer;
    const bool focusAppliesToActor = snap.focus.Enabled && !d.isPlayer;
    const float focusTarget = focusAppliesToActor ? (isFocused ? 1.0f : .0f) : 1.0f;
    if (cameraRayFacing && focusAppliesToActor)
    {
        // Ray hits expose auxiliary rows immediately.
        entry.focusSmooth = 1.0f;
    }
    else if (snap.focus.SettleTime <= .0f)
    {
        entry.focusSmooth = focusTarget;
    }
    else
    {
        entry.focusSmooth +=
            (focusTarget - entry.focusSmooth) * ExpApproachAlpha(dt, snap.focus.SettleTime);
    }
    const float mainAlphaMul = !focusAppliesToActor
                                   ? 1.0f
                                   : snap.focus.AmbientDimFactor +
                                         (1.0f - snap.focus.AmbientDimFactor) * entry.focusSmooth;

    // Register hiding applies only to neutral/allied NPCs.
    float registerMul = GetState().regAlphaMul;
    if (!d.isPlayer &&
        (d.relationship == RelationshipKind::Neutral || d.relationship == RelationshipKind::Ally))
    {
        registerMul *= 1.0f - GetState().regHideNeutral;
    }

    // Yield to another HUD widget and restore alpha on the same settle curve.
    entry.yieldSmooth += ((d.yieldPlate ? 1.0f : .0f) - entry.yieldSmooth) *
                         ExpApproachAlpha(dt, snap.compatYieldSettleTime);
    const float yieldMul = 1.0f - (1.0f - snap.compatTrueHUDYieldAlpha) * entry.yieldSmooth;

    // Omit quietSub: pans fold auxiliary content without fading name or particles.
    const float alpha = entry.alphaSmooth * entry.occlusionSmooth * entranceAlphaMul *
                        mainAlphaMul * registerMul * yieldMul * graffito.visibility;
    // Perspective already supplies distance scaling for world planes.
    const float textSizeScale = (graffito.active ? 1.0f : entry.textSizeScale) * entranceScaleMul;

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return;
    }
    // Keep a 100 px margin because text can remain visible past its anchor.
    const auto viewSize = renderer->GetScreenSize();
    if (screenPos.z < 0 || screenPos.z > 1.0f || screenPos.x < -100.0f ||
        screenPos.x > viewSize.width + 100.0f || screenPos.y < -100.0f ||
        screenPos.y > viewSize.height + 100.0f)
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    LabelStyle style = ComputeLabelStyle(d, entry.cachedNameLower, alpha, time, snap);
    if (graffito.active)
    {
        // CPU glow resets would replace the plane/depth shaders mid-channel.
        style.tierAllowsGlow = false;
        style.outlineGlowAllowed = false;
    }

    ImFont* nameFont = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    ImFont* levelFont = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    ImFont* titleFont = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!nameFont || !levelFont || !titleFont)
    {
        return;
    }
    style.nameOutlineWidth = style.CalcOutlineWidth(nameFont->FontSize * textSizeScale, snap);
    style.levelOutlineWidth = style.CalcOutlineWidth(levelFont->FontSize * textSizeScale, snap);
    style.titleOutlineWidth = style.CalcOutlineWidth(titleFont->FontSize * textSizeScale, snap);
    style.outlineWidth = style.nameOutlineWidth;

    // Smooth scene samples per actor to avoid abrupt exposure changes.
    if (snap.candleEnabled && SceneMeter::IsInitialized())
    {
        float lum = .0f;
        float rgb[3] = {};
        const ImVec2 meterPos =
            graffito.active ? ImVec2(graffito.screenPos.x, graffito.screenPos.y) : entry.smooth;
        if (SceneMeter::Sample(meterPos.x / static_cast<float>(viewSize.width),
                               meterPos.y / static_cast<float>(viewSize.height),
                               lum,
                               rgb))
        {
            if (entry.bgLum < .0f)
            {
                entry.bgLum = lum;
                entry.bgR = rgb[0];
                entry.bgG = rgb[1];
                entry.bgB = rgb[2];
            }
            else
            {
                const float k = ExpApproachAlpha(dt, snap.candleSettleTime);
                entry.bgLum += (lum - entry.bgLum) * k;
                entry.bgR += (rgb[0] - entry.bgR) * k;
                entry.bgG += (rgb[1] - entry.bgG) * k;
                entry.bgB += (rgb[2] - entry.bgB) * k;
            }
            const float bgRGB[3] = {entry.bgR, entry.bgG, entry.bgB};
            ApplyCandlelight(style, entry.bgLum, bgRGB, snap);
        }
    }

    // Focus fades title/info. Camera pans fold title/badges only; register
    // sub-line dimming multiplies both independently.
    const float paneFold = 1.0f - GetState().quietSub;  // 1 = settled, 0 = folded on a pan
    const float regSub = GetState().regSubLineMul;
    const float auxAlphaMul = focusAppliesToActor ? entry.focusSmooth : 1.0f;
    style.titleAlpha *= auxAlphaMul * regSub * paneFold;
    style.infoAlphaMul = auxAlphaMul * regSub;
    style.levelAlpha *= regSub;
    style.badgeAlphaMul = regSub * paneFold;

    LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale, snap);

    // Anchor world geometry to its exact projection; screen smoothing and overlap
    // relaxation would make it drift during camera motion.
    if (graffito.active)
    {
        TranslateLayout(
            layout,
            ImVec2(graffito.screenPos.x - entry.smooth.x, graffito.screenPos.y - entry.smooth.y));
    }

    if (entranceYOffset != .0f)
    {
        TranslateLayout(layout, ImVec2(.0f, entranceYOffset));
    }

    if (graffito.active && !FinalizeGraffitoPlate(graffito, layout, style, snap))
    {
        return;
    }

    if (graffito.active)
    {
        DrawGraffitoRelief(drawList, splitter, style, layout, graffito, snap);
    }
    const bool frontDepthBracket = GetState().depthClipFrame && !graffito.active;
    if (frontDepthBracket)
    {
        BracketPlateDepthClip(drawList, splitter, DepthClip::MakePlateParams(screenPos.z), snap);
    }

    // World-space trails use the current camera so stationary actors leave no smear.
    if (!graffito.active && snap.visual.EnableMotionTrail &&
        style.tierIdx >= snap.visual.TrailMinTier && entry.entranceDone)
    {
        // Reseed only on world-space teleports.
        const int lastIdx = (entry.trailIndex - 1 + ActorCache::TRAIL_HISTORY_SIZE) %
                            ActorCache::TRAIL_HISTORY_SIZE;
        if (entry.trailFilled || entry.trailIndex > 0)
        {
            const RE::NiPoint3& prevWorld = entry.trailHistory[lastIdx];
            const float wdx = d.worldPos.x - prevWorld.x;
            const float wdy = d.worldPos.y - prevWorld.y;
            const float wdz = d.worldPos.z - prevWorld.z;
            // Above locomotion speed even at low frame rates.
            constexpr float kTeleportDist = 256.0f;
            if (wdx * wdx + wdy * wdy + wdz * wdz > kTeleportDist * kTeleportDist)
            {
                ResetTrailHistory(entry, &d.worldPos);
            }
        }

        entry.trailHistory[entry.trailIndex] = d.worldPos;
        entry.trailIndex = (entry.trailIndex + 1) % ActorCache::TRAIL_HISTORY_SIZE;
        if (entry.trailIndex == 0)
        {
            entry.trailFilled = true;
        }

        const int count = entry.trailFilled ? ActorCache::TRAIL_HISTORY_SIZE : entry.trailIndex;
        const int trailLen = std::min(count, snap.visual.TrailLength);

        // Carry layout offsets onto every reprojected ghost to align it with the head.
        RE::NiPoint3 headScreen{};
        if (trailLen > 1 && WorldToScreen(d.worldPos, headScreen))
        {
            const ImVec2 trailTransientOffset(layout.startPos.x - headScreen.x,
                                              layout.startPos.y - headScreen.y);

            // Measure reprojected span in pixels to retain TrailMinDistance semantics.
            const int oldest = (entry.trailIndex - trailLen + ActorCache::TRAIL_HISTORY_SIZE) %
                               ActorCache::TRAIL_HISTORY_SIZE;
            RE::NiPoint3 oldestScreen{};
            const bool spanOk = WorldToScreen(entry.trailHistory[oldest], oldestScreen);
            const float dx = spanOk ? headScreen.x - oldestScreen.x : .0f;
            const float dy = spanOk ? headScreen.y - oldestScreen.y : .0f;
            const float dist = std::sqrt(dx * dx + dy * dy);

            if (spanOk && dist > snap.visual.TrailMinDistance)
            {
                const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
                const int chBack = gpuGlow ? 1 : 0;
                splitter->SetCurrentChannel(drawList, chBack);

                // Oldest first; skip the head sample.
                for (int i = trailLen - 1; i >= 1; --i)
                {
                    const int idx = (entry.trailIndex - 1 - i + ActorCache::TRAIL_HISTORY_SIZE) %
                                    ActorCache::TRAIL_HISTORY_SIZE;
                    RE::NiPoint3 ghostScreen{};
                    if (!WorldToScreen(entry.trailHistory[idx], ghostScreen))
                    {
                        continue;  // Behind the camera after a hard pan
                    }
                    const ImVec2 ghostPos(ghostScreen.x + trailTransientOffset.x,
                                          ghostScreen.y + trailTransientOffset.y);

                    float t = (float)i / (float)trailLen;
                    float ghostAlpha =
                        snap.visual.TrailAlpha * std::pow(1.0f - t, snap.visual.TrailFalloff);
                    ghostAlpha *= style.alpha;
                    ghostAlpha *= snap.innerTextAlpha;
                    if (ghostAlpha < .01f)
                    {
                        continue;
                    }

                    float ghostCursorX = ghostPos.x - layout.totalWidth * .5f;
                    float ghostY = ghostPos.y + layout.mainLineY;
                    for (const auto& seg : layout.segments)
                    {
                        if (seg.displayText.empty())
                        {
                            ghostCursorX += seg.size.x + layout.segmentPadding;
                            continue;
                        }
                        float vOff = (layout.mainLineHeight - seg.size.y) * .5f;
                        ImVec4 ghostCol =
                            seg.isLevel
                                ? ImVec4(
                                      style.LcLevel.x, style.LcLevel.y, style.LcLevel.z, ghostAlpha)
                                : ImVec4(
                                      style.LcName.x, style.LcName.y, style.LcName.z, ghostAlpha);
                        const ImVec2 ghostTextPos(ghostCursorX, ghostY + vOff);
                        const int segmentVertexStart = drawList->VtxBuffer.Size;
                        drawList->AddText(seg.font,
                                          seg.fontSize,
                                          ghostTextPos,
                                          ImGui::ColorConvertFloat4ToU32(ghostCol),
                                          seg.displayText.c_str());
                        ScaleNewVerticesX(
                            drawList, segmentVertexStart, ghostTextPos.x, seg.horizontalScale);
                        ghostCursorX += seg.size.x + layout.segmentPadding;
                    }
                }

                const int chFront = gpuGlow ? 2 : 1;
                splitter->SetCurrentChannel(drawList, chFront);
            }
        }
    }

    if (!graffito.active)
    {
        DrawBackgroundGlow(drawList, style, layout, df.lodTitleFactor, splitter, snap);
        DrawParticlesAndOrnaments(drawList,
                                  d,
                                  style,
                                  layout,
                                  df.lodEffectsFactor,
                                  time,
                                  splitter,
                                  snap.fastOutlines,
                                  snap);
    }
    if (graffito.active)
    {
        DrawGraffitoDecorations(
            drawList, splitter, d, style, layout, graffito, df.lodEffectsFactor, time, snap);
        DrawGraffitoFrontInk(
            drawList, splitter, style, layout, graffito, df.lodTitleFactor, time, snap);
    }
    else
    {
        DrawTitleText(
            drawList, style, layout, df.lodTitleFactor, splitter, snap.fastOutlines, snap);
        DrawMainLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
        DrawInfoLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
        DrawBadges(drawList, style, layout, splitter, snap.fastOutlines, snap, graffito.active);
        DrawTierEmblem(drawList, style, layout, time, splitter, snap, graffito.active);
    }
    if (frontDepthBracket)
    {
        EndPlateDepthClip(drawList, splitter, snap);
    }
    if (graffito.active)
    {
        DrawGraffitoFacetMarker(drawList, splitter, d, style, layout, graffito, snap);
    }
}

/**
 * @struct DeathRitePhases
 * @brief Normalized ink-drain and farewell progress for a death animation.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Rite phase math (pure - mirrored in tests/test_utils.cpp; keep in sync).
 * T in [0,1]:  hold [0,0.22)  drain [0.22,0.55)  farewell [0.55,1].  During
 * the hold the plate is untouched; the drain pulls the ink toward its rite
 * color; the farewell fades it out, and per creature crumbles or drifts it.
 */
struct DeathRitePhases
{
    float drainT;     // Ink drain progress [0,1]
    float dissolveT;  // Farewell progress [0,1]
};
/**
 * @fn static DeathRitePhases ComputeDeathRitePhases(float t)
 * @brief Resolve the ink-drain and farewell progress after the initial hold.
 * @author Alex (<https://github.com/lextpf>)
 */
static DeathRitePhases ComputeDeathRitePhases(float t)
{
    constexpr float kHoldEnd = .22f;
    constexpr float kDrainEnd = .55f;
    DeathRitePhases p{};
    p.drainT = TextEffects::Saturate((t - kHoldEnd) / (kDrainEnd - kHoldEnd));
    p.dissolveT = TextEffects::Saturate((t - kDrainEnd) / (1.0f - kDrainEnd));
    return p;
}

/**
 * @fn static int CountLayoutChars(const LabelLayout& layout)
 * @brief Count revealed UTF-8 characters in the typewriter accounting order.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Total revealed characters across a computed layout (main row -> info row ->
 * title, matching the typewriter's accounting order).
 */
static int CountLayoutChars(const LabelLayout& layout)
{
    size_t total = Utf8CharCount(layout.titleStr.c_str());
    for (const auto& seg : layout.segments)
    {
        total += Utf8CharCount(seg.text.c_str());
    }
    for (const auto& seg : layout.infoSegments)
    {
        total += Utf8CharCount(seg.text.c_str());
    }
    return static_cast<int>(total);
}

/**
 * @fn static void DrawDyingLabel(ActorCache& entry, const ActorDrawData& d, ImDrawList* drawList,
 *     ImDrawListSplitter* splitter, const RenderSettingsSnapshot& snap)
 * @brief Replay the last live plate through its one-shot death animation.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Freeze the last live world anchor and yaw for the one-shot rite. Graffito
 * reprojects them as the camera moves; ragdoll motion cannot drag the inscription.
 * Undead crumble, dragons sear, others fade and sink.
 */
static void DrawDyingLabel(ActorCache& entry,
                           const ActorDrawData& d,
                           ImDrawList* drawList,
                           ImDrawListSplitter* splitter,
                           const RenderSettingsSnapshot& snap)
{
    const float dt = ImGui::GetIO().DeltaTime;
    entry.deathPhase =
        std::min(1.0f, entry.deathPhase + dt / std::max(snap.deathRiteDuration, .05f));
    if (entry.deathPhase >= 1.0f)
    {
        entry.deathDone = true;
        entry.exitPhase = 1.0f;  // The rite is the exit - never replay one
        return;
    }
    const DeathRitePhases ph = ComputeDeathRitePhases(entry.deathPhase);

    const bool sear = d.creatureKind == CreatureKind::Dragon;
    const bool crumble = d.creatureKind == CreatureKind::Undead;

    const float epitaphProgress = snap.graffito.FallenEpitaphEnabled ? ph.drainT : .0f;
    // Freeze yaw so the landed inscription cannot swivel.
    GraffitoPlate graffito = PrepareGraffitoPlate(d, entry, snap, .0f, epitaphProgress);
    const bool graffitoMode = graffito.requested && Graffito::IsInitialized();
    if (graffitoMode && !graffito.active)
    {
        return;
    }
    const bool fallenEpitaphMode = graffito.active && snap.graffito.FallenEpitaphEnabled;

    const float fade = sear ? std::pow(ph.dissolveT, 1.35f) : ph.dissolveT;
    const float alpha = entry.alphaSmooth * entry.occlusionSmooth * GetState().regAlphaMul *
                        (1.0f - fade) * graffito.visibility;
    if (alpha < .01f)
    {
        return;
    }
    const float textSizeScale = graffito.active ? 1.0f : entry.textSizeScale;

    ImFont* nameFont = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    ImFont* levelFont = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    ImFont* titleFont = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!nameFont || !levelFont || !titleFont)
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    LabelStyle style = ComputeLabelStyle(d, entry.cachedNameLower, alpha, time, snap);
    if (graffito.active)
    {
        style.tierAllowsGlow = false;
        style.outlineGlowAllowed = false;
    }
    style.nameOutlineWidth = style.CalcOutlineWidth(nameFont->FontSize * textSizeScale, snap);
    style.levelOutlineWidth = style.CalcOutlineWidth(levelFont->FontSize * textSizeScale, snap);
    style.titleOutlineWidth = style.CalcOutlineWidth(titleFont->FontSize * textSizeScale, snap);
    style.outlineWidth = style.nameOutlineWidth;

    if (sear)
    {
        constexpr ImVec4 kSearBright{1.0f, .92f, .78f, 1.0f};
        constexpr ImVec4 kSearDark{.05f, .04f, .04f, 1.0f};
        const float searIn = TextEffects::Saturate(ph.drainT * 2.0f);
        const float searOut =
            TextEffects::Saturate((ph.drainT - .5f) * 2.0f) * .85f + ph.dissolveT * .15f;
        ApplyDeathRiteTint(style, kSearBright, searIn * .85f, snap);
        ApplyDeathRiteTint(style, kSearDark, searOut, snap);
    }
    else
    {
        constexpr ImVec4 kAsh{.62f, .60f, .57f, 1.0f};
        ApplyDeathRiteTint(style, kAsh, ph.drainT, snap);
    }

    // Sub-lines fade with the drain; the farewell applies to name and title.
    const float subMul =
        (1.0f - ph.drainT) * (1.0f - GetState().quietSub) * GetState().regSubLineMul;
    style.infoAlphaMul = subMul;
    style.badgeAlphaMul = subMul;
    style.levelAlpha *= 1.0f - ph.drainT * .5f;

    int forcedChars = -1;
    if (crumble && ph.dissolveT > .0f)
    {
        LabelLayout probe = ComputeLabelLayout(
            d, entry, style, textSizeScale, snap, (std::numeric_limits<int>::max)());
        const int total = CountLayoutChars(probe);
        forcedChars =
            static_cast<int>(std::ceil(static_cast<float>(total) * (1.0f - ph.dissolveT)));
    }

    LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale, snap, forcedChars);
    if (graffito.active)
    {
        TranslateLayout(
            layout,
            ImVec2(graffito.screenPos.x - entry.smooth.x, graffito.screenPos.y - entry.smooth.y));
    }

    // Dragons rise; undead stay fixed; other creatures sink.
    float yOffset = .0f;
    if (!fallenEpitaphMode)
    {
        if (sear)
        {
            yOffset = -10.0f * TextEffects::EaseInCubic(ph.dissolveT);
        }
        else if (!crumble)
        {
            yOffset = 6.0f * TextEffects::EaseInCubic(ph.dissolveT);
        }
    }
    TranslateLayout(layout, ImVec2(.0f, yOffset));

    DistanceFactors df = ComputeDistanceFactors(d, snap);

    if (graffito.active && !FinalizeGraffitoPlate(graffito, layout, style, snap))
    {
        return;
    }

    if (graffito.active)
    {
        DrawGraffitoRelief(drawList, splitter, style, layout, graffito, snap);
    }

    // Billboards share anchor depth; Graffito brackets each physical surface.
    const bool frontDepthBracket = GetState().depthClipFrame && !graffito.active;
    if (frontDepthBracket)
    {
        RE::NiPoint3 ritePos{};
        void* params = WorldToScreen(d.worldPos, ritePos) ? DepthClip::MakePlateParams(ritePos.z)
                                                          : DepthClip::MakeNeutralParams();
        BracketPlateDepthClip(drawList, splitter, params, snap);
    }

    // Rites retain only text and informational marks.
    if (!graffito.active)
    {
        DrawBackgroundGlow(drawList, style, layout, df.lodTitleFactor, splitter, snap);
    }
    if (graffito.active)
    {
        DrawGraffitoFrontInk(
            drawList, splitter, style, layout, graffito, df.lodTitleFactor, time, snap);
    }
    else
    {
        DrawTitleText(
            drawList, style, layout, df.lodTitleFactor, splitter, snap.fastOutlines, snap);
        DrawMainLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
        DrawInfoLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
        DrawBadges(drawList, style, layout, splitter, snap.fastOutlines, snap, graffito.active);
        DrawTierEmblem(drawList, style, layout, time, splitter, snap, graffito.active);
    }
    if (frontDepthBracket)
    {
        EndPlateDepthClip(drawList, splitter, snap);
    }
    if (graffito.active)
    {
        DrawGraffitoFacetMarker(drawList, splitter, d, style, layout, graffito, snap);
    }
}

/**
 * @fn static void DrawExitingLabel(ActorCache& entry, const ActorDrawData& d, ImDrawList* drawList,
 *     ImDrawListSplitter* splitter, const RenderSettingsSnapshot& snap)
 * @brief Replay the last plate while its exit animation completes.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Exit replays a smoothed billboard anchor or reprojected Graffito world pose.
 */
static void DrawExitingLabel(ActorCache& entry,
                             const ActorDrawData& d,
                             ImDrawList* drawList,
                             ImDrawListSplitter* splitter,
                             const RenderSettingsSnapshot& snap)
{
    const float dt = ImGui::GetIO().DeltaTime;
    entry.exitPhase += dt / std::max(snap.exitDuration, .05f);
    if (entry.exitPhase >= 1.0f)
    {
        entry.exitPhase = 1.0f;
    }

    // Ease-in: the label lingers, then accelerates away.
    const float e = TextEffects::EaseInCubic(entry.exitPhase);
    const float exitAlphaMul = 1.0f - e;
    const float exitScaleMul = 1.0f - .06f * e;
    const float exitYOffset = RenderConstants::EXIT_SINK_PX * e;

    GraffitoPlate graffito = PrepareGraffitoPlate(d, entry, snap, dt);
    const bool graffitoMode = graffito.requested && Graffito::IsInitialized();
    if (graffitoMode && !graffito.active)
    {
        return;
    }

    const float alpha = entry.alphaSmooth * entry.occlusionSmooth * exitAlphaMul *
                        GetState().regAlphaMul * graffito.visibility;
    if (alpha < .01f)
    {
        return;
    }
    const float textSizeScale = (graffito.active ? 1.0f : entry.textSizeScale) * exitScaleMul;

    ImFont* nameFont = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    ImFont* levelFont = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    ImFont* titleFont = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!nameFont || !levelFont || !titleFont)
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    LabelStyle style = ComputeLabelStyle(d, entry.cachedNameLower, alpha, time, snap);
    if (graffito.active)
    {
        style.tierAllowsGlow = false;
        style.outlineGlowAllowed = false;
    }
    style.nameOutlineWidth = style.CalcOutlineWidth(nameFont->FontSize * textSizeScale, snap);
    style.levelOutlineWidth = style.CalcOutlineWidth(levelFont->FontSize * textSizeScale, snap);
    style.titleOutlineWidth = style.CalcOutlineWidth(titleFont->FontSize * textSizeScale, snap);
    style.outlineWidth = style.nameOutlineWidth;

    // Keep ambient focus dimming and the live plate camera-pan fold.
    const float paneFold = 1.0f - GetState().quietSub;
    const float regSub = GetState().regSubLineMul;
    const bool focusApplies = snap.focus.Enabled && !d.isPlayer;
    const float auxAlphaMul = focusApplies ? entry.focusSmooth : 1.0f;
    style.titleAlpha *= auxAlphaMul * regSub * paneFold;
    style.infoAlphaMul = auxAlphaMul * regSub;
    style.levelAlpha *= regSub;
    style.badgeAlphaMul = regSub * paneFold;

    DistanceFactors df = ComputeDistanceFactors(d, snap);

    LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale, snap);
    if (graffito.active)
    {
        TranslateLayout(
            layout,
            ImVec2(graffito.screenPos.x - entry.smooth.x, graffito.screenPos.y - entry.smooth.y));
    }
    TranslateLayout(layout, ImVec2(.0f, exitYOffset));

    if (graffito.active && !FinalizeGraffitoPlate(graffito, layout, style, snap))
    {
        return;
    }

    if (graffito.active)
    {
        DrawGraffitoRelief(drawList, splitter, style, layout, graffito, snap);
    }

    // Failed ghost projection draws unclipped to avoid inheriting another plate
    // depth. Graffito brackets each layer independently.
    const bool frontDepthBracket = GetState().depthClipFrame && !graffito.active;
    if (frontDepthBracket)
    {
        RE::NiPoint3 ghostPos{};
        void* params = WorldToScreen(d.worldPos, ghostPos) ? DepthClip::MakePlateParams(ghostPos.z)
                                                           : DepthClip::MakeNeutralParams();
        BracketPlateDepthClip(drawList, splitter, params, snap);
    }

    if (!graffito.active)
    {
        DrawBackgroundGlow(drawList, style, layout, df.lodTitleFactor, splitter, snap);
        DrawParticlesAndOrnaments(drawList,
                                  d,
                                  style,
                                  layout,
                                  df.lodEffectsFactor,
                                  time,
                                  splitter,
                                  snap.fastOutlines,
                                  snap);
    }
    if (graffito.active)
    {
        DrawGraffitoDecorations(
            drawList, splitter, d, style, layout, graffito, df.lodEffectsFactor, time, snap);
        DrawGraffitoFrontInk(
            drawList, splitter, style, layout, graffito, df.lodTitleFactor, time, snap);
    }
    else
    {
        DrawTitleText(
            drawList, style, layout, df.lodTitleFactor, splitter, snap.fastOutlines, snap);
        DrawMainLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
        DrawInfoLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
        DrawBadges(drawList, style, layout, splitter, snap.fastOutlines, snap, graffito.active);
        DrawTierEmblem(drawList, style, layout, time, splitter, snap, graffito.active);
    }
    if (frontDepthBracket)
    {
        EndPlateDepthClip(drawList, splitter, snap);
    }
    if (graffito.active)
    {
        DrawGraffitoFacetMarker(drawList, splitter, d, style, layout, graffito, snap);
    }
}

/**
 * @fn static void DrawDebugOverlay(const RenderSettingsSnapshot& snap)
 * @brief Publish renderer state to the enabled diagnostic overlay.
 * @author Alex (<https://github.com/lextpf>)
 */
static void DrawDebugOverlay(const RenderSettingsSnapshot& snap)
{
    if (!snap.enableDebugOverlay)
    {
        return;
    }

    const float time = static_cast<float>(ImGui::GetTime());
    const float dt = ImGui::GetIO().DeltaTime;

    DebugOverlay::UpdateFrameStats(GetState().debugStats,
                                   dt,
                                   time,
                                   GetState().lastDebugUpdateTime,
                                   GetState().updateCounter,
                                   GetState().lastUpdateCount);

    GetState().debugStats.cacheSize = GetState().cache.size();

    DebugOverlay::Context ctx;
    ctx.stats = &GetState().debugStats;
    ctx.frameNumber = GetState().frame;
    ctx.postLoadCooldown = GetState().postLoadCooldown;
    ctx.lastReloadTime = GetState().lastReloadTime;
    ctx.actorCacheEntrySize = sizeof(ActorCache);
    ctx.actorDrawDataSize = sizeof(ActorDrawData);
    ctx.occlusionEnabled = snap.enableOcclusionCulling;
    ctx.glowEnabled = snap.enableGlow;
    ctx.typewriterEnabled = snap.enableTypewriter;
    ctx.hidePlayer = snap.hidePlayer;
    ctx.verticalOffset = snap.verticalOffset;
    ctx.maxPlates = snap.maxPlates;
    ctx.maxScanActors = snap.maxScanActors;
    ctx.tierCount = snap.tiers.size();
    ctx.reloadKey = snap.reloadKey;

    DebugOverlay::Render(ctx);
}

/**
 * @fn static void HandleHotReload()
 * @brief Coordinate settings reload with snapshot tasks and render-owned caches.
 * @author Alex (<https://github.com/lextpf>)
 *
 * A key edge latches the request. Later frames retry while snapshot work is active;
 * the render thread does not wait for that work. Pause is checked again before loading.
 * Completion clears the render cache and requests a game-thread occlusion-cache clear.
 *
 * ```mermaid
 * flowchart LR
 *     Request[Request latched] --> Idle{Snapshot tasks idle?}
 *     Idle -->|No| Request
 *     Idle -->|Yes| Pause[Pause and recheck]
 *     Pause --> Load[Load settings]
 *     Load --> Finish[Render thread clears cache and resumes snapshots]
 * ```
 *
 * Without the SKSE task interface, loading runs synchronously on the render thread.
 */
static void HandleHotReload()
{
    // Finalize reload here: only the render thread may clear its cache.
    if (GetState().reloadCompleted.exchange(false, std::memory_order_acq_rel))
    {
        GetState().lastReloadTime = static_cast<float>(ImGui::GetTime());
        GetState().cache.clear();
        GetState().clearOcclusionCacheRequested.store(true, std::memory_order_release);
        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
        GetState().reloadRequested.store(false, std::memory_order_release);
    }

    int reloadKey = 0;
    {
        const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
        reloadKey = Settings::Display().ReloadKey;
    }

    if (reloadKey <= 0)
    {
        return;
    }

    bool keyDown = (GetAsyncKeyState(reloadKey) & 0x8000) != 0;

    if (keyDown && !GetState().reloadKeyWasDown)
    {
        GetState().reloadRequested.store(true, std::memory_order_release);
    }

    if (!GetState().reloadRequested.load(std::memory_order_acquire))
    {
        GetState().reloadKeyWasDown = keyDown;
        return;
    }

    // Defer reload until in-flight snapshot updates are done; no render-thread busy wait.
    const bool queued = GetState().updateQueued.load(std::memory_order_acquire);
    const bool running = GetState().snapshotUpdateRunning.load(std::memory_order_acquire);
    if (queued || running)
    {
        GetState().reloadKeyWasDown = keyDown;
        return;
    }

    // seq_cst orders the pause store against loads of different in-flight atomics;
    // release/acquire alone supplies no cross-atomic ordering.
    GetState().pauseSnapshotUpdates.store(true, std::memory_order_seq_cst);
    const bool queuedAfterPause = GetState().updateQueued.load(std::memory_order_seq_cst);
    const bool runningAfterPause = GetState().snapshotUpdateRunning.load(std::memory_order_seq_cst);
    if (queuedAfterPause || runningAfterPause)
    {
        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
        GetState().reloadKeyWasDown = keyDown;
        return;
    }

    // Game-thread file I/O keeps the render thread paused until reloadCompleted.
    if (auto* task = SKSE::GetTaskInterface())
    {
        task->AddTask(
            []()
            {
                Settings::Load();

                GetState().reloadCompleted.store(true, std::memory_order_release);
            });
    }
    else
    {
        // Fallback: synchronous reload if task interface is unavailable
        Settings::Load();
        GetState().lastReloadTime = static_cast<float>(ImGui::GetTime());
        GetState().cache.clear();
        GetState().clearOcclusionCacheRequested.store(true, std::memory_order_release);

        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
        GetState().reloadRequested.store(false, std::memory_order_release);
    }

    GetState().reloadKeyWasDown = keyDown;
}

/**
 * @fn static void UpdateDebugStats(const std::vector<ActorDrawData>& snap, bool hidePlayer)
 * @brief Refresh diagnostic counts from the frame snapshot at a bounded rate.
 * @author Alex (<https://github.com/lextpf>)
 */
static void UpdateDebugStats(const std::vector<ActorDrawData>& snap, bool hidePlayer)
{
    GetState().debugStats.actorCount = 0;
    GetState().debugStats.visibleActors = 0;
    GetState().debugStats.occludedActors = 0;
    GetState().debugStats.playerVisible = 0;

    for (const auto& d : snap)
    {
        if (d.deckOnly || (hidePlayer && d.isPlayer))
        {
            continue;
        }
        GetState().debugStats.actorCount++;
        if (d.isPlayer)
        {
            GetState().debugStats.playerVisible = 1;
        }
        if (d.isOccluded)
        {
            GetState().debugStats.occludedActors++;
        }
        else
        {
            GetState().debugStats.visibleActors++;
        }
    }
    ++GetState().updateCounter;
}

/**
 * @fn static uint32_t RaycastCameraActor(const std::vector<ActorDrawData>& snap, const
 *     RenderSettingsSnapshot& snapSettings)
 * @brief Select the nearest eligible snapshot sphere along the current camera ray.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Ray-test snapshot actor bounds each render frame, independent of HUD target
 * filtering and game-thread lag. Zero means unavailable or no eligible hit.
 */
static uint32_t RaycastCameraActor(const std::vector<ActorDrawData>& snap,
                                   const RenderSettingsSnapshot& snapSettings)
{
    if (!snapSettings.graffito.Enabled || snap.empty())
    {
        return 0;
    }

    RE::NiPoint3 cameraPosition{};
    RE::NiPoint3 cameraForward{};
    if (!Occlusion::GetCameraInfo(cameraPosition, cameraForward))
    {
        return 0;
    }

    uint32_t bestID = 0;
    double bestHit = std::numeric_limits<double>::infinity();
    for (const auto& d : snap)
    {
        if (d.isPlayer || d.deckOnly || d.isDead || d.isOccluded || d.raycastRadius <= .0f ||
            (snapSettings.graffito.MaxDistance > .0f &&
             d.distToPlayer > snapSettings.graffito.MaxDistance))
        {
            continue;
        }

        const double hit =
            Graffito::Math::RaySphereHitDistance(ToGraffitoMath(cameraPosition),
                                                 ToGraffitoMath(cameraForward),
                                                 ToGraffitoMath(d.raycastCenter),
                                                 static_cast<double>(d.raycastRadius));
        if (hit < bestHit || (hit == bestHit && d.formID < bestID))
        {
            bestHit = hit;
            bestID = d.formID;
        }
    }
    return bestID;
}

uint32_t SelectFocusedActor(const std::vector<ActorDrawData>& snap,
                            const RenderSettingsSnapshot& snapSettings)
{
    if (!snapSettings.focus.Enabled || snap.empty())
    {
        return 0;
    }

    RE::NiPoint3 camPos{};
    RE::NiPoint3 camFwd{};
    if (!Occlusion::GetCameraInfo(camPos, camFwd))
    {
        return 0;
    }

    // Zero adds no bound beyond the game-thread MaxScanDistance filter.
    const float focusMaxDist =
        snapSettings.focus.MaxDistance > .0f ? snapSettings.focus.MaxDistance : 1e9f;
    const float maxDistSq = focusMaxDist * focusMaxDist;

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float coneRadians = snapSettings.focus.ConeAngleDegrees * kDegToRad;
    const float cosMinDot = std::cos(coneRadians);

    uint32_t bestID = 0;
    float bestDot = -2.0f;  // Higher dot = closer to forward (1.0 = perfect)
    float bestDist = std::numeric_limits<float>::infinity();

    for (const auto& d : snap)
    {
        if (d.isPlayer || d.deckOnly || d.isDead)
        {
            continue;  // Neither the player nor the dead can hold focus.
        }
        if (snapSettings.focus.IgnoreOccluded && d.isOccluded)
        {
            continue;
        }

        RE::NiPoint3 toActor = d.worldPos - camPos;
        const float len = toActor.Length();
        if (len < 1.0f)
        {
            continue;  // Coincident with camera; skip to avoid divide-by-zero.
        }
        if (len * len > maxDistSq)
        {
            continue;
        }

        const float invLen = 1.0f / len;
        toActor.x *= invLen;
        toActor.y *= invLen;
        toActor.z *= invLen;

        const float dot = toActor.x * camFwd.x + toActor.y * camFwd.y + toActor.z * camFwd.z;
        if (dot < cosMinDot)
        {
            continue;  // Outside cone.
        }

        const bool better = dot > bestDot || (dot == bestDot && d.distToPlayer < bestDist) ||
                            (dot == bestDot && d.distToPlayer == bestDist && d.formID < bestID);
        if (better)
        {
            bestDot = dot;
            bestDist = d.distToPlayer;
            bestID = d.formID;
        }
    }

    return bestID;
}

/**
 * @fn static void ResolveOverlaps(const std::vector<ActorDrawData>& localSnap, const
 *     RenderSettingsSnapshot& snap)
 * @brief Assign vertical offsets that separate projected plate rectangles.
 * @author Alex (<https://github.com/lextpf>)
 */
static void ResolveOverlaps(const std::vector<ActorDrawData>& localSnap,
                            const RenderSettingsSnapshot& snap)
{
    struct LabelRect
    {
        int idx;
        float cy, halfH, dist, yOffset;
        bool isPlayer;
    };
    std::vector<LabelRect> labelRects;

    for (int i = 0; i < static_cast<int>(localSnap.size()); ++i)
    {
        const auto& d = localSnap[i];
        if (d.deckOnly || d.isDead || (snap.hidePlayer && !snap.graffito.Enabled && d.isPlayer))
        {
            continue;  // A dying plate is frozen - it neither pushes nor moves.
        }
        auto cIt = GetState().cache.find(d.formID);
        if (cIt == GetState().cache.end() || !cIt->second.initialized)
        {
            continue;
        }

        const auto& entry = cIt->second;
        if (entry.alphaSmooth * entry.occlusionSmooth <= .02f)
        {
            continue;
        }

        // Layout runs later; approximate height as 1.5x font size.
        float approxHeight = snap.nameFontSize * entry.textSizeScale * 1.5f;
        labelRects.push_back(
            {i, entry.smooth.y, approxHeight * .5f, d.distToPlayer, .0f, d.isPlayer});
    }

    // Sort by priority: player first, then closest first
    std::sort(labelRects.begin(),
              labelRects.end(),
              [](const LabelRect& a, const LabelRect& b)
              {
                  if (a.isPlayer != b.isPlayer)
                  {
                      return a.isPlayer > b.isPlayer;
                  }
                  return a.dist < b.dist;
              });

    // Iterative relaxation: push lower-priority labels down
    float padding = snap.visual.OverlapPaddingY;
    for (int pass = 0; pass < snap.visual.OverlapIterations; ++pass)
    {
        for (int i = 0; i < static_cast<int>(labelRects.size()); ++i)
        {
            for (int j = i + 1; j < static_cast<int>(labelRects.size()); ++j)
            {
                float overlap =
                    (labelRects[i].cy + labelRects[i].yOffset + labelRects[i].halfH + padding) -
                    (labelRects[j].cy + labelRects[j].yOffset - labelRects[j].halfH);
                if (overlap > .0f)
                {
                    labelRects[j].yOffset += overlap;
                }
            }
        }
    }

    // Layout applies stored offsets; discard shifts below 0.01 px.
    for (const auto& lr : labelRects)
    {
        if (std::abs(lr.yOffset) > .01f)
        {
            OverlapOffsets()[localSnap[lr.idx].formID] = lr.yOffset;
        }
    }
}

RenderSettingsSnapshot RenderSettingsSnapshot::CaptureFromSettings()
{
    RenderSettingsSnapshot snap;
    const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());

    const auto& dist = Settings::Distance();
    snap.fadeStartDistance = dist.FadeStartDistance;
    snap.fadeEndDistance = dist.FadeEndDistance;
    snap.scaleStartDistance = dist.ScaleStartDistance;
    snap.scaleEndDistance = dist.ScaleEndDistance;
    snap.minimumScale = dist.MinimumScale;

    const auto& so = Settings::ShadowOutline();
    snap.outlineWidthMin = so.OutlineWidthMin;
    snap.outlineWidthMax = so.OutlineWidthMax;
    snap.titleShadowOffsetX = so.TitleShadowOffsetX;
    snap.titleShadowOffsetY = so.TitleShadowOffsetY;
    snap.mainShadowOffsetX = so.MainShadowOffsetX;
    snap.mainShadowOffsetY = so.MainShadowOffsetY;
    snap.fastOutlines = so.FastOutlines;
    snap.enableOutlineGlow = so.OutlineGlowEnabled;
    snap.outlineGlowScale = so.OutlineGlowScale;
    snap.outlineGlowAlpha = so.OutlineGlowAlpha;
    snap.outlineGlowRings = so.OutlineGlowRings;
    snap.outlineGlowR = so.OutlineGlowR;
    snap.outlineGlowG = so.OutlineGlowG;
    snap.outlineGlowB = so.OutlineGlowB;
    snap.outlineGlowTierTint = so.OutlineGlowTierTint;
    snap.dualOutlineEnabled = so.DualOutlineEnabled;
    snap.innerOutlineTint = so.InnerOutlineTint;
    snap.innerOutlineAlpha = so.InnerOutlineAlpha;
    snap.innerOutlineScale = so.InnerOutlineScale;
    snap.directionalLightAngle = so.DirectionalLightAngle;
    snap.directionalLightBias = so.DirectionalLightBias;
    snap.outlineColorTint = so.OutlineColorTint;
    snap.shadowColorTint = so.ShadowColorTint;
    snap.softShadowEnabled = so.SoftShadowEnabled;
    snap.softShadowDistance = so.SoftShadowDistance;
    snap.softShadowSoftness = so.SoftShadowSoftness;
    snap.softShadowOpacity = so.SoftShadowOpacity;
    snap.softShadowAngle = so.SoftShadowAngle;
    snap.softShadowSamples = so.SoftShadowSamples;

    const auto& gl = Settings::Glow();
    snap.enableGlow = gl.Enabled;
    snap.glowRadius = gl.Radius;
    snap.glowIntensity = gl.Intensity;
    snap.glowSamples = gl.Samples;
    snap.glowDivideStrength = gl.DivideStrength;

    const auto& sh = Settings::Shine();
    snap.enableShine = sh.Enabled;
    snap.shineIntensity = sh.Intensity;
    snap.shineFalloff = sh.Falloff;
    snap.textGlowAlpha = sh.TextGlowAlpha;

    const auto& tw = Settings::Typewriter();
    snap.enableTypewriter = tw.Enabled;
    snap.typewriterSpeed = tw.Speed;
    snap.typewriterDelay = tw.Delay;

    const auto& tr = Settings::Transition();
    snap.enableEntrance = tr.EnableEntrance;
    snap.entranceStyle = tr.EntranceStyle;
    snap.entranceDuration = tr.EntranceDuration;
    snap.enableExit = tr.EnableExit;
    snap.exitDuration = tr.ExitDuration;
    snap.entranceStaggerStep = tr.EntranceStaggerStep;
    snap.entranceStaggerMax = tr.EntranceStaggerMax;

    const auto& orn = Settings::Ornament();
    snap.enableOrnaments = orn.Enabled;
    snap.ornamentScale = orn.Scale;
    snap.ornamentSpacing = orn.Spacing;
    snap.ornamentFontPath = orn.FontPath;
    snap.ornamentFontSize = orn.FontSize;
    snap.ornamentAnchorToMainLine = orn.AnchorToMainLine;
    snap.ornamentOffsetY = orn.OffsetY;

    const auto& part = Settings::Particle();
    snap.enableParticleAura = part.Enabled;
    snap.useParticleTextures = part.UseParticleTextures;
    snap.particleCount = part.Count;
    snap.particleSize = part.Size;
    snap.particleSpeed = part.Speed;
    snap.particleSpread = part.Spread;
    snap.particleAlpha = part.Alpha;
    snap.particleBlendMode = part.BlendMode;
    snap.particleDepthStrength = part.DepthStrength;
    snap.particleColorWarmth = part.ColorWarmth;
    snap.particleGlowStrength = part.GlowStrength;
    snap.particleGlowSize = part.GlowSize;
    snap.particleShineThreshold = part.ShineThreshold;

    const auto& ac = Settings::AnimColor();
    snap.innerTextAlpha = ac.InnerTextAlpha;
    snap.outlineAlpha = ac.OutlineAlpha;
    snap.alphaSettleTime = ac.AlphaSettleTime;
    snap.scaleSettleTime = ac.ScaleSettleTime;
    snap.positionSettleTime = ac.PositionSettleTime;
    snap.occlusionSettleTime = Settings::Occlusion().SettleTime;

    const auto& nc = Settings::NpcColors();
    snap.npcColors.neutral = nc.NeutralColor;
    snap.npcColors.hostile = nc.HostileColor;
    snap.npcColors.follower = nc.FollowerColor;
    snap.npcColors.level = nc.LevelColor;
    snap.npcColors.title = nc.TitleColor;

    const auto& disp = Settings::Display();
    snap.enableDebugOverlay = disp.EnableDebugOverlay;
    snap.enableOcclusionCulling = Settings::Occlusion().Enabled;
    snap.verticalOffset = disp.VerticalOffset;
    snap.horizontalOffset = disp.HorizontalOffset;
    snap.hidePlayer = disp.HidePlayer;
    snap.maxPlates = disp.MaxPlates;
    snap.maxScanActors = disp.MaxScanActors;
    snap.reloadKey = disp.ReloadKey;

    snap.nameFontSize = Settings::Font().NameFontSize;
    snap.visual = Settings::Visual();
    snap.tiers = Settings::Tiers();
    snap.titleFormat = Settings::TitleFormat();
    snap.displayFormat = Settings::DisplayFormat();
    snap.infoFormat = Settings::InfoFormat();
    snap.specialTitles = Settings::SpecialTitles();

    const auto& lb = Settings::Labels();
    snap.labels.relFollower = lb.RelationshipFollower;
    snap.labels.relAlly = lb.RelationshipAlly;
    snap.labels.relNeutral = lb.RelationshipNeutral;
    snap.labels.relHostile = lb.RelationshipHostile;
    snap.labels.ldWeak = lb.LevelDeltaWeak;
    snap.labels.ldEven = lb.LevelDeltaEven;
    snap.labels.ldStrong = lb.LevelDeltaStrong;
    snap.labels.ldDeadly = lb.LevelDeltaDeadly;
    snap.labels.ctNPC = lb.CreatureTypeNPC;
    snap.labels.ctBeast = lb.CreatureTypeBeast;
    snap.labels.ctUndead = lb.CreatureTypeUndead;
    snap.labels.ctDaedra = lb.CreatureTypeDaedra;
    snap.labels.ctDragon = lb.CreatureTypeDragon;
    snap.labels.deltaWeakBelow = lb.WeakAtOrBelow;
    snap.labels.deltaStrongAbove = lb.StrongAtOrAbove;
    snap.labels.deltaDeadlyAbove = lb.DeadlyAtOrAbove;

    const auto& ic = Settings::Icons();
    snap.icons.enabled = ic.Enabled && !ic.Folder.empty();
    snap.icons.scale = ic.Scale;
    snap.icons.opacity = ic.Opacity;
    snap.icons.deadlyPulse = ic.DeadlyPulse;
    snap.icons.icoFollower = ic.FollowerIcon;
    snap.icons.icoAlly = ic.AllyIcon;
    snap.icons.icoHostile = ic.HostileIcon;
    snap.icons.icoWeak = ic.WeakIcon;
    snap.icons.icoStrong = ic.StrongIcon;
    snap.icons.icoDeadly = ic.DeadlyIcon;
    snap.icons.icoBeast = ic.BeastIcon;
    snap.icons.icoUndead = ic.UndeadIcon;
    snap.icons.icoDaedra = ic.DaedraIcon;
    snap.icons.icoDragon = ic.DragonIcon;
    snap.icons.colFollower = ic.FollowerColor;
    snap.icons.colAlly = ic.AllyColor;
    snap.icons.colHostile = ic.HostileColor;
    snap.icons.colWeak = ic.WeakColor;
    snap.icons.colStrong = ic.StrongColor;
    snap.icons.colDeadly = ic.DeadlyColor;
    snap.icons.colCreature = ic.CreatureColor;
    snap.icons.icoNeutral = ic.NeutralIcon;
    snap.icons.icoHumanoid = ic.HumanoidIcon;
    snap.icons.icoEven = ic.EvenIcon;
    snap.icons.icoGuard = ic.GuardIcon;
    snap.icons.icoMerchant = ic.MerchantIcon;
    snap.icons.icoCommoner = ic.CommonerIcon;
    snap.icons.icoEssential = ic.EssentialIcon;
    snap.icons.icoProtected = ic.ProtectedIcon;
    snap.icons.icoMortal = ic.MortalIcon;
    snap.icons.icoCombat = ic.CombatIcon;
    snap.icons.icoAlert = ic.AlertIcon;
    snap.icons.icoIdle = ic.IdleIcon;
    snap.icons.icoSneakHidden = ic.SneakHiddenIcon;
    snap.icons.icoSneakDetected = ic.SneakDetectedIcon;
    snap.icons.icoSneakOff = ic.SneakOffIcon;
    snap.icons.icoEncumbered = ic.EncumberedIcon;
    snap.icons.icoNormalWeight = ic.NormalWeightIcon;
    snap.icons.icoWanted = ic.WantedIcon;
    snap.icons.icoBountyClear = ic.BountyClearIcon;
    snap.icons.icoTierLow = ic.TierLowIcon;
    snap.icons.icoTierMid = ic.TierMidIcon;
    snap.icons.icoTierHigh = ic.TierHighIcon;
    snap.icons.colGuard = ic.GuardColor;
    snap.icons.colMerchant = ic.MerchantColor;
    snap.icons.colEssential = ic.EssentialColor;
    snap.icons.colProtected = ic.ProtectedColor;
    snap.icons.colCombat = ic.CombatColor;
    snap.icons.colAlert = ic.AlertColor;
    snap.icons.colSneakHidden = ic.SneakHiddenColor;
    snap.icons.colSneakDetected = ic.SneakDetectedColor;
    snap.icons.colEncumbered = ic.EncumberedColor;
    snap.icons.colWanted = ic.WantedColor;
    snap.icons.colTierLow = ic.TierLowColor;
    snap.icons.colTierMid = ic.TierMidColor;
    snap.icons.colTierHigh = ic.TierHighColor;
    snap.icons.tierBadgeImages = ic.TierBadgeImages;
    snap.icons.tierBadgeGamma = ic.TierBadgeGamma;
    snap.icons.tierBadgeScale = ic.TierBadgeScale;
    snap.icons.tierImageCount = BadgeTextures::TierImageCount();
    // Warn once per settings generation about a Badge key the manifest cannot satisfy.
    if (snap.icons.tierImageCount > 0)
    {
        for (std::size_t i = 0; i < snap.tiers.size(); ++i)
        {
            if (snap.tiers[i].badgeIndex > snap.icons.tierImageCount)
            {
                SKSE::log::warn(
                    "Tier{}: Badge = {} exceeds the {} loaded emblems; falls back to the "
                    "TierBadgeGamma curve",
                    i,
                    snap.tiers[i].badgeIndex,
                    snap.icons.tierImageCount);
            }
        }
    }
    snap.icons.colNeutral = ic.NeutralColor;
    snap.icons.colHumanoid = ic.HumanoidColor;
    snap.icons.colCommoner = ic.CommonerColor;
    snap.icons.colMortal = ic.MortalColor;
    snap.icons.colEven = ic.EvenColor;
    snap.icons.colIdle = ic.IdleColor;
    snap.icons.colSneakOff = ic.SneakOffColor;
    snap.icons.colNormalWeight = ic.NormalWeightColor;
    snap.icons.colBountyClear = ic.BountyClearColor;
    snap.icons.colMuted = ic.MutedColor;
    snap.icons.relationshipEnabled = ic.RelationshipEnabled;
    snap.icons.creatureEnabled = ic.CreatureEnabled;
    snap.icons.threatEnabled = ic.ThreatEnabled;
    snap.icons.roleEnabled = ic.RoleEnabled;
    snap.icons.protectionEnabled = ic.ProtectionEnabled;
    snap.icons.engagementEnabled = ic.EngagementEnabled;
    snap.icons.combatStateEnabled = ic.CombatStateEnabled;
    snap.icons.alertStateEnabled = ic.AlertStateEnabled;
    snap.icons.sneakEnabled = ic.SneakEnabled;
    snap.icons.playerCombatEnabled = ic.PlayerCombatEnabled;
    snap.icons.encumberedEnabled = ic.EncumberedEnabled;
    snap.icons.bountyEnabled = ic.BountyEnabled;
    snap.icons.tierEnabled = ic.TierEnabled;
    snap.icons.mutedAlpha = ic.MutedAlpha;
    snap.icons.mutedDesat = ic.MutedDesat;
    snap.icons.playerStripBedEnabled = ic.PlayerStripBedEnabled;
    snap.icons.playerStripBedAlpha = ic.PlayerStripBedAlpha;
    snap.icons.playerStripBedSize = ic.PlayerStripBedSize;
    snap.icons.playerStripBedBreatheHz = ic.PlayerStripBedBreatheHz;
    snap.icons.playerStripBedColor = ic.PlayerStripBedColor;
    snap.icons.emblemBacklightEnabled = ic.EmblemBacklightEnabled;
    snap.icons.emblemBacklightSize = ic.EmblemBacklightSize;
    snap.icons.emblemBacklightAlpha = ic.EmblemBacklightAlpha;
    snap.icons.emblemBacklightBreatheHz = ic.EmblemBacklightBreatheHz;
    snap.icons.emblemCrispAlpha = ic.EmblemCrispAlpha;
    snap.icons.emblemBacklightColor = ic.EmblemBacklightColor;
    snap.icons.playerRimLightEnabled = ic.PlayerRimLightEnabled;
    snap.icons.playerRimAlpha = ic.PlayerRimAlpha;
    snap.icons.playerCarveAlpha = ic.PlayerCarveAlpha;
    snap.icons.playerRimOffset = ic.PlayerRimOffset;
    snap.icons.playerRimColor = ic.PlayerRimColor;
    snap.icons.emblemKeyFillEnabled = ic.EmblemKeyFillEnabled;
    snap.icons.emblemKeyAlpha = ic.EmblemKeyAlpha;
    snap.icons.emblemFillAlpha = ic.EmblemFillAlpha;
    snap.icons.emblemKeyRise = ic.EmblemKeyRise;
    snap.icons.emblemFillDrop = ic.EmblemFillDrop;
    snap.icons.emblemKeyColor = ic.EmblemKeyColor;
    snap.icons.emblemFillColor = ic.EmblemFillColor;
    snap.focus = Settings::Focus();
    snap.graffito = Settings::Graffito();
    snap.graffitoWrapRadians =
        snap.graffito.WrapDegrees * static_cast<float>(Graffito::Math::PI / 180.0);
    snap.quiet = Settings::Quiet();

    const auto& dr = Settings::DeathRite();
    snap.deathRiteEnabled = dr.Enabled;
    snap.deathRiteDuration = dr.Duration;

    const auto& rc = Settings::RegisterConfig();
    snap.registersEnabled = rc.Enabled;
    snap.registerTransitionTime = rc.TransitionTime;
    snap.registers = Settings::Registers();

    const auto& compat = Settings::Compat();
    snap.compatTrueHUDYieldAlpha = compat.TrueHUDYieldAlpha;
    snap.compatYieldSettleTime = compat.YieldSettleTime;

    const auto& candle = Settings::Candlelight();
    snap.candleEnabled = candle.Enabled;
    snap.candleStrength = candle.Strength;
    snap.candleWarmth = candle.Warmth;
    snap.candleSettleTime = candle.SettleTime;

    const auto& dc = Settings::DepthClipConfig();
    snap.depthClipEnabled = dc.Enabled;
    snap.depthClipFeather = dc.Feather;

    return snap;
}

void RenderSettingsSnapshot::PopulateSortedSpecialTitles()
{
    sortedSpecialTitles.clear();
    sortedSpecialTitles.reserve(specialTitles.size());
    for (const auto& st : specialTitles)
    {
        if (!st.keywordLower.empty())
        {
            sortedSpecialTitles.push_back(&st);
        }
    }
    std::sort(sortedSpecialTitles.begin(),
              sortedSpecialTitles.end(),
              [](const auto* a, const auto* b) { return a->priority > b->priority; });
}

/**
 * @fn static void RefreshCachedSettingsSnapshot()
 * @brief Recapture settings when their published generation or the emblem count changes.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The generation covers an INI reload. The emblem count covers a texture rebuild that keeps the
 * generation: the badge textures are refreshed before the frame draws, so a reload that publishes
 * between that refresh and this call captures the old count, and the rebuilt count then never
 * reaches the cache. A stale `icons.tierImageCount` selects emblems against the wrong slot count,
 * on plates and on Deck cards, for the rest of the generation. The count query is lock-free.
 *
 * Check both triggers again under the lock; unchanged frames need no lock or copy.
 */
static void RefreshCachedSettingsSnapshot()
{
    uint32_t currentGen = Settings::Generation().load(std::memory_order_acquire);
    int tierImageCount = BadgeTextures::TierImageCount();
    if (currentGen == GetState().lastSnapGeneration &&
        tierImageCount == GetState().cachedSnap.icons.tierImageCount)
    {
        return;
    }
    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
    currentGen = Settings::Generation().load(std::memory_order_acquire);
    tierImageCount = BadgeTextures::TierImageCount();
    if (currentGen == GetState().lastSnapGeneration &&
        tierImageCount == GetState().cachedSnap.icons.tierImageCount)
    {
        return;
    }
    GetState().cachedSnap = RenderSettingsSnapshot::CaptureFromSettings();
    GetState().cachedSnap.PopulateSortedSpecialTitles();
    GetState().lastSnapGeneration = currentGen;
}

/**
 * @fn static float DistanceToSegmentSquared(const ImVec2& point, const ImVec2& a, const ImVec2& b)
 * @brief Measure squared pixel distance to the nearest point on a segment.
 * @author Alex (<https://github.com/lextpf>)
 */
static float DistanceToSegmentSquared(const ImVec2& point, const ImVec2& a, const ImVec2& b)
{
    const ImVec2 ab = b - a;
    const float lengthSq = ab.x * ab.x + ab.y * ab.y;
    if (lengthSq <= 1e-4f)
    {
        const ImVec2 delta = point - a;
        return delta.x * delta.x + delta.y * delta.y;
    }
    const ImVec2 ap = point - a;
    const float t = std::clamp((ap.x * ab.x + ap.y * ab.y) / lengthSq, .0f, 1.0f);
    const ImVec2 closest = a + ab * t;
    const ImVec2 delta = point - closest;
    return delta.x * delta.x + delta.y * delta.y;
}

/**
 * @fn static Deck::RarityArchetype ToDeckArchetype(CreatureKind kind)
 * @brief Map the snapshot creature category to a card rarity archetype.
 * @author Alex (<https://github.com/lextpf>)
 */
static Deck::RarityArchetype ToDeckArchetype(CreatureKind kind)
{
    switch (kind)
    {
        case CreatureKind::Beast:
            return Deck::RarityArchetype::Beast;
        case CreatureKind::Undead:
            return Deck::RarityArchetype::Undead;
        case CreatureKind::Daedra:
            return Deck::RarityArchetype::Daedra;
        case CreatureKind::Dragon:
            return Deck::RarityArchetype::Dragon;
        case CreatureKind::NPC:
        default:
            return Deck::RarityArchetype::Humanoid;
    }
}

/**
 * @fn static float NextDeckRarityRoll(std::uint32_t formID)
 * @brief Generate a new rarity sample for a render-thread card capture.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Clock and serial randomize repeat captures of one FormID. Render-thread only
 * because the serial is mutable; 0x9E3779B9 disperses bits.
 */
static float NextDeckRarityRoll(std::uint32_t formID)
{
    static std::uint32_t serial = 0;
    const auto ticks =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint32_t seed = formID ^ static_cast<std::uint32_t>(ticks) ^
                               static_cast<std::uint32_t>(ticks >> 32) ^ (++serial * 0x9E3779B9u);
    return Deck::DeterministicPhase(seed);
}

void PrepareDeckCaptureRT()
{
    if (!Deck::ConsumeCaptureRequest())
    {
        return;
    }

    Settings::DeckSettings deckSettings;
    {
        const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
        deckSettings = Settings::Deck();
    }
    if (!deckSettings.Enabled)
    {
        return;
    }

    RefreshCachedSettingsSnapshot();
    const auto& snapSettings = GetState().cachedSnap;

    std::vector<ActorDrawData> actors;
    std::uint32_t crosshairID = 0;
    {
        const std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        actors = GetState().snapshot;
        crosshairID = GetState().crosshairTarget;
    }
    if (actors.empty())
    {
        Deck::NotifyError("No actor data is available for a card yet");
        return;
    }

    const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
    const float screenWidth = static_cast<float>(screen.width);
    const float screenHeight = static_cast<float>(screen.height);
    if (screenWidth <= .0f || screenHeight <= .0f)
    {
        Deck::NotifyError("Deck could not resolve the current screen size");
        return;
    }

    const ActorDrawData* target = nullptr;
    const ActorDrawData* player = nullptr;
    for (const auto& actor : actors)
    {
        if (actor.isPlayer)
        {
            player = &actor;
        }
        if (!actor.isDead && crosshairID != 0 && actor.formID == crosshairID)
        {
            target = &actor;
            break;
        }
    }

    // Distant and third-person targets can lack CrosshairPickData; use projected bounds.
    if (!target)
    {
        const ImVec2 center(screenWidth * .5f, screenHeight * .5f);
        const float maxDistSq = static_cast<float>(deckSettings.TargetRadius) *
                                static_cast<float>(deckSettings.TargetRadius);
        float bestDistSq = maxDistSq;
        for (const auto& actor : actors)
        {
            if (actor.isPlayer || actor.isDead || actor.isOccluded)
            {
                continue;
            }
            RE::NiPoint3 head{};
            RE::NiPoint3 feet{};
            if (!WorldToScreen(actor.worldPos, head) || !WorldToScreen(actor.feetPos, feet) ||
                head.z < .0f || feet.z < .0f)
            {
                continue;
            }
            const float distanceSq =
                DistanceToSegmentSquared(center, ImVec2(head.x, head.y), ImVec2(feet.x, feet.y));
            if (distanceSq <= bestDistSq)
            {
                bestDistSq = distanceSq;
                target = &actor;
            }
        }
    }
    if (!target && deckSettings.PlayerFallback)
    {
        target = player;
    }
    if (!target)
    {
        Deck::NotifyError("No actor is under the crosshair");
        return;
    }

    float headX = std::numeric_limits<float>::quiet_NaN();
    float headY = std::numeric_limits<float>::quiet_NaN();
    float feetX = std::numeric_limits<float>::quiet_NaN();
    float feetY = std::numeric_limits<float>::quiet_NaN();
    RE::NiPoint3 headScreen{};
    RE::NiPoint3 feetScreen{};
    if (WorldToScreen(target->worldPos, headScreen) && WorldToScreen(target->feetPos, feetScreen) &&
        headScreen.z >= .0f && feetScreen.z >= .0f)
    {
        headX = headScreen.x;
        headY = headScreen.y;
        feetX = feetScreen.x;
        feetY = feetScreen.y;
    }

    const float cardScale = Deck::CardLayoutScale(static_cast<float>(deckSettings.CardWidth),
                                                  static_cast<float>(deckSettings.CardHeight));
    const float portraitWidth = deckSettings.CardWidth - 76.0f * cardScale;
    const float portraitHeight = deckSettings.CardHeight * .685f - 42.0f * cardScale;
    const float portraitAspect = portraitWidth / std::max(1.0f, portraitHeight);
    const Deck::PortraitCrop crop = Deck::ComputePortraitCrop(
        screenWidth, screenHeight, headX, headY, feetX, feetY, portraitAspect);

    std::string nameLower = target->name;
    std::transform(nameLower.begin(),
                   nameLower.end(),
                   nameLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const LabelStyle style = ComputeLabelStyle(*target, nameLower, 1.0f, .0f, snapSettings);
    const int tierCount = std::max(1, static_cast<int>(snapSettings.tiers.size()));
    const Deck::ActorRarityProfile rarityProfile{
        .level = target->level,
        .archetype = ToDeckArchetype(target->creatureKind),
        .essential = target->protection == ProtectionKind::Essential,
    };
    const Deck::Rarity intrinsicRarity = Deck::RarityFromActor(rarityProfile);
    const Deck::Rarity tierRarity = Deck::RarityFromTier(style.tierIdx, tierCount);
    Deck::Rarity rarity = static_cast<Deck::Rarity>(
        std::max(static_cast<int>(intrinsicRarity), static_cast<int>(tierRarity)));
    if (deckSettings.RarityRolls && target->isUnique)
    {
        rarity = Deck::ApplyRarityRoll(rarity, NextDeckRarityRoll(target->formID));
    }
    const int treatmentTierIdx = Deck::TreatmentTierForRarity(rarity, style.tierIdx, tierCount);
    const auto& tier =
        snapSettings.tiers.empty() ? *style.tier : snapSettings.tiers[treatmentTierIdx];

    const auto lerpWhite = [](const Settings::Color3& c, float t)
    {
        return Settings::Color3(
            c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t, c.b + (1.0f - c.b) * t);
    };
    const auto fromVec = [](const ImVec4& c) { return Settings::Color3(c.x, c.y, c.z); };

    Deck::CardRequest request;
    request.formID = target->formID;
    request.name = target->name.empty() ? "Unknown Actor" : target->name;
    request.level = target->level;
    // Resolve from the card's treatment tier so a rarity bump moves the emblem with the frame.
    request.tierImageIndex = snapSettings.icons.tierBadgeImages
                                 ? TierEmblem::Select(tier.badgeIndex,
                                                      treatmentTierIdx,
                                                      tierCount,
                                                      snapSettings.icons.tierImageCount,
                                                      snapSettings.icons.tierBadgeGamma)
                                 : -1;
    request.tierName = tier.title;
    request.outputFolder = deckSettings.OutputFolder;
    request.width = deckSettings.CardWidth;
    request.height = deckSettings.CardHeight;
    request.rarity = rarity;
    request.portrait = {crop.x / screenWidth,
                        crop.y / screenHeight,
                        (crop.x + crop.width) / screenWidth,
                        (crop.y + crop.height) / screenHeight};

    request.frameLeft = tier.leftColor;
    request.frameRight = tier.rightColor;
    request.highlight = tier.highlightColor;
    request.titleLeft = tier.titleLeftColor.value_or(lerpWhite(tier.leftColor, .25f));
    request.titleRight = tier.titleRightColor.value_or(lerpWhite(tier.rightColor, .25f));
    request.particleColor = tier.particleColor.value_or(tier.highlightColor);
    request.ornamentLeft = tier.ornamentLeftColor.value_or(request.titleLeft);
    request.ornamentRight = tier.ornamentRightColor.value_or(request.titleRight);
    request.particleTypes = tier.particleTypes;
    request.particleCount =
        tier.particleCount > 0 ? tier.particleCount : snapSettings.particleCount;

    // Same precedence as the plate: console override, special title, honorific, tier.
    request.title = (target->overrides && target->overrides->title) ? *target->overrides->title
                    : style.specialTitle
                        ? style.specialTitle->displayTitle
                        : (!target->honorific.empty() ? target->honorific : tier.title);
    request.leftOrnaments = style.specialTitle && !style.specialTitle->leftOrnaments.empty()
                                ? style.specialTitle->leftOrnaments
                                : tier.leftOrnaments;
    request.rightOrnaments = style.specialTitle && !style.specialTitle->rightOrnaments.empty()
                                 ? style.specialTitle->rightOrnaments
                                 : tier.rightOrnaments;

    if (rarity == Deck::Rarity::Common)
    {
        // Common stays plain even when tier0 defines an animated effect.
        request.nameLeft = fromVec(style.LcName);
        request.nameRight = request.nameLeft;
        request.titleLeft = fromVec(style.LcTitle);
        request.titleRight = request.titleLeft;
        request.nameEffect.type = Settings::EffectType::None;
        request.titleEffect.type = Settings::EffectType::None;
    }
    else
    {
        request.nameLeft = style.specialTitle ? style.specialTitle->color : tier.leftColor;
        request.nameRight = style.specialTitle ? style.specialTitle->color : tier.rightColor;
        if (style.specialTitle)
        {
            request.titleLeft = style.specialTitle->color;
            request.titleRight = style.specialTitle->color;
        }
        if (rarity == Deck::Rarity::Uncommon)
        {
            request.nameEffect.type = Settings::EffectType::Gradient;
            request.titleEffect.type = Settings::EffectType::Gradient;
        }
        else
        {
            request.nameEffect = tier.nameEffect;
            request.titleEffect = tier.titleEffect;
        }
    }

    const BadgeComposition badgeComposition =
        ComposeBadges(*target, snapSettings.icons, treatmentTierIdx, tierCount, false);
    request.badges.reserve(badgeComposition.count);
    for (int i = 0; i < badgeComposition.count; ++i)
    {
        const auto& badge = badgeComposition.slots[i];
        request.badges.push_back(
            {std::string(badge.icon), badge.color, badge.muted, badge.tierImage});
    }

    Deck::Queue(std::move(request));
}

void Draw()
{
    HandleHotReload();

    // Skip while Settings::Load mutates non-POD values.
    if (GetState().reloadRequested.load(std::memory_order_acquire))
    {
        return;
    }

    RefreshCachedSettingsSnapshot();

    // Consume rename requests on the cache-owning thread and allow a fresh snapshot.
    if (GetState().pendingIdentityRefresh.exchange(false, std::memory_order_acq_rel))
    {
        GetState().cache.erase(0x14);  // Player FormID
        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
    }
    const RenderSettingsSnapshot& snap = GetState().cachedSnap;

    // Console edits restart only reveal state, preserving pose smoothing.
    // The override store mutex is a leaf lock.
    {
        std::vector<uint32_t> dirty;
        ActorOverrides::DrainDirty(dirty);
        for (const uint32_t formID : dirty)
        {
            auto it = GetState().cache.find(formID);
            if (it != GetState().cache.end())
            {
                it->second.typewriterTime = .0f;
                it->second.typewriterComplete = false;
                it->second.revealArmed = snap.enableTypewriter;
            }
        }
    }

    // Use the published gate; live cell reads can race with streaming teardown.
    if (!GetState().allowOverlay.load(std::memory_order_acquire))
    {
        GetState().wasInInvalidState = true;
        return;
    }

    if (GetState().wasInInvalidState)
    {
        GetState().wasInInvalidState = false;
        // Suppress 300 render frames after wake; TickRT keeps snapshots current.
        GetState().postLoadCooldown = 300;
        // Replay entrances as a staggered cascade after wake.
        GetState().wakeReplayPending = true;
    }

    if (GetState().postLoadCooldown > 0)
    {
        --GetState().postLoadCooldown;
        return;
    }

    QueueSnapshotUpdate_RenderThread();

    auto* bsRenderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!bsRenderer)
    {
        return;
    }

    const auto viewSize = bsRenderer->GetScreenSize();
    ++GetState().frame;

    std::vector<ActorDrawData> localSnap;
    uint32_t crosshairFormID = 0;
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        localSnap = GetState().snapshot;
        crosshairFormID = GetState().crosshairTarget;
    }

    if (localSnap.empty())
    {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)viewSize.width, (float)viewSize.height));
    ImGui::Begin("glyphOverlay",
                 nullptr,
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Capture before overlay draws so the meter cannot sample its own text.
    // Copy-only work needs no render-state reset.
    if (snap.candleEnabled && SceneMeter::IsInitialized())
    {
        drawList->AddCallback(SceneMeter::CaptureCallback, nullptr);
    }

    if (snap.enableDebugOverlay)
    {
        UpdateDebugStats(localSnap, snap.hidePlayer && !snap.graffito.Enabled);
    }

    UpdateQuietFrame(snap, ImGui::GetIO().DeltaTime);

    UpdateRegisters(snap, ImGui::GetIO().DeltaTime);

    if (snap.candleEnabled && SceneMeter::IsInitialized())
    {
        SceneMeter::CollectResults();
    }

    // Unavailable depth or indeterminate polarity leaves this frame unclipped.
    GetState().depthClipFrame = false;
    if (snap.depthClipEnabled && DepthClip::IsInitialized())
    {
        const float polarity = ComputeDepthPolarity();
        GetState().depthClipFrame =
            polarity != .0f && DepthClip::BeginFrame(snap.depthClipFeather, polarity);
    }

    // Discard per-frame projections and lazily recapture ImGui VS/CB state.
    Graffito::BeginFrame();

    // Rearm entrances in snapshot order after suppression.
    GetState().entrancesStartedThisFrame = 0;
    if (GetState().wakeReplayPending)
    {
        GetState().wakeReplayPending = false;
        for (const auto& d : localSnap)
        {
            auto cIt = GetState().cache.find(d.formID);
            if (cIt == GetState().cache.end())
            {
                continue;
            }
            auto& entry = cIt->second;
            // Do not replay deaths that occurred while the overlay was suppressed.
            if (d.isDead)
            {
                entry.deathPhase = 1.0f;
                entry.deathDone = true;
                entry.exitPhase = 1.0f;
                continue;
            }
            if (snap.enableEntrance && entry.entranceDone)
            {
                entry.entranceDone = false;
                entry.entrancePhase = .0f;
                entry.entranceDelay = -1.0f;
            }
            if (entry.typewriterComplete)
            {
                entry.typewriterTime = .0f;
                entry.typewriterComplete = false;
            }
        }
    }

    OverlapOffsets().clear();
    if (snap.visual.EnableOverlapPrevention &&
        !(snap.graffito.Enabled && Graffito::IsInitialized()))
    {
        ResolveOverlaps(localSnap, snap);
    }

    const uint32_t focusedFormID = SelectFocusedActor(localSnap, snap);
    const uint32_t aimedFormID =
        snap.graffito.Enabled ? RaycastCameraActor(localSnap, snap) : crosshairFormID;

    // Channels with GPU glow: 0 capture/backplate, 1 particles, 2 text.
    // Without glow: 0 particles, 1 text. GraffitoInkChannel follows this shift.
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const bool gpuDivide = snap.glowDivideStrength > .0f && TextPostProcess::IsInitialized();
    const int channelCount = gpuGlow ? 3 : 2;
    ImDrawListSplitter splitter;
    splitter.Split(drawList, channelCount);

    // Each merged channel stream needs a balanced sampler scope.
    for (int channel = 0; channel < channelCount; ++channel)
    {
        splitter.SetCurrentChannel(drawList, channel);
        RenderSampling::PushFontSampler(drawList);
    }
    splitter.SetCurrentChannel(drawList, 0);

    if (gpuGlow)
    {
        TextPostProcess::SetGlowParams(snap.glowRadius, snap.glowIntensity);
        splitter.SetCurrentChannel(drawList, 0);
        drawList->AddCallback(TextPostProcess::BeginGlowCapture, nullptr);
    }

    // Divide needs the scene captured before nametag text.
    if (gpuDivide)
    {
        TextPostProcess::SetDivideParams(snap.glowDivideStrength);
        drawList->AddCallback(TextPostProcess::BeginDivideCapture, nullptr);
    }

    for (auto& d : localSnap)
    {
        // Deck-only entries never draw plates. HidePlayer hides billboards only;
        // Graffito keeps the self-plate, matching playerPlateVisible in collection.
        if (d.deckOnly || (snap.hidePlayer && !snap.graffito.Enabled && d.isPlayer))
        {
            continue;
        }
        if (d.isDead)
        {
            // Rites require a cached live frame and cannot replay after completion.
            auto cIt = GetState().cache.find(d.formID);
            if (!snap.deathRiteEnabled || cIt == GetState().cache.end() || !cIt->second.sawAlive ||
                cIt->second.deathDone)
            {
                continue;
            }
            auto ld = GetState().lastDrawData.find(d.formID);
            if (ld == GetState().lastDrawData.end())
            {
                cIt->second.deathDone = true;
                continue;
            }
            DrawDyingLabel(cIt->second, ld->second, drawList, &splitter, snap);
            continue;
        }
        DrawLabel(d, drawList, &splitter, snap, focusedFormID, aimedFormID);
    }

    // Exit ghosts replay live facts and do not participate in focus or overlap.
    if (snap.enableExit)
    {
        std::unordered_set<uint32_t> visible;
        visible.reserve(localSnap.size());
        for (const auto& d : localSnap)
        {
            visible.insert(d.formID);
        }
        for (auto& [formID, entry] : GetState().cache)
        {
            if (visible.count(formID) != 0 || !entry.initialized || !entry.entranceDone ||
                entry.exitPhase >= 1.0f || entry.deathPhase > .0f)
            {
                continue;
            }
            auto ld = GetState().lastDrawData.find(formID);
            if (ld == GetState().lastDrawData.end())
            {
                continue;
            }
            DrawExitingLabel(entry, ld->second, drawList, &splitter, snap);
        }
    }

    // Restore sampling before trailing post-process/render-state callbacks.
    for (int channel = 0; channel < channelCount; ++channel)
    {
        splitter.SetCurrentChannel(drawList, channel);
        RenderSampling::PopSampler(drawList);
    }

    // The final channel reset restores ImGui shaders for subsequent windows.
    if (GetState().depthClipFrame || (snap.graffito.Enabled && Graffito::IsInitialized()))
    {
        splitter.SetCurrentChannel(drawList, gpuGlow ? 2 : 1);
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    if (gpuGlow)
    {
        splitter.SetCurrentChannel(drawList, 0);
        drawList->AddCallback(TextPostProcess::EndGlowAndComposite, nullptr);
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    splitter.Merge(drawList);

    if (gpuDivide)
    {
        drawList->AddCallback(TextPostProcess::EndDivideAndComposite, nullptr);
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    ImGui::End();

    DrawDebugOverlay(snap);
    PruneCacheToSnapshot(localSnap);
}

void TickRT()
{
    Settings::DeckSettings deckSettings;
    {
        const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
        deckSettings = Settings::Deck();
    }
    Deck::PollInput(deckSettings.Enabled,
                    deckSettings.Key,
                    GetState().allowDeck.load(std::memory_order_acquire));

    // TickRT must queue updates on hidden frames; otherwise allowOverlay
    // never bootstraps and drawing cannot resume.
    QueueSnapshotUpdate_RenderThread();
}
}  // namespace Renderer
