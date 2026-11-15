#include "Common.hlsli"
// Lặp bitmap bằng frac() trong shader (view.z = 1) thay vì sampler WRAP:
// GPU feature level 9_x (vd. Lumia 650, Adreno 304) không cho WRAP với texture không phải luỹ thừa của 2.
float4 main(VSOut i) : SV_Target
{
    float2 uv = view.z > 0.5 ? frac(i.uv) : i.uv;
    return ApplyCtPremul(tex0.Sample(samp0, uv));
}
