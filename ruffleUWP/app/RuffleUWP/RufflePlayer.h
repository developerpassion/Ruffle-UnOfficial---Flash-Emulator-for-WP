#pragma once
#include "ruffle_uwp.h"
#include "AudioOutput.h"
#include "GpuRenderer.h"

namespace RuffleUWP
{
    // D3D11 swapchain trên SwapChainPanel + vòng lặp render thread gọi Rust.
    class RufflePlayer
    {
    public:
        explicit RufflePlayer(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);
        ~RufflePlayer();

        bool Load(std::vector<uint8_t> swf); // true nếu tạo player thành công
        void Resize(float dipW, float dipH);
        // Zoom hiển thị (1..4) và độ dịch (DIP, <= 0). Toạ độ chạm truyền vào các hàm khác là toạ độ màn hình.
        void SetView(float zoom, float tx, float ty);
        float Zoom() const { return m_zoom; }
        float ViewX() const { return m_viewX; }
        float ViewY() const { return m_viewY; }
        // Hệ số độ phân giải vẽ: 0 = tự động (tối đa ~1280x720), 1 = đầy đủ, 0.5 = một nửa...
        void SetRenderScale(float s);
        float RenderScaleSetting() const { return m_renderScaleSetting; }
        void PointerEvent(int kind, float dipX, float dipY);   // kind 0 = move
        // kind 1=down 2=up; button 0=trái 1=phải 2=giữa. Trả true nếu SWF xử lý.
        bool MouseButton(int kind, int button, float dipX, float dipY);
        void MouseWheel(double lines);
        struct MenuItem { std::wstring caption; bool enabled, checked, separatorBefore; };
        std::vector<MenuItem> PrepareContextMenu();
        void ContextMenuSelect(uint32_t index);
        void ContextMenuClosed(float dipX, float dipY);
        void KeyEvent(uint32_t vk, uint32_t mods, bool down);
        void CharEvent(uint32_t codepoint);
        void SetSuspended(bool suspended);
        // GPU (Direct3D 11) hay CPU (software). Đổi sẽ nạp lại SWF hiện tại.
        void SetUseGpu(bool gpu);
        bool UseGpu() const { return m_useGpu; }
        bool GpuAvailable() const { return m_gpu != nullptr; }
        // MSAA 4x (chỉ có tác dụng với renderer GPU)
        bool MsaaSupported() const { return m_gpu && m_gpu->MsaaSupported(); }
        bool MsaaDefault() const { return m_gpu && m_gpu->MsaaDefault(); }
        void SetMsaa(bool on);
        void SetAudioEnabled(bool on);         // đổi -> nạp lại game
        void SetQuality(int quality);          // 0 Low 1 Medium 2 High 3 Best
        void SetFrameLimit(int fps) { m_frameLimit = fps; } // 0 = không giới hạn
        // FPS overlay: số khung hình/giây và thời gian tick+vẽ+present trung bình (ms), cập nhật mỗi giây
        float Fps() const { return m_fps; }
        float FrameWorkMs() const { return m_frameWorkMs; }
        bool UsingGpu() const { return m_ruffleGpu; }
        bool MsaaOn() const { return m_gpu && m_gpu->Samples() > 1; }
        // S_OK nếu âm thanh (XAudio2) khởi tạo được
        HRESULT AudioStatus() const { return m_audio ? m_audio->InitResult() : E_POINTER; }
        void RequestFrameDump(std::wstring path) { std::lock_guard<std::mutex> g(m_lock); m_dumpPath = std::move(path); }

    private:
        void CreateDevice();
        void CreateSwapChain(UINT w, UINT h);
        void RenderLoop(Windows::Foundation::IAsyncAction^ action);
        void Present();

        Platform::Agile<Windows::UI::Xaml::Controls::SwapChainPanel> m_panel;
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging; // CPU-writable, copy vào backbuffer

        std::mutex m_lock;                  // bảo vệ m_ruffle + swapchain
        RuffleHandle* m_ruffle = nullptr;
        std::unique_ptr<GpuRenderer> m_gpu;   // null nếu GPU init lỗi -> dùng renderer CPU
        bool m_useGpu = true;
        bool m_ruffleGpu = false;              // m_ruffle hiện tại tạo bằng ruffle_create_gpu
        std::vector<uint8_t> m_swf;            // giữ để nạp lại khi đổi renderer
        std::mutex m_audioLock;             // giữ khi thread audio dùng m_ruffle hoặc khi đổi/xoá m_ruffle
        std::unique_ptr<AudioOutput> m_audio;
        std::atomic<bool> m_suspended{ false };
        UINT m_width = 1, m_height = 1;
        float m_scale = 1.0f;                // scale gửi cho Ruffle + đổi toạ độ chạm DIP -> pixel
        float m_scaleX = 1.0f, m_scaleY = 1.0f; // SwapChainPanel CompositionScale
        float m_renderScaleSetting = 0.0f;      // 0 = tự động, còn lại 0.25..1
        float m_renderScale = 1.0f;             // giá trị đang dùng
        float m_dipW = 0, m_dipH = 0;
        float m_zoom = 1.0f, m_viewX = 0.0f, m_viewY = 0.0f; // zoom/dịch hiển thị
        std::atomic<int> m_frameLimit{ 0 };
        bool m_audioEnabled = true;
        int m_quality = 2;
        bool m_viewDirty = false;                       // cần vẽ lại + present để ma trận mới có hiệu lực
        void ApplyViewTransform();
        // màn hình (DIP) -> toạ độ nội dung chưa zoom (DIP)
        float ContentX(float x) const { return (x - m_viewX) / m_zoom; }
        float ContentY(float y) const { return (y - m_viewY) / m_zoom; }
        bool m_needUpload = true;               // framebuffer chưa được đưa lên GPU
        double dipToBufX() const { return m_width ? m_dipW / m_width : 1.0; }
        double dipToBufY() const { return m_height ? m_dipH / m_height : 1.0; }
        // Thống kê FPS (ghi log mỗi 5 giây)
        double m_statMs = 0, m_statTickMs = 0, m_statUploadMs = 0;
        int m_statFrames = 0, m_statLoops = 0;
        double m_ovMs = 0, m_ovWorkMs = 0;
        int m_ovFrames = 0;
        std::atomic<float> m_fps{ 0 }, m_frameWorkMs{ 0 };
        Windows::Foundation::IAsyncAction^ m_loop;
        std::atomic<bool> m_running{ true };
        std::wstring m_dumpPath;
        double m_playedMs = 0;
        void DumpFrame();
    };
}
