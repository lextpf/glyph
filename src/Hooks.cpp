#include "Hooks.h"

#include "AppearanceTemplate.h"
#include "GameState.h"
#include "ParticleTextures.h"
#include "Renderer.h"
#include "Settings.h"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <exception>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Hooks
{
// Atomic flag indicating whether ImGui has been initialized
std::atomic<bool> initialized = false;
std::atomic<bool> initializing = false;

// Flag indicating whether mipmapped font atlas has been created
std::atomic<bool> mipmapsGenerated = false;

// Flag indicating whether particle textures have been loaded
std::atomic<bool> particleTexturesLoaded = false;

// Flag indicating overlay should be rendered this frame
std::atomic<bool> shouldRenderOverlay = false;

// Flag indicating overlay has been rendered this frame
std::atomic<bool> overlayRenderedThisFrame = false;

// Render exception metrics (logged with throttling).
std::atomic<uint32_t> renderExceptionCount = 0;
std::atomic<bool> missingPresentLogged = false;
std::atomic<bool> deviceChangeLogged = false;
std::atomic<bool> backendReinitRequested = false;
static std::mutex& StateMutex()
{
    static std::mutex instance;
    return instance;
}

// Stored D3D11 device for mipmap generation
ID3D11Device* g_device = nullptr;

// Stored D3D11 context for mipmap generation
ID3D11DeviceContext* g_context = nullptr;

// Stored swap chain for Present hook
IDXGISwapChain* g_swapChain = nullptr;

// Original Present function pointer
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
std::atomic<PresentFn> g_originalPresent{nullptr};

// Forward declaration of Present hook
HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

// Forward declaration of overlay render function (used by screenshot hook)
void RenderOverlayNow();

// Installs the Present hook on a specific swapchain and stores original Present.
bool TryInstallPresentHook(IDXGISwapChain* swapChain);

bool TryInstallPresentHook(IDXGISwapChain* swapChain)
{
    if (!swapChain)
    {
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!vtable || !vtable[8])
    {
        return false;
    }

    const auto currentPresent = reinterpret_cast<PresentFn>(vtable[8]);
    const auto ourPresent = reinterpret_cast<PresentFn>(&PresentHook);
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        if (currentPresent == ourPresent)
        {
            return g_originalPresent.load(std::memory_order_relaxed) != nullptr;
        }
        g_originalPresent.store(currentPresent, std::memory_order_release);
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        logger::error("Hooks: VirtualProtect failed while patching Present vtable slot");
        return false;
    }
    vtable[8] = reinterpret_cast<void*>(&PresentHook);
    if (!VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect))
    {
        logger::warn("Hooks: Failed to restore Present vtable memory protection");
    }
    return true;
}

// Hook for D3D11 device/swap chain creation
struct CreateD3DAndSwapChain
{
    static void thunk()
    {
        func();

        if (initialized.load(std::memory_order_acquire))
        {
            // Detect runtime swap-chain/device changes and refresh cached pointers.
            auto renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (renderer && renderer->data.renderWindows)
            {
                auto& data = renderer->data;
                auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
                auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
                auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
                bool changed = false;
                if (swapChain && device && context)
                {
                    const std::lock_guard<std::mutex> lock(StateMutex());
                    changed =
                        (swapChain != g_swapChain || device != g_device || context != g_context);
                    if (changed)
                    {
                        g_swapChain = swapChain;
                        g_device = device;
                        g_context = context;
                    }
                }
                if (changed)
                {
                    ParticleTextures::Shutdown();
                    mipmapsGenerated.store(false, std::memory_order_release);
                    particleTexturesLoaded.store(false, std::memory_order_release);
                    backendReinitRequested.store(true, std::memory_order_release);
                    if (!TryInstallPresentHook(swapChain))
                    {
                        logger::error(
                            "Hooks: Failed to (re)install Present hook on updated swapchain");
                    }
                    if (!deviceChangeLogged.exchange(true, std::memory_order_acq_rel))
                    {
                        logger::warn(
                            "Hooks: Detected renderer device/swapchain change, scheduling backend "
                            "refresh");
                    }
                }
            }
            return;
        }

        // Ensure only one thread attempts initialization at a time.
        bool expected = false;
        if (!initializing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return;
        }
        struct InitScope
        {
            ~InitScope() { initializing.store(false, std::memory_order_release); }
        } _;

        // Get Skyrim's renderer singleton
        auto renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer)
        {
            return;
        }

        // Access the renderer's data which contains D3D objects
        auto& data = renderer->data;
        if (!data.renderWindows)
        {
            return;
        }

        // Get the swap chain from the first render window
        auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
        if (!swapChain)
        {
            return;
        }

        // Retrieve swap chain description to get window handle
        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(swapChain->GetDesc(std::addressof(desc))))
        {
            return;
        }

        // Get D3D11 device and context from Skyrim's renderer
        const auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
        const auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);

        if (!device || !context)
        {
            return;
        }

        // Store for later mipmap generation
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            g_device = device;
            g_context = context;
        }

        bool contextCreated = false;
        bool win32Initialized = false;
        bool dx11Initialized = false;
        auto cleanupFailedInit = [&]()
        {
            if (dx11Initialized)
            {
                ImGui_ImplDX11_Shutdown();
                dx11Initialized = false;
            }
            if (win32Initialized)
            {
                ImGui_ImplWin32_Shutdown();
                win32Initialized = false;
            }
            if (contextCreated)
            {
                ImGui::DestroyContext();
                contextCreated = false;
            }
            {
                const std::lock_guard<std::mutex> lock(StateMutex());
                g_device = nullptr;
                g_context = nullptr;
                g_swapChain = nullptr;
                g_originalPresent.store(nullptr, std::memory_order_release);
            }
            ParticleTextures::Shutdown();
        };

        // Create ImGui context for our overlay
        ImGui::CreateContext();
        contextCreated = true;

        // Configure ImGui I/O settings
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard navigation
        io.MouseDrawCursor = false;                            // Do not draw ImGui cursor
        io.IniFilename = nullptr;                              // Disable imgui.ini file

        // Load custom fonts for different text elements
        // Character range: Basic Latin + Latin-1 Supplement
        static const ImWchar ranges[] = {
            0x0020,
            0x00FF,
            0,
        };
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());

        // Font config: FreeType with light hinting for smooth scaled text
        ImFontConfig config;
        config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        config.OversampleH = 2;  // FreeType + mipmaps make 4x unnecessary
        config.OversampleV = 2;
        config.PixelSnapH = false;  // Disable pixel snapping for smooth subpixel rendering

        // Font Index 0: Name font
        ImFont* nameFont = io.Fonts->AddFontFromFileTTF(
            Settings::NameFontPath.c_str(), Settings::NameFontSize, &config, ranges);
        if (!nameFont)
        {
            // Fallback to default font if custom font fails to load
            io.Fonts->AddFontDefault();
        }

        // Font Index 1: Level font
        ImFont* levelFont = io.Fonts->AddFontFromFileTTF(
            Settings::LevelFontPath.c_str(), Settings::LevelFontSize, &config, ranges);
        if (!levelFont)
        {
            io.Fonts->AddFontDefault();
        }

        // Font Index 2: Title font
        ImFont* titleFont = io.Fonts->AddFontFromFileTTF(
            Settings::TitleFontPath.c_str(), Settings::TitleFontSize, &config, ranges);
        if (!titleFont)
        {
            io.Fonts->AddFontDefault();
        }

        // Font Index 3: Ornament font
        if (!Settings::OrnamentFontPath.empty())
        {
            ImFont* ornamentFont = io.Fonts->AddFontFromFileTTF(
                Settings::OrnamentFontPath.c_str(), Settings::OrnamentFontSize, &config, ranges);
            if (!ornamentFont)
            {
                io.Fonts->AddFontDefault();
            }
        }
        else
        {
            // Add placeholder so font indices stay consistent
            io.Fonts->AddFontDefault();
        }

        // Initialize ImGui backends for Win32 and DirectX 11
        if (!ImGui_ImplWin32_Init(desc.OutputWindow))
        {
            logger::error("Hooks: ImGui Win32 backend initialization failed");
            cleanupFailedInit();
            return;
        }
        win32Initialized = true;
        if (!ImGui_ImplDX11_Init(device, context))
        {
            logger::error("Hooks: ImGui DX11 backend initialization failed");
            cleanupFailedInit();
            return;
        }
        dx11Initialized = true;

        // Store swap chain and hook Present for post-upscaler rendering
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            g_swapChain = swapChain;
        }

        if (!TryInstallPresentHook(swapChain))
        {
            logger::error("Hooks: Failed to install Present hook");
            cleanupFailedInit();
            return;
        }

        // Mark initialization as complete
        initialized.store(true, std::memory_order_release);
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// Render overlay immediately
void RenderOverlayNow()
{
    if (!initialized.load(std::memory_order_acquire))
    {
        return;
    }
    if (!ImGui::GetCurrentContext())
    {
        return;
    }

    try
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            device = g_device;
            context = g_context;
        }

        if (backendReinitRequested.exchange(false, std::memory_order_acq_rel))
        {
            if (!device || !context)
            {
                logger::error("Hooks: Backend refresh requested without valid D3D device/context");
                return;
            }
            ImGui_ImplDX11_Shutdown();
            if (!ImGui_ImplDX11_Init(device, context))
            {
                logger::error(
                    "Hooks: Failed to reinitialize ImGui DX11 backend after device change");
                initialized.store(false, std::memory_order_release);
                return;
            }
            mipmapsGenerated.store(false, std::memory_order_release);
            particleTexturesLoaded.store(false, std::memory_order_release);
            logger::info("Hooks: Reinitialized ImGui DX11 backend after device change");
        }

        // Start new ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        // Generate mipmapped font atlas on first frame
        if (!mipmapsGenerated.load(std::memory_order_acquire) && device && context)
        {
            auto& io = ImGui::GetIO();
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

            bool mipmapsReady = false;
            if (pixels && width > 0 && height > 0)
            {
                int mipLevels = 1;
                int maxDim = (width > height) ? width : height;
                while (maxDim > 1)
                {
                    maxDim >>= 1;
                    mipLevels++;
                }

                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = width;
                texDesc.Height = height;
                texDesc.MipLevels = mipLevels;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

                ID3D11Texture2D* fontTexture = nullptr;
                if (SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, &fontTexture)))
                {
                    context->UpdateSubresource(fontTexture, 0, nullptr, pixels, width * 4, 0);

                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = mipLevels;

                    ID3D11ShaderResourceView* fontSRV = nullptr;
                    if (SUCCEEDED(
                            device->CreateShaderResourceView(fontTexture, &srvDesc, &fontSRV)))
                    {
                        context->GenerateMips(fontSRV);
                        if (io.Fonts->TexID)
                        {
                            ((ID3D11ShaderResourceView*)io.Fonts->TexID)->Release();
                        }
                        io.Fonts->SetTexID((ImTextureID)fontSRV);
                        mipmapsReady = true;
                    }
                    fontTexture->Release();
                }
            }
            if (mipmapsReady)
            {
                mipmapsGenerated.store(true, std::memory_order_release);
            }
        }

        bool useParticleTextures = false;
        {
            const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
            useParticleTextures = Settings::UseParticleTextures;
        }

        // Load particle textures on first frame
        if (useParticleTextures && !particleTexturesLoaded.load(std::memory_order_acquire) &&
            device)
        {
            if (ParticleTextures::Initialize(device))
            {
                particleTexturesLoaded.store(true, std::memory_order_release);
            }
        }

        // Set display size to actual screen resolution
        {
            const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
            auto& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(screenSize.width);
            io.DisplaySize.y = static_cast<float>(screenSize.height);
        }

        ImGui::NewFrame();

        // Disable nav system
        if (auto g = ImGui::GetCurrentContext())
        {
            g->NavWindowingTarget = nullptr;
        }

        // Draw overlay
        Renderer::Draw();

        // Finalize and render
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Mark that overlay was rendered this frame
        overlayRenderedThisFrame.store(true, std::memory_order_release);
    }
    catch (const std::exception& e)
    {
        const uint32_t count = renderExceptionCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count <= 5 || (count % 120) == 0)
        {
            logger::error("Hooks: Exception in RenderOverlayNow (#{}): {}", count, e.what());
        }
    }
    catch (...)
    {
        const uint32_t count = renderExceptionCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count <= 5 || (count % 120) == 0)
        {
            logger::error("Hooks: Unknown exception in RenderOverlayNow (#{}).", count);
        }
    }
}

// Present hook, safety net for overlay rendering.
// Some upscalers (DLSS, FSR, etc.) restructure the rendering pipeline in
// ways that can cause PostDisplay to be skipped or deferred.
// This hook catches that case, we render it here as a last resort,
// right before the original Present flips the backbuffer to screen.
HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    if (shouldRenderOverlay.load(std::memory_order_acquire) &&
        !overlayRenderedThisFrame.load(std::memory_order_acquire))
    {
        RenderOverlayNow();
    }

    // Fast path: atomic load avoids the mutex for the common case.
    PresentFn originalPresent = g_originalPresent.load(std::memory_order_acquire);

    if (!originalPresent)
    {
        // Slow path: lock and re-read in case of a concurrent store.
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            originalPresent = g_originalPresent.load(std::memory_order_relaxed);
        }

        if (!originalPresent)
        {
            // Attempt on-the-fly recovery from the swapchain vtable.
            if (swapChain)
            {
                void** vtable = *reinterpret_cast<void***>(swapChain);
                if (vtable && vtable[8])
                {
                    auto candidate = reinterpret_cast<PresentFn>(vtable[8]);
                    if (candidate && candidate != reinterpret_cast<PresentFn>(&PresentHook))
                    {
                        const std::lock_guard<std::mutex> lock(StateMutex());
                        if (!g_originalPresent.load(std::memory_order_relaxed))
                        {
                            g_originalPresent.store(candidate, std::memory_order_release);
                        }
                        originalPresent = g_originalPresent.load(std::memory_order_relaxed);
                    }
                }
            }

            if (!originalPresent)
            {
                if (!missingPresentLogged.exchange(true, std::memory_order_acq_rel))
                {
                    logger::error(
                        "Hooks: Missing original IDXGISwapChain::Present pointer, returning "
                        "success to avoid frame hard-fail");
                }
                return S_OK;
            }
        }
    }

    // Forward to the real IDXGISwapChain::Present we saved during init.
    return originalPresent(swapChain, syncInterval, flags);
}

// VTable hook for `HUDMenu::PostDisplay` (vtable index 6).
//
// Intercepts the game's HUD post-display call each frame to inject
// overlay rendering. The hook executes on the render thread and
// performs the following sequence:
//
// 1. Resets per-frame render flags
// 2. Validates ImGui initialization and menu state
// 3. Updates render-thread state via Renderer::TickRT()
// 4. Checks for pending appearance template changes
// 5. Calls the original `PostDisplay` so the game HUD renders first
// 6. Renders the overlay on top (if allowed)
//
// Rendering after the original function ensures the overlay appears
// above the HUD and is captured in screenshots.
//
// Executes on the render thread every frame.
// Installed via `Stl::WriteVfunc<RE::HUDMenu, PostDisplay>()`.
//
// See: Renderer::TickRT, Renderer::IsOverlayAllowedRT, Renderer::Draw
// See: PresentHook for the fallback path when upscalers skip PostDisplay
struct PostDisplay
{
    // Hook thunk called in place of the original `HUDMenu::PostDisplay`.
    // a_menu: Pointer to the HUD menu instance.
    static void thunk(RE::IMenu* a_menu)
    {
        // Reset render flags at start of frame
        shouldRenderOverlay.store(false, std::memory_order_release);
        overlayRenderedThisFrame.store(false, std::memory_order_release);

        // Early exit checks
        if (!initialized.load(std::memory_order_acquire) || !CanDrawOverlay())
        {
            func(a_menu);
            return;
        }

        // Verify menu is valid and visible
        if (!a_menu || !a_menu->uiMovie || !a_menu->uiMovie->GetVisible())
        {
            func(a_menu);
            return;
        }

        // Update render thread state to queue actor data updates
        Renderer::TickRT();

        // Check if we need to apply appearance template
        AppearanceTemplate::CheckPendingAppearanceTemplate();

        // Check if overlay should be rendered
        bool shouldRender = Renderer::IsOverlayAllowedRT();
        shouldRenderOverlay.store(shouldRender, std::memory_order_release);

        // Call the original PostDisplay function first
        func(a_menu);

        if (shouldRender && !overlayRenderedThisFrame.load(std::memory_order_acquire))
        {
            RenderOverlayNow();
        }
    }

    // Original function pointer
    static inline REL::Relocation<decltype(thunk)> func;
    // Virtual function table index for PostDisplay
    static inline std::size_t idx = 0x6;
};

void Install()
{
    bool d3dHookInstalled = false;
    bool hudHookInstalled = false;

    try
    {
        // Hook D3D11 device creation for ImGui initialization
        REL::Relocation<std::uintptr_t> target{RELOCATION_ID(75595, 77226),
                                               GLYPH_OFFSET(0x9, 0x275)};
        Stl::WriteThunkCall<CreateD3DAndSwapChain>(target.address());
        d3dHookInstalled = true;
    }
    catch (const std::exception& e)
    {
        SKSE::log::error("Hooks: Failed to install CreateD3DAndSwapChain hook: {}", e.what());
    }
    catch (...)
    {
        SKSE::log::error("Hooks: Failed to install CreateD3DAndSwapChain hook (unknown error)");
    }

    try
    {
        // Hook HUD post-display, renders overlay after game HUD is complete
        Stl::WriteVfunc<RE::HUDMenu, PostDisplay>();
        hudHookInstalled = true;
    }
    catch (const std::exception& e)
    {
        SKSE::log::error("Hooks: Failed to install HUDMenu::PostDisplay hook: {}", e.what());
    }
    catch (...)
    {
        SKSE::log::error("Hooks: Failed to install HUDMenu::PostDisplay hook (unknown error)");
    }

    if (d3dHookInstalled && hudHookInstalled)
    {
        SKSE::log::info("Hooks: Installed");
    }
    else
    {
        SKSE::log::warn("Hooks: Partial install (CreateD3DAndSwapChain={}, HUDPostDisplay={})",
                        d3dHookInstalled,
                        hudHookInstalled);
    }
}
}  // namespace Hooks
