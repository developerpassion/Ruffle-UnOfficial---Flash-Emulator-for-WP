#pragma once
// GPU renderer cho WP 8.0 / Silverlight 8.1: port từ ruffleUWP\app\RuffleUWP\GpuRenderer (cùng shader, cùng RuffleGpuHost).
// Khác bản UWP:
//  - Vẽ vào render target mà DrawingSurfaceBackgroundGrid đưa mỗi khung (SetScreen), không có swapchain riêng.
//  - Viết lại cho VS2012 (v110_wp80): không khởi tạo thành viên trong class, không gán bằng {...}.
#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>
#include <unordered_map>
#include <map>
#include <tuple>
#include <vector>
#include <string>
#include "ruffle_uwp.h"

namespace RuffleWP80Native
{
    class GpuRenderer
    {
    public:
        GpuRenderer(ID3D11Device* device, ID3D11DeviceContext* context);
        bool Ok() const { return m_ok; }
        const std::string& Error() const { return m_error; }
        const RuffleGpuHost* Host() const { return &m_host; }
        ID3D11Device* Device() const { return m_device.Get(); }
        int Samples() const { return m_samples; }
        bool MsaaSupported() const { return m_msaaSupported; }
        void SetMsaa(bool on);

        // Mỗi khung, trước ruffle_tick: render target của hệ thống (RTV + texture, cỡ w x h, luôn dọc).
        // Rust LUÔN vẽ vào texture riêng m_off (cỡ nội dung); Present() vẽ m_off lên RT hệ thống: phóng (độ phân giải
        // render < 100%), xoay 90° khi ngang (DrawingSurfaceBackgroundGrid không tự xoay). Nhờ vậy khung bị bỏ qua
        // (giới hạn FPS) chỉ cần Present() lại hình cũ.
        // rotation: 0 = dọc, 1 = LandscapeLeft, 2 = LandscapeRight.
        // visH: chiều cao vùng NHÌN THẤY (pixel, hướng dọc, tính từ đỉnh RT) — RT phủ cả dải nút ảo ở đáy máy
        // (vd. HTC One M8) nên chỉ vẽ trong vùng này để hình và toạ độ chạm khớp nhau.
        // scale: độ phân giải render (0 < scale <= 1) so với vùng nhìn thấy.
        void SetScreen(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, ID3D11Texture2D* tex,
                       UINT w, UINT h, UINT visH, int rotation, float scale);
        // Kích thước nội dung Ruffle vẽ (đã đổi chiều khi xoay, đã nhân độ phân giải render)
        UINT ContentWidth() const { return m_contentW; }
        UINT ContentHeight() const { return m_contentH; }
        // Vẽ hình mới nhất (GPU: m_off, CPU: texture framebuffer) lên RT hệ thống
        void Present();
        // Gọi sau ruffle_tick/ruffle_render ở chế độ GPU: hình mới nhất là m_off
        void MarkGpuFrame();
        // Zoom 2 ngón (1..4) + vị trí khung nhìn (toạ độ chuẩn hoá 0..1-1/zoom trên nội dung)
        void SetView(float zoom, float panX, float panY);
        // Renderer CPU: đưa framebuffer RGBA (cỡ nội dung) lên GPU rồi Present
        void PresentCpu(const uint8_t* rgba, UINT w, UINT h, UINT stride);

        // D3D device mới (vd. quay lại app): đọc mọi texture/mesh từ device cũ rồi tạo lại trên device mới với CÙNG id
        // -> player Rust giữ nguyên, game không phải nạp lại. false = không chuyển được (gọi nơi khác tạo lại tất cả).
        bool MigrateTo(ID3D11Device* device, ID3D11DeviceContext* context);

    private:
        struct Tex
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
            UINT w, h;
            Tex() : w(0), h(0) {}
        };
        struct Mesh
        {
            Microsoft::WRL::ComPtr<ID3D11Buffer> vb, ib;
            DXGI_FORMAT indexFormat; // FL 9_1 chỉ hỗ trợ R16_UINT
            Mesh() : indexFormat(DXGI_FORMAT_R32_UINT) {}
        };
        struct Target
        {
            ID3D11RenderTargetView* rtv;
            ID3D11DepthStencilView* dsv;
            UINT w, h;
            bool screen;
            Target() : rtv(nullptr), dsv(nullptr), w(0), h(0), screen(false) {}
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

        // Hàm tĩnh cho bảng con trỏ hàm của Rust
        static uint32_t HCreateTexture(void* c, uint32_t w, uint32_t h, const uint8_t* p);
        static void HUpdateTexture(void* c, uint32_t t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* p, uint32_t s);
        static void HReadTexture(void* c, uint32_t t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* o);
        static void HDestroyTexture(void* c, uint32_t t);
        static uint32_t HCreateMesh(void* c, const RuffleGpuVertex* v, uint32_t nv, const uint32_t* i, uint32_t ni);
        static void HDestroyMesh(void* c, uint32_t m);
        static void HBeginTarget(void* c, uint32_t t, int32_t clear, const float* rgba);
        static void HEndTarget(void* c);
        static void HDraw(void* c, const RuffleGpuDraw* d);
        static uint32_t HCreateLayer(void* c);
        static void HComposite(void* c, uint32_t l, uint32_t m, uint32_t ml, uint32_t r);

        void Init();
        void InitStates(); // shader, state, caps, bảng hàm (gọi lại khi chuyển device)
        bool ReadBuffer(ID3D11Buffer* buf, std::vector<uint8_t>& out);
        void MakeBlend(D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND_OP op, Microsoft::WRL::ComPtr<ID3D11BlendState>& out,
                       D3D11_BLEND srcA, D3D11_BLEND dstA, D3D11_BLEND_OP opA, UINT8 mask);
        void BindTarget(const Target& t);
        ID3D11DepthStencilView* StencilFor(UINT w, UINT h, size_t depth);
        void SetupScreenTargets(UINT w, UINT h, DXGI_FORMAT fmt);
        ID3D11SamplerState* Sampler(bool smooth, bool repeat) { return m_samplers[(smooth ? 1 : 0) + (repeat ? 2 : 0)].Get(); }
        void UploadCB(const RuffleGpuDraw& d, const Target& t);
        bool EnsureTex(Tex& t, UINT w, UINT h);
        void Blit(ID3D11ShaderResourceView* srv); // vẽ srv phủ RT hệ thống, xoay theo m_rotation

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
        RuffleGpuHost m_host;
        bool m_ok;
        std::string m_error;

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps[5];      // theo RUFFLE_KIND_*
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psLayer, m_psLayerMask;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplers[4];
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendDraw, m_blendNoColor, m_blendComposite[10];
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dss[3];  // theo RUFFLE_MASK_*
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;
        uint32_t m_quad; // mesh quad đơn vị cho composite

        std::unordered_map<uint32_t, Tex> m_tex;
        std::unordered_map<uint32_t, Mesh> m_mesh;
        uint32_t m_nextId;
        std::vector<Target> m_stack;
        std::map<std::tuple<UINT, UINT, size_t>, Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> m_stencilPool;

        // Màn hình: RT của hệ thống (đổi mỗi khung), hoặc RT MSAA rồi resolve sang nó
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_back;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backRtv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_msaa;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_msaaRtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_screenDsv;
        UINT m_screenW, m_screenH;
        DXGI_FORMAT m_screenFormat;
        // RT thật của hệ thống + texture trung gian khi xoay / khi dùng renderer CPU
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_realRtv;
        UINT m_realW, m_realH, m_visH;
        UINT m_contentW, m_contentH;
        int m_rotation;
        float m_scale;
        float m_zoom, m_panX, m_panY;
        Tex m_off, m_cpuTex;
        bool m_lastIsCpu; // hình mới nhất để Present: m_cpuTex (CPU) hay m_off (GPU)
        int m_samples;
        uint32_t m_maxTex;
        bool m_msaaSupported;
        bool m_index16; // FL 9_1: index buffer 16-bit, tối đa 65535 đỉnh mỗi mesh
    };
}
