#include <SKSE/SKSE.h>

#include "ParticleTextures.h"
#include "Settings.h"

#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// Lifecycle: Initialize() loads textures once after D3D11 device creation.
// Shutdown() releases resources and allows safe re-initialization.
namespace ParticleTextures
{
// Number of particle texture types
static constexpr int NUM_TYPES = 5;  // Stars, Sparks, Wisps, Runes, Orbs

// Texture info struct
struct TextureInfo
{
    ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
};

// Multiple textures per particle type
static std::array<std::vector<TextureInfo>, NUM_TYPES>& Textures()
{
    static std::array<std::vector<TextureInfo>, NUM_TYPES> instance;
    return instance;
}
static std::atomic<bool> s_Initialized{false};
static std::mutex& InitMutex()
{
    static std::mutex instance;
    return instance;
}

// Point sampler for small sprites
static ComPtr<ID3D11SamplerState> s_PointSampler;
// Linear sampler for high-resolution textures
static ComPtr<ID3D11SamplerState> s_LinearSampler;
static ComPtr<ID3D11BlendState> s_AdditiveBlend;
static ComPtr<ID3D11BlendState> s_ScreenBlend;
static ComPtr<ID3D11Device> s_Device;
static ComPtr<ID3D11DeviceContext> s_Context;

static void ReleaseResources_NoLock()
{
    for (auto& typeTextures : Textures())
    {
        for (auto& tex : typeTextures)
        {
            tex.srv.Reset();
        }
        typeTextures.clear();
    }

    s_PointSampler.Reset();
    s_LinearSampler.Reset();
    s_AdditiveBlend.Reset();
    s_ScreenBlend.Reset();
    s_Context.Reset();
    s_Device.Reset();
    s_Initialized = false;
}

// Load a PNG file using WIC and create a D3D11 texture.
// Returns TextureInfo with dimensions.
static TextureInfo LoadTextureFromFile(ID3D11Device* device,
                                       const std::string& path,
                                       IWICImagingFactory* wicFactory)
{
    TextureInfo info;
    if (!device || path.empty())
    {
        return info;
    }

    // Convert path to wide string
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        return info;
    }

    std::wstring widePath(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &widePath[0], wideLen);

    // Load the image
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory->CreateDecoderFromFilename(
        widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr))
    {
        SKSE::log::debug("ParticleTextures: Failed to load image: {}", path);
        return info;
    }

    // Get first frame
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        return info;
    }

    // Convert to RGBA
    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        return info;
    }

    hr = converter->Initialize(frame.Get(),
                               GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone,
                               nullptr,
                               .0f,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        return info;
    }

    // Get dimensions
    UINT width, height;
    hr = converter->GetSize(&width, &height);
    if (FAILED(hr))
    {
        return info;
    }

    // Read pixels
    if (width == 0 || height == 0 || width > ((std::numeric_limits<UINT>::max)() / 4))
    {
        SKSE::log::warn(
            "ParticleTextures: Invalid texture dimensions {}x{} for {}", width, height, path);
        return info;
    }
    const UINT stride = width * 4;
    const uint64_t bufferSize64 = static_cast<uint64_t>(stride) * static_cast<uint64_t>(height);
    if (bufferSize64 > static_cast<uint64_t>((std::numeric_limits<UINT>::max)()))
    {
        SKSE::log::warn(
            "ParticleTextures: Texture too large to load ({}x{}) {}", width, height, path);
        return info;
    }
    const UINT bufferSize = static_cast<UINT>(bufferSize64);
    std::vector<BYTE> pixels(static_cast<size_t>(bufferSize));
    hr = converter->CopyPixels(nullptr, stride, bufferSize, pixels.data());
    if (FAILED(hr))
    {
        return info;
    }

    // Sanitize transparent pixels so blend modes (especially screen-like) don't pick up
    // hidden RGB from fully transparent texels and produce box artifacts.
    {
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const size_t idx = i * 4;
            BYTE& r = pixels[idx + 0];
            BYTE& g = pixels[idx + 1];
            BYTE& b = pixels[idx + 2];
            const BYTE a = pixels[idx + 3];
            if (a == 0)
            {
                r = 0;
                g = 0;
                b = 0;
            }
        }
    }

    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = stride;

    ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr))
    {
        SKSE::log::warn("ParticleTextures: Failed to create texture for {}", path);
        return info;
    }

    // Create SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(
        texture.Get(), &srvDesc, info.srv.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        SKSE::log::warn("ParticleTextures: Failed to create SRV for {}", path);
        return info;
    }

    info.width = static_cast<int>(width);
    info.height = static_cast<int>(height);

    SKSE::log::debug("ParticleTextures: Loaded {}x{} texture: {}", width, height, path);
    return info;
}

// Load all PNG files from a folder into the texture array for a particle type.
static int LoadTexturesFromFolder(ID3D11Device* device,
                                  int styleIndex,
                                  const std::string& folderPath,
                                  IWICImagingFactory* wicFactory)
{
    if (styleIndex < 0 || styleIndex >= NUM_TYPES)
    {
        return 0;
    }
    if (folderPath.empty())
    {
        return 0;
    }

    int loadedCount = 0;

    try
    {
        fs::path folder(folderPath);
        if (!fs::exists(folder) || !fs::is_directory(folder))
        {
            SKSE::log::debug("ParticleTextures: Folder not found: {}", folderPath);
            return 0;
        }

        std::vector<fs::path> pngFiles;
        for (const auto& entry : fs::directory_iterator(folder))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            auto ext = entry.path().extension().string();
            std::transform(ext.begin(),
                           ext.end(),
                           ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".png")
            {
                pngFiles.push_back(entry.path());
            }
        }

        std::sort(pngFiles.begin(),
                  pngFiles.end(),
                  [](const fs::path& a, const fs::path& b)
                  { return a.generic_u8string() < b.generic_u8string(); });

        for (const auto& texturePath : pngFiles)
        {
            auto info = LoadTextureFromFile(device, texturePath.string(), wicFactory);
            if (info.srv)
            {
                Textures()[styleIndex].push_back(info);
                loadedCount++;
            }
        }

        if (loadedCount > 0)
        {
            SKSE::log::info(
                "ParticleTextures: Loaded {} textures from {}", loadedCount, folderPath);
        }
    }
    catch (const std::exception& e)
    {
        SKSE::log::warn("ParticleTextures: Error scanning folder {}: {}", folderPath, e.what());
    }

    return loadedCount;
}

// RAII wrapper for COM initialization.
// Only calls CoUninitialize if this scope actually initialized COM
// (avoids unbalancing the ref count when a third-party hook already initialized it).
struct ComScope
{
    bool ownsInit = false;
    bool usable = false;

    ComScope()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ownsInit = SUCCEEDED(hr);
        usable = ownsInit || hr == RPC_E_CHANGED_MODE;
        if (!usable)
        {
            SKSE::log::warn("ParticleTextures: COM initialization failed (hr=0x{:08X})",
                            static_cast<unsigned int>(hr));
        }
    }

    ~ComScope()
    {
        if (ownsInit)
        {
            CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

bool Initialize(ID3D11Device* device)
{
    std::lock_guard<std::mutex> lock(InitMutex());
    if (!device)
    {
        return false;
    }
    if (s_Initialized.load(std::memory_order_acquire))
    {
        return true;
    }

    // Reset stale partial state from previous failed initialization attempts.
    ReleaseResources_NoLock();

    SKSE::log::info("ParticleTextures: Initializing particle textures...");

    // Initialize COM for WIC texture loading. RAII ensures cleanup on all exit paths.
    ComScope com;
    if (!com.usable)
    {
        return false;
    }

    // Store device and get context
    s_Device = device;
    device->GetImmediateContext(s_Context.ReleaseAndGetAddressOf());
    if (!s_Context)
    {
        SKSE::log::warn("ParticleTextures: Failed to get immediate device context");
    }

    // Create point sampler for small sprites
    D3D11_SAMPLER_DESC pointDesc = {};
    pointDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;  // No interpolation
    pointDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    pointDesc.MipLODBias = .0f;
    pointDesc.MaxAnisotropy = 1;
    pointDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    pointDesc.MinLOD = 0;
    pointDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState(&pointDesc, s_PointSampler.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        SKSE::log::warn("ParticleTextures: Failed to create point sampler");
    }
    else
    {
        SKSE::log::info("ParticleTextures: Created point sampler for small sprites");
    }

    // Create linear sampler for high-resolution textures
    D3D11_SAMPLER_DESC linearDesc = pointDesc;
    linearDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    hr = device->CreateSamplerState(&linearDesc, s_LinearSampler.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        SKSE::log::warn("ParticleTextures: Failed to create linear sampler");
    }
    else
    {
        SKSE::log::info("ParticleTextures: Created linear sampler for HD particles");
    }

    // Create additive blend state for bright magical particles
    D3D11_BLEND_DESC addDesc = {};
    addDesc.AlphaToCoverageEnable = FALSE;
    addDesc.IndependentBlendEnable = FALSE;
    addDesc.RenderTarget[0].BlendEnable = TRUE;
    addDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    addDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    addDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    addDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    addDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    addDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    addDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&addDesc, s_AdditiveBlend.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        SKSE::log::warn("ParticleTextures: Failed to create additive blend state");
    }
    else
    {
        SKSE::log::info("ParticleTextures: Created additive blend state");
    }

    // Create a screen-like blend state for softer luminous sprites.
    // Gate source contribution by source alpha to avoid rectangle artifacts.
    D3D11_BLEND_DESC screenDesc = addDesc;
    screenDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    screenDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    hr = device->CreateBlendState(&screenDesc, s_ScreenBlend.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        SKSE::log::warn("ParticleTextures: Failed to create screen blend state");
    }
    else
    {
        SKSE::log::info("ParticleTextures: Created screen blend state");
    }

    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT wicHr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    if (FAILED(wicHr))
    {
        SKSE::log::error("ParticleTextures: Failed to create WIC factory (hr=0x{:08X})",
                         static_cast<unsigned int>(wicHr));
        return false;
    }

    // Base path for particle textures
    const std::string basePath = "Data/SKSE/Plugins/glyph/particles/";

    // Map particle styles to folder names
    struct FolderMapping
    {
        int style;
        const char* folder;
    };

    FolderMapping mappings[] = {
        {0, "stars"},   // Stars
        {1, "sparks"},  // Sparks
        {2, "wisps"},   // Wisps
        {3, "runes"},   // Runes
        {4, "orbs"},    // Orbs
    };

    int totalLoaded = 0;
    for (const auto& m : mappings)
    {
        std::string folderPath = basePath + m.folder;
        int count = LoadTexturesFromFolder(device, m.style, folderPath, wicFactory.Get());
        if (count > 0)
        {
            SKSE::log::info(
                "ParticleTextures: [{}] loaded {} textures from {}", m.folder, count, folderPath);
        }
        else
        {
            SKSE::log::warn("ParticleTextures: [{}] NO textures found in {}", m.folder, folderPath);
        }
        totalLoaded += count;
    }

    s_Initialized.store(totalLoaded > 0, std::memory_order_release);
    SKSE::log::info("ParticleTextures: === TOTAL: {} particle textures loaded ===", totalLoaded);
    if (!s_Initialized.load(std::memory_order_acquire))
    {
        SKSE::log::error("ParticleTextures: NO TEXTURES LOADED - falling back to shape rendering");
        ReleaseResources_NoLock();
    }
    return s_Initialized.load(std::memory_order_acquire);
}

// Callback to set a specific sampler before drawing a sprite.
static void SetSamplerCallback(const ImDrawList*, const ImDrawCmd* cmd)
{
    auto* sampler = reinterpret_cast<ID3D11SamplerState*>(cmd ? cmd->UserCallbackData : nullptr);
    if (s_Context && sampler)
    {
        s_Context->PSSetSamplers(0, 1, &sampler);
    }
}

static void SetBlendCallback(const ImDrawList*, const ImDrawCmd* cmd)
{
    auto* blend = reinterpret_cast<ID3D11BlendState*>(cmd ? cmd->UserCallbackData : nullptr);
    if (s_Context && blend)
    {
        constexpr float blendFactor[4] = {0, 0, 0, 0};
        s_Context->OMSetBlendState(blend, blendFactor, 0xFFFFFFFF);
    }
}

bool IsInitialized()
{
    return s_Initialized.load(std::memory_order_acquire);
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(InitMutex());
    ReleaseResources_NoLock();
}

int GetTextureCount(int style)
{
    if (style < 0 || style >= NUM_TYPES)
    {
        return 0;
    }
    return static_cast<int>(Textures()[style].size());
}

// Simple hash for better texture distribution while remaining stable
static size_t HashIndex(int style, int particleIndex)
{
    // Mix style and index for varied distribution
    // Using prime multipliers for better spread
    size_t hash = static_cast<size_t>(particleIndex);
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    hash ^= static_cast<size_t>(style) * 2654435761;
    return hash;
}

static const TextureInfo* GetTextureInfoForIndex(int style, int particleIndex)
{
    if (style < 0 || style >= NUM_TYPES)
    {
        return nullptr;
    }
    if (Textures()[style].empty())
    {
        return nullptr;
    }

    const size_t texCount = Textures()[style].size();
    const size_t texIndex = HashIndex(style, particleIndex) % texCount;
    return &Textures()[style][texIndex];
}

ImTextureID GetRandomTexture(int style, int particleIndex)
{
    const TextureInfo* info = GetTextureInfoForIndex(style, particleIndex);
    return info ? reinterpret_cast<ImTextureID>(info->srv.Get()) : ImTextureID{};
}

void DrawSpriteWithIndex(ImDrawList* list,
                         const ImVec2& center,
                         float size,
                         int style,
                         int particleIndex,
                         ImU32 color,
                         BlendMode blendMode,
                         float rotation)
{
    if (!list || style < 0 || style >= NUM_TYPES)
    {
        return;
    }

    const TextureInfo* texInfo = GetTextureInfoForIndex(style, particleIndex);
    if (!texInfo || !texInfo->srv)
    {
        return;
    }
    ImTextureID tex = reinterpret_cast<ImTextureID>(texInfo->srv.Get());

    // Normalize display size for high-resolution textures.
    // 2K (2048px) sources should display at a visible size, not shrink to dots.
    const int texMaxPx = (texInfo->width > texInfo->height) ? texInfo->width : texInfo->height;
    const float texMaxDim = static_cast<float>(texMaxPx);
    const float resolutionScale =
        (texMaxDim > .0f) ? std::clamp(1200.0f / texMaxDim, .45f, 1.0f) : 1.0f;
    const float scaledSize = size * resolutionScale;
    float halfSize = scaledSize * .5f;
    if (halfSize <= .01f)
    {
        return;
    }

    // Use linear filtering for HD textures, point filtering for tiny sprites.
    ID3D11SamplerState* samplerToUse =
        (texMaxDim > 64.0f && s_LinearSampler) ? s_LinearSampler.Get() : s_PointSampler.Get();

    ID3D11BlendState* blendToUse = nullptr;
    switch (blendMode)
    {
        case BlendMode::Additive:
            blendToUse = s_AdditiveBlend.Get();
            break;
        case BlendMode::Screen:
            blendToUse = s_ScreenBlend.Get();
            break;
        case BlendMode::Alpha:
        default:
            blendToUse = nullptr;
            break;
    }

    if (samplerToUse)
    {
        list->AddCallback(SetSamplerCallback, samplerToUse);
    }
    if (blendToUse)
    {
        list->AddCallback(SetBlendCallback, blendToUse);
    }

    if (rotation == .0f)
    {
        // Simple axis-aligned quad
        ImVec2 pMin(center.x - halfSize, center.y - halfSize);
        ImVec2 pMax(center.x + halfSize, center.y + halfSize);
        list->AddImage(tex, pMin, pMax, ImVec2(0, 0), ImVec2(1, 1), color);
    }
    else
    {
        // Rotated quad using AddImageQuad
        float cosR = std::cos(rotation);
        float sinR = std::sin(rotation);

        // Calculate rotated corners
        ImVec2 corners[4];
        float offsets[4][2] = {
            {-halfSize, -halfSize},  // Top-left
            {halfSize, -halfSize},   // Top-right
            {halfSize, halfSize},    // Bottom-right
            {-halfSize, halfSize},   // Bottom-left
        };

        for (int i = 0; i < 4; i++)
        {
            float rx = offsets[i][0] * cosR - offsets[i][1] * sinR;
            float ry = offsets[i][0] * sinR + offsets[i][1] * cosR;
            corners[i] = ImVec2(center.x + rx, center.y + ry);
        }

        // UV coordinates for the corners
        ImVec2 uvs[4] = {
            ImVec2(0, 0),  // Top-left
            ImVec2(1, 0),  // Top-right
            ImVec2(1, 1),  // Bottom-right
            ImVec2(0, 1),  // Bottom-left
        };

        list->AddImageQuad(tex,
                           corners[0],
                           corners[1],
                           corners[2],
                           corners[3],
                           uvs[0],
                           uvs[1],
                           uvs[2],
                           uvs[3],
                           color);
    }

    // Reset to let ImGui restore its default sampler
    if (samplerToUse || blendToUse)
    {
        list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
}

void PushAdditiveBlend(ImDrawList* dl)
{
    if (dl && s_AdditiveBlend)
    {
        dl->AddCallback(SetBlendCallback, s_AdditiveBlend.Get());
    }
}

void PopBlendState(ImDrawList* dl)
{
    if (dl)
    {
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
}
}  // namespace ParticleTextures
