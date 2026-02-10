// Color Divide composite pixel shader.
// Reads the captured nametag text (t0) and the pre-nametag backbuffer
// snapshot (t1), then blends between normal alpha compositing and
// Photoshop-style Color Divide (background / foreground).
//
// Dark pixels (outlines, shadows) are composited normally via a
// luminance-based smoothstep mask to prevent bright inversions.

Texture2D    TextRT   : register(t0);
Texture2D    Snapshot : register(t1);
SamplerState Sampler  : register(s0);

cbuffer DivideCB : register(b0)
{
    float Strength;   // 0 = normal composite, 1 = full divide
    float3 _Pad;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 text = TextRT.Sample(Sampler, uv);
    float4 bg   = Snapshot.Sample(Sampler, uv);

    if (text.a < 0.001)
        discard;

    // Luminance of the text pixel - dark outlines/shadows are composited
    // normally so they stay clean instead of producing bright inversions.
    float lum = dot(text.rgb, float3(0.299, 0.587, 0.114));
    float divideMask = smoothstep(0.10, 0.25, lum);

    // Divide: bg / text_color (clamped to [0,1])
    float3 divided = float3(
        text.r > 0.001 ? saturate(bg.r / text.r) : 1.0,
        text.g > 0.001 ? saturate(bg.g / text.g) : 1.0,
        text.b > 0.001 ? saturate(bg.b / text.b) : 1.0
    );

    // Normal alpha composite as fallback for dark pixels
    float3 normal = lerp(bg.rgb, text.rgb, text.a);

    // Blend: dark pixels use normal composite, bright pixels use divide
    float effectiveStrength = Strength * divideMask;
    float3 result = lerp(normal, divided, effectiveStrength);
    return float4(result, 1.0);
}
