#include <SKSE/SKSE.h>

#include "BadgeTextures.hpp"

#include <nanosvg.h>
#include <nanosvgrast.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace BadgeTextures
{
using Microsoft::WRL::ComPtr;

namespace
{
// Rasterization canvas. Power of two so the mip chain halves cleanly.
constexpr int BADGE_TEX_SIZE = 128;

std::mutex& Mutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>>& Map()
{
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> m;
    return m;
}

std::atomic<bool>& Initialized()
{
    static std::atomic<bool> b{false};
    return b;
}

// Rasterize <path> into a centered square float-alpha mask of BADGE_TEX_SIZE.
// The SVG's own layer opacities (duotone secondary .4, primary 1.0) land in
// the alpha; icons whose strongest layer is translucent are normalized so
// their peak alpha is 1.0.
bool RasterizeIcon(NSVGrasterizer* rast, const std::string& path, std::vector<float>& outAlpha)
{
    NSVGimage* img = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!img)
    {
        return false;
    }
    if (img->width <= .0f || img->height <= .0f)
    {
        nsvgDelete(img);
        return false;
    }

    const int size = BADGE_TEX_SIZE;
    const float maxDim = (img->width > img->height) ? img->width : img->height;
    const float scale = static_cast<float>(size) / maxDim;
    const float tx = (static_cast<float>(size) - img->width * scale) * .5f;
    const float ty = (static_cast<float>(size) - img->height * scale) * .5f;

    std::vector<unsigned char> rgba(static_cast<size_t>(size) * size * 4, 0);
    nsvgRasterize(rast, img, tx, ty, scale, rgba.data(), size, size, size * 4);
    nsvgDelete(img);

    outAlpha.resize(static_cast<size_t>(size) * size);
    float maxAlpha = .0f;
    for (size_t i = 0; i < outAlpha.size(); ++i)
    {
        outAlpha[i] = static_cast<float>(rgba[i * 4 + 3]) * (1.0f / 255.0f);
        maxAlpha = (std::max)(maxAlpha, outAlpha[i]);
    }
    if (maxAlpha <= .0f)
    {
        return false;
    }
    if (maxAlpha < 1.0f)
    {
        const float norm = 1.0f / maxAlpha;
        for (float& a : outAlpha)
        {
            a = (std::min)(1.0f, a * norm);
        }
    }
    return true;
}

// Create a mipmapped white-RGBA texture from a float alpha mask (same
// pipeline as the procedural particle sprites: 2x2 box reduction in float,
// black RGB on fully transparent texels).
ComPtr<ID3D11ShaderResourceView> CreateBadgeTexture(ID3D11Device* device,
                                                    std::vector<float> levelAlpha)
{
    const int size = BADGE_TEX_SIZE;
    int mipLevels = 1;
    for (int d = size; d > 1; d >>= 1)
    {
        ++mipLevels;
    }

    std::vector<std::vector<BYTE>> mipPixels(static_cast<size_t>(mipLevels));
    std::vector<D3D11_SUBRESOURCE_DATA> initData(static_cast<size_t>(mipLevels));
    int dim = size;
    for (int level = 0; level < mipLevels; ++level)
    {
        if (level > 0)
        {
            const int prevDim = dim;
            dim = (std::max)(1, dim >> 1);
            std::vector<float> reduced(static_cast<size_t>(dim) * dim);
            for (int y = 0; y < dim; ++y)
            {
                for (int x = 0; x < dim; ++x)
                {
                    const size_t i00 =
                        static_cast<size_t>(y) * 2 * prevDim + static_cast<size_t>(x) * 2;
                    const size_t i10 = i00 + prevDim;
                    reduced[static_cast<size_t>(y) * dim + x] =
                        (levelAlpha[i00] + levelAlpha[i00 + 1] + levelAlpha[i10] +
                         levelAlpha[i10 + 1]) *
                        .25f;
                }
            }
            levelAlpha = std::move(reduced);
        }

        auto& pixels = mipPixels[level];
        pixels.resize(static_cast<size_t>(dim) * dim * 4);
        for (int i = 0; i < dim * dim; ++i)
        {
            const float a = levelAlpha[static_cast<size_t>(i)];
            const BYTE alphaByte =
                static_cast<BYTE>(std::clamp(static_cast<int>(a * 255.0f + .5f), 0, 255));
            const BYTE rgb = (alphaByte == 0) ? 0 : 255;
            pixels[static_cast<size_t>(i) * 4 + 0] = rgb;
            pixels[static_cast<size_t>(i) * 4 + 1] = rgb;
            pixels[static_cast<size_t>(i) * 4 + 2] = rgb;
            pixels[static_cast<size_t>(i) * 4 + 3] = alphaByte;
        }
        initData[level].pSysMem = pixels.data();
        initData[level].SysMemPitch = static_cast<UINT>(dim) * 4;
        initData[level].SysMemSlicePitch = 0;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(size);
    texDesc.Height = static_cast<UINT>(size);
    texDesc.MipLevels = static_cast<UINT>(mipLevels);
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&texDesc, initData.data(), &texture)))
    {
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(mipLevels);

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf())))
    {
        return nullptr;
    }
    return srv;
}
}  // namespace

bool Initialize(ID3D11Device* device,
                const std::string& folder,
                const std::vector<std::string>& names)
{
    const std::lock_guard<std::mutex> lock(Mutex());
    Map().clear();

    if (!device || folder.empty())
    {
        Initialized().store(true, std::memory_order_release);
        return false;
    }

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast)
    {
        Initialized().store(true, std::memory_order_release);
        return false;
    }

    int loaded = 0;
    int requested = 0;
    for (const auto& name : names)
    {
        if (name.empty() || Map().contains(name))
        {
            continue;
        }
        ++requested;

        const std::string path = folder + "/" + name + ".svg";
        std::vector<float> alpha;
        if (!RasterizeIcon(rast, path, alpha))
        {
            SKSE::log::warn("BadgeTextures: failed to load '{}'", path);
            continue;
        }
        auto srv = CreateBadgeTexture(device, std::move(alpha));
        if (!srv)
        {
            SKSE::log::warn("BadgeTextures: texture creation failed for '{}'", path);
            continue;
        }
        Map().emplace(name, std::move(srv));
        ++loaded;
    }
    nsvgDeleteRasterizer(rast);

    SKSE::log::info("BadgeTextures: loaded {}/{} badge icons from '{}'", loaded, requested, folder);
    Initialized().store(true, std::memory_order_release);
    return loaded > 0;
}

bool IsInitialized()
{
    return Initialized().load(std::memory_order_acquire);
}

void Shutdown()
{
    const std::lock_guard<std::mutex> lock(Mutex());
    Map().clear();
    Initialized().store(false, std::memory_order_release);
}

ImTextureID Get(const std::string& name)
{
    const std::lock_guard<std::mutex> lock(Mutex());
    auto it = Map().find(name);
    if (it == Map().end() || !it->second)
    {
        return 0;
    }
    return reinterpret_cast<ImTextureID>(it->second.Get());
}
}  // namespace BadgeTextures
