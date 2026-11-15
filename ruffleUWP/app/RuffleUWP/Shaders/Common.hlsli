// Dùng chung cho mọi shader của GpuRenderer. Giữ khớp struct DrawCB trong GpuRenderer.cpp.
cbuffer Draw : register(b0)
{
    float4 world0; // a, c, tx  (x' = a*x + c*y + tx)
    float4 world1; // b, d, ty
    float4 uv0;
    float4 uv1;
    float4 ctMul;
    float4 ctAdd;
    float4 view;   // 2/w, -2/h, wrap (0 pad 1 reflect 2 repeat), focal
};

struct VSOut
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
Texture2D tex1 : register(t1);
SamplerState samp1 : register(s1);

// màu straight -> color transform -> premultiplied
float4 ApplyCt(float4 straight)
{
    float4 c = saturate(straight * ctMul + ctAdd);
    return float4(c.rgb * c.a, c.a);
}

float4 ApplyCtPremul(float4 p)
{
    float3 rgb = p.a > 0.0 ? p.rgb / p.a : float3(0.0, 0.0, 0.0);
    return ApplyCt(float4(rgb, p.a));
}

float Spread(float t)
{
    if (view.z < 0.5) return saturate(t);                       // pad
    if (view.z < 1.5) return 1.0 - abs(frac(t * 0.5) * 2.0 - 1.0); // reflect
    return frac(t);                                              // repeat
}

float4 Lut(float t)
{
    return tex0.Sample(samp0, float2((Spread(t) * 255.0 + 0.5) / 256.0, 0.5));
}
