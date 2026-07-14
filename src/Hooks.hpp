#pragma once

#include "PCH.hpp"

/**
 * @namespace Hooks
 * @brief D3D11 initialization and overlay frame hooks.
 * @author Alex (<https://github.com/lextpf>)
 * @ingroup Hooks
 *
 * ### :material-hook: Hook architecture
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef core fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef hook fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef note fill:transparent,stroke:#94a3b8,color:#e2e8f0,stroke-dasharray:6 4
 *     subgraph GE[Game engine]
 *         direction TB
 *         Game[Skyrim engine]:::core
 *     end
 *     subgraph HS[Hook system]
 *         direction TB
 *         Hook[Hooks::Install]:::hook
 *         D3D[CreateD3DAndSwapChain]:::hook
 *         HUD[HUDMenu::PostDisplay]:::hook
 *         PRE[IDXGISwapChain::Present]:::hook
 *     end
 *     subgraph R[Rendering]
 *         direction TB
 *         Boot[ImGui initialization]:::render
 *         Renderer[Renderer::Draw]:::render
 *     end
 *     Game -->|SKSEPlugin_Load| Hook
 *     Hook -->|Install thunk call hook| D3D
 *     Hook -->|Install vtable hook| HUD
 *     Game -->|Create swap chain| D3D
 *     D3D -->|Call original function| D3D
 *     D3D -->|Start init| Boot
 *     HUD -->|Retry init on 5 s backoff| Boot
 *     Boot -->|Install COM vtable hook| PRE
 *     N[Runtime execution]:::note
 *     Boot --- N --- Renderer
 *     subgraph LOOP[Every frame]
 *         direction TB
 *         Game2[Game]:::core -->|PostDisplay| HUD2[HUDMenu::PostDisplay]:::hook
 *         HUD2 -->|Call original function| HUD2
 *         HUD2 -->|Draw overlays| Renderer2[Renderer::Draw]:::render
 *         Game3[Game]:::core -->|Present, PostDisplay skipped| PRE2[PresentHook]:::hook
 *         PRE2 -->|Draw overlays| Renderer2
 *     end
 * ```
 *
 * Only creation and PostDisplay initialize ImGui. Present cannot bootstrap alone.
 * Creation patches CALL rel32 at RELOCATION_ID(75595, 77226) plus SE 0x9/AE 0x275.
 * PostDisplay uses vtable[6]; Present uses COM vtable[8].
 *
 * Initialization installs Present. The creation thunk detects changed D3D pointers
 * and schedules GPU resource rebuilds; swaps that bypass it remain undetected.
 * Frame hooks copy retained COM interfaces under `StateMutex` before using them.
 * Resource rebuilds run before draw commands can retain old font or badge texture IDs.
 *
 * ### :material-hook: Hook flow
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef core fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef render fill:#2e1f5e,stroke:#8b5cf6,color:#e2e8f0
 *     classDef entity fill:#4a3520,stroke:#f59e0b,color:#e2e8f0
 *
 *     A[Game calls hooked function]:::core --> B{Hook installed?}
 *     B -->|No| F[Normal execution]:::core
 *     B -->|Yes| C[Enter thunk]:::entity
 *     C --> P["Pre-pass: PostDisplay and Present only"]:::render
 *     P --> D["Call original: T::func, or the saved Present"]:::core
 *     D --> E["Post-pass: PostDisplay and CreateD3DAndSwapChain only"]:::render
 * ```
 *
 * Creation calls original before initialization. PostDisplay runs pre-pass, original,
 * then overlay; Present runs fallback before original.
 *
 * ### :material-lock-outline: Thread safety
 *
 * Frame hooks run on the render thread; creation runs on the device-creating thread.
 * Use Renderer::IsOverlayAllowedRT for game-state gating. Live actor/cell reads race
 * with streaming teardown.
 *
 * @note SE/AE Steam/GOG only; VR is unsupported.
 * @note Hook failures are logged without preventing plugin loading.
 * @see Stl::WriteThunkCall, Stl::WriteVfunc, Renderer::Draw
 */
namespace Hooks
{
/**
 * @fn void Install()
 * @brief Install the creation and PostDisplay hooks once during plugin loading.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Rejects non-SE/AE runtimes and creation patch sites without opcode 0xE8.
 * One surviving hook can initialize ImGui; both failing leaves the overlay inactive.
 * PostDisplay retries initialization after 5 s; Present never retries it.
 *
 * @pre Address library loaded and SKSE trampoline allocated with sufficient space.
 * @see Renderer::Draw, Renderer::TickRT
 */
void Install();
}  // namespace Hooks
