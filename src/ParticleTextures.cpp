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
static constexpr int NUM_TYPES = 6;  // Stars, Sparks, Wisps, Runes, Orbs, Crystals

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

// ============ Procedural Texture Generation ============
//
// Generates clean, white-on-transparent particle sprites at runtime.
// These are 256x256 with mathematically defined alpha for perfect edges.
// White base enables proper tier color tinting via vertex color multiplication.

static constexpr int PROC_SIZE = 256;
static constexpr float PROC_PI = 3.14159265f;

static float PGaussian(float x, float sigma)
{
    return std::exp(-(x * x) / (2.0f * sigma * sigma));
}

static float PSmoothstep(float edge0, float edge1, float x)
{
    float t = std::clamp((x - edge0) / (edge1 - edge0), .0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float PLineDist(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float t =
        std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy + 1e-8f), .0f, 1.0f);
    float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

static float PRingDist(float px, float py, float radius)
{
    return std::abs(std::sqrt(px * px + py * py) - radius);
}

static float PLineAlpha(float dist, float width, float soft)
{
    return PSmoothstep(width + soft, width - soft, dist);
}

using PPixelFn = float (*)(float, float);

// ---- Stars ----

// 4-pointed starburst with secondary spikes, ring accent, and bright core
static float Star4Cross(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Bright concentrated core with steep falloff
    float core = std::pow(PGaussian(r, .12f), .6f);
    float glow = PGaussian(r, .45f) * .3f;
    // Main 4-point spikes
    float sH = PGaussian(ny, .035f) * PGaussian(r, .65f);
    float sV = PGaussian(nx, .035f) * PGaussian(r, .65f);
    float mainSpikes = (std::max)(sH, sV);
    // Secondary spikes at 45 degrees (thinner, shorter)
    float d45a = (nx + ny) * .7071f, d45b = (-nx + ny) * .7071f;
    float secSpikes =
        (std::max)(PGaussian(d45b, .02f), PGaussian(d45a, .02f)) * PGaussian(r, .45f) * .35f;
    // Subtle ring accent
    float ring = PGaussian(std::abs(r - .45f), .025f) * .1f;
    return std::clamp(core + glow + mainSpikes * .85f + secSpikes + ring, .0f, 1.0f);
}

// 6-pointed star with bright core, 6 spikes, and ring
static float Star6Point(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .1f), .6f);
    float glow = PGaussian(r, .4f) * .25f;
    // 6 spikes at 30-degree intervals
    float ms = .0f;
    for (int i = 0; i < 6; ++i)
    {
        float a = static_cast<float>(i) * PROC_PI / 3.0f;
        float py = -nx * std::sin(a) + ny * std::cos(a);
        ms = (std::max)(ms, PGaussian(py, .03f) * PGaussian(r, .55f));
    }
    // Ring accent
    float ring = PGaussian(std::abs(r - .35f), .03f) * .1f;
    return std::clamp(core + glow + ms * .75f + ring, .0f, 1.0f);
}

// Diamond sparkle with inner cross and faceted edge
static float StarDiamond(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float rx = (nx + ny) * .7071f, ry = (-nx + ny) * .7071f;
    float diamond = (std::max)(std::abs(rx), std::abs(ry));
    float shape = PSmoothstep(.5f, .2f, diamond);
    float core = std::pow(PGaussian(r, .08f), .5f);
    float glow = PGaussian(r, .35f) * .2f;
    // Inner cross accent
    float cH = PGaussian(ny, .03f) * PGaussian(r, .35f) * .2f;
    float cV = PGaussian(nx, .03f) * PGaussian(r, .35f) * .2f;
    return std::clamp(shape * .5f + core * .7f + glow + (std::max)(cH, cV), .0f, 1.0f);
}

// Lens flare with diffraction spikes, secondary spikes, and aperture ring
static float StarFlare(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .05f), .5f);
    float halo = PGaussian(r, .5f) * .3f;
    // 4 very thin main spikes
    float sH = PGaussian(ny, .012f) * PGaussian(r, .75f);
    float sV = PGaussian(nx, .012f) * PGaussian(r, .75f);
    float spikes = (std::max)(sH, sV) * .5f;
    // Finer secondary spikes at 45 degrees
    float d1 = (nx + ny) * .7071f, d2 = (-nx + ny) * .7071f;
    float sec = (std::max)(PGaussian(d1, .008f), PGaussian(d2, .008f)) * PGaussian(r, .5f) * .2f;
    // Aperture ring
    float ring = PGaussian(std::abs(r - .55f), .02f) * .08f;
    return std::clamp(core + halo + spikes + sec + ring, .0f, 1.0f);
}

// 8-pointed compass rose with tapered main + diagonal spikes
static float StarCompass(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .09f), .5f);
    float glow = PGaussian(r, .4f) * .2f;
    // 4 main cardinal spikes (longer, thicker)
    float sH = PGaussian(ny, .03f) * PGaussian(r, .7f);
    float sV = PGaussian(nx, .03f) * PGaussian(r, .7f);
    float main4 = (std::max)(sH, sV) * .8f;
    // 4 diagonal spikes (shorter, thinner)
    float d1 = (nx + ny) * .7071f, d2 = (-nx + ny) * .7071f;
    float diag = (std::max)(PGaussian(d1, .02f), PGaussian(d2, .02f)) * PGaussian(r, .5f) * .5f;
    // Tapered tips: spikes narrow toward ends
    float taper = PSmoothstep(.7f, .2f, r);
    float ring = PGaussian(std::abs(r - .3f), .02f) * .08f;
    return std::clamp(core + glow + main4 * taper + diag * taper + ring, .0f, 1.0f);
}

// 5-armed spiral pinwheel with curved spikes
static float StarPinwheel(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float angle = std::atan2(ny, nx);
    float core = std::pow(PGaussian(r, .1f), .6f);
    float glow = PGaussian(r, .4f) * .2f;
    // 5 spiraling arms: each arm is a spike whose angle offset increases with radius
    float arms = .0f;
    for (int i = 0; i < 5; ++i)
    {
        float armAngle = static_cast<float>(i) * PROC_PI * .4f;  // 72-degree spacing
        float spiral = angle - armAngle - r * 2.5f;              // spiral twist
        // Normalize angle to [-pi, pi]
        spiral = spiral - std::floor(spiral / (2.0f * PROC_PI) + .5f) * 2.0f * PROC_PI;
        float arm = PGaussian(spiral, .15f) * PGaussian(r - .3f, .25f);
        arms = (std::max)(arms, arm);
    }
    return std::clamp(core + glow + arms * .7f, .0f, 1.0f);
}

// Nova burst: 12+ thin radiating lines with shockwave ring
static float StarNova(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .07f), .4f);
    float glow = PGaussian(r, .35f) * .25f;
    // 12 thin radiating lines of varying length
    float rays = .0f;
    for (int i = 0; i < 12; ++i)
    {
        float a = static_cast<float>(i) * PROC_PI / 6.0f;
        float py = -nx * std::sin(a) + ny * std::cos(a);
        // Alternating long/short rays
        float len = (i % 2 == 0) ? .7f : .5f;
        rays = (std::max)(rays, PGaussian(py, .01f) * PGaussian(r, len));
    }
    // Shockwave ring
    float ring = PGaussian(std::abs(r - .4f), .025f) * .3f;
    float innerRing = PGaussian(std::abs(r - .2f), .015f) * .12f;
    return std::clamp(core + glow + rays * .45f + ring + innerRing, .0f, 1.0f);
}

// ---- Sparks ----

// Round ember with hot core and layered glow
static float SparkEmber(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .15f), .5f);
    float mid = PGaussian(r, .3f) * .4f;
    float outer = PGaussian(r, .55f) * .15f;
    return std::clamp(core + mid + outer, .0f, 1.0f);
}

// Intense flash point with micro-spikes and inner ring
static float SparkFlash(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .06f), .4f);
    float glow = PGaussian(r, .3f) * .3f;
    // 4 micro-spikes
    float sH = PGaussian(ny, .025f) * PGaussian(r, .4f) * .25f;
    float sV = PGaussian(nx, .025f) * PGaussian(r, .4f) * .25f;
    float ring = PGaussian(std::abs(r - .25f), .025f) * .08f;
    return std::clamp(core + glow + (std::max)(sH, sV) + ring, .0f, 1.0f);
}

// Elongated shard with directional bias and accent
static float SparkShard(float nx, float ny)
{
    float ex = nx * 1.5f;
    float r = std::sqrt(ex * ex + ny * ny);
    float core = std::pow(PGaussian(r, .12f), .5f);
    float glow = PGaussian(r, .35f) * .25f;
    // Vertical spike (main direction)
    float spike = PGaussian(nx, .04f) * PGaussian(ny, .5f) * .5f;
    // Slight horizontal accent
    float accent = PGaussian(ny, .04f) * PGaussian(nx, .25f) * .15f;
    return std::clamp(core + glow + spike + accent, .0f, 1.0f);
}

// Teardrop comet with bright head and fading tail
static float SparkComet(float nx, float ny)
{
    // Asymmetric: bright at top (ny < 0), fading downward
    float ey = ny + .15f;  // shift center upward
    float r = std::sqrt(nx * nx + ey * ey);
    float core = std::pow(PGaussian(r, .1f), .5f);
    // Tail extends downward
    float tail = PGaussian(nx, .08f) * PSmoothstep(-.3f, .6f, ny) * PGaussian(ny - .2f, .35f);
    float glow = PGaussian(r, .3f) * .25f;
    return std::clamp(core + tail * .6f + glow, .0f, 1.0f);
}

// Electric crackle: irregular angular bright spikes
static float SparkCrackle(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .08f), .4f);
    float glow = PGaussian(r, .35f) * .2f;
    // 5 spikes at irregular deterministic angles
    float spikes = .0f;
    float angles[] = {.0f, 1.15f, 2.4f, 3.7f, 5.1f};
    float lengths[] = {.55f, .4f, .6f, .35f, .5f};
    for (int i = 0; i < 5; ++i)
    {
        float py = -nx * std::sin(angles[i]) + ny * std::cos(angles[i]);
        spikes = (std::max)(spikes, PGaussian(py, .015f) * PGaussian(r, lengths[i]));
    }
    return std::clamp(core + glow + spikes * .5f, .0f, 1.0f);
}

// Soft firefly: tiny bright core with large gentle halo
static float SparkFirefly(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .04f), .3f) * .6f;
    float innerGlow = PGaussian(r, .15f) * .35f;
    float outerGlow = PGaussian(r, .4f) * .15f;
    float haze = PGaussian(r, .6f) * .06f;
    return std::clamp(core + innerGlow + outerGlow + haze, .0f, 1.0f);
}

// ---- Wisps ----

// Multi-layered crescent arc with bright inner edge
static float WispCrescent(float nx, float ny)
{
    float d1 = std::sqrt(nx * nx + ny * ny);
    // Primary crescent
    float ox1 = nx + .25f;
    float d2 = std::sqrt(ox1 * ox1 + ny * ny);
    float shape1 = PSmoothstep(.6f, .4f, d1) * (1.0f - PSmoothstep(.65f, .45f, d2));
    // Inner highlight crescent (brighter, thinner)
    float ox2 = nx + .2f;
    float d3 = std::sqrt(ox2 * ox2 + ny * ny);
    float shape2 = PSmoothstep(.45f, .35f, d1) * (1.0f - PSmoothstep(.5f, .38f, d3));
    // Bright inner edge
    float edge = shape1 * PGaussian(d2 - .55f, .08f) * .3f;
    float glow = PGaussian(d1, .55f) * .08f;
    return std::clamp(shape1 * .6f + shape2 * .35f + edge + glow, .0f, 1.0f);
}

// Curling fern frond with tapering width and secondary frondlet
static float WispFernCurl(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float angle = std::atan2(ny, nx);
    // Fern spine: tighter logarithmic spiral than WispSpiral
    float spiralAngle = angle - r * 6.0f;
    spiralAngle = spiralAngle - std::floor(spiralAngle / (2.0f * PROC_PI) + .5f) * 2.0f * PROC_PI;
    float spiralDist = std::abs(spiralAngle) * r;
    // Width tapers: thick near center, thin at tip
    float width = (std::max)(.02f, .12f - r * .1f);
    float spine = PGaussian(spiralDist, width);
    // Visible from r=0.08 to r=0.65
    float radialFade = PSmoothstep(.05f, .12f, r) * PSmoothstep(.65f, .4f, r);
    // Secondary frondlet at faster winding rate
    float spiral2Angle = angle - r * 8.0f + 1.2f;
    spiral2Angle =
        spiral2Angle - std::floor(spiral2Angle / (2.0f * PROC_PI) + .5f) * 2.0f * PROC_PI;
    float spiral2Dist = std::abs(spiral2Angle) * r;
    float frondlet = PGaussian(spiral2Dist, .05f) * PSmoothstep(.15f, .25f, r) *
                     PSmoothstep(.45f, .35f, r) * .35f;
    // Curl tip highlight at center
    float curlTip = PGaussian(r, .06f) * .4f;
    float core = PGaussian(r, .06f) * .25f;
    float glow = PGaussian(r, .35f) * .06f;
    return std::clamp(
        spine * radialFade * .85f + frondlet * radialFade + curlTip + core + glow, .0f, 1.0f);
}

// Flowing S-curve tendril with secondary branch
static float WispTendril(float nx, float ny)
{
    // Primary S-curve
    float curve1 = std::sin(ny * PROC_PI * .9f) * .22f;
    float dist1 = std::abs(nx - curve1);
    float width1 = .1f + .06f * std::cos(ny * PROC_PI * .5f);
    float t1 = PSmoothstep(width1 + .03f, width1 - .015f, dist1);
    // Secondary curve (branching feel)
    float curve2 = std::sin(ny * PROC_PI * 1.3f + .8f) * .15f + .1f;
    float dist2 = std::abs(nx - curve2);
    float t2 = PSmoothstep(.06f, .02f, dist2) * .4f;
    // Length fade
    float fade = PSmoothstep(-1.0f, -.55f, ny) * PSmoothstep(1.0f, .55f, ny);
    // Inner glow along main tendril
    float glow = PGaussian(dist1, .2f) * .12f * fade;
    return std::clamp((t1 * .75f + t2) * fade + glow, .0f, 1.0f);
}

// Spiral arm wrapping ~270 degrees, fading at tail
static float WispSpiral(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float angle = std::atan2(ny, nx);
    // Spiral: distance from the spiral curve
    float spiralAngle = angle - r * 4.0f;  // tighter spiral
    spiralAngle = spiralAngle - std::floor(spiralAngle / (2.0f * PROC_PI) + .5f) * 2.0f * PROC_PI;
    float spiralDist = std::abs(spiralAngle) * r;  // scale with radius for even width
    float spiral = PGaussian(spiralDist, .08f) * PSmoothstep(.7f, .1f, r);
    // Fade at center and far edge
    float radialFade = PSmoothstep(.05f, .15f, r) * PSmoothstep(.7f, .5f, r);
    float core = PGaussian(r, .08f) * .3f;
    float glow = PGaussian(r, .4f) * .08f;
    return std::clamp(spiral * radialFade * .8f + core + glow, .0f, 1.0f);
}

// Moth silhouette with paired wing lobes and thin antennae
static float WispMoth(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Left wing: offset and horizontally squeezed ellipse
    float lwx = (nx + .18f) * 1.6f;
    float lwDist = std::sqrt(lwx * lwx + ny * ny);
    float leftWing = PSmoothstep(.35f, .18f, lwDist);
    // Right wing: mirrored
    float rwx = (nx - .18f) * 1.6f;
    float rwDist = std::sqrt(rwx * rwx + ny * ny);
    float rightWing = PSmoothstep(.35f, .18f, rwDist);
    // Wing edge glow (bright rim at shape boundary)
    float leftEdge = PGaussian(lwDist - .27f, .04f) * .4f;
    float rightEdge = PGaussian(rwDist - .27f, .04f) * .4f;
    // Thin vertical body at center
    float body = PGaussian(nx, .04f) * PGaussian(ny, .2f) * .5f;
    // Diverging antennae from top-center
    float ant1 = PLineAlpha(PLineDist(nx, ny, .0f, -.05f, -.2f, -.4f), .012f, .01f) * .35f;
    float ant2 = PLineAlpha(PLineDist(nx, ny, .0f, -.05f, .2f, -.4f), .012f, .01f) * .35f;
    // Antenna tip dots
    float tipL =
        PGaussian(std::sqrt((nx + .2f) * (nx + .2f) + (ny + .4f) * (ny + .4f)), .025f) * .3f;
    float tipR =
        PGaussian(std::sqrt((nx - .2f) * (nx - .2f) + (ny + .4f) * (ny + .4f)), .025f) * .3f;
    float wings = (std::max)(leftWing, rightWing) * .55f + leftEdge + rightEdge;
    float core = PGaussian(r, .06f) * .3f;
    return std::clamp(wings + body + ant1 + ant2 + tipL + tipR + core, .0f, 1.0f);
}

// Water droplet with bright caustic crescent and specular highlight
static float WispDewdrop(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Soft ring outline
    float ring = PGaussian(std::abs(r - .4f), .05f) * .5f;
    // Caustic crescent inside the droplet (offset subtracted circles)
    float cx = nx + .12f;
    float cy = ny + .12f;
    float cd = std::sqrt(cx * cx + cy * cy);
    float caustic = PSmoothstep(.3f, .18f, r) * (1.0f - PSmoothstep(.35f, .2f, cd));
    // Soft inner body fill
    float body = PGaussian(r, .3f) * .3f;
    // Specular highlight dot (upper-left)
    float specX = nx + .12f;
    float specY = ny + .15f;
    float specDist = std::sqrt(specX * specX + specY * specY);
    float specular = PGaussian(specDist, .06f) * .6f;
    // Subtle downward gravity glow
    float gravity = PGaussian(nx, .2f) * PSmoothstep(-.1f, .3f, ny) * PGaussian(r, .4f) * .15f;
    float core = PGaussian(r, .06f) * .25f;
    float glow = PGaussian(r, .5f) * .06f;
    return std::clamp(ring + caustic * .65f + body + specular + gravity + core + glow, .0f, 1.0f);
}

// Bright offset core with three diverging trail lines
static float WispFirefly(float nx, float ny)
{
    // Off-center core
    float ox = nx + .1f;
    float oy = ny - .05f;
    float offR = std::sqrt(ox * ox + oy * oy);
    // Bright concentrated core
    float core = std::pow(PGaussian(offR, .05f), .4f) * .7f;
    float innerGlow = PGaussian(offR, .15f) * .35f;
    // Three trail lines radiating from core at asymmetric angles
    // Fade trails outward from core center
    float radFade = PSmoothstep(.0f, .15f, offR);
    float radFadeShort = PSmoothstep(.0f, .1f, offR);
    float t1 = PLineAlpha(PLineDist(nx, ny, -.1f, .05f, .35f, -.3f), .018f, .015f);
    float t2 = PLineAlpha(PLineDist(nx, ny, -.1f, .05f, .3f, .4f), .015f, .012f);
    float t3 = PLineAlpha(PLineDist(nx, ny, -.1f, .05f, -.45f, .15f), .012f, .01f) * .5f;

    float trails = t1 * radFade * .4f + t2 * radFade * .35f + t3 * radFadeShort * .2f;
    float outerGlow = PGaussian(offR, .4f) * .08f;
    return std::clamp(core + innerGlow + trails + outerGlow, .0f, 1.0f);
}

// Cluster of 5 pollen motes drifting along a gentle arc
static float WispPollenDrift(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // 5 mote positions along a gentle arc
    const float mx[] = {-.18f, -.08f, .02f, .1f, .06f};
    const float my[] = {-.22f, -.08f, .05f, .18f, .32f};
    const float ms[] = {.06f, .05f, .07f, .045f, .055f};
    float motes = .0f;
    for (int i = 0; i < 5; ++i)
    {
        float dx = nx - mx[i];
        float dy = ny - my[i];
        float d = std::sqrt(dx * dx + dy * dy);
        float moteCore = std::pow(PGaussian(d, ms[i] * .4f), .5f) * .5f;
        float moteHalo = PGaussian(d, ms[i]) * .6f;
        motes = (std::max)(motes, moteCore + moteHalo);
    }
    // Faint connecting glow between consecutive motes
    float connGlow = .0f;
    for (int i = 0; i < 4; ++i)
    {
        float seg = PLineDist(nx, ny, mx[i], my[i], mx[i + 1], my[i + 1]);
        connGlow = (std::max)(connGlow, PGaussian(seg, .06f) * .12f);
    }
    float overallGlow = PGaussian(r, .4f) * .05f;
    return std::clamp(motes + connGlow + overallGlow, .0f, 1.0f);
}

// ---- Runes ----

// Double-ring ward with inscribed cross and intersection dots
static float RuneWard(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Double ring
    float ring1 = PLineAlpha(PRingDist(nx, ny, .55f), .035f, .025f);
    float ring2 = PLineAlpha(PRingDist(nx, ny, .48f), .015f, .015f) * .4f;
    // Cross lines
    float crossH = PLineAlpha(PLineDist(nx, ny, -.42f, .0f, .42f, .0f), .028f, .02f);
    float crossV = PLineAlpha(PLineDist(nx, ny, .0f, -.42f, .0f, .42f), .028f, .02f);
    float cross = (std::max)(crossH, crossV);
    // Dots at cardinal intersections with ring
    float dots = .0f;
    for (int i = 0; i < 4; ++i)
    {
        float a = static_cast<float>(i) * PROC_PI * .5f;
        float dx = nx - std::cos(a) * .55f, dy = ny - std::sin(a) * .55f;
        dots = (std::max)(dots, PGaussian(std::sqrt(dx * dx + dy * dy), .04f));
    }
    // Glow along lines
    float lineGlow = (std::max)(PGaussian(std::abs(ny), .08f), PGaussian(std::abs(nx), .08f)) *
                     PGaussian(r, .45f) * .1f;
    float core = PGaussian(r, .1f) * .4f;
    return std::clamp((std::max)(ring1, ring2) + cross + dots * .6f + core + lineGlow, .0f, 1.0f);
}

// Triangle with inner inverted triangle, vertex dots, and center eye
static float RuneTriangle(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float s = .55f, si = .28f;
    // Outer triangle
    float e1 = PLineAlpha(PLineDist(nx, ny, .0f, -s, -s * .866f, s * .5f), .028f, .02f);
    float e2 = PLineAlpha(PLineDist(nx, ny, -s * .866f, s * .5f, s * .866f, s * .5f), .028f, .02f);
    float e3 = PLineAlpha(PLineDist(nx, ny, s * .866f, s * .5f, .0f, -s), .028f, .02f);
    float outer = (std::max)(e1, (std::max)(e2, e3));
    // Inner inverted triangle
    float i1 = PLineAlpha(PLineDist(nx, ny, .0f, si, -si * .866f, -si * .5f), .018f, .015f) * .5f;
    float i2 =
        PLineAlpha(PLineDist(nx, ny, -si * .866f, -si * .5f, si * .866f, -si * .5f), .018f, .015f) *
        .5f;
    float i3 = PLineAlpha(PLineDist(nx, ny, si * .866f, -si * .5f, .0f, si), .018f, .015f) * .5f;
    float inner = (std::max)(i1, (std::max)(i2, i3));
    // Vertex dots
    float dots = .0f;
    float verts[][2] = {{.0f, -s}, {-s * .866f, s * .5f}, {s * .866f, s * .5f}};
    for (auto& v : verts)
    {
        float dx = nx - v[0], dy = ny - v[1];
        dots = (std::max)(dots, PGaussian(std::sqrt(dx * dx + dy * dy), .035f));
    }
    // Center eye (ring + dot)
    float eye = PLineAlpha(PRingDist(nx, ny, .12f), .02f, .015f) * .5f;
    float center = PGaussian(r, .05f) * .6f;
    return std::clamp(
        outer + inner + dots * .5f + eye + center + PGaussian(r, .45f) * .06f, .0f, 1.0f);
}

// Double diamond with internal lattice (protection glyph)
static float RuneDiamond(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float s = .58f, si = .32f;
    // Outer diamond
    float e1 = PLineAlpha(PLineDist(nx, ny, .0f, -s, s * .6f, .0f), .028f, .02f);
    float e2 = PLineAlpha(PLineDist(nx, ny, s * .6f, .0f, .0f, s), .028f, .02f);
    float e3 = PLineAlpha(PLineDist(nx, ny, .0f, s, -s * .6f, .0f), .028f, .02f);
    float e4 = PLineAlpha(PLineDist(nx, ny, -s * .6f, .0f, .0f, -s), .028f, .02f);
    float outer = (std::max)((std::max)(e1, e2), (std::max)(e3, e4));
    // Inner diamond
    float j1 = PLineAlpha(PLineDist(nx, ny, .0f, -si, si * .6f, .0f), .018f, .015f) * .45f;
    float j2 = PLineAlpha(PLineDist(nx, ny, si * .6f, .0f, .0f, si), .018f, .015f) * .45f;
    float j3 = PLineAlpha(PLineDist(nx, ny, .0f, si, -si * .6f, .0f), .018f, .015f) * .45f;
    float j4 = PLineAlpha(PLineDist(nx, ny, -si * .6f, .0f, .0f, -si), .018f, .015f) * .45f;
    float inr = (std::max)((std::max)(j1, j2), (std::max)(j3, j4));
    // Vertical axis line
    float axis = PLineAlpha(PLineDist(nx, ny, .0f, -s * .7f, .0f, s * .7f), .015f, .015f) * .35f;
    // Corner accent dots
    float dots = .0f;
    float pts[][2] = {{.0f, -s}, {s * .6f, .0f}, {.0f, s}, {-s * .6f, .0f}};
    for (auto& p : pts)
    {
        float dx = nx - p[0], dy = ny - p[1];
        dots = (std::max)(dots, PGaussian(std::sqrt(dx * dx + dy * dy), .03f) * .5f);
    }
    float core = PGaussian(r, .08f) * .4f;
    return std::clamp(outer + inr + axis + dots + core + PGaussian(r, .45f) * .06f, .0f, 1.0f);
}

// Triple concentric rings with 8 dots and radial accents
static float RuneRings(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float ring1 = PLineAlpha(PRingDist(nx, ny, .55f), .028f, .02f);
    float ring2 = PLineAlpha(PRingDist(nx, ny, .38f), .022f, .018f);
    float ring3 = PLineAlpha(PRingDist(nx, ny, .2f), .015f, .012f) * .5f;
    float rings = (std::max)(ring1, (std::max)(ring2, ring3));
    // 8 cardinal + ordinal dots
    float dots = .0f;
    for (int i = 0; i < 8; ++i)
    {
        float a = static_cast<float>(i) * PROC_PI * .25f;
        float dr = .47f;
        float dx = nx - std::cos(a) * dr, dy = ny - std::sin(a) * dr;
        float dotSize = (i % 2 == 0) ? .04f : .025f;  // cardinal dots larger
        dots = (std::max)(dots, PGaussian(std::sqrt(dx * dx + dy * dy), dotSize));
    }
    // 4 short radial lines connecting outer to middle ring
    float radials = .0f;
    for (int i = 0; i < 4; ++i)
    {
        float a = static_cast<float>(i) * PROC_PI * .5f;
        float ca = std::cos(a), sa = std::sin(a);
        float rl = PLineDist(nx, ny, ca * .38f, sa * .38f, ca * .55f, sa * .55f);
        radials = (std::max)(radials, PLineAlpha(rl, .012f, .01f) * .4f);
    }
    float core = PGaussian(r, .06f) * .5f;
    return std::clamp(rings + dots * .6f + radials + core + PGaussian(r, .45f) * .06f, .0f, 1.0f);
}

// Five-pointed star inscribed in circle with vertex dots
static float RunePentagram(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Outer circle
    float circle = PLineAlpha(PRingDist(nx, ny, .55f), .025f, .02f);
    // 5-pointed star: 5 line segments connecting alternating vertices
    float star = .0f;
    float vx[5], vy[5];
    for (int i = 0; i < 5; ++i)
    {
        float a = static_cast<float>(i) * PROC_PI * .4f - PROC_PI * .5f;
        vx[i] = std::cos(a) * .5f;
        vy[i] = std::sin(a) * .5f;
    }
    for (int i = 0; i < 5; ++i)
    {
        int j = (i + 2) % 5;  // connect every other vertex
        star = (std::max)(star,
                          PLineAlpha(PLineDist(nx, ny, vx[i], vy[i], vx[j], vy[j]), .02f, .018f));
    }
    // Vertex dots
    float dots = .0f;
    for (int i = 0; i < 5; ++i)
    {
        float dx = nx - vx[i], dy = ny - vy[i];
        dots = (std::max)(dots, PGaussian(std::sqrt(dx * dx + dy * dy), .035f));
    }
    float core = PGaussian(r, .08f) * .4f;
    return std::clamp(
        circle + star * .8f + dots * .5f + core + PGaussian(r, .45f) * .06f, .0f, 1.0f);
}

// Stylized all-seeing eye: almond shape + circle pupil + center dot
static float RuneEye(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Almond/eye shape: two arcs meeting at left and right points
    // Upper arc (concave down) and lower arc (concave up)
    float eyeTop = ny + .35f - (nx * nx) * 1.2f;   // parabolic upper lid
    float eyeBot = -ny + .35f - (nx * nx) * 1.2f;  // parabolic lower lid
    float eyeShape = (std::min)(eyeTop, eyeBot);
    float eyeMask = PSmoothstep(.0f, .06f, eyeShape);
    // Eye outline (where eyeShape ~= 0)
    float outline = PGaussian(eyeShape, .03f) * .7f;
    // Pupil: circle in center
    float pupil = PLineAlpha(PRingDist(nx, ny, .15f), .025f, .02f) * .6f;
    // Iris dot
    float iris = PGaussian(r, .06f) * .7f;
    // Subtle glow
    float glow = PGaussian(r, .4f) * .08f;
    return std::clamp(outline + pupil + iris + eyeMask * .15f + glow, .0f, 1.0f);
}

// ---- Orbs ----

// Multi-layered gaussian sphere with hot center
static float OrbGaussian(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .08f), .5f) * .4f;
    float inner = PGaussian(r, .2f) * .6f;
    float mid = PGaussian(r, .35f) * .35f;
    float outer = PGaussian(r, .55f) * .12f;
    return std::clamp(core + inner + mid + outer, .0f, 1.0f);
}

// Orb with pronounced fresnel ring and inner glow
static float OrbRinged(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float body = PGaussian(r, .28f) * .5f;
    // Strong fresnel-like edge ring
    float ring = PGaussian(std::abs(r - .4f), .04f) * .6f;
    float innerRing = PGaussian(std::abs(r - .22f), .025f) * .15f;
    float core = std::pow(PGaussian(r, .06f), .5f) * .45f;
    float glow = PGaussian(r, .5f) * .1f;
    return std::clamp(body + ring + innerRing + core + glow, .0f, 1.0f);
}

// Double-ring halo with center spark
static float OrbHalo(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Primary ring
    float ring1 = PGaussian(std::abs(r - .38f), .06f) * .7f;
    // Secondary inner ring
    float ring2 = PGaussian(std::abs(r - .2f), .04f) * .3f;
    // Center spark
    float spark = std::pow(PGaussian(r, .04f), .5f) * .35f;
    // Fill glow
    float fill = PGaussian(r, .45f) * .12f;
    return std::clamp(ring1 + ring2 + spark + fill, .0f, 1.0f);
}

// Pulsar: bright core with 3 concentric ripple rings
static float OrbPulsar(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float core = std::pow(PGaussian(r, .06f), .4f) * .5f;
    float glow = PGaussian(r, .3f) * .25f;
    // 3 expanding rings at different radii
    float ring1 = PGaussian(std::abs(r - .2f), .025f) * .4f;
    float ring2 = PGaussian(std::abs(r - .38f), .02f) * .3f;
    float ring3 = PGaussian(std::abs(r - .55f), .018f) * .2f;
    return std::clamp(core + glow + ring1 + ring2 + ring3, .0f, 1.0f);
}

// Nebula: large asymmetric multi-center soft glow
static float OrbNebula(float nx, float ny)
{
    float g1 = PGaussian(std::sqrt((nx - .1f) * (nx - .1f) + (ny + .05f) * (ny + .05f)), .35f);
    float g2 = PGaussian(std::sqrt((nx + .12f) * (nx + .12f) + (ny - .1f) * (ny - .1f)), .3f);
    float g3 = PGaussian(std::sqrt((nx - .05f) * (nx - .05f) + (ny + .15f) * (ny + .15f)), .25f);
    float g4 = PGaussian(std::sqrt((nx + .08f) * (nx + .08f) + (ny + .08f) * (ny + .08f)), .28f);
    float g5 = PGaussian(std::sqrt((nx - .15f) * (nx - .15f) + (ny - .08f) * (ny - .08f)), .22f);
    float g6 = PGaussian(std::sqrt((nx + .05f) * (nx + .05f) + (ny - .15f) * (ny - .15f)), .2f);
    // Soft average blend for nebula-like diffusion
    float combined = (g1 + g2 + g3 + g4 + g5 + g6) / 6.0f;
    float r = std::sqrt(nx * nx + ny * ny);
    float highlight = (std::max)(g1, (std::max)(g2, g3)) * .2f;
    return std::clamp(combined * .7f + highlight + PGaussian(r, .06f) * .15f, .0f, 1.0f);
}

// ---- Crystals ----

// Hexagonal outline with internal facet lines and center glow
static float CrystalHex(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Hexagonal distance (max of 3 axes)
    float h1 = std::abs(ny);
    float h2 = std::abs(nx * .866f + ny * .5f);
    float h3 = std::abs(nx * .866f - ny * .5f);
    float hexDist = (std::max)(h1, (std::max)(h2, h3));
    // Hex outline
    float hexOutline = PGaussian(std::abs(hexDist - .45f), .025f) * .8f;
    // Internal facet lines (3 axes through center)
    float facets = .0f;
    for (int i = 0; i < 3; ++i)
    {
        float a = static_cast<float>(i) * PROC_PI / 3.0f;
        float py = -nx * std::sin(a) + ny * std::cos(a);
        facets = (std::max)(facets, PLineAlpha(std::abs(py), .012f, .01f) * PGaussian(r, .4f));
    }
    float core = PGaussian(r, .08f) * .5f;
    float glow = PGaussian(r, .4f) * .1f;
    return std::clamp(hexOutline + facets * .3f + core + glow, .0f, 1.0f);
}

// Elongated angular crystal shard (pointed top/bottom)
static float CrystalShard(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // Elongated diamond shape: pointed top and bottom, angled sides
    float s = .6f;
    float shardW = .3f;
    // Diamond with different aspect ratio for elongation
    float shardDist = std::abs(nx) / shardW + std::abs(ny) / s;
    float shape = PSmoothstep(1.1f, .85f, shardDist);
    // Edge highlight
    float edge = PGaussian(std::abs(shardDist - 1.0f), .06f) * .4f;
    // Internal facet line (vertical)
    float facet = PLineAlpha(std::abs(nx), .01f, .008f) * PSmoothstep(s, .0f, std::abs(ny)) * .25f;
    float core = PGaussian(r, .1f) * .5f;
    return std::clamp(shape * .4f + edge + facet + core + PGaussian(r, .4f) * .08f, .0f, 1.0f);
}

// Triangular prism cross-section with radial lines to center
static float CrystalPrism(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    float s = .5f;
    // Equilateral triangle outline
    float e1 = PLineAlpha(PLineDist(nx, ny, .0f, -s, -s * .866f, s * .5f), .025f, .02f);
    float e2 = PLineAlpha(PLineDist(nx, ny, -s * .866f, s * .5f, s * .866f, s * .5f), .025f, .02f);
    float e3 = PLineAlpha(PLineDist(nx, ny, s * .866f, s * .5f, .0f, -s), .025f, .02f);
    float tri = (std::max)(e1, (std::max)(e2, e3));
    // Radial lines from center to each vertex
    float rd1 = PLineDist(nx, ny, 0.0f, 0.0f, 0.0f, -s);
    float rd2 = PLineDist(nx, ny, 0.0f, 0.0f, -s * .866f, s * .5f);
    float rd3 = PLineDist(nx, ny, 0.0f, 0.0f, s * .866f, s * .5f);
    float radials =
        (std::max)(PLineAlpha(rd1, .01f, .01f),
                   (std::max)(PLineAlpha(rd2, .01f, .01f), PLineAlpha(rd3, .01f, .01f))) *
        .35f;
    float core = PGaussian(r, .07f) * .5f;
    return std::clamp(tri * .8f + radials + core + PGaussian(r, .4f) * .08f, .0f, 1.0f);
}

// Crystal cluster: multiple overlapping small diamond shapes
static float CrystalCluster(float nx, float ny)
{
    float r = std::sqrt(nx * nx + ny * ny);
    // 4 small overlapping diamonds at offset positions
    float cluster = .0f;
    float offsets[][2] = {{-.12f, -.15f}, {.15f, -.08f}, {-.08f, .12f}, {.1f, .15f}};
    float sizes[] = {.28f, .22f, .25f, .2f};
    float rotations[] = {.0f, .3f, -.2f, .5f};
    for (int i = 0; i < 4; ++i)
    {
        float ox = nx - offsets[i][0], oy = ny - offsets[i][1];
        // Rotate
        float ca = std::cos(rotations[i]), sa = std::sin(rotations[i]);
        float rx = ox * ca - oy * sa, ry = ox * sa + oy * ca;
        float diam = std::abs(rx) + std::abs(ry);
        float shape = PSmoothstep(sizes[i] + .05f, sizes[i] - .02f, diam);
        float edge = PGaussian(std::abs(diam - sizes[i]), .03f) * .4f;
        cluster = (std::max)(cluster, shape * .4f + edge);
    }
    float core = PGaussian(r, .08f) * .3f;
    return std::clamp(cluster + core + PGaussian(r, .4f) * .06f, .0f, 1.0f);
}

// Generator function arrays indexed by style
static const PPixelFn kStarGens[] = {
    Star4Cross, Star6Point, StarDiamond, StarFlare, StarCompass, StarPinwheel, StarNova};
static const PPixelFn kSparkGens[] = {
    SparkEmber, SparkFlash, SparkShard, SparkComet, SparkCrackle, SparkFirefly};
static const PPixelFn kWispGens[] = {WispCrescent,
                                     WispFernCurl,
                                     WispTendril,
                                     WispSpiral,
                                     WispMoth,
                                     WispDewdrop,
                                     WispFirefly,
                                     WispPollenDrift};
static const PPixelFn kRuneGens[] = {
    RuneWard, RuneTriangle, RuneDiamond, RuneRings, RunePentagram, RuneEye};
static const PPixelFn kOrbGens[] = {OrbGaussian, OrbRinged, OrbHalo, OrbPulsar, OrbNebula};
static const PPixelFn kCrystalGens[] = {CrystalHex, CrystalShard, CrystalPrism, CrystalCluster};

struct GenArray
{
    const PPixelFn* fns;
    int count;
};

static const GenArray kAllGens[NUM_TYPES] = {
    {kStarGens, 7},     // Stars
    {kSparkGens, 6},    // Sparks
    {kWispGens, 8},     // Wisps
    {kRuneGens, 6},     // Runes
    {kOrbGens, 5},      // Orbs
    {kCrystalGens, 4},  // Crystals
};

// Create a D3D11 texture from a pixel generator function (white RGBA with computed alpha).
static TextureInfo CreateProceduralTexture(ID3D11Device* device, PPixelFn generator)
{
    TextureInfo info;
    if (!device || !generator)
    {
        return info;
    }

    const int size = PROC_SIZE;
    const UINT stride = static_cast<UINT>(size) * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(size) * size * 4);

    for (int y = 0; y < size; ++y)
    {
        float ny = 2.0f * static_cast<float>(y) / static_cast<float>(size - 1) - 1.0f;
        for (int x = 0; x < size; ++x)
        {
            float nx = 2.0f * static_cast<float>(x) / static_cast<float>(size - 1) - 1.0f;
            float alpha = std::clamp(generator(nx, ny), .0f, 1.0f);

            // Universal circular mask: forces alpha to zero at the quad boundary so
            // particles never clip as visible squares under additive blending.
            float mr = std::sqrt(nx * nx + ny * ny);
            alpha *= PSmoothstep(1.0f, .7f, mr);

            size_t idx = (static_cast<size_t>(y) * size + x) * 4;
            pixels[idx + 0] = 255;
            pixels[idx + 1] = 255;
            pixels[idx + 2] = 255;
            pixels[idx + 3] = static_cast<BYTE>(alpha * 255.0f);
        }
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(size);
    texDesc.Height = static_cast<UINT>(size);
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
    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, &texture);
    if (FAILED(hr))
    {
        return info;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(
        texture.Get(), &srvDesc, info.srv.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        return info;
    }

    info.width = size;
    info.height = size;
    return info;
}

// Generate all procedural textures for a particle style.
static int GenerateProceduralTextures(ID3D11Device* device, int styleIndex)
{
    if (styleIndex < 0 || styleIndex >= NUM_TYPES)
    {
        return 0;
    }

    const auto& gens = kAllGens[styleIndex];
    int loaded = 0;

    for (int i = 0; i < gens.count; ++i)
    {
        auto info = CreateProceduralTexture(device, gens.fns[i]);
        if (info.srv)
        {
            Textures()[styleIndex].push_back(std::move(info));
            ++loaded;
        }
    }

    if (loaded > 0)
    {
        static const char* kStyleNames[] = {
            "stars", "sparks", "wisps", "runes", "orbs", "crystals"};
        const char* name =
            (styleIndex >= 0 && styleIndex < NUM_TYPES) ? kStyleNames[styleIndex] : "?";
        SKSE::log::info("ParticleTextures: [{}] generated {} procedural textures", name, loaded);
    }

    return loaded;
}

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

    // Try to load user-provided textures from folders (overrides procedural)
    int totalLoaded = 0;
    {
        ComScope com;
        if (com.usable)
        {
            ComPtr<IWICImagingFactory> wicFactory;
            HRESULT wicHr = CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
            if (SUCCEEDED(wicHr))
            {
                const std::string basePath = "Data/SKSE/Plugins/glyph/particles/";

                struct FolderMapping
                {
                    int style;
                    const char* folder;
                };

                FolderMapping mappings[] = {
                    {0, "stars"},
                    {1, "sparks"},
                    {2, "wisps"},
                    {3, "runes"},
                    {4, "orbs"},
                    {5, "crystals"},
                };

                for (const auto& m : mappings)
                {
                    std::string folderPath = basePath + m.folder;
                    int count =
                        LoadTexturesFromFolder(device, m.style, folderPath, wicFactory.Get());
                    if (count > 0)
                    {
                        SKSE::log::info("ParticleTextures: [{}] loaded {} user textures from {}",
                                        m.folder,
                                        count,
                                        folderPath);
                        totalLoaded += count;
                    }
                }
            }
            else
            {
                SKSE::log::warn(
                    "ParticleTextures: WIC unavailable (hr=0x{:08X}), skipping file textures",
                    static_cast<unsigned int>(wicHr));
            }
        }
    }

    // Generate procedural textures for any style that has no user-provided textures
    for (int i = 0; i < NUM_TYPES; ++i)
    {
        if (Textures()[i].empty())
        {
            totalLoaded += GenerateProceduralTextures(device, i);
        }
    }

    s_Initialized.store(totalLoaded > 0, std::memory_order_release);
    SKSE::log::info("ParticleTextures: === TOTAL: {} particle textures ready ===", totalLoaded);
    if (!s_Initialized.load(std::memory_order_acquire))
    {
        SKSE::log::error(
            "ParticleTextures: NO TEXTURES AVAILABLE - falling back to shape rendering");
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

void PushScreenBlend(ImDrawList* dl)
{
    if (dl && s_ScreenBlend)
    {
        dl->AddCallback(SetBlendCallback, s_ScreenBlend.Get());
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
