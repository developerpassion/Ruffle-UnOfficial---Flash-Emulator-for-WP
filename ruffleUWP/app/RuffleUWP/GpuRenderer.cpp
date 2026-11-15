#include "pch.h"
#include "GpuRenderer.h"

// Shader biên dịch lúc build (FxCompile -> $(IntDir)Shaders\*.h, biến g_<tên>)
#include "Shaders\VS.h"
#include "Shaders\PS_Color.h"
#include "Shaders\PS_Linear.h"
#include "Shaders\PS_Radial.h"
#include "Shaders\PS_Focal.h"
#include "Shaders\PS_Bitmap.h"
#include "Shaders\PS_Layer.h"
#include "Shaders\PS_LayerMask.h"

using namespace RuffleUWP;
using Microsoft::WRL::ComPtr;

namespace
{
    // Khớp cbuffer Draw trong Shaders/Common.hlsli
    struct DrawCB
    {
        float world0[4], world1[4], uv0[4], uv1[4], ctMul[4], ctAdd[4], view[4];
    };

    GpuRenderer* Self(void* ctx) { return static_cast<GpuRenderer*>(ctx); }
}

GpuRenderer::GpuRenderer(ID3D11Device* device, ID3D11DeviceContext* context) : m_device(device), m_ctx(context)
{
    try { Init(); m_ok = true; }
    catch (Platform::Exception^ ex)
    {
        DebugLogW((L"GpuRenderer init failed: " + std::wstring(ex->Message->Data()) + L"\n").c_str());
        m_ok = false;
    }
}

static void Check(HRESULT hr) { if (FAILED(hr)) throw Platform::Exception::CreateException(hr); }

void GpuRenderer::Init()
{
    Check(m_device->CreateVertexShader(g_VS, sizeof(g_VS), nullptr, &m_vs));
    Check(m_device->CreatePixelShader(g_PS_Color, sizeof(g_PS_Color), nullptr, &m_ps[RUFFLE_KIND_COLOR]));
    Check(m_device->CreatePixelShader(g_PS_Linear, sizeof(g_PS_Linear), nullptr, &m_ps[RUFFLE_KIND_LINEAR]));
    Check(m_device->CreatePixelShader(g_PS_Radial, sizeof(g_PS_Radial), nullptr, &m_ps[RUFFLE_KIND_RADIAL]));
    Check(m_device->CreatePixelShader(g_PS_Focal, sizeof(g_PS_Focal), nullptr, &m_ps[RUFFLE_KIND_FOCAL]));
    Check(m_device->CreatePixelShader(g_PS_Bitmap, sizeof(g_PS_Bitmap), nullptr, &m_ps[RUFFLE_KIND_BITMAP]));
    Check(m_device->CreatePixelShader(g_PS_Layer, sizeof(g_PS_Layer), nullptr, &m_psLayer));
    Check(m_device->CreatePixelShader(g_PS_LayerMask, sizeof(g_PS_LayerMask), nullptr, &m_psLayerMask));

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    Check(m_device->CreateInputLayout(il, 2, g_VS, sizeof(g_VS), &m_layout));

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(DrawCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Check(m_device->CreateBuffer(&cbd, nullptr, &m_cb));

    for (int i = 0; i < 4; ++i)
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = (i & 1) ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        D3D11_TEXTURE_ADDRESS_MODE a = (i & 2) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressU = sd.AddressV = sd.AddressW = a;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        Check(m_device->CreateSamplerState(&sd, &m_samplers[i]));
    }

    auto blend = [&](D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, ComPtr<ID3D11BlendState>& out,
                     D3D11_BLEND srcA = D3D11_BLEND_ONE, D3D11_BLEND dstA = D3D11_BLEND_INV_SRC_ALPHA,
                     D3D11_BLEND_OP opA = D3D11_BLEND_OP_ADD, UINT8 mask = D3D11_COLOR_WRITE_ENABLE_ALL) {
        D3D11_BLEND_DESC bd = {};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = src; rt.DestBlend = dst; rt.BlendOp = op;
        rt.SrcBlendAlpha = srcA; rt.DestBlendAlpha = dstA; rt.BlendOpAlpha = opA;
        rt.RenderTargetWriteMask = mask;
        Check(m_device->CreateBlendState(&bd, &out));
    };
    // Mọi màu là premultiplied: "over" = ONE, INV_SRC_ALPHA
    blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, m_blendDraw);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, m_blendNoColor,
          D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, 0);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, m_blendComposite[RUFFLE_COMPOSITE_NORMAL]);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, m_blendComposite[RUFFLE_COMPOSITE_ADD]);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_REV_SUBTRACT, m_blendComposite[RUFFLE_COMPOSITE_SUBTRACT]);
    blend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, m_blendComposite[RUFFLE_COMPOSITE_MULTIPLY]);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_OP_ADD, m_blendComposite[RUFFLE_COMPOSITE_SCREEN]);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, m_blendComposite[RUFFLE_COMPOSITE_LIGHTEN]);
    blend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN, m_blendComposite[RUFFLE_COMPOSITE_DARKEN]);
    blend(D3D11_BLEND_ZERO, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_OP_ADD, m_blendComposite[RUFFLE_COMPOSITE_ALPHA],
          D3D11_BLEND_ZERO, D3D11_BLEND_SRC_ALPHA);
    blend(D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, m_blendComposite[RUFFLE_COMPOSITE_ERASE],
          D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA);
    blend(D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, m_blendComposite[RUFFLE_COMPOSITE_INVERT]);

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
        dd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, pass, D3D11_COMPARISON_EQUAL };
        dd.BackFace = dd.FrontFace;
        Check(m_device->CreateDepthStencilState(&dd, &m_dss[i]));
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    Check(m_device->CreateRasterizerState(&rd, &m_raster));

    // Kích thước texture tối đa theo feature level (9_1/9_2: 2048, 9_3: 4096, 10+: 8192)
    D3D_FEATURE_LEVEL fl = m_device->GetFeatureLevel();
    m_maxTex = fl >= D3D_FEATURE_LEVEL_10_0 ? 8192 : fl >= D3D_FEATURE_LEVEL_9_3 ? 4096 : 2048;
    m_index16 = fl < D3D_FEATURE_LEVEL_9_2;

    // MSAA 4x nếu GPU hỗ trợ cho RGBA8 + D24S8. Mặc định chỉ bật khi feature level >= 10_0:
    // GPU 9_x (vd. Adreno 304 của Lumia 650) fill-rate thấp, MSAA làm giật. Người dùng đổi trong Settings (SetMsaa).
    UINT q = 0, qd = 0;
    m_msaaSupported = SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &q)) && q > 0 &&
                      SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, 4, &qd)) && qd > 0;
    m_msaaDefault = m_msaaSupported && fl >= D3D_FEATURE_LEVEL_10_0;
    m_samples = m_msaaDefault ? 4 : 1;

    // Bảng hàm cho Rust
    m_host.ctx = this;
    m_host.create_texture = [](void* c, uint32_t w, uint32_t h, const uint8_t* p) { return Self(c)->CreateTexture(w, h, p); };
    m_host.update_texture = [](void* c, uint32_t t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* p, uint32_t s) { Self(c)->UpdateTexture(t, x, y, w, h, p, s); };
    m_host.read_texture = [](void* c, uint32_t t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* o) { Self(c)->ReadTexture(t, x, y, w, h, o); };
    m_host.destroy_texture = [](void* c, uint32_t t) { Self(c)->DestroyTexture(t); };
    m_host.create_mesh = [](void* c, const RuffleGpuVertex* v, uint32_t nv, const uint32_t* i, uint32_t ni) { return Self(c)->CreateMesh(v, nv, i, ni); };
    m_host.destroy_mesh = [](void* c, uint32_t m) { Self(c)->DestroyMesh(m); };
    m_host.begin_target = [](void* c, uint32_t t, int32_t clear, const float* rgba) { Self(c)->BeginTarget(t, clear != 0, rgba); };
    m_host.end_target = [](void* c) { Self(c)->EndTarget(); };
    m_host.draw = [](void* c, const RuffleGpuDraw* d) { Self(c)->Draw(*d); };
    m_host.create_layer = [](void* c) { return Self(c)->CreateLayer(); };
    m_host.composite = [](void* c, uint32_t l, uint32_t m, uint32_t ml, uint32_t r) { Self(c)->Composite(l, m, ml, r); };

    RuffleGpuVertex quad[4] = {
        { 0, 0, { 1, 1, 1, 1 } }, { 1, 0, { 1, 1, 1, 1 } }, { 1, 1, { 1, 1, 1, 1 } }, { 0, 1, { 1, 1, 1, 1 } } };
    uint32_t qi[6] = { 0, 1, 2, 0, 2, 3 };
    m_quad = CreateMesh(quad, 4, qi, 6);

    char msg[96];
    sprintf_s(msg, "GPU renderer OK (MSAA %dx)\n", m_samples);
    DebugLog(msg);
}

// ---------------------------------------------------------------- screen

void GpuRenderer::ReleaseScreen()
{
    m_back.Reset();
    m_backRtv.Reset();
}

void GpuRenderer::SetupScreenTargets(UINT w, UINT h)
{
    m_msaa.Reset(); m_msaaRtv.Reset(); m_screenDsv.Reset();
    m_screenW = w; m_screenH = h;
    if (m_samples > 1)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    Check(m_device->CreateTexture2D(&dd, nullptr, &ds));
    Check(m_device->CreateDepthStencilView(ds.Get(), nullptr, &m_screenDsv));
}

void GpuRenderer::SetScreen(ID3D11Texture2D* backbuffer, UINT w, UINT h)
{
    if (m_back.Get() != backbuffer)
    {
        m_back = backbuffer;
        m_backRtv.Reset();
        Check(m_device->CreateRenderTargetView(backbuffer, nullptr, &m_backRtv));
    }
    if (w != m_screenW || h != m_screenH || !m_screenDsv) SetupScreenTargets(w, h);
}

bool GpuRenderer::ReadScreen(std::vector<uint8_t>& out, UINT& w, UINT& h)
{
    if (!m_back) return false;
    D3D11_TEXTURE2D_DESC d;
    m_back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(m_device->CreateTexture2D(&d, nullptr, &st))) return false;
    m_ctx->CopyResource(st.Get(), m_back.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    w = d.Width; h = d.Height;
    out.resize((size_t)w * h * 4);
    for (UINT y = 0; y < h; ++y) memcpy(&out[(size_t)y * w * 4], (uint8_t*)map.pData + y * map.RowPitch, w * 4);
    m_ctx->Unmap(st.Get(), 0);
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
    m_tex.emplace(id, std::move(t));
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
        // FL 9_1 không có index 32-bit: chuyển sang 16-bit. Mesh > 65535 đỉnh không vẽ được ở mức này -> bỏ qua.
        if (nv > 65535) return 0;
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
    m_mesh.emplace(id, std::move(m));
    return id;
}

void GpuRenderer::DestroyMesh(uint32_t id)
{
    m_mesh.erase(id);
    if (m_lastMesh == id) m_lastMesh = 0;
}

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
    // Gỡ texture khỏi slot shader trước khi dùng làm render target
    ID3D11ShaderResourceView* none[2] = { nullptr, nullptr };
    m_ctx->PSSetShaderResources(0, 2, none);
    m_ctx->OMSetRenderTargets(1, &t.rtv, t.dsv);
    D3D11_VIEWPORT vp = { 0, 0, (float)t.w, (float)t.h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
}

void GpuRenderer::BeginTarget(uint32_t tex, bool clear, const float* rgba)
{
    Target t = {};
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
        if (it == m_tex.end()) { m_stack.push_back({ nullptr, nullptr, 0, 0, false }); return; }
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
        m_ctx->ResolveSubresource(m_back.Get(), 0, m_msaa.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
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

    RuffleGpuDraw d = {};
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
