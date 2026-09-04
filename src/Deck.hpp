#pragma once

#include "DeckUtils.hpp"
#include "Settings.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * @namespace Deck
 * @brief Character-card capture and PNG export.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Renderer
 *
 * Render-thread calls use plain actor data without an internal lock. Shutdown also runs
 * on the device-creation thread. Only PNG encoding runs on the private worker.
 * One card may compose or await GPU readback at a time; queued encodes permit another capture.
 * The encoder owns copied pixel buffers and never accesses D3D11 resources. Completion results
 * remain pending until Process consumes them, so NeedsFrame stays true until they are reported.
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef rt fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef gpu fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef worker fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *
 *     A[PollInput - key edge]:::rt --> B[ConsumeCaptureRequest]:::rt
 *     B --> C[Queue - CardRequest]:::rt
 *     C --> D[CaptureScene - scene copy]:::gpu
 *     D --> E[Process - draw card, copy to staging]:::gpu
 *     E --> F[PollReadback - later frame, event query, RGBA to BGRA]:::gpu
 *     F --> G[Encoder worker - writes the PNG]:::worker
 *     G --> H[DrawNotification - result toast]:::rt
 * ```
 *
 * | Capture path            | Portrait contents                |
 * |-------------------------|----------------------------------|
 * | HUDMenu::PostDisplay    | Scene before the HUD movie draws |
 * | IDXGISwapChain::Present | Scene with the HUD already drawn |
 * | Late Process capture    | Scene with the HUD already drawn |
 */
namespace Deck
{
/**
 * @struct PortraitUV
 * @brief Scene texture coordinates with a top-left origin and positive v downward.
 * @author Alex (<https://github.com/lextpf>)
 */
struct PortraitUV
{
    float left = .0f;  ///< Scene width fraction in [0, 1]
    float top = .0f;   ///< Scene height fraction in [0, 1]
    float right = 1.0f;
    float bottom = 1.0f;
};

/**
 * @struct CardBadge
 * @brief One icon in the card badge strip.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Unloaded textures are skipped; the remaining strip is centered and shrunk to fit.
 */
struct CardBadge
{
    std::string icon;          ///< Duotone name; ignored when tierImage >= 0
    Settings::Color3 color{};  ///< Duotone tint; ignored for tier emblems
    bool muted = false;        ///< Desaturated tint and lower opacity
    int tierImage = -1;        ///< Untinted emblem index; -1 selects icon
};

/**
 * @struct CardRequest
 * @brief Resolved actor data that remains valid across capture frames.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Contains no game objects. The treatment tier supplies styling; special titles can override
 * name and title colors. Common cards use plain text without effects.
 *
 * Relative output paths use the game directory. An empty outputFolder selects
 * Data/SKSE/Plugins/glyph/cards; the encoder creates the directory on demand.
 * Particle tokens ignore case, whitespace, and weights. Only the first two recognized
 * styles draw, and only for Legendary cards.
 */
struct CardRequest
{
    /// Seeds repeatable visual effects and identifies the printed card and filename.
    std::uint32_t formID = 0;
    std::string name;  ///< UTF-8
    /// Console override, special title, honorific, then tier title; empty hides the row.
    std::string title;
    std::string tierName;       ///< Treatment tier title for the footer
    std::string outputFolder;   ///< Output directory; empty selects the default.
    std::string particleTypes;  ///< Comma-separated ParticleTypes tokens.

    std::uint16_t level = 0;
    int tierImageIndex = -1;  ///< Emblem resolved by TierEmblem::Select; -1 draws no emblem
    int width = 750;          ///< Pixels
    int height = 1050;        ///< Pixels
    Rarity rarity = Rarity::Common;
    PortraitUV portrait{};

    Settings::Color3 nameLeft{};
    Settings::Color3 nameRight{};
    Settings::Color3 titleLeft{};
    Settings::Color3 titleRight{};
    Settings::Color3 frameLeft{};
    Settings::Color3 frameRight{};
    Settings::Color3 highlight{};
    Settings::Color3 particleColor{};
    Settings::Color3 ornamentLeft{};
    Settings::Color3 ornamentRight{};
    Settings::EffectParams nameEffect{};
    Settings::EffectParams titleEffect{};
    std::string leftOrnaments;
    std::string rightOrnaments;
    int particleCount = 0;  ///< Halved, then clamped to [4, 10] per style

    /// Draw order; empty omits the strip.
    std::vector<CardBadge> badges;
};

/**
 * @fn void PollInput(bool enabled, int virtualKey, bool worldReady)
 * @brief Poll a capture key edge and show an acceptance or rejection toast.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Pending capture or GPU readback rejects another edge. GetAsyncKeyState also detects edges
 * when the game window has no focus.
 *
 * @param enabled False clears the key state and unconsumed request; queued cards continue.
 * @param virtualKey Win32 virtual-key code; non-positive values clear state as disabled does.
 * @param worldReady False rejects the edge with a loading/menu toast.
 */
void PollInput(bool enabled, int virtualKey, bool worldReady);

/**
 * @fn bool ConsumeCaptureRequest()
 * @brief Consume the pending capture key edge.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return True once per accepted edge; clears the pending request.
 */
bool ConsumeCaptureRequest();

/**
 * @fn bool Queue(CardRequest request)
 * @brief Move resolved card data into the pending capture slot.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Acceptance resets the scene copy and shows a status toast.
 * The caller supplies valid positive image dimensions; Queue does not validate the request.
 *
 * @param request Plain actor data, moved into storage.
 * @return False while a card is queued or awaiting readback; already shows an error toast.
 *         Queued PNG encodes do not block acceptance.
 */
bool Queue(CardRequest request);

/**
 * @fn void NotifyError(std::string message)
 * @brief Show a five-second error toast and log the reason at warning level.
 * @author Alex (<https://github.com/lextpf>)
 */
void NotifyError(std::string message);

/**
 * @fn bool NeedsSceneCapture()
 * @brief Report whether a queued card still needs its scene copy.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @return False after a successful capture, even while composition is pending.
 */
bool NeedsSceneCapture();

/**
 * @fn bool CaptureScene(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain*
 *     swapChain = nullptr)
 * @brief Capture the portrait source from the backbuffer or bound render target.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Resolves multisampling. sRGB sources use a typeless copy with a UNORM view to avoid a
 * second gamma conversion in the PNG.
 *
 * @param device Non-null D3D11 device.
 * @param context Non-null immediate context.
 * @param swapChain Null or failed backbuffer access falls back to the bound render target.
 * @return True on capture. False on missing work, null arguments, D3D failure, unsupported
 *         target dimension, or multiple RTV array slices. Use NeedsSceneCapture to distinguish
 *         missing work from failure.
 */
bool CaptureScene(ID3D11Device* device,
                  ID3D11DeviceContext* context,
                  IDXGISwapChain* swapChain = nullptr);

/**
 * @fn bool NeedsFrame()
 * @brief Report whether capture work or a visible toast needs a render frame.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Independent of nameplate enable state; includes key edges, readback, and queued encodes.
 */
bool NeedsFrame();

/**
 * @fn void Process(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain* swapChain =
 *     nullptr)
 * @brief Compose the card, poll GPU readback, and queue completed pixels for encoding.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Call after ImGui::Render and before the normal ImGui backbuffer draw. A missing scene copy
 * is captured here after the HUD. Failures drop the queued card and show an error toast.
 * Encoder results drain even with null device/context. Render target, viewport, and modified
 * pipeline state are restored.
 *
 * @param device Device for the card and staging textures.
 * @param context Immediate context for drawing and readback.
 * @param swapChain Late-capture source; null uses the bound render target.
 */
void Process(ID3D11Device* device,
             ID3D11DeviceContext* context,
             IDXGISwapChain* swapChain = nullptr);

/**
 * @fn void DrawNotification()
 * @brief Draw the active status toast at the display's top-right corner.
 * @author Alex (<https://github.com/lextpf>)
 *
 * @pre Call between ImGui::NewFrame and ImGui::Render.
 */
void DrawNotification();

/**
 * @fn void Shutdown()
 * @brief Release device resources and abandon incomplete GPU captures.
 * @author Alex (<https://github.com/lextpf>)
 *
 * The device-creation hook calls this on device or swap-chain changes. Interrupted captures
 * show an error toast; unconsumed key edges survive. This is not a process-exit handler.
 *
 * @warning Queued PNG encodes keep creating directories and writing files after return.
 *          The worker ends during static destruction and drops jobs still waiting then.
 */
void Shutdown();
}  // namespace Deck
