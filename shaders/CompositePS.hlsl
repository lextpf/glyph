// Composite pixel shader.
// Samples the blurred glow texture and outputs it for additive blending.
// The blend state on the D3D11 side controls the blending mode.

Texture2D    InputTex : register(t0);
SamplerState Sampler  : register(s0);

cbuffer CompositeCB : register(b0)
{
    float Intensity;   // Overall glow brightness multiplier
    float3 _Pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = InputTex.Sample(Sampler, uv);
    return float4(c.rgb * Intensity, c.a * Intensity);
}
