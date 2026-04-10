#include "PCH.hpp"

#include "DepthClip.hpp"

#include <SKSE/SKSE.h>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <deque>

using Microsoft::WRL::ComPtr;

namespace DepthClip
{
namespace
{
// Pixel shader: ImGui's stock shader plus a feathered scene-depth compare.
// The plate's viewport-space depth comes from the same projection the game
// rasterized with, so a direct compare against the depth buffer is exact by
// construction.  Five taps at the feather radius soften the intersection
// edge so the world appears to pass in front of the type rather than
// shearing it.  polarity 0 disables the test (neutral params).
constexpr const char* kDepthClipPS = R"(
cbuffer DepthClipCB : register(b0)
{
    float plateDepth;
    float featherPx;
    float polarity;
    float _pad0;
    float2 invViewport;
    float2 _pad1;
};
Texture2D texture0 : register(t0);
Texture2D<float> sceneDepth : register(t1);
SamplerState sampler0 : register(s0);

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

float VisAt(float2 uv)
{
    float scene = sceneDepth.SampleLevel(sampler0, uv, 0);
    return ((scene - plateDepth) * polarity >= 0.0f) ? 1.0f : 0.0f;
}

float4 main(PS_INPUT input) : SV_Target
{
    float4 col = input.col * texture0.Sample(sampler0, input.uv);
    if (polarity != 0.0f)
    {
        float2 uv = input.pos.xy * invViewport;
        float2 o = featherPx * invViewport;
        float vis = VisAt(uv) * 2.0f
                  + VisAt(uv + float2( o.x,  o.y))
                  + VisAt(uv + float2(-o.x,  o.y))
                  + VisAt(uv + float2( o.x, -o.y))
                  + VisAt(uv + float2(-o.x, -o.y));
        col.a *= vis / 6.0f;
    }
    return col;
}
)";

struct CBData
{
    float plateDepth = 1.0f;
    float featherPx = 2.5f;
    float polarity = .0f;
    float pad0 = .0f;
    float invViewport[2] = {};
    float pad1[2] = {};
};

struct PlateParams
{
    float depth = 1.0f;
    bool neutral = false;
};

ComPtr<ID3D11Device> s_Device;
ComPtr<ID3D11DeviceContext> s_Context;
ComPtr<ID3D11PixelShader> s_PS;
ComPtr<ID3D11Buffer> s_CB;

// Per-frame state (render thread only).
ID3D11ShaderResourceView* s_DepthSRV = nullptr;  // borrowed from the game
float s_FeatherPx = 2.5f;
float s_Polarity = .0f;
std::deque<PlateParams> s_ParamArena;  // deque: stable addresses across push_back

bool s_Initialized = false;
bool s_SrvWarned = false;
}  // namespace

bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context)
    {
        return false;
    }

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kDepthClipPS,
                            strlen(kDepthClipPS),
                            nullptr,
                            nullptr,
                            nullptr,
                            "main",
                            "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0,
                            blob.GetAddressOf(),
                            errors.GetAddressOf());
    if (FAILED(hr))
    {
        if (errors)
        {
            logger::warn("DepthClip: shader compile failed: {}",
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }

    hr = device->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, s_PS.GetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(CBData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cbDesc, nullptr, s_CB.GetAddressOf());
    if (FAILED(hr))
    {
        s_PS.Reset();
        return false;
    }

    s_Device = device;
    s_Context = context;
    s_Initialized = true;
    s_SrvWarned = false;
    logger::info("DepthClip: initialized (per-pixel depth occlusion available)");
    return true;
}

bool IsInitialized()
{
    return s_Initialized;
}

void Shutdown()
{
    s_PS.Reset();
    s_CB.Reset();
    s_Context.Reset();
    s_Device.Reset();
    s_DepthSRV = nullptr;
    s_ParamArena.clear();
    s_Initialized = false;
    s_SrvWarned = false;
}

bool BeginFrame(float featherPx, float polarity)
{
    s_DepthSRV = nullptr;
    s_ParamArena.clear();
    if (!s_Initialized || polarity == .0f)
    {
        return false;
    }

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
    {
        return false;
    }

    // Only the post-Z-prepass COPY is safe to sample: the live kMAIN depth
    // may still be bound as a DSV during UI rendering, and D3D silently
    // nulls a conflicting SRV binding -- which would read as depth 0 and
    // make every plate invisible.  If the copy is absent (some ENB or
    // upscaler stacks), the frame simply goes unclipped and the existing
    // LOS culling remains the only occlusion.
    auto& data = renderer->data;
    s_DepthSRV = data.depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
    if (!s_DepthSRV)
    {
        if (!s_SrvWarned)
        {
            s_SrvWarned = true;
            logger::warn(
                "DepthClip: no scene depth SRV available -- falling back to LOS-only occlusion");
        }
        return false;
    }

    s_FeatherPx = std::clamp(featherPx, .0f, 8.0f);
    s_Polarity = polarity;
    return true;
}

void* MakePlateParams(float plateDepthNDC)
{
    s_ParamArena.push_back({plateDepthNDC, false});
    return &s_ParamArena.back();
}

void* MakeNeutralParams()
{
    s_ParamArena.push_back({1.0f, true});
    return &s_ParamArena.back();
}

void ApplyCallback(const ImDrawList* /*dl*/, const ImDrawCmd* cmd)
{
    if (!s_Context || !s_PS || !s_CB || !s_DepthSRV || !cmd || !cmd->UserCallbackData)
    {
        return;
    }
    const auto* params = static_cast<const PlateParams*>(cmd->UserCallbackData);

    // The active viewport tells us the pixel->uv mapping for whichever
    // channel is executing (full-res backbuffer, full-res divide RT, or the
    // half-res glow capture) -- SV_Position is always in that space.
    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    s_Context->RSGetViewports(&vpCount, &vp);
    if (vpCount == 0 || vp.Width <= .0f || vp.Height <= .0f)
    {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(s_Context->Map(s_CB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        return;
    }
    auto* cb = static_cast<CBData*>(mapped.pData);
    cb->plateDepth = params->depth;
    cb->featherPx = s_FeatherPx;
    cb->polarity = params->neutral ? .0f : s_Polarity;
    cb->pad0 = .0f;
    cb->invViewport[0] = 1.0f / vp.Width;
    cb->invViewport[1] = 1.0f / vp.Height;
    cb->pad1[0] = .0f;
    cb->pad1[1] = .0f;
    s_Context->Unmap(s_CB.Get(), 0);

    s_Context->PSSetShader(s_PS.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = {s_CB.Get()};
    s_Context->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[1] = {s_DepthSRV};
    s_Context->PSSetShaderResources(1, 1, srvs);
}
}  // namespace DepthClip
