#include "Hooks.hpp"

#include "ActorOverrides.hpp"
#include "BadgeTextures.hpp"
#include "Deck.hpp"
#include "DepthClip.hpp"
#include "GameState.hpp"
#include "Graffito.hpp"
#include "ParticleTextures.hpp"
#include "ProjectManifest.hpp"
#include "RasterQuality.hpp"
#include "Renderer.hpp"
#include "RenderSampling.hpp"
#include "SceneMeter.hpp"
#include "Settings.hpp"
#include "TextPostProcess.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui_freetype.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <wrl/client.h>
#include <chrono>
#include <exception>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Hooks
{

// State accessors: Init owns resource/reinit flags; Frame owns draw markers;
// Diag owns log counters; D3D owns cached pointers and the mipmapped atlas.
// StateMutex guards device/context/swapchain, originalPresent is atomic, and
// fontAtlasSRV is render-thread only. Clear loaded flags or set
// backendReinitRequested to rebuild on the next frame.

/**
 * @struct InitFlags
 * @brief Atomic initialization gates and pending GPU resource refreshes.
 * @author Alex (<https://github.com/lextpf>)
 */
struct InitFlags
{
    std::atomic<bool> initialized{false};
    std::atomic<bool> initializing{false};
    std::atomic<std::uint64_t> nextInitRetryAtMs{0};
    std::atomic<bool> mipmapsGenerated{false};
    std::atomic<bool> particleTexturesLoaded{false};
    std::atomic<bool> badgeTexturesLoaded{false};
    std::atomic<uint32_t> badgeTexturesGen{0};
    std::atomic<uint32_t> badgeOverrideIconVersion{0};
    std::atomic<bool> postProcessInitialized{false};
    std::atomic<std::uint64_t> nextGraffitoRetryAtMs{0};
    std::atomic<bool> backendReinitRequested{false};
};

/**
 * @struct FrameFlags
 * @brief Frame markers shared by PostDisplay and Present fallback drawing.
 * @author Alex (<https://github.com/lextpf>)
 */
struct FrameFlags
{
    // Recorded gate decision; no reader currently consumes it.
    std::atomic<bool> shouldRenderOverlay{false};
    // Marks a completed overlay frame so PostDisplay and Present cannot draw twice.
    std::atomic<bool> overlayRenderedThisFrame{false};
};

/**
 * @struct DiagFlags
 * @brief Counters and one-shot flags that bound repeated hook diagnostics.
 * @author Alex (<https://github.com/lextpf>)
 */
struct DiagFlags
{
    std::atomic<uint32_t> renderExceptionCount{0};
    std::atomic<bool> missingPresentLogged{false};
    std::atomic<bool> deviceChangeLogged{false};
    std::atomic<bool> imguiInitializedLogged{false};
    std::atomic<bool> firstPostDisplayLogged{false};
    std::atomic<bool> presentBootstrapLogged{false};
};

/**
 * @fn static InitFlags& Init()
 * @brief Access initialization and GPU rebuild flags with process lifetime.
 * @author Alex (<https://github.com/lextpf>)
 */
static InitFlags& Init()
{
    static InitFlags f;
    return f;
}
/**
 * @fn static FrameFlags& Frame()
 * @brief Access markers that coordinate the two overlay frame hooks.
 * @author Alex (<https://github.com/lextpf>)
 */
static FrameFlags& Frame()
{
    static FrameFlags f;
    return f;
}
/**
 * @fn static DiagFlags& Diag()
 * @brief Access counters that limit repeated hook diagnostics.
 * @author Alex (<https://github.com/lextpf>)
 */
static DiagFlags& Diag()
{
    static DiagFlags f;
    return f;
}
/**
 * @fn static std::mutex& StateMutex()
 * @brief Access the mutex that guards cached D3D interfaces and Present installation.
 * @author Alex (<https://github.com/lextpf>)
 */
static std::mutex& StateMutex()
{
    static std::mutex instance;
    return instance;
}

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

/**
 * @struct D3DState
 * @brief Retained device interfaces and the independently owned font texture.
 * @author Alex (<https://github.com/lextpf>)
 */
struct D3DState
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
    std::atomic<PresentFn> originalPresent{nullptr};
    // The plugin owns this atlas; ImGui owns and frees its separate backend texture.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fontAtlasSRV;
};

/**
 * @fn static D3DState& D3D()
 * @brief Access retained D3D interfaces and the owned mipmapped font atlas.
 * @author Alex (<https://github.com/lextpf>)
 */
static D3DState& D3D()
{
    static D3DState s;
    return s;
}

/**
 * @fn HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
 * @brief Draw a fallback overlay frame before forwarding Present.
 * @author Alex (<https://github.com/lextpf>)
 */
HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);

/**
 * @fn void RenderOverlayNow()
 * @brief Refresh GPU resources and submit one ImGui overlay frame.
 * @author Alex (<https://github.com/lextpf>)
 */
void RenderOverlayNow();

/**
 * @fn bool TryInstallPresentHook(IDXGISwapChain* swapChain)
 * @brief Patch the swap-chain Present slot while retaining its current target.
 * @author Alex (<https://github.com/lextpf>)
 */
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

    // Publish originalPresent and the vtable patch under one lock.
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

/**
 * @fn static void HandleDeviceChange()
 * @brief Release device resources and schedule their recreation when D3D changes.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Schedule GPU rebuilds and rehook Present when cached D3D pointers change.
 * Only the creation thunk detects changes; swaps that bypass it remain unseen.
 */
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
        BadgeTextures::Shutdown();
        Deck::Shutdown();
        TextPostProcess::Shutdown();
        SceneMeter::Shutdown();
        DepthClip::Shutdown();
        Graffito::Shutdown();
        RenderSampling::Shutdown();
        Init().mipmapsGenerated.store(false, std::memory_order_release);
        Init().particleTexturesLoaded.store(false, std::memory_order_release);
        Init().badgeTexturesLoaded.store(false, std::memory_order_release);
        Init().postProcessInitialized.store(false, std::memory_order_release);
        Init().nextGraffitoRetryAtMs.store(0, std::memory_order_release);
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

/**
 * @fn static bool BuildFontAtlas(float density)
 * @brief Build fixed font slots at the requested raster density.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Caller holds a shared Settings::Mutex lock. RasterizerDensity changes sampling,
 * not the configured font size.
 */
static bool BuildFontAtlas(float density)
{
    auto& atlas = *ImGui::GetIO().Fonts;
    atlas.Clear();
    atlas.TexGlyphPadding = RasterQuality::FONT_GLYPH_PADDING;

    // Only U+0020-U+00FF are loaded; Cyrillic/CJK use the fallback glyph.
    // TODO: allow configured glyph ranges for broader locale support.
    static const ImWchar ranges[] = {
        0x0020,
        0x00FF,
        0,
    };

    ImFontConfig config;
    config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
    config.OversampleH = 2;  // FreeType plus mipmaps make 4x unnecessary
    config.OversampleV = 2;
    config.PixelSnapH = false;  // Off, so glyphs keep subpixel positions
    config.RasterizerDensity = density;
    config.RasterizerMultiply = 1.15f;  // Give title, name, and level strokes more weight

    // Manifest font paths take precedence over INI paths.
    const auto fontPath = [](const std::string& mapped,
                             const std::string& ini) -> const std::string&
    { return mapped.empty() ? ini : mapped; };

    const auto addFontSlot = [&](const std::string& path, float size) -> bool
    {
        if (!path.empty() &&
            atlas.AddFontFromFileTTF(path.c_str(), size, &config, ranges) != nullptr)
        {
            return true;
        }
        return atlas.AddFontDefault(&config) != nullptr;
    };

    // Load order fixes renderer font indices; missing assets use one fallback per slot.
    const auto& font = Settings::Font();
    bool slotsReady =
        addFontSlot(fontPath(ProjectManifest::FontName(), font.NameFontPath), font.NameFontSize);
    slotsReady = addFontSlot(fontPath(ProjectManifest::FontLevel(), font.LevelFontPath),
                             font.LevelFontSize) &&
                 slotsReady;
    slotsReady = addFontSlot(fontPath(ProjectManifest::FontTitle(), font.TitleFontPath),
                             font.TitleFontSize) &&
                 slotsReady;

    config.RasterizerMultiply = 1.0f;  // Preserve the ornament font's authored weight.
    const auto& ornament = Settings::Ornament();
    slotsReady = addFontSlot(fontPath(ProjectManifest::FontOrnament(), ornament.FontPath),
                             ornament.FontSize) &&
                 slotsReady;

    return slotsReady && atlas.Build();
}

/**
 * @fn static bool FontAtlasFitsD3D11()
 * @brief Check that the built font atlas fits D3D11 texture limits.
 * @author Alex (<https://github.com/lextpf>)
 */
static bool FontAtlasFitsD3D11()
{
    const auto& atlas = *ImGui::GetIO().Fonts;
    return atlas.TexWidth > 0 && atlas.TexHeight > 0 &&
           atlas.TexWidth <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           atlas.TexHeight <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

/**
 * @fn static bool InitializeImGui()
 * @brief Create the ImGui backends and install the Present fallback.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Failed initialization unwinds created resources so later frames can retry.
 */
static bool InitializeImGui()
{
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return false;
    }

    auto& data = renderer->data;
    if (!data.renderWindows)
    {
        return false;
    }

    auto swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
    if (!swapChain)
    {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(std::addressof(desc))))
    {
        return false;
    }

    const auto device = reinterpret_cast<ID3D11Device*>(data.forwarder);
    const auto context = reinterpret_cast<ID3D11DeviceContext*>(data.context);

    if (!device || !context)
    {
        return false;
    }

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
        SceneMeter::Shutdown();
        DepthClip::Shutdown();
        Graffito::Shutdown();
        RenderSampling::Shutdown();
    };

    ImGui::CreateContext();
    contextCreated = true;

    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = false;
    io.IniFilename = nullptr;

    const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
    float atlasDensity = RasterQuality::FONT_DENSITY;
    bool atlasReady = BuildFontAtlas(RasterQuality::FONT_DENSITY);
    if (!atlasReady || !FontAtlasFitsD3D11())
    {
        logger::warn(
            "Hooks: {:.1f}x font atlas build produced {}x{} pixels or failed; rebuilding "
            "at {:.1f}x",
            RasterQuality::FONT_DENSITY,
            io.Fonts->TexWidth,
            io.Fonts->TexHeight,
            RasterQuality::FONT_FALLBACK_DENSITY);
        atlasReady = BuildFontAtlas(RasterQuality::FONT_FALLBACK_DENSITY);
        if (!atlasReady || !FontAtlasFitsD3D11())
        {
            logger::error(
                "Hooks: fallback font atlas build failed or exceeded D3D11 limits "
                "({}x{})",
                io.Fonts->TexWidth,
                io.Fonts->TexHeight);
            cleanupFailedInit();
            return false;
        }
        atlasDensity = RasterQuality::FONT_FALLBACK_DENSITY;
    }
    logger::info("Hooks: built font atlas at {:.1f}x ({}x{}, {} px glyph padding)",
                 atlasDensity,
                 io.Fonts->TexWidth,
                 io.Fonts->TexHeight,
                 io.Fonts->TexGlyphPadding);

    if (!ImGui_ImplWin32_Init(desc.OutputWindow))
    {
        logger::error("Hooks: ImGui Win32 backend initialization failed");
        cleanupFailedInit();
        return false;
    }
    win32Initialized = true;
    if (!ImGui_ImplDX11_Init(device, context))
    {
        logger::error("Hooks: ImGui DX11 backend initialization failed");
        cleanupFailedInit();
        return false;
    }
    dx11Initialized = true;

    {
        const std::lock_guard<std::mutex> lock(StateMutex());
        D3D().swapChain = swapChain;
    }

    if (!TryInstallPresentHook(swapChain))
    {
        logger::error("Hooks: Failed to install Present hook");
        cleanupFailedInit();
        return false;
    }

    Init().initialized.store(true, std::memory_order_release);
    Init().nextInitRetryAtMs.store(0, std::memory_order_release);
    if (!Diag().imguiInitializedLogged.exchange(true, std::memory_order_acq_rel))
    {
        logger::info("Hooks: ImGui/DX11 initialized");
    }
    return true;
}

/**
 * @fn static void EnsureOverlayInitialized()
 * @brief Attempt initialization with a shared guard and retry delay.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Bootstrap from creation or PostDisplay; Present never initializes.
 * Initialized uses acquire-load; failed attempts wait 5 s. A CAS guard lets competing
 * calls return without blocking on ImGui/D3D initialization. Failed DX11 backend
 * reinit clears initialized so bootstrap can retry.
 */
static void EnsureOverlayInitialized()
{
    static constexpr std::uint64_t INIT_RETRY_INTERVAL_MS = 5000;
    const auto nowMs =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());

    if (Init().initialized.load(std::memory_order_acquire))
    {
        return;
    }

    const auto nextRetryAt = Init().nextInitRetryAtMs.load(std::memory_order_acquire);
    if (nextRetryAt != 0 && nowMs < nextRetryAt)
    {
        return;
    }

    bool expected = false;
    if (!Init().initializing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }
    struct InitScope
    {
        /**
         * @fn ~InitScope()
         * @brief Release the initialization guard on every exit.
         * @author Alex (<https://github.com/lextpf>)
         */
        ~InitScope() { Init().initializing.store(false, std::memory_order_release); }
    } _;

    if (!InitializeImGui())
    {
        Init().nextInitRetryAtMs.store(nowMs + INIT_RETRY_INTERVAL_MS, std::memory_order_release);
    }
}

/**
 * @struct CreateD3DAndSwapChain
 * @brief Device-creation hook that initializes or refreshes overlay resources.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Run original creation first, then initialize or detect a device change.
 */
struct CreateD3DAndSwapChain
{
    /**
     * @fn static void thunk()
     * @brief Forward device creation and initialize or refresh overlay resources.
     * @author Alex (<https://github.com/lextpf>)
     */
    static void thunk()
    {
        func();

        if (Init().initialized.load(std::memory_order_acquire))
        {
            HandleDeviceChange();
            return;
        }

        EnsureOverlayInitialized();
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

/**
 * @fn static void GenerateMipmappedFontAtlas(ID3D11Device* device, ID3D11DeviceContext* context)
 * @brief Replace the font texture with an owned mipmapped atlas.
 * @author Alex (<https://github.com/lextpf>)
 *
 * One mipmapped atlas per device keeps downscaled text clean.
 */
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
                // ImGui still owns bd->pFontTextureView. Releasing it here would cause a
                // double-free at backend invalidation. The plugin owns the replacement ComPtr.
                D3D().fontAtlasSRV = fontSRV;
                io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(fontSRV.Get()));
                mipmapsReady = true;
            }
        }
    }
    if (mipmapsReady)
    {
        Init().mipmapsGenerated.store(true, std::memory_order_release);
    }
}

/**
 * @fn static void EnsureParticleTexturesLoaded(ID3D11Device* device, bool useParticleTextures)
 * @brief Load enabled particle textures before recording draw commands.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Disabling particles retains textures. Device changes or failed ImGui init
 * release them; failed texture creation retries on the next frame.
 */
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

/**
 * @fn static void EnsureBadgeTexturesLoaded(ID3D11Device* device)
 * @brief Refresh badge resources before recording texture IDs in draw lists.
 * @author Alex (<https://github.com/lextpf>)
 *
 * Settings generation changes rebuild the SVG cache; icon override versions only
 * add names. Run before any BadgeTextures::Get: rebuilding releases texture IDs
 * that pending draws may reference.
 */
static void EnsureBadgeTexturesLoaded(ID3D11Device* device)
{
    if (!device)
    {
        return;
    }
    const uint32_t gen = Settings::Generation().load(std::memory_order_acquire);
    const uint32_t iconVersion = ActorOverrides::IconSetVersion();
    const bool fullRebuild = !Init().badgeTexturesLoaded.load(std::memory_order_acquire) ||
                             Init().badgeTexturesGen.load(std::memory_order_acquire) != gen;
    if (!fullRebuild &&
        Init().badgeOverrideIconVersion.load(std::memory_order_acquire) == iconVersion)
    {
        return;
    }

    bool enabled = false;
    bool tierImages = false;
    std::string folder;
    std::vector<std::string> names;
    {
        const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
        const auto& ic = Settings::Icons();
        enabled = ic.Enabled && !ic.Folder.empty();
        folder = ic.Folder;
        tierImages = ic.TierBadgeImages;
        names = {ic.FollowerIcon,    ic.AllyIcon,       ic.HostileIcon,      ic.WeakIcon,
                 ic.StrongIcon,      ic.DeadlyIcon,     ic.BeastIcon,        ic.UndeadIcon,
                 ic.DaedraIcon,      ic.DragonIcon,     ic.NeutralIcon,      ic.HumanoidIcon,
                 ic.EvenIcon,        ic.GuardIcon,      ic.MerchantIcon,     ic.CommonerIcon,
                 ic.EssentialIcon,   ic.ProtectedIcon,  ic.MortalIcon,       ic.CombatIcon,
                 ic.AlertIcon,       ic.IdleIcon,       ic.SneakHiddenIcon,  ic.SneakDetectedIcon,
                 ic.SneakOffIcon,    ic.EncumberedIcon, ic.NormalWeightIcon, ic.WantedIcon,
                 ic.BountyClearIcon, ic.TierLowIcon,    ic.TierMidIcon,      ic.TierHighIcon};
    }

    // Take the leaf override lock after releasing Settings::Mutex. Sample version
    // before copying names so concurrent additions force another refresh.
    const std::vector<std::string> overrideNames = ActorOverrides::IconNames();

    if (fullRebuild)
    {
        if (enabled)
        {
            names.insert(names.end(), overrideNames.begin(), overrideNames.end());
            BadgeTextures::Initialize(device, folder, names);
        }
        else
        {
            BadgeTextures::Shutdown();
        }
        // Manifest rank order selects emblems; an empty list clears them.
        static const std::vector<std::string> kNoBadges{};
        BadgeTextures::InitializeTierImages(
            device, (enabled && tierImages) ? ProjectManifest::TierBadges() : kNoBadges);
    }
    else if (enabled)
    {
        BadgeTextures::AddIcons(device, folder, overrideNames);
    }

    // Folder changes can invalidate accepted override names; log each dropped icon.
    if (enabled)
    {
        for (const auto& name : overrideNames)
        {
            if (BadgeTextures::Get(name) == 0)
            {
                logger::warn(
                    "Badges: console override icon '{}' did not load from '{}'", name, folder);
            }
        }
    }
    Init().badgeTexturesGen.store(gen, std::memory_order_release);
    Init().badgeOverrideIconVersion.store(iconVersion, std::memory_order_release);
    Init().badgeTexturesLoaded.store(true, std::memory_order_release);
}

// Skip without ImGui context. Log caught exceptions for the first five failures,
// then every 120th.
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
        Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            device = D3D().device;
            context = D3D().context;
            swapChain = D3D().swapChain;
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
            Init().nextGraffitoRetryAtMs.store(0, std::memory_order_release);
            logger::info("Hooks: Reinitialized ImGui DX11 backend after device change");
        }

        // Sampler initialization is once per device; failure preserves ImGui sampling.
        RenderSampling::Initialize(device.Get(), context.Get());

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        GenerateMipmappedFontAtlas(device.Get(), context.Get());

        bool useParticleTextures = false;
        {
            const std::shared_lock<std::shared_mutex> settingsReadLock(Settings::Mutex());
            useParticleTextures = Settings::Particle().UseParticleTextures;
        }

        EnsureParticleTexturesLoaded(device.Get(), useParticleTextures);

        EnsureBadgeTexturesLoaded(device.Get());

        // SceneMeter and DepthClip retry with the TextPostProcess gate; each failure is a no-op.
        if (!Init().postProcessInitialized.load(std::memory_order_acquire) && device && context)
        {
            if (TextPostProcess::Initialize(device.Get(), context.Get()))
            {
                Init().postProcessInitialized.store(true, std::memory_order_release);
            }
            SceneMeter::Initialize(device.Get(), context.Get());
            DepthClip::Initialize(device.Get(), context.Get());
        }

        // Retry Graffito independently on a slow cadence after transient shader failure.
        if (!Graffito::IsInitialized() && device && context)
        {
            static constexpr std::uint64_t GRAFFITO_RETRY_INTERVAL_MS = 5000;
            const auto nowMs =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
            const auto nextRetryAt = Init().nextGraffitoRetryAtMs.load(std::memory_order_acquire);
            if (nextRetryAt == 0 || nowMs >= nextRetryAt)
            {
                if (Graffito::Initialize(device.Get(), context.Get()))
                {
                    Init().nextGraffitoRetryAtMs.store(0, std::memory_order_release);
                }
                else
                {
                    Init().nextGraffitoRetryAtMs.store(nowMs + GRAFFITO_RETRY_INTERVAL_MS,
                                                       std::memory_order_release);
                }
            }
        }

        {
            const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
            auto& io = ImGui::GetIO();
            io.DisplaySize.x = static_cast<float>(screenSize.width);
            io.DisplaySize.y = static_cast<float>(screenSize.height);
            TextPostProcess::OnResize(screenSize.width, screenSize.height);
            SceneMeter::OnResize(screenSize.width, screenSize.height);
        }

        ImGui::NewFrame();

        // The overlay must not take keyboard navigation.
        if (auto g = ImGui::GetCurrentContext())
        {
            g->NavWindowingTarget = nullptr;
        }

        // Deck status needs its own frame gate; it must not enable ambient plates.
        if (Renderer::IsOverlayAllowedRT())
        {
            Renderer::Draw();
        }
        Deck::DrawNotification();

        ImGui::EndFrame();
        ImGui::Render();
        // Compose before frame submission. Earlier hooks supply a pre-HUD scene;
        // Deck::Process falls back to the current post-HUD target when no copy exists.
        Deck::Process(device.Get(), context.Get(), swapChain.Get());
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

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

// Fallback draws before Present when PostDisplay was skipped.
// If originalPresent is missing, only a third-party vtable replacement can be
// adopted; it must not chain back to PresentHook. Normal installation stores the
// original before publishing the hook.
HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
    // consume/reset the frame marker so Present-only pipelines render every frame.
    const bool renderedByPostDisplay =
        Frame().overlayRenderedThisFrame.exchange(false, std::memory_order_acq_rel);
    if (!renderedByPostDisplay)
    {
        // Tick hidden frames to publish gates; CanDrawOverlay reads game-thread cell state.
        Renderer::TickRT();
        Renderer::PrepareDeckCaptureRT();

        if (Deck::NeedsSceneCapture())
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            {
                const std::lock_guard<std::mutex> lock(StateMutex());
                device = D3D().device;
                context = D3D().context;
            }
            Deck::CaptureScene(device.Get(), context.Get(), swapChain);
        }

        const bool shouldRender = Init().initialized.load(std::memory_order_acquire) &&
                                  (Renderer::IsOverlayAllowedRT() || Deck::NeedsFrame());
        Frame().shouldRenderOverlay.store(shouldRender, std::memory_order_release);

        if (shouldRender &&
            !Diag().presentBootstrapLogged.exchange(true, std::memory_order_acq_rel))
        {
            logger::info("Hooks: Present fallback bootstrapped overlay rendering");
        }
        if (shouldRender)
        {
            RenderOverlayNow();
            Frame().overlayRenderedThisFrame.store(false, std::memory_order_release);
        }
    }
    Frame().shouldRenderOverlay.store(false, std::memory_order_release);

    PresentFn originalPresent = D3D().originalPresent.load(std::memory_order_acquire);

    if (!originalPresent)
    {
        // Recheck under the lock after a concurrent store.
        {
            const std::lock_guard<std::mutex> lock(StateMutex());
            originalPresent = D3D().originalPresent.load(std::memory_order_relaxed);
        }

        if (!originalPresent)
        {
            if (swapChain)
            {
                void** vtable = *reinterpret_cast<void***>(swapChain);
                if (vtable && vtable[8])
                {
                    auto candidate = reinterpret_cast<PresentFn>(vtable[8]);
                    if (candidate != reinterpret_cast<PresentFn>(&PresentHook))
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

    return originalPresent(swapChain, syncInterval, flags);
}

/**
 * @struct PostDisplay
 * @brief HUD hook that captures the scene before drawing the overlay.
 * @author Alex (<https://github.com/lextpf>)
 *
 * PostDisplay draws the overlay after the HUD, including screenshot frames.
 */
struct PostDisplay
{
    /**
     * @fn static void thunk(RE::IMenu* a_menu)
     * @brief Capture the scene before HUD drawing and submit the overlay afterward.
     * @author Alex (<https://github.com/lextpf>)
     *
     * a_menu may be null.
     */
    static void thunk(RE::IMenu* a_menu)
    {
        Frame().shouldRenderOverlay.store(false, std::memory_order_release);
        Frame().overlayRenderedThisFrame.store(false, std::memory_order_release);

        if (!Diag().firstPostDisplayLogged.exchange(true, std::memory_order_acq_rel))
        {
            logger::debug("Hooks: HUDMenu::PostDisplay hit");
        }

        EnsureOverlayInitialized();

        // Queue snapshots even while hidden; game-state reads can race with cell teardown.
        Renderer::TickRT();

        if (!Init().initialized.load(std::memory_order_acquire))
        {
            func(a_menu);
            return;
        }

        // Screenshots hide the HUD movie; require its existence, not visibility.
        if (!a_menu || !a_menu->uiMovie)
        {
            func(a_menu);
            return;
        }

        // Capture before HUD crosshair/meters; failure falls back to post-HUD in Process.
        Renderer::PrepareDeckCaptureRT();
        if (Deck::NeedsSceneCapture())
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
            {
                const std::lock_guard<std::mutex> lock(StateMutex());
                device = D3D().device;
                context = D3D().context;
                swapChain = D3D().swapChain;
            }
            Deck::CaptureScene(device.Get(), context.Get(), swapChain.Get());
        }

        bool shouldRender = Renderer::IsOverlayAllowedRT() || Deck::NeedsFrame();
        Frame().shouldRenderOverlay.store(shouldRender, std::memory_order_release);

        func(a_menu);

        if (shouldRender && !Frame().overlayRenderedThisFrame.load(std::memory_order_acquire))
        {
            RenderOverlayNow();
        }
    }

    static inline REL::Relocation<decltype(thunk)> func;
    static inline std::size_t idx = 0x6;
};

void Install()
{
    // Guard patch sites even when called outside normal SE/AE plugin loading.
    if (!REL::Module::IsSE() && !REL::Module::IsAE())
    {
        SKSE::log::error("Hooks: Unsupported Skyrim runtime; skipping hook installation");
        return;
    }

    bool d3dHookInstalled = false;
    bool hudHookInstalled = false;

    try
    {
        REL::Relocation<std::uintptr_t> target{RELOCATION_ID(75595, 77226),
                                               GLYPH_OFFSET(0x9, 0x275)};
        // WriteThunkCall requires CALL rel32 (0xE8); reject offset drift before patching.
        const auto patchOpcode = *reinterpret_cast<const std::uint8_t*>(target.address());
        if (patchOpcode != 0xE8)
        {
            SKSE::log::error(
                "Hooks: CreateD3DAndSwapChain patch site reads 0x{:02X}, expected 0xE8 "
                "(CALL rel32); aborting D3D hook to avoid corrupting code",
                patchOpcode);
        }
        else
        {
            Stl::WriteThunkCall<CreateD3DAndSwapChain>(target.address());
            d3dHookInstalled = true;
        }
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
