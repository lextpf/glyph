#include <SKSE/SKSE.h>

#include "TextPostProcess.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
namespace logger = SKSE::log;

namespace TextPostProcess
{

// ============================================================================
// Embedded HLSL sources
// ============================================================================

static const char* kFullscreenVS_HLSL = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

static const char* kGaussianBlurPS_HLSL = R"(
Texture2D    InputTex : register(t0);
SamplerState Sampler  : register(s0);
cbuffer BlurCB : register(b0) {
    float2 TexelDir;
    float  Sigma;
    float  _Pad;
};
static const int HALF_KERNEL = 3;
float Gauss(float x, float s) { return exp(-0.5 * (x * x) / (s * s)); }
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float sigma = max(Sigma, 0.001);
    float4 sum  = InputTex.Sample(Sampler, uv) * Gauss(0, sigma);
    float  wSum = Gauss(0, sigma);
    [unroll] for (int i = 1; i <= HALF_KERNEL; ++i) {
        float w = Gauss((float)i, sigma);
        sum  += InputTex.Sample(Sampler, uv + TexelDir * (float)i) * w;
        sum  += InputTex.Sample(Sampler, uv - TexelDir * (float)i) * w;
        wSum += w * 2.0;
    }
    return sum / wSum;
}
)";

static const char* kCompositePS_HLSL = R"(
Texture2D    InputTex : register(t0);
SamplerState Sampler  : register(s0);
cbuffer CompositeCB : register(b0) {
    float Intensity;
    float3 _Pad;
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = InputTex.Sample(Sampler, uv);
    return float4(c.rgb * Intensity, c.a * Intensity);
}
)";

// ============================================================================
// Constant buffer layouts
// ============================================================================

struct BlurConstants
{
    float texelDirX, texelDirY;
    float sigma;
    float pad;
};

struct CompositeConstants
{
    float intensity;
    float pad[3];
};

// ============================================================================
// File-scope resources
// ============================================================================

static std::atomic<bool> s_Initialized{false};
static std::mutex& InitMutex()
{
    static std::mutex m;
    return m;
}

static ComPtr<ID3D11Device> s_Device;
static ComPtr<ID3D11DeviceContext> s_Context;

// Ping-pong render targets (half-res)
static ComPtr<ID3D11Texture2D> s_RT_A;
static ComPtr<ID3D11RenderTargetView> s_RTV_A;
static ComPtr<ID3D11ShaderResourceView> s_SRV_A;
static ComPtr<ID3D11Texture2D> s_RT_B;
static ComPtr<ID3D11RenderTargetView> s_RTV_B;
static ComPtr<ID3D11ShaderResourceView> s_SRV_B;

// Shaders
static ComPtr<ID3D11VertexShader> s_FullscreenVS;
static ComPtr<ID3D11PixelShader> s_BlurPS;
static ComPtr<ID3D11PixelShader> s_CompositePS;

// Pipeline objects
static ComPtr<ID3D11Buffer> s_BlurCB;
static ComPtr<ID3D11Buffer> s_CompositeCB;
static ComPtr<ID3D11SamplerState> s_LinearClampSampler;
static ComPtr<ID3D11BlendState> s_AdditiveBlend;
static ComPtr<ID3D11RasterizerState> s_NoCullRS;

// Saved main RT (between Begin/End callbacks)
static ComPtr<ID3D11RenderTargetView> s_SavedRTV;
static ComPtr<ID3D11DepthStencilView> s_SavedDSV;
static D3D11_VIEWPORT s_SavedViewport{};
static UINT s_SavedViewportCount = 0;

// RT dimensions
static uint32_t s_HalfWidth = 0;
static uint32_t s_HalfHeight = 0;
static uint32_t s_FullWidth = 0;
static uint32_t s_FullHeight = 0;

// Glow params (set per-frame)
static float s_GlowRadius = 4.0f;
static float s_GlowIntensity = .5f;

// ============================================================================
// Helpers
// ============================================================================

static ComPtr<ID3DBlob> CompileShader(const char* source, const char* target, const char* entry)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source,
                            strlen(source),
                            nullptr,
                            nullptr,
                            nullptr,
                            entry,
                            target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0,
                            blob.GetAddressOf(),
                            errors.GetAddressOf());
    if (FAILED(hr))
    {
        if (errors)
        {
            logger::error("TextPostProcess: Shader compile failed: {}",
                          static_cast<const char*>(errors->GetBufferPointer()));
        }
        return nullptr;
    }
    return blob;
}

static bool CreateRenderTarget(uint32_t w,
                               uint32_t h,
                               ComPtr<ID3D11Texture2D>& tex,
                               ComPtr<ID3D11RenderTargetView>& rtv,
                               ComPtr<ID3D11ShaderResourceView>& srv)
{
    tex.Reset();
    rtv.Reset();
    srv.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = s_Device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
    if (FAILED(hr))
        return false;

    hr = s_Device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr))
        return false;

    hr = s_Device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
    if (FAILED(hr))
        return false;

    return true;
}

static void ReleaseResources()
{
    s_RT_A.Reset();
    s_RTV_A.Reset();
    s_SRV_A.Reset();
    s_RT_B.Reset();
    s_RTV_B.Reset();
    s_SRV_B.Reset();
    s_FullscreenVS.Reset();
    s_BlurPS.Reset();
    s_CompositePS.Reset();
    s_BlurCB.Reset();
    s_CompositeCB.Reset();
    s_LinearClampSampler.Reset();
    s_AdditiveBlend.Reset();
    s_NoCullRS.Reset();
    s_SavedRTV.Reset();
    s_SavedDSV.Reset();
    s_Context.Reset();
    s_Device.Reset();
    s_HalfWidth = 0;
    s_HalfHeight = 0;
    s_FullWidth = 0;
    s_FullHeight = 0;
    s_Initialized = false;
}

// Draw a fullscreen triangle (no vertex buffer, 3 vertices from SV_VertexID)
static void DrawFullscreenTriangle()
{
    s_Context->IASetInputLayout(nullptr);
    s_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_Context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    s_Context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
    s_Context->Draw(3, 0);
}

// ============================================================================
// Public API
// ============================================================================

bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    const std::lock_guard<std::mutex> lock(InitMutex());
    if (s_Initialized.load(std::memory_order_acquire))
        return true;

    if (!device || !context)
        return false;

    s_Device = device;
    s_Context = context;

    // Compile shaders
    auto vsBlob = CompileShader(kFullscreenVS_HLSL, "vs_5_0", "main");
    auto blurBlob = CompileShader(kGaussianBlurPS_HLSL, "ps_5_0", "main");
    auto compositeBlob = CompileShader(kCompositePS_HLSL, "ps_5_0", "main");

    if (!vsBlob || !blurBlob || !compositeBlob)
    {
        logger::error("TextPostProcess: Failed to compile one or more shaders");
        ReleaseResources();
        return false;
    }

    HRESULT hr;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                    vsBlob->GetBufferSize(),
                                    nullptr,
                                    s_FullscreenVS.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("TextPostProcess: Failed to create vertex shader");
        ReleaseResources();
        return false;
    }

    hr = device->CreatePixelShader(
        blurBlob->GetBufferPointer(), blurBlob->GetBufferSize(), nullptr, s_BlurPS.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("TextPostProcess: Failed to create blur pixel shader");
        ReleaseResources();
        return false;
    }

    hr = device->CreatePixelShader(compositeBlob->GetBufferPointer(),
                                   compositeBlob->GetBufferSize(),
                                   nullptr,
                                   s_CompositePS.GetAddressOf());
    if (FAILED(hr))
    {
        logger::error("TextPostProcess: Failed to create composite pixel shader");
        ReleaseResources();
        return false;
    }

    // Constant buffers
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(BlurConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cbDesc, nullptr, s_BlurCB.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    cbDesc.ByteWidth = sizeof(CompositeConstants);
    hr = device->CreateBuffer(&cbDesc, nullptr, s_CompositeCB.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    // Linear clamp sampler
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxAnisotropy = 1;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampDesc, s_LinearClampSampler.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    // Additive blend state
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blendDesc, s_AdditiveBlend.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    // No-cull rasterizer state for fullscreen triangle
    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.ScissorEnable = FALSE;
    rsDesc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rsDesc, s_NoCullRS.GetAddressOf());
    if (FAILED(hr))
    {
        ReleaseResources();
        return false;
    }

    s_Initialized.store(true, std::memory_order_release);
    logger::info("TextPostProcess: Initialized (shaders compiled, pipeline ready)");
    return true;
}

bool IsInitialized()
{
    return s_Initialized.load(std::memory_order_acquire);
}

void Shutdown()
{
    const std::lock_guard<std::mutex> lock(InitMutex());
    if (!s_Initialized.load(std::memory_order_acquire))
        return;
    logger::info("TextPostProcess: Shutting down");
    ReleaseResources();
}

void OnResize(uint32_t width, uint32_t height)
{
    if (!s_Initialized.load(std::memory_order_acquire))
        return;
    if (width == 0 || height == 0)
        return;
    if (width == s_FullWidth && height == s_FullHeight)
        return;

    s_FullWidth = width;
    s_FullHeight = height;
    s_HalfWidth = (std::max)(width / 2u, 1u);
    s_HalfHeight = (std::max)(height / 2u, 1u);

    bool ok = CreateRenderTarget(s_HalfWidth, s_HalfHeight, s_RT_A, s_RTV_A, s_SRV_A) &&
              CreateRenderTarget(s_HalfWidth, s_HalfHeight, s_RT_B, s_RTV_B, s_SRV_B);
    if (!ok)
    {
        logger::error(
            "TextPostProcess: Failed to create render targets ({}x{})", s_HalfWidth, s_HalfHeight);
    }
    else
    {
        logger::info("TextPostProcess: Render targets created ({}x{} half-res from {}x{})",
                     s_HalfWidth,
                     s_HalfHeight,
                     width,
                     height);
    }
}

void SetGlowParams(float radius, float intensity)
{
    s_GlowRadius = radius;
    s_GlowIntensity = intensity;
}

// ============================================================================
// ImDrawCallbacks
// ============================================================================

void BeginGlowCapture(const ImDrawList* /*dl*/, const ImDrawCmd* /*cmd*/)
{
    if (!s_Context || !s_RTV_A)
        return;

    // Save current render target and viewport
    s_SavedRTV.Reset();
    s_SavedDSV.Reset();
    s_Context->OMGetRenderTargets(1, s_SavedRTV.GetAddressOf(), s_SavedDSV.GetAddressOf());
    s_SavedViewportCount = 1;
    s_Context->RSGetViewports(&s_SavedViewportCount, &s_SavedViewport);

    // Switch to glow RT A (half-res)
    ID3D11RenderTargetView* rtv = s_RTV_A.Get();
    s_Context->OMSetRenderTargets(1, &rtv, nullptr);

    const float clearColor[4] = {0, 0, 0, 0};
    s_Context->ClearRenderTargetView(s_RTV_A.Get(), clearColor);

    // Set half-res viewport so ImGui text maps correctly
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(s_HalfWidth);
    vp.Height = static_cast<float>(s_HalfHeight);
    vp.MaxDepth = 1.0f;
    s_Context->RSSetViewports(1, &vp);
}

void EndGlowAndComposite(const ImDrawList* /*dl*/, const ImDrawCmd* /*cmd*/)
{
    if (!s_Context || !s_RTV_A || !s_RTV_B || !s_SavedRTV)
        return;

    // RT A now contains the glow text rendered at half-res.
    // Apply separable Gaussian blur: A -> B (horizontal), B -> A (vertical).

    const float sigma = s_GlowRadius * .5f;  // radius to sigma

    // Unbind RT A as target, we'll read from it
    ID3D11RenderTargetView* nullRTV = nullptr;
    s_Context->OMSetRenderTargets(1, &nullRTV, nullptr);

    // Common state for blur passes
    s_Context->VSSetShader(s_FullscreenVS.Get(), nullptr, 0);
    s_Context->PSSetShader(s_BlurPS.Get(), nullptr, 0);
    s_Context->RSSetState(s_NoCullRS.Get());

    ID3D11SamplerState* sampler = s_LinearClampSampler.Get();
    s_Context->PSSetSamplers(0, 1, &sampler);

    // Disable blending for blur passes (overwrite)
    const float blendFactor[4] = {0, 0, 0, 0};
    s_Context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);

    D3D11_VIEWPORT halfVP{};
    halfVP.Width = static_cast<float>(s_HalfWidth);
    halfVP.Height = static_cast<float>(s_HalfHeight);
    halfVP.MaxDepth = 1.0f;
    s_Context->RSSetViewports(1, &halfVP);

    // --- Horizontal blur: read A -> write B ---
    {
        const float clearColor[4] = {0, 0, 0, 0};
        s_Context->ClearRenderTargetView(s_RTV_B.Get(), clearColor);

        ID3D11RenderTargetView* rtv = s_RTV_B.Get();
        s_Context->OMSetRenderTargets(1, &rtv, nullptr);

        ID3D11ShaderResourceView* srv = s_SRV_A.Get();
        s_Context->PSSetShaderResources(0, 1, &srv);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(s_Context->Map(s_BlurCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto* cb = static_cast<BlurConstants*>(mapped.pData);
            cb->texelDirX = 1.0f / static_cast<float>(s_HalfWidth);
            cb->texelDirY = 0;
            cb->sigma = sigma;
            cb->pad = 0;
            s_Context->Unmap(s_BlurCB.Get(), 0);
        }

        ID3D11Buffer* cb = s_BlurCB.Get();
        s_Context->PSSetConstantBuffers(0, 1, &cb);
        DrawFullscreenTriangle();

        // Unbind SRV to allow B to be read next
        ID3D11ShaderResourceView* nullSRV = nullptr;
        s_Context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Vertical blur: read B -> write A ---
    {
        const float clearColor[4] = {0, 0, 0, 0};
        s_Context->ClearRenderTargetView(s_RTV_A.Get(), clearColor);

        ID3D11RenderTargetView* rtv = s_RTV_A.Get();
        s_Context->OMSetRenderTargets(1, &rtv, nullptr);

        ID3D11ShaderResourceView* srv = s_SRV_B.Get();
        s_Context->PSSetShaderResources(0, 1, &srv);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(s_Context->Map(s_BlurCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto* cb = static_cast<BlurConstants*>(mapped.pData);
            cb->texelDirX = 0;
            cb->texelDirY = 1.0f / static_cast<float>(s_HalfHeight);
            cb->sigma = sigma;
            cb->pad = 0;
            s_Context->Unmap(s_BlurCB.Get(), 0);
        }

        ID3D11Buffer* cb = s_BlurCB.Get();
        s_Context->PSSetConstantBuffers(0, 1, &cb);
        DrawFullscreenTriangle();

        ID3D11ShaderResourceView* nullSRV = nullptr;
        s_Context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // --- Composite: read blurred A -> main RT, additive blend ---
    {
        // Restore main render target
        ID3D11RenderTargetView* mainRTV = s_SavedRTV.Get();
        s_Context->OMSetRenderTargets(1, &mainRTV, s_SavedDSV.Get());
        s_Context->RSSetViewports(s_SavedViewportCount, &s_SavedViewport);

        s_Context->PSSetShader(s_CompositePS.Get(), nullptr, 0);
        s_Context->OMSetBlendState(s_AdditiveBlend.Get(), blendFactor, 0xFFFFFFFF);

        ID3D11ShaderResourceView* srv = s_SRV_A.Get();
        s_Context->PSSetShaderResources(0, 1, &srv);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(s_Context->Map(s_CompositeCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto* cb = static_cast<CompositeConstants*>(mapped.pData);
            cb->intensity = s_GlowIntensity;
            cb->pad[0] = cb->pad[1] = cb->pad[2] = 0;
            s_Context->Unmap(s_CompositeCB.Get(), 0);
        }

        ID3D11Buffer* cb = s_CompositeCB.Get();
        s_Context->PSSetConstantBuffers(0, 1, &cb);
        DrawFullscreenTriangle();

        // Unbind SRV
        ID3D11ShaderResourceView* nullSRV = nullptr;
        s_Context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Clean up saved state refs
    s_SavedRTV.Reset();
    s_SavedDSV.Reset();

    // ImDrawCallback_ResetRenderState (added after this callback in the draw list)
    // will restore ImGui's own shader/blend/viewport/sampler state.
    // The render target is already restored above.
}

}  // namespace TextPostProcess
