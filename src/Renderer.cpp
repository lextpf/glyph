#include "Renderer.hpp"

#include "RendererInternal.hpp"

#include "AppearanceTemplate.hpp"
#include "BadgeTextures.hpp"
#include "DepthClip.hpp"
#include "GameState.hpp"
#include "Occlusion.hpp"
#include "ParticleTextures.hpp"
#include "SceneMeter.hpp"
#include "TextPostProcess.hpp"

#include <SKSE/SKSE.h>

#include <cmath>

namespace Renderer
{
// ============================================================================
// Singleton accessors (defined here, declared in RendererInternal.h)
// ============================================================================

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

// Per-frame overlap Y offsets, keyed by form ID
std::unordered_map<uint32_t, float>& OverlapOffsets()
{
    static std::unordered_map<uint32_t, float> offsets;
    return offsets;
}

// ============================================================================
// Public API
// ============================================================================

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

// ============================================================================
// Font helper
// ============================================================================

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

// ============================================================================
// Cache management
// ============================================================================

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
            it->second.lastSeenFrame = GetState().frame;  // Update last seen
        }

        if (!inSnapshot)
        {
            // Check if grace period has expired. Keep an entry alive while it is
            // actively exiting (0 < exitPhase < 1), up to a hard ceiling so a
            // mid-exit entry can't leak if the exit animation is toggled off.
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

// Compute blend factor for frame-rate independent exponential smoothing.
// Returns alpha in [0,1] for use with: current = lerp(current, target, alpha)
float ExpApproachAlpha(float dt, float settleTime, float epsilon)
{
    dt = std::max(.0f, dt);
    settleTime = std::max(1e-5f, settleTime);
    return std::clamp(1.0f - std::pow(epsilon, dt / settleTime), .0f, 1.0f);
}

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

// ============================================================================
// Distance and smoothing helpers (file-local)
// ============================================================================

// Compute distance-based visual factors (fade, LOD, scale).
static DistanceFactors ComputeDistanceFactors(const ActorDrawData& d,
                                              const RenderSettingsSnapshot& snap)
{
    DistanceFactors df{};
    const float dist = d.distToPlayer;

    // Registers can pull the fade envelope in (or push it out) per scene.
    const float regFade = GetState().regFadeMul;
    const float fadeStart = snap.fadeStartDistance * regFade;
    const float fadeEnd = snap.fadeEndDistance * regFade;

    // Alpha fade using squared smoothstep
    const float fadeRange = std::max(1.0f, fadeEnd - fadeStart);
    float fadeT = TextEffects::SmoothStep((dist - fadeStart) / fadeRange);
    df.alphaTarget = 1.0f - fadeT;
    df.alphaTarget = df.alphaTarget * df.alphaTarget;

    // LOD factors
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

    // Font size scale with sqrt falloff
    const float scaleRange = std::max(1.0f, snap.scaleEndDistance - snap.scaleStartDistance);
    float scaleT = TextEffects::Saturate((dist - snap.scaleStartDistance) / scaleRange);
    constexpr float SCALE_GAMMA = .5f;
    scaleT = std::pow(scaleT, SCALE_GAMMA);
    df.textScaleTarget = 1.0f + (snap.minimumScale - 1.0f) * scaleT;

    // Also factor in camera distance for more accurate near-camera scaling
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

    // Enforce minimum readable size
    if (snap.visual.MinimumPixelHeight > .0f)
    {
        float minScale = snap.visual.MinimumPixelHeight / snap.nameFontSize;
        df.textScaleTarget = std::max(df.textScaleTarget, minScale);
    }

    return df;
}

// Project world position to screen coordinates.
// Single shared definition; declared in RendererInternal.h.
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

// ============================================================================
// The Quiet Frame (camera-motion quieting)
// ============================================================================

// Map camera angular speed (deg/s) onto a [0,1] quiet target: untouched below
// `lo`, fully quiet above `hi`, smoothstepped between.  Pure function --
// mirrored in tests/test_utils.cpp; keep the logic in sync.
static float QuietTarget(float degPerSec, float lo, float hi)
{
    if (hi <= lo)
    {
        return degPerSec >= hi ? 1.0f : .0f;
    }
    return TextEffects::SmoothStep(TextEffects::Saturate((degPerSec - lo) / (hi - lo)));
}

// During fast camera pans the overlay exhales: the title and the status-badge
// strip above the name fold away, while the name, level, and particles stay
// full so the core readout never blinks (applied in DrawLabel via quietSub).
// The envelope is asymmetric -- fast attack toward quiet, slow weighty release.
//
// quietName / QuietNameFloor / QuietNameReleaseTime are retained (so the name
// could opt back into thinning) but are currently unused -- the name no longer
// responds to the Quiet Frame.
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

// ============================================================================
// Cut by the World (per-pixel depth occlusion)
// ============================================================================

// Determine the depth-buffer convention from the game's own projection:
// project two probe points at different view depths and compare their
// viewport z.  +1 = standard (farther is larger), -1 = reversed, 0 =
// indeterminate (skip clipping this frame).  WorldToScreen uses the same
// matrices the rasterizer wrote depth with, so this is exact by
// construction -- no readback or calibration pass needed.
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

// Bracket the current plate's draws (all splitter channels) with the
// depth-clip shader.  Channel streams are contiguous per label, so one
// Apply per channel re-establishes the shader + this plate's constants for
// exactly this plate's content in that channel.
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

// ============================================================================
// Registers (context-conditional profiles)
// ============================================================================

// Ease the effective register knobs toward the active register's values (or
// the 1/1/1/0 base state when none matches).  The game thread publishes the
// active index each snapshot; easing here makes scene transitions swell and
// recede like a score instead of snapping between modes.
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

// Update cache entry smoothing and return whether the label is visible.
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
            // Snap the head so it never lags a hard screen jump.  The trail is
            // NOT wiped here: it is world-space now, so a camera-induced screen
            // jump leaves the stored world points untouched (they reproject onto
            // the head).  Genuine world teleports are handled in the trail block.
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

// ============================================================================
// Per-actor label orchestrator
// ============================================================================

static void DrawLabel(const ActorDrawData& d,
                      ImDrawList* drawList,
                      ImDrawListSplitter* splitter,
                      const RenderSettingsSnapshot& snap,
                      uint32_t focusedFormID)
{
    auto it = GetState().cache.find(d.formID);
    if (it == GetState().cache.end())
    {
        ActorCache newEntry{};
        newEntry.lastSeenFrame = GetState().frame;
        it = GetState().cache.emplace(d.formID, newEntry).first;
    }
    auto& entry = it->second;
    const uint32_t prevLastSeenFrame = entry.lastSeenFrame;

    // Detect name changes and reset typewriter
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

    // Reset typewriter and entrance on re-entry (actor reappearing or becoming unoccluded)
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
    entry.sawAlive = true;  // Last Rites plays only for actors seen alive

    // Capture live draw data and cancel any pending exit: a present actor is
    // never mid-exit. The exit pass replays this snapshot after the actor leaves.
    GetState().lastDrawData[d.formID] = d;
    entry.exitPhase = .0f;

    // Compute distance-based visual factors
    DistanceFactors df = ComputeDistanceFactors(d, snap);

    RE::NiPoint3 screenPos;
    if (!WorldToScreen(d.worldPos, screenPos))
    {
        return;
    }

    const float dt = ImGui::GetIO().DeltaTime;
    if (!UpdateCacheSmoothing(entry, d, df, screenPos, dt, snap))
    {
        return;
    }

    // Advance entrance/exit animation
    float entranceAlphaMul = 1.0f;
    float entranceScaleMul = 1.0f;
    float entranceYOffset = .0f;
    if (snap.enableEntrance && !entry.entranceDone)
    {
        // Roll Call: each entrance starting this frame claims the next
        // stagger slot.  The snapshot is distance-sorted, so slots fall
        // near-to-far and a waking scene rolls in as a quiet cascade; a
        // lone actor entering range claims slot 0 and starts immediately.
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
            entry.typewriterTime = .0f;  // reveal starts with the entrance, not the wait
            return;
        }
        entry.entrancePhase += dt / std::max(snap.entranceDuration, .05f);
        if (entry.entrancePhase >= 1.0f)
        {
            entry.entrancePhase = 1.0f;
            entry.entranceDone = true;
        }
        const float t = entry.entrancePhase;
        // Cinematic ease-out: a front-loaded reveal that settles with weight and
        // never overshoots.  Every style fades in and rises gently into place;
        // only the scale treatment differs between styles.
        const float ease = TextEffects::EaseOutCubic(t);

        entranceAlphaMul = ease;

        // Gentle upward settle, applied to all styles: the label covers most of
        // the travel immediately, then eases the final pixels for a deliberate
        // "locking in" feel (applied via the Y-offset block below).
        entranceYOffset = RenderConstants::ENTRANCE_RISE_PX * (1.0f - TextEffects::EaseOutExpo(t));

        if (snap.entranceStyle == 1)  // SlideDown: rise + fade only, no scale
        {
            entranceScaleMul = 1.0f;
        }
        else  // PopIn (0) / Expand (2): gentle scale settle, no overshoot
        {
            const float scaleStart = snap.entranceStyle == 2 ? .88f : .90f;
            entranceScaleMul = scaleStart + ease * (1.0f - scaleStart);
        }
    }

    // Focus-target state: drive a per-actor focusSmooth in [0,1] and use it
    // to dim ambient (non-focused) actors and fade in the title/info rows
    // for the focused actor.  Player is exempt -- always rendered as if
    // "focused" so the player nameplate retains full alpha and full content.
    const bool isFocused = snap.focus.Enabled && d.formID == focusedFormID && !d.isPlayer;
    const bool focusAppliesToActor = snap.focus.Enabled && !d.isPlayer;
    const float focusTarget = focusAppliesToActor ? (isFocused ? 1.0f : .0f) : 1.0f;
    if (snap.focus.SettleTime <= .0f)
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

    // Registers: overlay-wide alpha, plus the option to retire neutral and
    // ally plates entirely in matching scenes (crowded cities).  Followers,
    // hostiles, and the player always keep their plates.
    float registerMul = GetState().regAlphaMul;
    if (!d.isPlayer &&
        (d.relationship == RelationshipKind::Neutral || d.relationship == RelationshipKind::Ally))
    {
        registerMul *= 1.0f - GetState().regHideNeutral;
    }

    // One Voice Per Actor: while another HUD mod floats a widget over this
    // actor, the plate bows out to the configured yield alpha and returns
    // with the same synchronized fade when the widget goes away.
    entry.yieldSmooth += ((d.yieldPlate ? 1.0f : .0f) - entry.yieldSmooth) *
                         ExpApproachAlpha(dt, snap.compatYieldSettleTime);
    const float yieldMul = 1.0f - (1.0f - snap.compatTrueHUDYieldAlpha) * entry.yieldSmooth;

    // Cull off-screen labels.  Quiet Frame is intentionally absent here: the
    // name, level, and particles all derive from this alpha and must stay full
    // through a camera pan.  Only the title and badge strip fold (below).
    const float alpha = entry.alphaSmooth * entry.occlusionSmooth * entranceAlphaMul *
                        mainAlphaMul * registerMul * yieldMul;
    const float textSizeScale = entry.textSizeScale * entranceScaleMul;

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return;
    }
    const auto viewSize = renderer->GetScreenSize();
    if (screenPos.z < 0 || screenPos.z > 1.0f || screenPos.x < -100.0f ||
        screenPos.x > viewSize.width + 100.0f || screenPos.y < -100.0f ||
        screenPos.y > viewSize.height + 100.0f)
    {
        return;
    }

    // Cut by the World: everything this plate draws is depth-tested per
    // pixel at the plate's own viewport depth, so world geometry slices the
    // type instead of the whole plate popping on LOS changes.
    if (GetState().depthClipFrame)
    {
        BracketPlateDepthClip(drawList, splitter, DepthClip::MakePlateParams(screenPos.z), snap);
    }

    // Compute style, layout, and dispatch to sub-renderers
    const float time = (float)ImGui::GetTime();
    LabelStyle style = ComputeLabelStyle(d, entry.cachedNameLower, alpha, time, snap);

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

    // Candlelight Metering: let this plate's ink sit in the scene's light.
    // The sample is smoothed per actor so exposure changes settle with the
    // same weight as every other transition in the system.
    if (snap.candleEnabled && SceneMeter::IsInitialized())
    {
        float lum = .0f;
        float rgb[3] = {};
        if (SceneMeter::Sample(entry.smooth.x / static_cast<float>(viewSize.width),
                               entry.smooth.y / static_cast<float>(viewSize.height),
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

    // Fade title and info rows to zero on ambient (non-focused) actors so
    // only the main line remains.  Crossfades on focus transitions via the
    // same focusSmooth that drives the main-alpha dim.
    //
    // The Quiet Frame's camera-pan fold (paneFold) folds ONLY the title and the
    // status-badge strip above the name -- the name, level, and particles stay
    // full so the core readout never blinks during a pan.  regSubLineMul (the
    // register scene-dimming knob) is a separate feature and still composes on
    // every sub-line.
    const float paneFold = 1.0f - GetState().quietSub;  // 1 = settled, 0 = folded on a pan
    const float regSub = GetState().regSubLineMul;
    const float auxAlphaMul = focusAppliesToActor ? entry.focusSmooth : 1.0f;
    style.titleAlpha *= auxAlphaMul * regSub * paneFold;
    style.infoAlphaMul = auxAlphaMul * regSub;
    style.levelAlpha *= regSub;
    style.badgeAlphaMul = regSub * paneFold;

    LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale, snap);

    // Apply entrance rise Y offset (all styles settle gently into place)
    if (entranceYOffset != .0f)
    {
        layout.startPos.y += entranceYOffset;
        layout.nameplateCenter.y += entranceYOffset;
        layout.nameplateTop += entranceYOffset;
        layout.nameplateBottom += entranceYOffset;
        layout.mainLineCenterY += entranceYOffset;
    }

    // Motion trail: store the actor's WORLD position each frame and draw ghost
    // copies reprojected through the CURRENT camera.  Because a pure camera pan
    // reprojects every stored world point with the same transform, the ghosts
    // collapse onto the head and leave no smear -- only genuine actor movement
    // spreads them into a trail.
    if (snap.visual.EnableMotionTrail && style.tierIdx >= snap.visual.TrailMinTier &&
        entry.entranceDone)
    {
        // Reseed across a world-space teleport (scripted move, same-pass fast
        // travel) so the trail never stretches across the jump.  A camera pan
        // never trips this -- the stored positions are world space, not screen.
        const int lastIdx = (entry.trailIndex - 1 + ActorCache::TRAIL_HISTORY_SIZE) %
                            ActorCache::TRAIL_HISTORY_SIZE;
        if (entry.trailFilled || entry.trailIndex > 0)
        {
            const RE::NiPoint3& prevWorld = entry.trailHistory[lastIdx];
            const float wdx = d.worldPos.x - prevWorld.x;
            const float wdy = d.worldPos.y - prevWorld.y;
            const float wdz = d.worldPos.z - prevWorld.z;
            // Far beyond any locomotion speed even at low frame rates; only a
            // teleport clears this bar.
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

        // Reproject the head (this frame's world pos).  The drawn plate sits at
        // layout.startPos, which differs from the raw projection only by layout
        // offsets (overlap relaxation, entrance rise); carry that same offset
        // onto every ghost so head and trail stay coincident.
        RE::NiPoint3 headScreen{};
        if (trailLen > 1 && WorldToScreen(d.worldPos, headScreen))
        {
            const ImVec2 trailTransientOffset(layout.startPos.x - headScreen.x,
                                              layout.startPos.y - headScreen.y);

            // Camera-compensated distance gate: measure the reprojected screen
            // span between the oldest sample and the head.  Still in pixels, so
            // TrailMinDistance keeps its meaning; a static actor projects to
            // ~one point (span 0) and no trail renders even under a hard pan.
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

                // Draw ghosts from oldest to newest (skip i=0 -- the head draws it).
                for (int i = trailLen - 1; i >= 1; --i)
                {
                    const int idx = (entry.trailIndex - 1 - i + ActorCache::TRAIL_HISTORY_SIZE) %
                                    ActorCache::TRAIL_HISTORY_SIZE;
                    RE::NiPoint3 ghostScreen{};
                    if (!WorldToScreen(entry.trailHistory[idx], ghostScreen))
                    {
                        continue;  // behind the camera after a hard pan
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

                    // Render ghost text for each main line segment
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
                        drawList->AddText(seg.font,
                                          seg.fontSize,
                                          ImVec2(ghostCursorX, ghostY + vOff),
                                          ImGui::ColorConvertFloat4ToU32(ghostCol),
                                          seg.displayText.c_str());
                        ghostCursorX += seg.size.x + layout.segmentPadding;
                    }
                }

                // Restore to front channel
                const int chFront = gpuGlow ? 2 : 1;
                splitter->SetCurrentChannel(drawList, chFront);
            }
        }
    }

    DrawBackgroundGlow(drawList, style, layout, df.lodTitleFactor, splitter, snap);
    DrawParticlesAndOrnaments(
        drawList, d, style, layout, df.lodEffectsFactor, time, splitter, snap.fastOutlines, snap);
    DrawTitleText(drawList, style, layout, df.lodTitleFactor, splitter, snap.fastOutlines, snap);
    DrawMainLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawInfoLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawBadges(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawTierEmblem(drawList, style, layout, time, splitter, snap);
}

// ============================================================================
// Last Rites (death valediction)
// ============================================================================

// Rite phase math (pure -- mirrored in tests/test_utils.cpp; keep in sync).
// t in [0,1]:  hold [0,0.22)  drain [0.22,0.55)  farewell [0.55,1].  During
// the hold the plate is untouched; the drain pulls the ink toward its rite
// color; the farewell fades (and, per creature, crumbles or drifts) it out.
struct DeathRitePhases
{
    float drainT;     ///< Ink drain progress [0,1]
    float dissolveT;  ///< Farewell progress [0,1]
};
static DeathRitePhases ComputeDeathRitePhases(float t)
{
    constexpr float kHoldEnd = .22f;
    constexpr float kDrainEnd = .55f;
    DeathRitePhases p{};
    p.drainT = TextEffects::Saturate((t - kHoldEnd) / (kDrainEnd - kHoldEnd));
    p.dissolveT = TextEffects::Saturate((t - kDrainEnd) / (1.0f - kDrainEnd));
    return p;
}

// Total revealed characters across a computed layout (main row -> info row ->
// title, matching the typewriter's accounting order).
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

// Render the one-shot valediction for an actor that died in view.  The plate
// holds perfectly still (no reprojection -- worldPos of a ragdolling corpse
// must not drag the name around), the ink drains, and the farewell is keyed
// to what the actor was: a mortal's name gutters out and sinks like a snuffed
// candle, a draugr's crumbles letter by letter, a dragon's sears bright and
// goes dark as the soul leaves it.  Replays the last live draw data so the
// styling (relationship color, badges) never flips post-mortem.
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
        entry.exitPhase = 1.0f;  // the rite is the exit -- never replay one
        return;
    }
    const DeathRitePhases ph = ComputeDeathRitePhases(entry.deathPhase);

    const bool sear = d.creatureKind == CreatureKind::Dragon;
    const bool crumble = d.creatureKind == CreatureKind::Undead;

    const float fade = sear ? std::pow(ph.dissolveT, 1.35f) : ph.dissolveT;
    // Quiet Frame does not touch the name/level during a rite either -- the
    // rite's own drain (below) governs how the sub-lines bow out.
    const float alpha =
        entry.alphaSmooth * entry.occlusionSmooth * GetState().regAlphaMul * (1.0f - fade);
    if (alpha < .01f)
    {
        return;
    }
    const float textSizeScale = entry.textSizeScale;

    ImFont* nameFont = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    ImFont* levelFont = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    ImFont* titleFont = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!nameFont || !levelFont || !titleFont)
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    LabelStyle style = ComputeLabelStyle(d, entry.cachedNameLower, alpha, time, snap);
    style.nameOutlineWidth = style.CalcOutlineWidth(nameFont->FontSize * textSizeScale, snap);
    style.levelOutlineWidth = style.CalcOutlineWidth(levelFont->FontSize * textSizeScale, snap);
    style.titleOutlineWidth = style.CalcOutlineWidth(titleFont->FontSize * textSizeScale, snap);
    style.outlineWidth = style.nameOutlineWidth;

    // Ink treatment.
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

    // Sub-lines bow out with the drain; the farewell belongs to name + title.
    const float subMul =
        (1.0f - ph.drainT) * (1.0f - GetState().quietSub) * GetState().regSubLineMul;
    style.infoAlphaMul = subMul;
    style.badgeAlphaMul = subMul;
    style.levelAlpha *= 1.0f - ph.drainT * .5f;

    // Creature-keyed farewell: reverse-typewriter crumble for the undead.
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

    // Vertical drift: mortals sink like a snuffed candle, a dragon's name
    // rises with the departing soul, a crumbling draugr stays put.
    float yOffset = .0f;
    if (sear)
    {
        yOffset = -10.0f * TextEffects::EaseInCubic(ph.dissolveT);
    }
    else if (!crumble)
    {
        yOffset = 6.0f * TextEffects::EaseInCubic(ph.dissolveT);
    }
    layout.startPos.y += yOffset;
    layout.nameplateCenter.y += yOffset;
    layout.nameplateTop += yOffset;
    layout.nameplateBottom += yOffset;
    layout.mainLineCenterY += yOffset;

    DistanceFactors df = ComputeDistanceFactors(d, snap);

    // Cut by the World: the frozen plate still respects world geometry.
    if (GetState().depthClipFrame)
    {
        RE::NiPoint3 ritePos{};
        void* params = WorldToScreen(d.worldPos, ritePos) ? DepthClip::MakePlateParams(ritePos.z)
                                                          : DepthClip::MakeNeutralParams();
        BracketPlateDepthClip(drawList, splitter, params, snap);
    }

    // No particles/ornaments/trail during a rite -- the farewell is pure
    // typography, held ruthlessly sparse on purpose.
    DrawBackgroundGlow(drawList, style, layout, df.lodTitleFactor, splitter, snap);
    DrawTitleText(drawList, style, layout, df.lodTitleFactor, splitter, snap.fastOutlines, snap);
    DrawMainLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawInfoLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawBadges(drawList, style, layout, splitter, snap.fastOutlines, snap);
}

// Render a nameplate that has just left the snapshot, fading and sinking out of
// view over snap.exitDuration. Replays the last cached draw data at the last
// smoothed screen position (entry.smooth) -- no reprojection or new smoothing.
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

    const float alpha =
        entry.alphaSmooth * entry.occlusionSmooth * exitAlphaMul * GetState().regAlphaMul;
    if (alpha < .01f)
    {
        return;
    }
    const float textSizeScale = entry.textSizeScale * exitScaleMul;

    ImFont* nameFont = GetFontAt(RenderConstants::FONT_INDEX_NAME);
    ImFont* levelFont = GetFontAt(RenderConstants::FONT_INDEX_LEVEL);
    ImFont* titleFont = GetFontAt(RenderConstants::FONT_INDEX_TITLE);
    if (!nameFont || !levelFont || !titleFont)
    {
        return;
    }

    const float time = (float)ImGui::GetTime();
    LabelStyle style = ComputeLabelStyle(d, entry.cachedNameLower, alpha, time, snap);
    style.nameOutlineWidth = style.CalcOutlineWidth(nameFont->FontSize * textSizeScale, snap);
    style.levelOutlineWidth = style.CalcOutlineWidth(levelFont->FontSize * textSizeScale, snap);
    style.titleOutlineWidth = style.CalcOutlineWidth(titleFont->FontSize * textSizeScale, snap);
    style.outlineWidth = style.nameOutlineWidth;

    // Preserve the last focus state so an ambient label keeps its dimmed title.
    // Exiting ghosts honor the Quiet Frame like live plates: only the title and
    // badge strip fold on a pan; name, level, and particles stay full.
    const float paneFold = 1.0f - GetState().quietSub;
    const float regSub = GetState().regSubLineMul;
    const bool focusApplies = snap.focus.Enabled && !d.isPlayer;
    const float auxAlphaMul = focusApplies ? entry.focusSmooth : 1.0f;
    style.titleAlpha *= auxAlphaMul * regSub * paneFold;
    style.infoAlphaMul = auxAlphaMul * regSub;
    style.levelAlpha *= regSub;
    style.badgeAlphaMul = regSub * paneFold;

    // Reuse distance-derived LOD factors from the cached distance.
    DistanceFactors df = ComputeDistanceFactors(d, snap);

    LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale, snap);
    layout.startPos.y += exitYOffset;
    layout.nameplateCenter.y += exitYOffset;
    layout.nameplateTop += exitYOffset;
    layout.nameplateBottom += exitYOffset;
    layout.mainLineCenterY += exitYOffset;

    // Cut by the World: ghosts reproject their last world position; if that
    // fails (behind camera) they render unclipped rather than inheriting the
    // previous plate's depth.
    if (GetState().depthClipFrame)
    {
        RE::NiPoint3 ghostPos{};
        void* params = WorldToScreen(d.worldPos, ghostPos) ? DepthClip::MakePlateParams(ghostPos.z)
                                                           : DepthClip::MakeNeutralParams();
        BracketPlateDepthClip(drawList, splitter, params, snap);
    }

    // No motion trail on exit (the label is not moving); everything else renders
    // as a fading, sinking copy of the last live frame.
    DrawBackgroundGlow(drawList, style, layout, df.lodTitleFactor, splitter, snap);
    DrawParticlesAndOrnaments(
        drawList, d, style, layout, df.lodEffectsFactor, time, splitter, snap.fastOutlines, snap);
    DrawTitleText(drawList, style, layout, df.lodTitleFactor, splitter, snap.fastOutlines, snap);
    DrawMainLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawInfoLineSegments(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawBadges(drawList, style, layout, splitter, snap.fastOutlines, snap);
    DrawTierEmblem(drawList, style, layout, time, splitter, snap);
}

// ============================================================================
// Debug overlay
// ============================================================================

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
    ctx.tierCount = snap.tiers.size();
    ctx.reloadKey = snap.reloadKey;

    DebugOverlay::Render(ctx);
}

// ============================================================================
// Hot reload
// ============================================================================

static void HandleHotReload()
{
    // Complete an async reload that the game thread finished on a prior frame.
    // cache.clear() must stay on the render thread since the cache is only
    // accessed here.
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

    // seq_cst: the pause-flag store and the in-flight loads target *different*
    // atomics, so a release/acquire pair alone establishes no happens-before
    // on weakly-ordered archs. seq_cst gives the global ordering we need.
    GetState().pauseSnapshotUpdates.store(true, std::memory_order_seq_cst);
    const bool queuedAfterPause = GetState().updateQueued.load(std::memory_order_seq_cst);
    const bool runningAfterPause = GetState().snapshotUpdateRunning.load(std::memory_order_seq_cst);
    if (queuedAfterPause || runningAfterPause)
    {
        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
        GetState().reloadKeyWasDown = keyDown;
        return;
    }

    // Queue Settings::Load() to the game thread to avoid a frame hitch
    // from synchronous file I/O on the render thread.  The render thread
    // stays paused (pauseSnapshotUpdates) until reloadCompleted is set.
    if (auto* task = SKSE::GetTaskInterface())
    {
        task->AddTask(
            []()
            {
                Settings::Load();

                bool shouldReapply = false;
                {
                    const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
                    shouldReapply = Settings::Appearance().TemplateReapplyOnReload &&
                                    Settings::Appearance().UseTemplateAppearance;
                }
                if (shouldReapply)
                {
                    AppearanceTemplate::ResetAppliedFlag();
                    AppearanceTemplate::ApplyIfConfigured();
                }

                // Signal the render thread to finalize the reload on the next frame.
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

        bool shouldReapplyFallback = false;
        {
            const std::shared_lock<std::shared_mutex> lock(Settings::Mutex());
            shouldReapplyFallback = Settings::Appearance().TemplateReapplyOnReload &&
                                    Settings::Appearance().UseTemplateAppearance;
        }
        if (shouldReapplyFallback)
        {
            AppearanceTemplate::ResetAppliedFlag();
            if (auto* fallbackTask = SKSE::GetTaskInterface())
            {
                fallbackTask->AddTask([]() { AppearanceTemplate::ApplyIfConfigured(); });
            }
        }

        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
        GetState().reloadRequested.store(false, std::memory_order_release);
    }

    GetState().reloadKeyWasDown = keyDown;
}

// ============================================================================
// Debug stats
// ============================================================================

static void UpdateDebugStats(const std::vector<ActorDrawData>& snap)
{
    GetState().debugStats.actorCount = static_cast<int>(snap.size());
    GetState().debugStats.visibleActors = 0;
    GetState().debugStats.occludedActors = 0;
    GetState().debugStats.playerVisible = 0;

    for (const auto& d : snap)
    {
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

// ============================================================================
// Focus-target selection
// ============================================================================

// Pick the formID of the actor whose direction from the camera lies inside the
// configured cone with the smallest angular offset. Returns 0 if nothing
// qualifies (camera unavailable, cone empty, all candidates filtered).
// Tiebreakers: smaller player distance, then smaller formID for determinism.
// The player is never picked.
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

    // Resolve max distance: 0 = reuse global MaxScanDistance from snapshot.
    // (The snapshot already filters actors beyond MaxScanDistance, but
    // FocusMaxDistance can shrink the cone further.)
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
        if (d.isPlayer || d.isDead)
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

        // Higher dot wins; ties broken by closer-to-player; then formID.
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

// ============================================================================
// Overlap resolution
// ============================================================================

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
        if (d.isDead)
        {
            continue;  // A dying plate is frozen -- it neither pushes nor moves.
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

    // Store offsets for DrawLabel to apply
    for (const auto& lr : labelRects)
    {
        if (std::abs(lr.yOffset) > .01f)
        {
            OverlapOffsets()[localSnap[lr.idx].formID] = lr.yOffset;
        }
    }
}

// ============================================================================
// Settings snapshot factory
// ============================================================================

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
    snap.hidePlayer = disp.HidePlayer;
    snap.reloadKey = disp.ReloadKey;

    snap.nameFontSize = Settings::Font().NameFontSize;
    snap.visual = Settings::Visual();
    snap.tiers = Settings::Tiers();
    snap.titleFormat = Settings::TitleFormat();
    snap.displayFormat = Settings::DisplayFormat();
    snap.infoFormat = Settings::InfoFormat();
    snap.specialTitles = Settings::SpecialTitles();

    // Contextual label tokens + classification thresholds.
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

    // Status icon badges -- colors pre-derived in ClampAndValidate.
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
    // Expanded always-on slots (more NPC + player indicators).
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
    snap.focus = Settings::Focus();
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

// ============================================================================
// Main entry points
// ============================================================================

void Draw()
{
    HandleHotReload();

    // While a hot reload is in flight (Settings::Load() running on game thread),
    // skip rendering to avoid reading non-POD Settings concurrently with mutation.
    if (GetState().reloadRequested.load(std::memory_order_acquire))
    {
        return;
    }

    // Re-capture settings snapshot only when generation changes (i.e. after Load()).
    // This avoids per-frame heap allocations for vectors/strings that rarely change.
    const uint32_t currentGen = Settings::Generation().load(std::memory_order_acquire);
    if (currentGen != GetState().lastSnapGeneration)
    {
        GetState().cachedSnap = RenderSettingsSnapshot::CaptureFromSettings();
        GetState().cachedSnap.PopulateSortedSpecialTitles();
        GetState().lastSnapGeneration = currentGen;
    }

    // A RaceMenu rename fires this: drop the player's cache entry so the live
    // name (re-read every snapshot) re-reveals via the typewriter. Safe here --
    // the cache is render-thread-only.
    if (GetState().pendingIdentityRefresh.exchange(false, std::memory_order_acq_rel))
    {
        GetState().cache.erase(0x14);  // player FormID
        GetState().pauseSnapshotUpdates.store(false, std::memory_order_release);
    }
    const RenderSettingsSnapshot& snap = GetState().cachedSnap;

    // Gate on the game-thread-published atomic rather than calling
    // GameState::CanDrawOverlay() here: Draw() runs on the render thread, where a
    // direct player->GetParentCell() read can race with cell teardown.
    if (!GetState().allowOverlay.load(std::memory_order_acquire))
    {
        GetState().wasInInvalidState = true;
        return;
    }

    if (GetState().wasInInvalidState)
    {
        GetState().wasInInvalidState = false;
        GetState().postLoadCooldown = 300;
        // Roll Call: replay entrances on the first frame drawn after the
        // overlay wakes so the scene re-introduces itself as a cascade.
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
    {
        std::lock_guard<std::mutex> lock(GetState().snapshotLock);
        localSnap = GetState().snapshot;
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

    // Candlelight Metering: sample the composed scene before any overlay
    // draws land on it -- the meter must never read glyph's own text back.
    // (Pure copies; alters no pipeline state, so no ResetRenderState.)
    if (snap.candleEnabled && SceneMeter::IsInitialized())
    {
        drawList->AddCallback(SceneMeter::CaptureCallback, nullptr);
    }

    if (snap.enableDebugOverlay)
    {
        UpdateDebugStats(localSnap);
    }

    // Quiet Frame: advance the camera-motion quiet factors once per frame.
    UpdateQuietFrame(snap, ImGui::GetIO().DeltaTime);

    // Registers: ease the effective scene-profile knobs.
    UpdateRegisters(snap, ImGui::GetIO().DeltaTime);

    // Candlelight Metering: pull last frame's scene sample into the CPU grid.
    if (snap.candleEnabled && SceneMeter::IsInitialized())
    {
        SceneMeter::CollectResults();
    }

    // Cut by the World: arm per-pixel depth clipping for this frame when the
    // game's depth buffer is reachable and the projection's depth convention
    // is determinate.  Failure means this frame renders exactly as before.
    GetState().depthClipFrame = false;
    if (snap.depthClipEnabled && DepthClip::IsInitialized())
    {
        const float polarity = ComputeDepthPolarity();
        GetState().depthClipFrame =
            polarity != .0f && DepthClip::BeginFrame(snap.depthClipFeather, polarity);
    }

    // Roll Call wake pass: the overlay just resumed after suppression
    // (combat end, menu close, cell load).  Re-arm every visible plate's
    // entrance and typewriter; the entrance block below then hands out
    // stagger slots in snapshot order, which is already near-to-far.
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
            // Deaths that happened while the overlay was suppressed (player
            // combat) are not replayed as stale rites after the fact.
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
    if (snap.visual.EnableOverlapPrevention)
    {
        ResolveOverlaps(localSnap, snap);
    }

    const uint32_t focusedFormID = SelectFocusedActor(localSnap, snap);

    // 3-channel splitter: [0]=glow capture/backplate, [1]=particles, [2]=text+shadow+outline
    const bool gpuGlow = snap.enableGlow && TextPostProcess::IsInitialized();
    const bool gpuDivide = snap.glowDivideStrength > .0f && TextPostProcess::IsInitialized();
    ImDrawListSplitter splitter;
    splitter.Split(drawList, gpuGlow ? 3 : 2);

    if (gpuGlow)
    {
        TextPostProcess::SetGlowParams(snap.glowRadius, snap.glowIntensity);
        splitter.SetCurrentChannel(drawList, 0);
        drawList->AddCallback(TextPostProcess::BeginGlowCapture, nullptr);
    }

    // Color-divide capture: snapshot the back-buffer before any nametag text
    // so the divide shader can read the original scene behind the text.
    if (gpuDivide)
    {
        TextPostProcess::SetDivideParams(snap.glowDivideStrength);
        drawList->AddCallback(TextPostProcess::BeginDivideCapture, nullptr);
    }

    for (auto& d : localSnap)
    {
        if (d.isDead)
        {
            // Last Rites: one-shot valediction for actors this overlay saw
            // alive.  Corpses first seen dead (or already mourned) never
            // render -- the overlay is a record of the living.
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
        DrawLabel(d, drawList, &splitter, snap, focusedFormID);
    }

    // Exit pass: actors that just left the snapshot fade + sink out of view over
    // snap.exitDuration before their cache entry is pruned. Ghosts replay their
    // last live draw data and never participate in focus/overlap (live-only).
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

    // Cut by the World: the front channel executes last, so its trailing
    // reset returns the ImGui backend to its own shader before anything
    // outside the overlay (debug HUD, other windows) renders.
    if (GetState().depthClipFrame)
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

    // Color-divide composite: blend nametag text with the pre-snapshot using
    // the Photoshop-style divide blend for a light-emission look.
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
    // Must queue snapshot updates here too, not just in Draw(): Draw() runs only
    // when shouldRenderOverlay is true, but allowOverlay (which gates that
    // flag) is set inside UpdateSnapshot_GameThread. Without this call the
    // overlay never bootstraps -- allowOverlay stays false because the
    // snapshot update is never scheduled.
    QueueSnapshotUpdate_RenderThread();
}
}  // namespace Renderer
