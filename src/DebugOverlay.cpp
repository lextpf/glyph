#include "DebugOverlay.hpp"

#include <imgui.h>
#include <algorithm>

namespace DebugOverlay
{
void UpdateFrameStats(Stats& stats,
                      float deltaTime,
                      float currentTime,
                      float& lastUpdateTime,
                      int updateCounter,
                      int& lastUpdateCount)
{
    // Average frame history to reduce display jitter.
    constexpr int SAMPLES = RenderConstants::FRAME_TIME_SAMPLES;
    stats.frameTimeHistory[stats.frameTimeIndex] = deltaTime * 1000.0f;
    stats.frameTimeIndex = (stats.frameTimeIndex + 1) % SAMPLES;

    float sum = .0f;
    for (int i = 0; i < SAMPLES; ++i)
    {
        sum += stats.frameTimeHistory[i];
    }
    stats.avgFrameTimeMs = sum / static_cast<float>(SAMPLES);

    stats.frameTimeMs = deltaTime * 1000.0f;
    stats.fps = (deltaTime > .0f) ? (1.0f / deltaTime) : .0f;

    // Sample the cumulative counter once per second for a stable rate.
    if (currentTime - lastUpdateTime >= 1.0f)
    {
        stats.updatesPerSecond = updateCounter - lastUpdateCount;
        lastUpdateCount = updateCounter;
        lastUpdateTime = currentTime;
    }
}

void Render(const Context& ctx)
{
    if (!ctx.stats)
    {
        return;
    }

    const Stats& stats = *ctx.stats;
    const float time = static_cast<float>(ImGui::GetTime());

    // Height zero sizes the window to its content.
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(.75f);  // semi-transparent so game is visible behind

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("glyph Debug", nullptr, flags))
    {
        ImGui::TextColored(ImVec4(.4f, .8f, 1.0f, 1.0f), "glyph Debug");

        float timeSinceReload = time - ctx.lastReloadTime;
        if (timeSinceReload < RenderConstants::RELOAD_NOTIFICATION_DURATION)
        {
            ImGui::SameLine();
            float flashAlpha =
                1.0f - timeSinceReload / RenderConstants::RELOAD_NOTIFICATION_DURATION;
            ImGui::TextColored(ImVec4(.2f, 1.0f, .2f, flashAlpha), " [Reloaded!]");
        }

        ImGui::Separator();

        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Performance");
        ImGui::Text("FPS: %.1f", stats.fps);
        ImGui::Text("Frame: %.2f ms", stats.frameTimeMs);
        ImGui::Text("Avg:   %.2f ms", stats.avgFrameTimeMs);

        // ASCII bar reaches full scale at 60 FPS.
        float fpsNorm = std::clamp(stats.fps / 60.0f, .0f, 1.0f);

        ImVec4 fpsColor = (stats.fps >= 60.0f)   ? ImVec4(.2f, .9f, .2f, 1.0f)  // green - smooth
                          : (stats.fps >= 30.0f) ? ImVec4(.9f, .9f, .2f, 1.0f)  // yellow - playable
                                                 : ImVec4(.9f, .2f, .2f, 1.0f);  // red - laggy

        ImGui::TextColored(fpsColor, "[");
        ImGui::SameLine(0, 0);
        int bars = static_cast<int>(fpsNorm * 20);
        for (int i = 0; i < 20; ++i)
        {
            if (i < bars)
            {
                ImGui::TextColored(fpsColor, "|");
            }
            else
            {
                ImGui::TextColored(ImVec4(.3f, .3f, .3f, 1.0f), ".");
            }
            ImGui::SameLine(0, 0);  // no spacing between characters
        }
        ImGui::TextColored(fpsColor, "]");

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Actors");
        ImGui::Text("Total:    %d", stats.actorCount);      // Plate-drawing actors in the snapshot
        ImGui::Text("Visible:  %d", stats.visibleActors);   // passed visibility checks
        ImGui::Text("Occluded: %d", stats.occludedActors);  // hidden behind geometry
        ImGui::Text("Player:   %s", stats.playerVisible ? "Yes" : "No");  // player nameplate state

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Cache");
        ImGui::Text("Entries: %zu", stats.cacheSize);  // cached actor count
        ImGui::Text("Frame:   %u", ctx.frameNumber);   // current render frame number

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Updates");
        ImGui::Text("Updates/sec: %d", stats.updatesPerSecond);  // Data refreshes per second
        ImGui::Text("Cooldown:    %d",
                    ctx.postLoadCooldown);  // frames until full processing resumes

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Settings");
        ImGui::Text("Occlusion: %s", ctx.occlusionEnabled ? "On" : "Off");
        ImGui::Text("Glow:      %s", ctx.glowEnabled ? "On" : "Off");
        ImGui::Text("Typewriter:%s", ctx.typewriterEnabled ? "On" : "Off");
        ImGui::Text("HidePlayer:%s", ctx.hidePlayer ? "On" : "Off");
        ImGui::Text("V.Offset:  %.1f", ctx.verticalOffset);  // nameplate height offset
        ImGui::Text("Plate cap: %d", ctx.maxPlates);
        ImGui::Text("Scan cap:  %d", ctx.maxScanActors);
        ImGui::Text("Tiers:     %zu", ctx.tierCount);  // color tier definitions
        if (ctx.reloadKey > 0)
        {
            ImGui::Text("Reload Key: 0x%X", ctx.reloadKey);
        }
        else
        {
            ImGui::TextColored(ImVec4(.5f, .5f, .5f, 1.0f), "Reload Key: Disabled");
        }

        ImGui::Spacing();

        // Struct-size estimates exclude allocator overhead.
        ImGui::TextColored(ImVec4(1.0f, .8f, .4f, 1.0f), "Memory (Est.)");
        size_t cacheMemory = stats.cacheSize * ctx.actorCacheEntrySize;
        size_t snapshotMemory = stats.actorCount * ctx.actorDrawDataSize;
        ImGui::Text("Cache:    ~%zu bytes", cacheMemory);     // persistent actor cache
        ImGui::Text("Snapshot: ~%zu bytes", snapshotMemory);  // per-frame draw data
    }
    ImGui::End();
}

}  // namespace DebugOverlay
