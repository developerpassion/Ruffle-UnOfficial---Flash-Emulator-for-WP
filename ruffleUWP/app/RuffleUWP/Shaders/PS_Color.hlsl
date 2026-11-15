#include "Common.hlsli"
float4 main(VSOut i) : SV_Target { return ApplyCt(i.color); }
