#include "Hooks.h"

#include "AppearanceTemplate.h"
#include "GameState.h"
#include "ParticleTextures.h"
#include "Renderer.h"
#include "Settings.h"
#include "TextPostProcess.h"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <wrl/client.h>
#include <exception>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Hooks
{
// ============================================================================
// Initialization and per-frame state machine
//
// State is organized into three flag structs accessed via static functions:
//
//   Init()  -- Initialization lifecycle flags
//     initialized, initializing                         (UNINITIALIZED)
//     -> CreateD3DAndSwapChain::thunk CAS initializing=true  (INITIALIZING)
//     -> On success: initialized=true, initializing=false    (INITIALIZED)
//     -> On failure: initializing=false                      (UNINITIALIZED)
//     backendReinitRequested  -- triggers DX11 backend re-init
//     mipmapsGenerated        -- reset to force font atlas rebuild
//     particleTexturesLoaded  -- reset to force texture reload
//
//   Frame() -- Per-frame flags (reset at start of each PostDisplay::thunk call)
//     shouldRenderOverlay      -- set if overlay is allowed this frame
//     overlayRenderedThisFrame -- set after RenderOverlayNow completes
//
//   Diag()  -- Diagnostics
//     renderExceptionCount     -- throttled error logging
//     missingPresentLogged     -- one-shot warning for missing Present
//     deviceChangeLogged       -- one-shot warning for device change
// ============================================================================

struct InitFlags
{
    std::atomic<bool> initialized{false};
    std::atomic<bool> initializing{false};
    std::atomic<bool> mipmapsGenerated{false};
    std::atomic<bool> particleTexturesLoaded{false};
    std::atomic<bool> postProcessInitialized{false};
    std::atomic<bool> backendReinitRequested{false};
};

struct FrameFlags
{
    std::atomic<bool> shouldRenderOverlay{false};
    std::atomic<bool> overlayRenderedThisFrame{false};
};

struct DiagFlags
{
    std::atomic<uint32_t> renderExceptionCount{0};
    std::atomic<bool> missingPresentLogged{false};
    std::atomic<bool> deviceChangeLogged{false};
};

static InitFlags& Init()
{
    static InitFlags f;
    return f;
}
static FrameFlags& Frame()
{
    static FrameFlags f;
    return f;
}
static DiagFlags& Diag()
{
    static DiagFlags f;
    return f;
}
static std::mutex& StateMutex()
{
    static std::mutex instance;
    return instance;
}

// Original Present function pointer
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

// D3D11 device, context, swap chain, and original Present pointer.
// Wrapped in a function-local static to avoid non-trivially destructible
// namespace-scope globals (static destruction order risk on DLL unload).
struct D3DState
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
    std::atomic<PresentFn> originalPresent{nullptr};
};

static D3DState& D3D()
{
    static D3DState s;
    return s;
}

// Forward declaration of Present hook
HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

// Forward declaration of overlay render function (called by PresentHook and PostDisplay::thunk)
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

    // Hold the lock across both the original-pointer store and the vtable
    // write so that concurrent Present calls never observe one without the other.
    const std::lock_guard<std::mutex> lock(StateMutex());
    if (currentPresent == ourPresent)
    {
        return D3D().originalPresent.load(std::memory_order_relaxed) != nullptr;
    }
    D3D().originalPresent.store(currentPresent, std::memory_order_release);

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        logger::error("Hooks: VirtualProtect failed while patching Present vtable slot");
        return false;
    }
    vtable[8] = reinterpret_cast<void*>(&PresentHook);
    VirtualProtect(&vtable[8], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// Detect runtime swap-chain/device changes and refresh cached pointers.
static void HandleDeviceChange()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer || !renderer->data.renderWindows)
    {
        return;
    }

    auto& data = renderer->data;
    auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
    auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
    auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
    bool changed = false;
    if (swapChain && device && context)
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        changed = (swapChain != D3D().swapChain.Get() || device != D3D().device.Get() ||
                   context != D3D().context.Get());
        if (changed)
        {
            D3D().swapChain = swapChain;
            D3D().device = device;
            D3D().context = context;
        }
    }
    if (changed)
    {
        ParticleTextures::Shutdown();
        TextPostProcess::Shutdown();
        Init().mipmapsGenerated.store(false, std::memory_order_release);
        Init().particleTexturesLoaded.store(false, std::memory_order_release);
        Init().postProcessInitialized.store(false, std::memory_order_release);
        Init().backendReinitRequested.store(true, std::memory_order_release);
        if (!TryInstallPresentHook(swapChain))
        {
            logger::error("Hooks: Failed to (re)install Present hook on updated swapchain");
        }
        if (!Diag().deviceChangeLogged.exchange(true, std::memory_order_acq_rel))
        {
            logger::warn(
                "Hooks: Detected renderer device/swapchain change, scheduling backend "
                "refresh");
        }
    }
}

// Perform first-time ImGui initialization: create context, load fonts,
// initialize Win32/DX11 backends, and install the Present hook.
static void InitializeImGui()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return;
    }

    auto& data = renderer->data;
    if (!data.renderWindows)
    {
        return;
    }

    auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
    if (!swapChain)
    {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(std::addressof(desc))))
    {
        return;
    }

    const auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
    const auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);

    if (!device || !context)
    {
        return;
    }

    // Store for later mipmap generation
    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        D3D().device = device;
        D3D().context = context;
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
            D3D().device.Reset();
            D3D().context.Reset();
            D3D().swapChain.Reset();
            D3D().originalPresent.store(nullptr, std::memory_order_release);
        }
        ParticleTextures::Shutdown();
        TextPostProcess::Shutdown();
    };

    ImGui::CreateContext();
    contextCreated = true;

    // Configure ImGui I/O settings
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = false;
    io.IniFilename = nullptr;

    // Load custom fonts for different text elements
    // Character range: Basic Latin + Latin-1 Supplement (0x0020-0x00FF).
    // This covers Western European languages but excludes Cyrillic (0x0400+),
    // CJK (0x4E00+), and other non-Latin scripts.  Actors with localized
    // names in unsupported scripts will display ImGui fallback glyphs.
    // TODO: Consider configurable glyph ranges for broader locale support.
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
    const auto& font = Settings::Font();
    ImFont* nameFont =
        io.Fonts->AddFontFromFileTTF(font.NameFontPath.c_str(), font.NameFontSize, &config, ranges);
    if (!nameFont)
    {
        // Fallback to default font if custom font fails to load
        io.Fonts->AddFontDefault();
    }

    // Font Index 1: Level font
    ImFont* levelFont = io.Fonts->AddFontFromFileTTF(
        font.LevelFontPath.c_str(), font.LevelFontSize, &config, ranges);
    if (!levelFont)
    {
        io.Fonts->AddFontDefault();
    }

    // Font Index 2: Title font
    ImFont* titleFont = io.Fonts->AddFontFromFileTTF(
        font.TitleFontPath.c_str(), font.TitleFontSize, &config, ranges);
    if (!titleFont)
    {
        io.Fonts->AddFontDefault();
    }

    // Font Index 3: Ornament font
    const auto& orn = Settings::Ornament();
    if (!orn.FontPath.empty())
    {
        ImFont* ornamentFont =
            io.Fonts->AddFontFromFileTTF(orn.FontPath.c_str(), orn.FontSize, &config, ranges);
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
        D3D().swapChain = swapChain;
    }

    if (!TryInstallPresentHook(swapChain))
    {
        logger::error("Hooks: Failed to install Present hook");
        cleanupFailedInit();
        return;
    }

    Init().initialized.store(true, std::memory_order_release);
}

// Hook for D3D11 device/swap chain creation
struct CreateD3DAndSwapChain
{
    static void thunk()
    {
        func();

        if (Init().initialized.load(std::memory_order_acquire))
        {
            HandleDeviceChange();
            return;
        }

        // Ensure only one thread attempts initialization at a time.
        bool expected = false;
        if (!Init().initializing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return;
        }
        struct InitScope
        {
            ~InitScope() { Init().initializing.store(false, std::memory_order_release); }
        } _;

        InitializeImGui();
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

// Generate mipmapped font atlas for improved text rendering at various scales.
static void GenerateMipmappedFontAtlas(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (Init().mipmapsGenerated.load(std::memory_order_acquire) || !device || !context)
    {
        return;
    }

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

        Microsoft::WRL::ComPtr<ID3D11Texture2D> fontTexture;
        if (SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, fontTexture.GetAddressOf())))
        {
            context->UpdateSubresource(fontTexture.Get(), 0, nullptr, pixels, width * 4, 0);

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = mipLevels;

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fontSRV;
            if (SUCCEEDED(device->CreateShaderResourceView(
                    fontTexture.Get(), &srvDesc, fontSRV.GetAddressOf())))
            {
                context->GenerateMips(fontSRV.Get());
                if (io.Fonts->TexID)
                {
                    reinterpret_cast<ID3D11ShaderResourceView*>(io.Fonts->TexID)->Release();
                }
                io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(fontSRV.Detach()));
                mipmapsReady = true;
            }
        }
    }
    if (mipmapsReady)
    {
        Init().mipmapsGenerated.store(true, std::memory_order_release);
    }
}

// Load particle textures if enabled and not yet loaded.
static void EnsureParticleTexturesLoaded(ID3D11Device* device, bool useParticleTextures)
{
    if (useParticleTextures && !Init().particleTexturesLoaded.load(std::memory_order_acquire) &&
        device)
    {
        if (ParticleTextures::Initialize(device))
        {
            Init().particleTexturesLoaded.store(true, std::memory_order_release);
        }
    }
}

// Render overlay immediately
void RenderOverlayNow()
{
    if (!Init().initialized.load(std::memory_order_acquire))
    {
        return;
    }
    if (!ImGui::GetCurrentContext())
    {
        return;
    }

    try
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            device = D3D().device;
            context = D3D().context;
        }

        if (Init().backendReinitRequested.exchange(false, std::memory_order_acq_rel))
        {
            if (!device || !context)
            {
                logger::error("Hooks: Backend refresh requested without valid D3D device/context");
                return;
            }
            ImGui_ImplDX11_Shutdown();
            if (!ImGui_ImplDX11_Init(device.Get(), context.Get()))
            {
                logger::error(
                    "Hooks: Failed to reinitialize ImGui DX11 backend after device change");
                Init().initialized.store(false, std::memory_order_release);
                return;
            }
            Init().mipmapsGenerated.store(false, std::memory_order_release);
            Init().particleTexturesLoaded.store(false, std::memory_order_release);
            Init().postProcessInitialized.store(false, std::memory_order_release);
            logger::info("Hooks: Reinitialized ImGui DX11 backend after device change");
        }

        // Start new ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        // Generate mipmapped font atlas on first frame
        GenerateMipmappedFontAtlas(device.Get(), context.Get());

        bool useParticleTextures = false;
        {
            const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
            useParticleTextures = Settings::Particle().UseParticleTextures;
        }

        // Load particle textures on first frame
        EnsureParticleTexturesLoaded(device.Get(), useParticleTextures);

        // Initialize GPU post-processing on first frame
        if (!Init().postProcessInitialized.load(std::memory_order_acquire) && device && context)
        {
            if (TextPostProcess::Initialize(device.Get(), context.Get()))
            {
                Init().postProcessInitialized.store(true, std::memory_order_release);
            }
        }

        // Set display size to actual screen resolution
        {
            const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
            auto& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(screenSize.width);
            io.DisplaySize.y = static_cast<float>(screenSize.height);
            TextPostProcess::OnResize(screenSize.width, screenSize.height);
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
        Frame().overlayRenderedThisFrame.store(true, std::memory_order_release);
    }
    catch (const std::exception& e)
    {
        const uint32_t count =
            Diag().renderExceptionCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count <= 5 || (count % 120) == 0)
        {
            logger::error("Hooks: Exception in RenderOverlayNow (#{}): {}", count, e.what());
        }
    }
    catch (...)
    {
        const uint32_t count =
            Diag().renderExceptionCount.fetch_add(1, std::memory_order_acq_rel) + 1;
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
//
// Recovery limitation: if D3D().originalPresent is null (should not happen in
// normal operation), the recovery path reads vtable[8] from the swapchain.
// Because we already replaced vtable[8] with PresentHook, the candidate
// will equal PresentHook and be rejected.  Recovery can only succeed if a
// *third-party* hook has since overwritten vtable[8] with its own function,
// in which case we adopt that function as "original".  This is correct for
// linear hook chains but would produce incorrect call ordering if the
// third-party hook also saved our PresentHook as *its* original.  In
// practice this edge case is unreachable: D3D().originalPresent is always set
// during TryInstallPresentHook before any Present call can occur.
HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    if (Frame().shouldRenderOverlay.load(std::memory_order_acquire) &&
        !Frame().overlayRenderedThisFrame.load(std::memory_order_acquire))
    {
        RenderOverlayNow();
    }

    // Fast path: atomic load avoids the mutex for the common case.
    PresentFn originalPresent = D3D().originalPresent.load(std::memory_order_acquire);

    if (!originalPresent)
    {
        // Slow path: lock and re-read in case of a concurrent store.
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            originalPresent = D3D().originalPresent.load(std::memory_order_relaxed);
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
                        if (!D3D().originalPresent.load(std::memory_order_relaxed))
                        {
                            D3D().originalPresent.store(candidate, std::memory_order_release);
                        }
                        originalPresent = D3D().originalPresent.load(std::memory_order_relaxed);
                    }
                }
            }

            if (!originalPresent)
            {
                if (!Diag().missingPresentLogged.exchange(true, std::memory_order_acq_rel))
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
        Frame().shouldRenderOverlay.store(false, std::memory_order_release);
        Frame().overlayRenderedThisFrame.store(false, std::memory_order_release);

        // Early exit checks
        if (!Init().initialized.load(std::memory_order_acquire) || !GameState::CanDrawOverlay())
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
        Frame().shouldRenderOverlay.store(shouldRender, std::memory_order_release);

        // Call the original PostDisplay function first
        func(a_menu);

        if (shouldRender && !Frame().overlayRenderedThisFrame.load(std::memory_order_acquire))
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
