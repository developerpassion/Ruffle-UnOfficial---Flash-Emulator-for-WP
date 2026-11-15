#include "Common.hlsli"
float4 main(VSOut i) : SV_Target { return tex0.Sample(samp0, i.uv); }
