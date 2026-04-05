// Separable 1-D Gaussian blur pixel shader.
// Run twice (horizontal then vertical) with ping-pong render targets.

Texture2D    InputTex : register(t0);
SamplerState Sampler  : register(s0);

cbuffer BlurCB : register(b0)
{
    float2 TexelDir;  // (1/w, 0) for H pass or (0, 1/h) for V pass
    float  Sigma;     // Gaussian sigma in texels
    float  _Pad;
};

// 7-tap kernel: center + 3 on each side.
// Weights are computed analytically from sigma so the radius
// adapts to the configured glow spread.
static const int HALF_KERNEL = 3;

float Gauss(float x, float s)
{
    return exp(-0.5 * (x * x) / (s * s));
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float sigma = max(Sigma, 0.001);

    float4 sum   = InputTex.Sample(Sampler, uv) * Gauss(0, sigma);
    float  wSum  = Gauss(0, sigma);

    [unroll]
    for (int i = 1; i <= HALF_KERNEL; ++i)
    {
        float w = Gauss((float)i, sigma);
        sum  += InputTex.Sample(Sampler, uv + TexelDir * (float)i) * w;
        sum  += InputTex.Sample(Sampler, uv - TexelDir * (float)i) * w;
        wSum += w * 2.0;
    }

    return sum / wSum;
}
