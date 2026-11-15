#include "Common.hlsli"
float4 main(VSOut i) : SV_Target { return ApplyCt(Lut(length(i.uv * 2.0 - 1.0))); }
