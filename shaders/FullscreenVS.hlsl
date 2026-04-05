// Fullscreen triangle vertex shader.
// Generates a screen-covering triangle from SV_VertexID (0,1,2)
// without any vertex buffer.  The oversized triangle is clipped
// to the viewport automatically by the rasterizer.

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    // id 0 -> (-1, -1)  uv (0, 1)
    // id 1 -> (-1,  3)  uv (0,-1)
    // id 2 -> ( 3, -1)  uv (2, 1)
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
