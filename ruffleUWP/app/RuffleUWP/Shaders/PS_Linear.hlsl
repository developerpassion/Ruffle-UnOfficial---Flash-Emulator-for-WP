#include "Common.hlsli"
float4 main(VSOut i) : SV_Target { return ApplyCt(Lut(i.uv.x)); }
