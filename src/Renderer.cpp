#include "Renderer.h"

#include "RendererInternal.h"

#include "AppearanceTemplate.h"
#include "GameState.h"
#include "ParticleTextures.h"

#include <SKSE/SKSE.h>

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
            // Check if grace period has expired
            uint32_t framesSinceLastSeen = GetState().frame - it->second.lastSeenFrame;
            if (framesSinceLastSeen > CACHE_GRACE_FRAMES)
            {
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

// ============================================================================
// Distance and smoothing helpers (file-local)
// ============================================================================

// Compute distance-based visual factors (fade, LOD, scale).
static DistanceFactors ComputeDistanceFactors(const ActorDrawData& d,
                                              const RenderSettingsSnapshot& snap)
{
    DistanceFactors df{};
    const float dist = d.distToPlayer;

    // Alpha fade using squared smoothstep
    const float fadeRange = std::max(1.0f, snap.fadeEndDistance - snap.fadeStartDistance);
    float fadeT = TextEffects::SmoothStep((dist - snap.fadeStartDistance) / fadeRange);
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
                      const RenderSettingsSnapshot& snap)
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

    // Reset typewriter on re-entry (actor reappearing or becoming unoccluded)
    constexpr uint32_t REENTRY_THRESHOLD = 30;
    if (entry.initialized && entry.typewriterComplete)
    {
        uint32_t framesSinceLastSeen = GetState().frame - prevLastSeenFrame;
        bool becameVisible = entry.wasOccluded && !d.isOccluded;
        if (framesSinceLastSeen >= REENTRY_THRESHOLD || becameVisible)
        {
            entry.typewriterTime = .0f;
            entry.typewriterComplete = false;
        }
    }

    entry.lastSeenFrame = GetState().frame;

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

    // Cull off-screen labels
    const float alpha = entry.alphaSmooth * entry.occlusionSmooth;
    const float textSizeScale = entry.textSizeScale;

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

    LabelLayout layout = ComputeLabelLayout(d, entry, style, textSizeScale, snap);

    DrawParticles(drawList, d, style, layout, df.lodEffectsFactor, time, splitter, snap);
    DrawOrnaments(
        drawList, d, style, layout, df.lodEffectsFactor, time, splitter, snap.fastOutlines, snap);
    DrawTitleText(drawList, d, style, layout, df.lodTitleFactor, splitter, snap.fastOutlines, snap);
    DrawMainLineSegments(drawList, d, style, layout, splitter, snap.fastOutlines, snap);
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

    // Use seq_cst to ensure the pause flag is globally visible before we
    // re-check for in-flight updates.  A release/acquire pair alone is
    // insufficient here because the store and loads target different
    // atomic variables, so no happens-before is established between them
    // on weakly-ordered architectures.
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
    snap.segmentPadding = so.SegmentPadding;
    snap.fastOutlines = so.FastOutlines;
    snap.titleMainGap = so.TitleMainGap;
    snap.outlineMinScale = so.OutlineMinScale;
    snap.proportionalSpacing = so.ProportionalSpacing;

    const auto& gl = Settings::Glow();
    snap.enableGlow = gl.Enabled;
    snap.glowRadius = gl.Radius;
    snap.glowIntensity = gl.Intensity;
    snap.glowSamples = gl.Samples;

    const auto& tw = Settings::Typewriter();
    snap.enableTypewriter = tw.Enabled;
    snap.typewriterSpeed = tw.Speed;
    snap.typewriterDelay = tw.Delay;

    const auto& orn = Settings::Ornament();
    snap.enableOrnaments = orn.Enabled;
    snap.ornamentScale = orn.Scale;
    snap.ornamentSpacing = orn.Spacing;
    snap.ornamentFontPath = orn.FontPath;
    snap.ornamentFontSize = orn.FontSize;
    snap.ornamentAnchorToMainLine = orn.AnchorToMainLine;

    const auto& part = Settings::Particle();
    snap.enableParticleAura = part.Enabled;
    snap.useParticleTextures = part.UseParticleTextures;
    snap.enableStars = part.EnableStars;
    snap.enableSparks = part.EnableSparks;
    snap.enableWisps = part.EnableWisps;
    snap.enableRunes = part.EnableRunes;
    snap.enableOrbs = part.EnableOrbs;
    snap.particleCount = part.Count;
    snap.particleSize = part.Size;
    snap.particleSpeed = part.Speed;
    snap.particleSpread = part.Spread;
    snap.particleAlpha = part.Alpha;
    snap.particleBlendMode = part.BlendMode;

    const auto& ac = Settings::AnimColor();
    snap.animSpeedLowTier = ac.AnimSpeedLowTier;
    snap.animSpeedMidTier = ac.AnimSpeedMidTier;
    snap.animSpeedHighTier = ac.AnimSpeedHighTier;
    snap.colorWashAmount = ac.ColorWashAmount;
    snap.nameColorMix = ac.NameColorMix;
    snap.effectAlphaMin = ac.EffectAlphaMin;
    snap.effectAlphaMax = ac.EffectAlphaMax;
    snap.strengthMin = ac.StrengthMin;
    snap.strengthMax = ac.StrengthMax;
    snap.alphaSettleTime = ac.AlphaSettleTime;
    snap.scaleSettleTime = ac.ScaleSettleTime;
    snap.positionSettleTime = ac.PositionSettleTime;
    snap.occlusionSettleTime = Settings::Occlusion().SettleTime;

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
    snap.specialTitles = Settings::SpecialTitles();

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
    const RenderSettingsSnapshot& snap = GetState().cachedSnap;

    if (!GameState::CanDrawOverlay())
    {
        GetState().wasInInvalidState = true;
        return;
    }

    if (GetState().wasInInvalidState)
    {
        GetState().wasInInvalidState = false;
        GetState().postLoadCooldown = 300;
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

    if (snap.enableDebugOverlay)
    {
        UpdateDebugStats(localSnap);
    }

    OverlapOffsets().clear();
    if (snap.visual.EnableOverlapPrevention)
    {
        ResolveOverlaps(localSnap, snap);
    }

    ImDrawListSplitter splitter;
    splitter.Split(drawList, 2);

    for (auto& d : localSnap)
    {
        DrawLabel(d, drawList, &splitter, snap);
    }

    splitter.Merge(drawList);

    ImGui::End();

    DrawDebugOverlay(snap);
    PruneCacheToSnapshot(localSnap);
}

void TickRT()
{
    // Must queue snapshot updates here, not only in Draw().  Draw() is only
    // called when shouldRenderOverlay is true, but allowOverlay (which gates
    // shouldRenderOverlay) is set by UpdateSnapshot_GameThread.  Without this
    // call the overlay can never bootstrap: allowOverlay stays false because
    // the snapshot update is never scheduled.
    QueueSnapshotUpdate_RenderThread();
}
}  // namespace Renderer
