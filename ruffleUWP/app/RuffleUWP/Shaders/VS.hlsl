#include "Common.hlsli"
VSOut main(float2 p : POSITION, float4 c : COLOR0)
{
    VSOut o;
    float3 q = float3(p, 1.0);
    float2 px = float2(dot(world0.xyz, q), dot(world1.xyz, q));
    o.pos = float4(px.x * view.x - 1.0, px.y * view.y + 1.0, 0.0, 1.0);
    o.color = c;
    o.uv = float2(dot(uv0.xyz, q), dot(uv1.xyz, q));
    return o;
}
