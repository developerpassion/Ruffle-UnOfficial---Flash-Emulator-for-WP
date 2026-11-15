#pragma once
#include "ruffle_uwp.h"
#include <unordered_map>
#include <map>
#include <tuple>

namespace RuffleUWP
{
    // Thực thi RuffleGpuHost (xem include/ruffle_uwp.h) bằng Direct3D 11.
    // Rust tessellate shape + duyệt CommandList; lớp này chỉ giữ tài nguyên GPU và vẽ.
    // Mọi hàm phải gọi khi đang giữ RufflePlayer::m_lock (immediate context không thread-safe).
    class GpuRenderer
    {
    public:
        GpuRenderer(ID3D11Device* device, ID3D11DeviceContext* context);
        bool Ok() const { return m_ok; }
        const RuffleGpuHost* Host() const { return &m_host; }
        int Samples() const { return m_samples; }
        bool MsaaSupported() const { return m_msaaSupported; }
        bool MsaaDefault() const { return m_msaaDefault; }   // bật mặc định (GPU feature level >= 10_0)
        void SetMsaa(bool on);                              // áp dụng ở khung hình kế tiếp

        // Gọi mỗi frame trước ruffle_tick: backbuffer của swapchain (kích thước w x h).
        void SetScreen(ID3D11Texture2D* backbuffer, UINT w, UINT h);
        // Gọi trước ResizeBuffers (nhả tham chiếu tới backbuffer).
        void ReleaseScreen();
        // Đọc backbuffer (BGRA? không: RGBA8) ra CPU để dump frame debug.
        bool ReadScreen(std::vector<uint8_t>& out, UINT& w, UINT& h);
        size_t TextureCount() const { return m_tex.size(); }

    private:
        struct Tex
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
            UINT w = 0, h = 0;
        };
        struct Mesh
        {
            Microsoft::WRL::ComPtr<ID3D11Buffer> vb, ib;
            DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT; // FL 9_1 chỉ hỗ trợ R16_UINT
        };
        struct Target
        {
            ID3D11RenderTargetView* rtv;
            ID3D11DepthStencilView* dsv;
            UINT w, h;
            bool screen;
        };

        // ---- RuffleGpuHost
        uint32_t CreateTexture(uint32_t w, uint32_t h, const uint8_t* rgba);
        void UpdateTexture(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride);
        void ReadTexture(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out);
        void DestroyTexture(uint32_t id);
        uint32_t CreateMesh(const RuffleGpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni);
        void DestroyMesh(uint32_t id);
        void BeginTarget(uint32_t tex, bool clear, const float* rgba);
        void EndTarget();
        void Draw(const RuffleGpuDraw& d);
        uint32_t CreateLayer();
        void Composite(uint32_t layer, uint32_t mode, uint32_t maskLayer, uint32_t maskRef);

        void Init();
        void BindTarget(const Target& t);
        ID3D11DepthStencilView* StencilFor(UINT w, UINT h, size_t depth);
        void SetupScreenTargets(UINT w, UINT h);
        ID3D11SamplerState* Sampler(bool smooth, bool repeat) { return m_samplers[(smooth ? 1 : 0) + (repeat ? 2 : 0)].Get(); }
        void UploadCB(const RuffleGpuDraw& d, const Target& t);

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
        RuffleGpuHost m_host = {};
        bool m_ok = false;

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps[5];      // theo RUFFLE_KIND_*
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psLayer, m_psLayerMask;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplers[4];
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendDraw, m_blendNoColor, m_blendComposite[10];
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dss[3];  // theo RUFFLE_MASK_*
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;
        uint32_t m_quad = 0; // mesh quad đơn vị cho composite

        std::unordered_map<uint32_t, Tex> m_tex;
        std::unordered_map<uint32_t, Mesh> m_mesh;
        uint32_t m_nextId = 1;
        std::vector<Target> m_stack;
        std::map<std::tuple<UINT, UINT, size_t>, Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> m_stencilPool;

        // Màn hình: render vào RT MSAA (nếu có) rồi resolve sang backbuffer
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_back;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backRtv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_msaa;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_msaaRtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_screenDsv;
        UINT m_screenW = 0, m_screenH = 0;
        int m_samples = 1;
        uint32_t m_maxTex = 4096;
        bool m_msaaSupported = false, m_msaaDefault = false;
        uint32_t m_lastMesh = 0;
        bool m_index16 = false; // FL 9_1: index buffer 16-bit, tối đa 65535 đỉnh mỗi mesh
    };
}
