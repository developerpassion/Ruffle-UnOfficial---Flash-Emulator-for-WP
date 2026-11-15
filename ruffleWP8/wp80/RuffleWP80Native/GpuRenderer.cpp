#include "GpuRenderer.h"
#include <cstdio>

// Shader dùng chung với ruffleUWP (FxCompile -> $(IntDir)Shaders\*.h, biến g_<tên>, profile 4.0_level_9_1)
#include "Shaders\VS.h"
#include "Shaders\PS_Color.h"
#include "Shaders\PS_Linear.h"
#include "Shaders\PS_Radial.h"
#include "Shaders\PS_Focal.h"
#include "Shaders\PS_Bitmap.h"
#include "Shaders\PS_Layer.h"
#include "Shaders\PS_LayerMask.h"

using namespace RuffleWP80Native;
using Microsoft::WRL::ComPtr;

namespace
{
    // Khớp cbuffer Draw trong Shaders/Common.hlsli
    struct DrawCB
    {
        float world0[4], world1[4], uv0[4], uv1[4], ctMul[4], ctAdd[4], view[4];
    };

    struct InitError
    {
        HRESULT hr;
        const char* what;
    };

    void Check(HRESULT hr, const char* what)
    {
        if (FAILED(hr)) { InitError e = { hr, what }; throw e; }
    }
}

GpuRenderer::GpuRenderer(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_ctx(context), m_ok(false), m_quad(0), m_nextId(1),
      m_screenW(0), m_screenH(0), m_screenFormat(DXGI_FORMAT_UNKNOWN), m_realW(0), m_realH(0), m_visH(0),
      m_contentW(0), m_contentH(0), m_rotation(0), m_scale(1.0f), m_zoom(1.0f), m_panX(0), m_panY(0), m_lastIsCpu(false),
      m_samples(1), m_maxTex(4096),
      m_msaaSupported(false), m_index16(false)
{
    memset(&m_host, 0, sizeof(m_host));
    try { Init(); m_ok = true; }
    catch (const InitError& e)
    {
        char msg[128];
        sprintf_s(msg, "GPU init failed: %s (0x%08X)", e.what, (unsigned)e.hr);
        m_error = msg;
        OutputDebugStringA(msg);
    }
}

void GpuRenderer::MakeBlend(D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, ComPtr<ID3D11BlendState>& out,
                            D3D11_BLEND srcA, D3D11_BLEND dstA, D3D11_BLEND_OP opA, UINT8 mask)
{
    D3D11_BLEND_DESC bd = {};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = src; rt.DestBlend = dst; rt.BlendOp = op;
    rt.SrcBlendAlpha = srcA; rt.DestBlendAlpha = dstA; rt.BlendOpAlpha = opA;
    rt.RenderTargetWriteMask = mask;
    Check(m_device->CreateBlendState(&bd, &out), "CreateBlendState");
}

void GpuRenderer::Init()
{
    InitStates();
    RuffleGpuVertex quad[4] = {
        { 0, 0, { 1, 1, 1, 1 } }, { 1, 0, { 1, 1, 1, 1 } }, { 1, 1, { 1, 1, 1, 1 } }, { 0, 1, { 1, 1, 1, 1 } } };
    uint32_t qi[6] = { 0, 1, 2, 0, 2, 3 };
    m_quad = CreateMesh(quad, 4, qi, 6);
}

void GpuRenderer::InitStates()
{
    Check(m_device->CreateVertexShader(g_VS, sizeof(g_VS), nullptr, &m_vs), "VS");
    Check(m_device->CreatePixelShader(g_PS_Color, sizeof(g_PS_Color), nullptr, &m_ps[RUFFLE_KIND_COLOR]), "PS_Color");
    Check(m_device->CreatePixelShader(g_PS_Linear, sizeof(g_PS_Linear), nullptr, &m_ps[RUFFLE_KIND_LINEAR]), "PS_Linear");
    Check(m_device->CreatePixelShader(g_PS_Radial, sizeof(g_PS_Radial), nullptr, &m_ps[RUFFLE_KIND_RADIAL]), "PS_Radial");
    Check(m_device->CreatePixelShader(g_PS_Focal, sizeof(g_PS_Focal), nullptr, &m_ps[RUFFLE_KIND_FOCAL]), "PS_Focal");
    Check(m_device->CreatePixelShader(g_PS_Bitmap, sizeof(g_PS_Bitmap), nullptr, &m_ps[RUFFLE_KIND_BITMAP]), "PS_Bitmap");
    Check(m_device->CreatePixelShader(g_PS_Layer, sizeof(g_PS_Layer), nullptr, &m_psLayer), "PS_Layer");
    Check(m_device->CreatePixelShader(g_PS_LayerMask, sizeof(g_PS_LayerMask), nullptr, &m_psLayerMask), "PS_LayerMask");

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    Check(m_device->CreateInputLayout(il, 2, g_VS, sizeof(g_VS), &m_layout), "InputLayout");

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(DrawCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Check(m_device->CreateBuffer(&cbd, nullptr, &m_cb), "ConstantBuffer");

    for (int i = 0; i < 4; ++i)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = (i & 1) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        D3D11_TEXTURE_ADDRESS_MODE a = (i & 2) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressU = sd.AddressV = sd.AddressW = a;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        Check(m_device->CreateSamplerState(&sd, &m_samplers[i]), "Sampler");
    }

    const D3D11_BLEND ONE = D3D11_BLEND_ONE, ISA = D3D11_BLEND_INV_SRC_ALPHA;
    const D3D11_BLEND_OP ADD = D3D11_BLEND_OP_ADD;
    const UINT8 ALL = D3D11_COLOR_WRITE_ENABLE_ALL;
    // Mọi màu là premultiplied: "over" = ONE, INV_SRC_ALPHA
    MakeBlend(ONE, ISA, ADD, m_blendDraw, ONE, ISA, ADD, ALL);
    MakeBlend(ONE, ISA, ADD, m_blendNoColor, ONE, ISA, ADD, 0);
    MakeBlend(ONE, ISA, ADD, m_blendComposite[RUFFLE_COMPOSITE_NORMAL], ONE, ISA, ADD, ALL);
    MakeBlend(ONE, ONE, ADD, m_blendComposite[RUFFLE_COMPOSITE_ADD], ONE, ISA, ADD, ALL);
    MakeBlend(ONE, ONE, D3D11_BLEND_OP_REV_SUBTRACT, m_blendComposite[RUFFLE_COMPOSITE_SUBTRACT], ONE, ISA, ADD, ALL);
    MakeBlend(D3D11_BLEND_DEST_COLOR, ISA, ADD, m_blendComposite[RUFFLE_COMPOSITE_MULTIPLY], ONE, ISA, ADD, ALL);
    MakeBlend(ONE, D3D11_BLEND_INV_SRC_COLOR, ADD, m_blendComposite[RUFFLE_COMPOSITE_SCREEN], ONE, ISA, ADD, ALL);
    MakeBlend(ONE, ONE, D3D11_BLEND_OP_MAX, m_blendComposite[RUFFLE_COMPOSITE_LIGHTEN], ONE, ISA, ADD, ALL);
    MakeBlend(ONE, ONE, D3D11_BLEND_OP_MIN, m_blendComposite[RUFFLE_COMPOSITE_DARKEN], ONE, ISA, ADD, ALL);
    MakeBlend(D3D11_BLEND_ZERO, D3D11_BLEND_SRC_ALPHA, ADD, m_blendComposite[RUFFLE_COMPOSITE_ALPHA],
              D3D11_BLEND_ZERO, D3D11_BLEND_SRC_ALPHA, ADD, ALL);
    MakeBlend(D3D11_BLEND_ZERO, ISA, ADD, m_blendComposite[RUFFLE_COMPOSITE_ERASE], D3D11_BLEND_ZERO, ISA, ADD, ALL);
    MakeBlend(D3D11_BLEND_INV_DEST_COLOR, ISA, ADD, m_blendComposite[RUFFLE_COMPOSITE_INVERT], ONE, ISA, ADD, ALL);

    // Stencil: luôn so "== ref"; WRITE tăng, CLEAR giảm (mask lồng nhau như renderer wgpu của Ruffle)
    for (int i = 0; i < 3; ++i)
    {
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = 0xFF;
        dd.StencilWriteMask = 0xFF;
        D3D11_STENCIL_OP pass = i == RUFFLE_MASK_WRITE ? D3D11_STENCIL_OP_INCR_SAT
                              : i == RUFFLE_MASK_CLEAR ? D3D11_STENCIL_OP_DECR_SAT : D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dd.FrontFace.StencilPassOp = pass;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
        dd.BackFace = dd.FrontFace;
        Check(m_device->CreateDepthStencilState(&dd, &m_dss[i]), "DepthStencilState");
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    Check(m_device->CreateRasterizerState(&rd, &m_raster), "RasterizerState");

    // Kích thước texture tối đa theo feature level (9_1/9_2: 2048, 9_3: 4096, 10+: 8192)
    D3D_FEATURE_LEVEL fl = m_device->GetFeatureLevel();
    m_maxTex = fl >= D3D_FEATURE_LEVEL_10_0 ? 8192 : fl >= D3D_FEATURE_LEVEL_9_3 ? 4096 : 2048;
    m_index16 = fl < D3D_FEATURE_LEVEL_9_2;

    // MSAA 4x: mặc định TẮT trên WP8 (GPU 9_x fill-rate thấp); bật qua SetMsaa
    UINT q = 0, qd = 0;
    m_msaaSupported = SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &q)) && q > 0 &&
                      SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, 4, &qd)) && qd > 0;
    m_samples = 1;

    // Bảng hàm cho Rust
    m_host.ctx = this;
    m_host.create_texture = &GpuRenderer::HCreateTexture;
    m_host.update_texture = &GpuRenderer::HUpdateTexture;
    m_host.read_texture = &GpuRenderer::HReadTexture;
    m_host.destroy_texture = &GpuRenderer::HDestroyTexture;
    m_host.create_mesh = &GpuRenderer::HCreateMesh;
    m_host.destroy_mesh = &GpuRenderer::HDestroyMesh;
    m_host.begin_target = &GpuRenderer::HBeginTarget;
    m_host.end_target = &GpuRenderer::HEndTarget;
    m_host.draw = &GpuRenderer::HDraw;
    m_host.create_layer = &GpuRenderer::HCreateLayer;
    m_host.composite = &GpuRenderer::HComposite;
}

// ---------------------------------------------------------------- bảng hàm tĩnh

static GpuRenderer* Self(void* c) { return static_cast<GpuRenderer*>(c); }
uint32_t GpuRenderer::HCreateTexture(void* c, uint32_t w, uint32_t h, const uint8_t* p) { return Self(c)->CreateTexture(w, h, p); }
void GpuRenderer::HUpdateTexture(void* c, uint32_t t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* p, uint32_t s) { Self(c)->UpdateTexture(t, x, y, w, h, p, s); }
void GpuRenderer::HReadTexture(void* c, uint32_t t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* o) { Self(c)->ReadTexture(t, x, y, w, h, o); }
void GpuRenderer::HDestroyTexture(void* c, uint32_t t) { Self(c)->DestroyTexture(t); }
uint32_t GpuRenderer::HCreateMesh(void* c, const RuffleGpuVertex* v, uint32_t nv, const uint32_t* i, uint32_t ni) { return Self(c)->CreateMesh(v, nv, i, ni); }
void GpuRenderer::HDestroyMesh(void* c, uint32_t m) { Self(c)->DestroyMesh(m); }
void GpuRenderer::HBeginTarget(void* c, uint32_t t, int32_t clear, const float* rgba) { Self(c)->BeginTarget(t, clear != 0, rgba); }
void GpuRenderer::HEndTarget(void* c) { Self(c)->EndTarget(); }
void GpuRenderer::HDraw(void* c, const RuffleGpuDraw* d) { Self(c)->Draw(*d); }
uint32_t GpuRenderer::HCreateLayer(void* c) { return Self(c)->CreateLayer(); }
void GpuRenderer::HComposite(void* c, uint32_t l, uint32_t m, uint32_t ml, uint32_t r) { Self(c)->Composite(l, m, ml, r); }

// ---------------------------------------------------------------- screen

void GpuRenderer::SetupScreenTargets(UINT w, UINT h, DXGI_FORMAT fmt)
{
    m_msaa.Reset(); m_msaaRtv.Reset(); m_screenDsv.Reset();
    m_screenW = w; m_screenH = h; m_screenFormat = fmt;
    if (m_samples > 1)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = m_samples; td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_msaa)) ||
            FAILED(m_device->CreateRenderTargetView(m_msaa.Get(), nullptr, &m_msaaRtv)))
        {
            m_samples = 1; m_msaa.Reset(); m_msaaRtv.Reset();
        }
    }
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = w; dd.Height = h; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = m_samples;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> ds;
    if (SUCCEEDED(m_device->CreateTexture2D(&dd, nullptr, &ds)))
        m_device->CreateDepthStencilView(ds.Get(), nullptr, &m_screenDsv);
}

void GpuRenderer::SetScreen(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, ID3D11Texture2D* tex,
                            UINT w, UINT h, UINT visH, int rotation, float scale)
{
    (void)tex;
    m_ctx = context; // context của hệ thống (có thể đổi giữa các khung)
    m_realRtv = rtv;
    m_realW = w; m_realH = h;
    m_visH = (visH > 0 && visH <= h) ? visH : h;
    m_rotation = (rotation == 1 || rotation == 2) ? rotation : 0;
    m_scale = (scale > 0.05f && scale <= 1.0f) ? scale : 1.0f;

    // Cỡ nội dung = vùng nhìn thấy theo hướng trang, nhân độ phân giải render
    UINT vw = m_rotation ? m_visH : m_realW, vh = m_rotation ? m_realW : m_visH;
    UINT cw = (UINT)(vw * m_scale + 0.5f), ch = (UINT)(vh * m_scale + 0.5f);
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;
    if (cw > m_maxTex) cw = m_maxTex;
    if (ch > m_maxTex) ch = m_maxTex;
    m_contentW = cw; m_contentH = ch;

    // Rust luôn vẽ vào m_off; Present() đưa lên RT hệ thống
    if (EnsureTex(m_off, cw, ch)) { m_back = m_off.tex; m_backRtv = m_off.rtv; }
    else { m_back.Reset(); m_backRtv.Reset(); }
    if (cw != m_screenW || ch != m_screenH || m_screenFormat != DXGI_FORMAT_R8G8B8A8_UNORM || !m_screenDsv)
        SetupScreenTargets(cw, ch, DXGI_FORMAT_R8G8B8A8_UNORM);
}

void GpuRenderer::SetView(float zoom, float panX, float panY)
{
    m_zoom = zoom < 1.0f ? 1.0f : (zoom > 4.0f ? 4.0f : zoom);
    float maxPan = 1.0f - 1.0f / m_zoom;
    m_panX = panX < 0 ? 0 : (panX > maxPan ? maxPan : panX);
    m_panY = panY < 0 ? 0 : (panY > maxPan ? maxPan : panY);
}

bool GpuRenderer::EnsureTex(Tex& t, UINT w, UINT h)
{
    if (t.tex && t.w == w && t.h == h) return true;
    t = Tex();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &t.tex)) ||
        FAILED(m_device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)) ||
        FAILED(m_device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)))
    {
        t = Tex();
        return false;
    }
    t.w = w; t.h = h;
    const float clear[4] = { 0, 0, 0, 1 };
    m_ctx->ClearRenderTargetView(t.rtv.Get(), clear);
    return true;
}

void GpuRenderer::Present()
{
    // GPU: hình mới nhất nằm trong m_off (Rust vẽ vào đó); CPU: m_cpuTex (PresentCpu)
    ID3D11ShaderResourceView* src = m_lastIsCpu ? m_cpuTex.srv.Get() : m_off.srv.Get();
    if (src) Blit(src);
}

void GpuRenderer::MarkGpuFrame()
{
    m_lastIsCpu = false;
}

void GpuRenderer::PresentCpu(const uint8_t* rgba, UINT w, UINT h, UINT stride)
{
    if (!EnsureTex(m_cpuTex, w, h)) return;
    m_ctx->UpdateSubresource(m_cpuTex.tex.Get(), 0, nullptr, rgba, stride, 0);
    m_lastIsCpu = true;
    Blit(m_cpuTex.srv.Get());
}

// Vẽ hình nội dung phủ vùng nhìn thấy của RT hệ thống (rộng m_realW, cao m_visH, từ đỉnh).
// (x, y) = toạ độ 0..1 trên vùng đó (hướng dọc) -> toạ độ chuẩn hoá trên màn theo hướng trang (sx, sy):
//   dọc: (x, y)   LandscapeLeft: (y, 1 - x)   LandscapeRight: (1 - y, x)   (đã kiểm tra trên HTC One M8)
// rồi zoom/kéo: uv = pan + (sx, sy) / zoom.
void GpuRenderer::Blit(ID3D11ShaderResourceView* srv)
{
    auto mi = m_mesh.find(m_quad);
    if (mi == m_mesh.end() || !m_realRtv) return;
    Target t;
    t.rtv = m_realRtv.Get(); t.w = m_realW; t.h = m_realH; t.screen = true;
    BindTarget(t);
    const float black[4] = { 0, 0, 0, 1 };
    m_ctx->ClearRenderTargetView(t.rtv, black);

    RuffleGpuDraw d;
    memset(&d, 0, sizeof(d));
    d.mesh = m_quad; d.index_count = 6;
    d.world[0] = (float)m_realW; d.world[3] = (float)m_visH;
    // sx = a0*x + a2*y + a4,  sy = a1*x + a3*y + a5   (cùng bố cục với RuffleGpuDraw::uv)
    float a[6] = { 1, 0, 0, 1, 0, 0 };
    if (m_rotation == 1) { a[0] = 0; a[1] = -1; a[2] = 1; a[3] = 0; a[4] = 0; a[5] = 1; }
    else if (m_rotation == 2) { a[0] = 0; a[1] = 1; a[2] = -1; a[3] = 0; a[4] = 1; a[5] = 0; }
    float iz = 1.0f / m_zoom;
    for (int k = 0; k < 4; ++k) d.uv[k] = a[k] * iz;
    d.uv[4] = a[4] * iz + m_panX;
    d.uv[5] = a[5] * iz + m_panY;
    for (int k = 0; k < 4; ++k) d.ct_mul[k] = 1;
    UploadCB(d, t);

    UINT stride = sizeof(RuffleGpuVertex), offset = 0;
    ID3D11Buffer* vb = mi->second.vb.Get();
    m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    m_ctx->IASetIndexBuffer(mi->second.ib.Get(), mi->second.indexFormat, 0);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(m_layout.Get());
    ID3D11Buffer* cb = m_cb.Get();
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, &cb);
    m_ctx->PSSetShader(m_psLayer.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &cb);
    m_ctx->PSSetShaderResources(0, 1, &srv);
    // Phóng to (độ phân giải < 100% hoặc zoom) -> lọc tuyến tính; điểm-điểm -> point
    bool scaled = m_scale < 0.999f || m_zoom > 1.001f;
    ID3D11SamplerState* s = Sampler(scaled, false);
    m_ctx->PSSetSamplers(0, 1, &s);
    float bf[4] = { 0, 0, 0, 0 };
    m_ctx->OMSetBlendState(m_blendComposite[RUFFLE_COMPOSITE_NORMAL].Get(), bf, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(nullptr, 0);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->DrawIndexed(6, 0, 0);
    ID3D11ShaderResourceView* none[1] = { nullptr };
    m_ctx->PSSetShaderResources(0, 1, none);
}

// ---------------------------------------------------------------- chuyển sang D3D device mới (quay lại app)

bool GpuRenderer::ReadBuffer(ID3D11Buffer* buf, std::vector<uint8_t>& out)
{
    D3D11_BUFFER_DESC bd;
    buf->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
    ComPtr<ID3D11Buffer> st;
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &st))) return false;
    m_ctx->CopyResource(st.Get(), buf);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    out.assign((uint8_t*)map.pData, (uint8_t*)map.pData + bd.ByteWidth);
    m_ctx->Unmap(st.Get(), 0);
    return true;
}

bool GpuRenderer::MigrateTo(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!m_ok || device == m_device.Get()) return m_ok;
    if (FAILED(m_device->GetDeviceRemovedReason())) return false; // device cũ đã mất -> không đọc được

    // 1) Đọc mọi texture và mesh từ device cũ (vẫn còn vì ta giữ tham chiếu)
    struct TexData { uint32_t id; UINT w, h; std::vector<uint8_t> px; };
    struct MeshData { uint32_t id; std::vector<uint8_t> vb, ib; DXGI_FORMAT fmt; };
    std::vector<TexData> texs;
    std::vector<MeshData> meshes;
    for (auto it = m_tex.begin(); it != m_tex.end(); ++it)
    {
        TexData t;
        t.id = it->first; t.w = it->second.w; t.h = it->second.h;
        t.px.resize((size_t)t.w * t.h * 4);
        ReadTexture(t.id, 0, 0, t.w, t.h, t.px.data());
        texs.push_back(std::move(t));
    }
    for (auto it = m_mesh.begin(); it != m_mesh.end(); ++it)
    {
        MeshData m;
        m.id = it->first; m.fmt = it->second.indexFormat;
        if (!ReadBuffer(it->second.vb.Get(), m.vb) || !ReadBuffer(it->second.ib.Get(), m.ib)) return false;
        meshes.push_back(std::move(m));
    }

    // 2) Bỏ mọi thứ của device cũ, tạo lại state trên device mới
    m_stack.clear();
    m_stencilPool.clear();
    m_tex.clear();
    m_mesh.clear();
    m_off = Tex(); m_cpuTex = Tex();
    m_back.Reset(); m_backRtv.Reset(); m_realRtv.Reset();
    m_msaa.Reset(); m_msaaRtv.Reset(); m_screenDsv.Reset();
    m_screenW = m_screenH = 0;
    int samples = m_samples;
    m_device = device;
    m_ctx = context;
    try { InitStates(); }
    catch (const InitError&) { m_ok = false; return false; }
    if (samples > 1) SetMsaa(true);

    // 3) Tạo lại texture/mesh với cùng id (Rust vẫn dùng id cũ)
    for (size_t i = 0; i < texs.size(); ++i)
    {
        Tex t; t.w = texs[i].w; t.h = texs[i].h;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = t.w; td.Height = t.h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA init = { texs[i].px.data(), t.w * 4, 0 };
        if (FAILED(m_device->CreateTexture2D(&td, &init, &t.tex)) ||
            FAILED(m_device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)) ||
            FAILED(m_device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) return false;
        m_tex[texs[i].id] = t;
    }
    for (size_t i = 0; i < meshes.size(); ++i)
    {
        Mesh m;
        m.indexFormat = meshes[i].fmt;
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = (UINT)meshes[i].vb.size();
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = { meshes[i].vb.data(), 0, 0 };
        if (FAILED(m_device->CreateBuffer(&bd, &sd, &m.vb))) return false;
        bd.ByteWidth = (UINT)meshes[i].ib.size();
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = meshes[i].ib.data();
        if (FAILED(m_device->CreateBuffer(&bd, &sd, &m.ib))) return false;
        m_mesh[meshes[i].id] = m;
    }
    return true;
}

// ---------------------------------------------------------------- textures / meshes

uint32_t GpuRenderer::CreateTexture(uint32_t w, uint32_t h, const uint8_t* rgba)
{
    if (w == 0 || h == 0 || w > m_maxTex || h > m_maxTex) return 0;
    Tex t; t.w = w; t.h = h;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    std::vector<uint8_t> zero;
    D3D11_SUBRESOURCE_DATA init = {};
    if (!rgba) { zero.assign((size_t)w * h * 4, 0); rgba = zero.data(); }
    init.pSysMem = rgba; init.SysMemPitch = w * 4;
    if (FAILED(m_device->CreateTexture2D(&td, &init, &t.tex))) return 0;
    if (FAILED(m_device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) return 0;
    if (FAILED(m_device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) return 0;
    uint32_t id = m_nextId++;
    m_tex[id] = t;
    return id;
}

void GpuRenderer::UpdateTexture(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride)
{
    auto it = m_tex.find(id);
    if (it == m_tex.end()) return;
    D3D11_BOX box = { x, y, 0, x + w, y + h, 1 };
    m_ctx->UpdateSubresource(it->second.tex.Get(), 0, &box, rgba, stride, 0);
}

void GpuRenderer::ReadTexture(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out)
{
    auto it = m_tex.find(id);
    if (it == m_tex.end()) { memset(out, 0, (size_t)w * h * 4); return; }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(m_device->CreateTexture2D(&d, nullptr, &st))) return;
    D3D11_BOX box = { x, y, 0, x + w, y + h, 1 };
    m_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, it->second.tex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
    for (uint32_t r = 0; r < h; ++r) memcpy(out + (size_t)r * w * 4, (uint8_t*)map.pData + r * map.RowPitch, w * 4);
    m_ctx->Unmap(st.Get(), 0);
}

void GpuRenderer::DestroyTexture(uint32_t id) { m_tex.erase(id); }

uint32_t GpuRenderer::CreateMesh(const RuffleGpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni)
{
    if (nv == 0 || ni == 0) return 0;
    Mesh m;
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = nv * sizeof(RuffleGpuVertex);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { v, 0, 0 };
    if (FAILED(m_device->CreateBuffer(&bd, &sd, &m.vb))) return 0;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    std::vector<uint16_t> idx16;
    if (m_index16)
    {
        if (nv > 65535) return 0; // FL 9_1: chỉ index 16-bit
        idx16.assign(idx, idx + ni);
        bd.ByteWidth = ni * sizeof(uint16_t);
        sd.pSysMem = idx16.data();
        m.indexFormat = DXGI_FORMAT_R16_UINT;
    }
    else
    {
        bd.ByteWidth = ni * sizeof(uint32_t);
        sd.pSysMem = idx;
    }
    if (FAILED(m_device->CreateBuffer(&bd, &sd, &m.ib))) return 0;
    uint32_t id = m_nextId++;
    m_mesh[id] = m;
    return id;
}

void GpuRenderer::DestroyMesh(uint32_t id) { m_mesh.erase(id); }

// ---------------------------------------------------------------- targets

ID3D11DepthStencilView* GpuRenderer::StencilFor(UINT w, UINT h, size_t depth)
{
    auto key = std::make_tuple(w, h, depth);
    auto it = m_stencilPool.find(key);
    if (it != m_stencilPool.end()) return it->second.Get();
    if (m_stencilPool.size() > 32) m_stencilPool.clear(); // tránh tích luỹ khi BitmapData nhiều kích thước
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = w; dd.Height = h; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> ds;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(m_device->CreateTexture2D(&dd, nullptr, &ds)) || FAILED(m_device->CreateDepthStencilView(ds.Get(), nullptr, &dsv)))
        return nullptr;
    m_stencilPool[key] = dsv;
    return dsv.Get();
}

void GpuRenderer::BindTarget(const Target& t)
{
    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    m_ctx->PSSetShaderResources(0, 2, none);
    m_ctx->OMSetRenderTargets(1, &t.rtv, t.dsv);
    D3D11_VIEWPORT vp = { 0, 0, (float)t.w, (float)t.h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
}

void GpuRenderer::BeginTarget(uint32_t tex, bool clear, const float* rgba)
{
    Target t;
    if (tex == 0)
    {
        if (!m_backRtv) return;
        t.rtv = m_samples > 1 ? m_msaaRtv.Get() : m_backRtv.Get();
        t.dsv = m_screenDsv.Get();
        t.w = m_screenW; t.h = m_screenH; t.screen = true;
    }
    else
    {
        auto it = m_tex.find(tex);
        if (it == m_tex.end()) { m_stack.push_back(Target()); return; }
        t.rtv = it->second.rtv.Get();
        t.w = it->second.w; t.h = it->second.h;
        t.dsv = StencilFor(t.w, t.h, m_stack.size());
    }
    m_stack.push_back(t);
    BindTarget(t);
    if (clear && t.rtv) m_ctx->ClearRenderTargetView(t.rtv, rgba);
    if (t.dsv) m_ctx->ClearDepthStencilView(t.dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void GpuRenderer::EndTarget()
{
    if (m_stack.empty()) return;
    Target t = m_stack.back();
    m_stack.pop_back();
    if (t.screen && m_samples > 1 && m_msaa && m_back)
        m_ctx->ResolveSubresource(m_back.Get(), 0, m_msaa.Get(), 0, m_screenFormat);
    if (!m_stack.empty() && m_stack.back().rtv) BindTarget(m_stack.back());
}

uint32_t GpuRenderer::CreateLayer()
{
    if (m_stack.empty() || !m_stack.back().rtv) return 0;
    return CreateTexture(m_stack.back().w, m_stack.back().h, nullptr);
}

// ---------------------------------------------------------------- draw

void GpuRenderer::UploadCB(const RuffleGpuDraw& d, const Target& t)
{
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    DrawCB* cb = static_cast<DrawCB*>(map.pData);
    const float* w = d.world;
    const float* u = d.uv;
    cb->world0[0] = w[0]; cb->world0[1] = w[2]; cb->world0[2] = w[4]; cb->world0[3] = 0;
    cb->world1[0] = w[1]; cb->world1[1] = w[3]; cb->world1[2] = w[5]; cb->world1[3] = 0;
    cb->uv0[0] = u[0]; cb->uv0[1] = u[2]; cb->uv0[2] = u[4]; cb->uv0[3] = 0;
    cb->uv1[0] = u[1]; cb->uv1[1] = u[3]; cb->uv1[2] = u[5]; cb->uv1[3] = 0;
    memcpy(cb->ctMul, d.ct_mul, 16);
    memcpy(cb->ctAdd, d.ct_add, 16);
    cb->view[0] = 2.0f / (float)t.w;
    cb->view[1] = -2.0f / (float)t.h;
    cb->view[2] = (float)d.wrap;
    cb->view[3] = d.focal;
    m_ctx->Unmap(m_cb.Get(), 0);
}

void GpuRenderer::Draw(const RuffleGpuDraw& d)
{
    if (m_stack.empty() || !m_stack.back().rtv || d.index_count == 0) return;
    auto mi = m_mesh.find(d.mesh);
    if (mi == m_mesh.end()) return;
    const Target& t = m_stack.back();

    UploadCB(d, t);
    UINT stride = sizeof(RuffleGpuVertex), offset = 0;
    ID3D11Buffer* vb = mi->second.vb.Get();
    m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    m_ctx->IASetIndexBuffer(mi->second.ib.Get(), mi->second.indexFormat, 0);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(m_layout.Get());
    ID3D11Buffer* cb = m_cb.Get();
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, &cb);
    uint32_t kind = d.kind <= RUFFLE_KIND_BITMAP ? d.kind : RUFFLE_KIND_COLOR;
    m_ctx->PSSetShader(m_ps[kind].Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &cb);
    if (kind != RUFFLE_KIND_COLOR)
    {
        auto ti = m_tex.find(d.texture);
        if (ti == m_tex.end()) return;
        ID3D11ShaderResourceView* srv = ti->second.srv.Get();
        m_ctx->PSSetShaderResources(0, 1, &srv);
        bool gradient = kind != RUFFLE_KIND_BITMAP;
        // Luôn CLAMP: lặp bitmap làm trong shader (frac) vì FL 9_x không WRAP được texture không phải 2^n
        ID3D11SamplerState* s = Sampler(gradient || d.smooth != 0, false);
        m_ctx->PSSetSamplers(0, 1, &s);
    }
    float bf[4] = { 0, 0, 0, 0 };
    m_ctx->OMSetBlendState(d.mask_mode == RUFFLE_MASK_DRAW ? m_blendDraw.Get() : m_blendNoColor.Get(), bf, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(m_dss[d.mask_mode <= 2 ? d.mask_mode : 0].Get(), d.mask_ref);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->DrawIndexed(d.index_count, d.first_index, 0);
}

void GpuRenderer::Composite(uint32_t layer, uint32_t mode, uint32_t maskLayer, uint32_t maskRef)
{
    if (m_stack.empty() || !m_stack.back().rtv) return;
    auto li = m_tex.find(layer);
    if (li == m_tex.end()) return;
    const Target& t = m_stack.back();
    auto mi = m_mesh.find(m_quad);
    if (mi == m_mesh.end()) return;

    RuffleGpuDraw d;
    memset(&d, 0, sizeof(d));
    d.mesh = m_quad; d.index_count = 6;
    d.world[0] = (float)t.w; d.world[3] = (float)t.h;
    d.uv[0] = 1; d.uv[3] = 1;
    for (int k = 0; k < 4; ++k) d.ct_mul[k] = 1;
    UploadCB(d, t);

    UINT stride = sizeof(RuffleGpuVertex), offset = 0;
    ID3D11Buffer* vb = mi->second.vb.Get();
    m_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    m_ctx->IASetIndexBuffer(mi->second.ib.Get(), mi->second.indexFormat, 0);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(m_layout.Get());
    ID3D11Buffer* cb = m_cb.Get();
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, &cb);

    ID3D11ShaderResourceView* srvs[2] = { li->second.srv.Get(), nullptr };
    auto mk = m_tex.find(maskLayer);
    bool masked = maskLayer != 0 && mk != m_tex.end();
    if (masked) srvs[1] = mk->second.srv.Get();
    m_ctx->PSSetShader(masked ? m_psLayerMask.Get() : m_psLayer.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &cb);
    m_ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* s[2] = { Sampler(false, false), Sampler(false, false) };
    m_ctx->PSSetSamplers(0, 2, s);

    float bf[4] = { 0, 0, 0, 0 };
    m_ctx->OMSetBlendState(m_blendComposite[mode < 10 ? mode : 0].Get(), bf, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(m_dss[RUFFLE_MASK_DRAW].Get(), maskRef);
    m_ctx->RSSetState(m_raster.Get());
    m_ctx->DrawIndexed(6, 0, 0);

    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    m_ctx->PSSetShaderResources(0, 2, none);
}

void GpuRenderer::SetMsaa(bool on)
{
    int want = (on && m_msaaSupported) ? 4 : 1;
    if (want == m_samples) return;
    m_samples = want;
    m_screenDsv.Reset(); // SetScreen lần tới sẽ tạo lại RT/stencil màn hình với số mẫu mới
    m_msaa.Reset();
    m_msaaRtv.Reset();
}
