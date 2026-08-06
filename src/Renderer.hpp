#pragma once

#include "PCH.hpp"

/**
 * @namespace Renderer
 * @brief Render-thread nameplates from game-thread snapshots.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Actor facts arrive as plain data under `snapshotLock`. The render thread owns
 * animation caches and must not resolve actor handles. Its engine reads are limited
 * to camera and renderer singletons for projection, aim, and screen size.
 *
 * ### :material-sitemap-outline: Thread ownership
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef thread fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef data fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *
 *     RT[Render thread]:::thread -->|Schedule update| GT[Game thread]:::thread
 *     GT -->|Publish ActorDrawData| Snap[Snapshot - guarded by snapshotLock]:::data
 *     Snap -->|Copy under snapshotLock| RT
 *     RT -->|Owns smoothing state| Cache[ActorCache - render thread only]:::data
 *     RT -->|Draw nameplates| ImGui[ImGui]:::render
 * ```
 *
 * ### :material-pipe: Frame gates
 *
 * The first three gates return before copying the snapshot.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef check fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef process fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef render fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *
 *     A[Hot reload in flight?]:::check --> B[Overlay allowed?]:::check
 *     B --> C[Post-load cooldown?]:::check
 *     C --> D[Queue actor update]:::process
 *     D --> E[Copy snapshot]:::process
 *     E --> F[Project / smooth]:::process
 *     F --> G[Draw plates, rites, exits]:::render
 *     G --> H[Composite and prune cache]:::render
 * ```
 *
 * ### :material-chart-bell-curve-cumulative: Smoothing
 *
 * $$v_{new} = v_{old} + (v_{target} - v_{old}) \cdot \alpha$$
 *
 * $\alpha = 1 - \epsilon^{\,\Delta t \,/\, T_{settle}}$, with $\epsilon = 0.01$
 * and settle time in seconds. Position also uses an 8-frame average and per-frame
 * large-movement blending; those terms depend on frame rate.
 *
 * ### :material-cube-scan: Projection
 *
 * WorldToScreen converts normalized camera coordinates to ImGui pixels:
 * $x_{px} = x \cdot w$, $y_{px} = (1 - y) \cdot h$. Callers reject depth outside [0,1].
 * ComputeDepthPolarity resolves depth polarity from two projected probes.
 *
 * @see Hooks::Install, TextEffects
 */
namespace Renderer
{
/**
 * @fn void Draw()
 * @brief Advance and draw the current nameplate frame.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Render-thread only. Advances reload handling, animation and cache pruning.
 * Returns before drawing during reload, a closed overlay gate, or the 300-frame
 * wake cooldown. The cooldown counts eligible Draw calls, so it is not a fixed time.
 * A missing renderer or empty snapshot also drops the frame.
 * TickRT must continue on skipped frames to keep snapshots current.
 *
 * @pre An initialized ImGui context inside NewFrame/EndFrame.
 * @see TickRT, IsOverlayAllowedRT
 */
void Draw();

/**
 * @fn void TickRT()
 * @brief Queue actor collection and poll the Deck capture key.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Render-thread only; call once per frame, including hidden frames, so the published
 * draw gate can bootstrap. Requests coalesce to one task. Key polling takes a shared
 * settings lock; PrepareDeckCaptureRT resolves the recorded press.
 */
void TickRT();

/**
 * @fn bool IsOverlayAllowedRT()
 * @brief Read the manual switch and last published game-state gate.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Reads the latest published game-state gate, which can precede the actor snapshot.
 * Deck has a separate gate and can still request an ImGui frame.
 *
 * @return True when both the manual switch and the published gate allow plates.
 *
 * @see TickRT, GameState::CanDrawOverlay
 */
bool IsOverlayAllowedRT();

/**
 * @fn bool ToggleEnabled()
 * @brief Atomically toggle the manual switch from any thread.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Takes effect on the next frame.
 *
 * @return New enabled state.
 */
bool ToggleEnabled();

/**
 * @fn void SetEnabled(bool enabled)
 * @brief Atomically set the manual switch from any thread.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Takes effect on the next frame.
 */
void SetEnabled(bool enabled);

/**
 * @fn bool IsEnabled()
 * @brief Read the manual switch from any thread.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Game-state gates still apply when the manual switch is enabled.
 *
 * @return The manual switch state, independent of game readiness.
 */
bool IsEnabled();

/**
 * @fn void RequestIdentityRefresh()
 * @brief Request a player-name refresh from any thread.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The next Draw erases the player cache entry and clears snapshot pause.
 * The next snapshot publishes the new name and the typewriter restarts.
 */
void RequestIdentityRefresh();

/**
 * @fn void PrepareDeckCaptureRT()
 * @brief Resolve a pending Deck key press into a plain-data card request.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Render-thread only; call before HUD drawing for a clean scene copy.
 * Target order: live crosshair actor, nearest live/unoccluded non-player projected
 * head-to-feet segment within DeckTargetRadius pixels, then the permitted player
 * fallback. Disabled Deck or no pending press does no work; failures use Deck's toast.
 */
void PrepareDeckCaptureRT();
}  // namespace Renderer
