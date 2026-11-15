#include "Common.hlsli"
float4 main(VSOut i) : SV_Target
{
    float fp = view.w;
    float2 p = i.uv * 2.0 - 1.0;
    float2 d = float2(fp - p.x, -p.y);
    float l = length(d);
    if (l > 0.0) d /= l;
    float t = l / (sqrt(max(1.0 - fp * fp * d.y * d.y, 0.0)) + fp * d.x);
    return ApplyCt(Lut(t));
}
