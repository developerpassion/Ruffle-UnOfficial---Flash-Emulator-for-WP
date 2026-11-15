#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>
#include <DrawingSurfaceNative.h>
#include <windows.phone.graphics.interop.h>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include "GpuRenderer.h"
#include "AudioOutput.h"

namespace RuffleWP80Native
{
    // Player Ruffle cho WP 8.0 / Silverlight 8.1: vẽ qua DrawingSurfaceBackgroundGrid (C# gọi SetBackgroundContentProvider).
    // Mọi lời gọi lõi Rust chạy trên thread vẽ (Draw); thread UI chỉ đặt lệnh/cài đặt dưới m_lock.
    // Viết cho VS2012: không khởi tạo thành viên trong class (gán trong constructor).
    public ref class RufflePlayer80 sealed
    {
    public:
        RufflePlayer80();

        Windows::Phone::Graphics::Interop::IDrawingSurfaceBackgroundContentProvider^ CreateContentProvider();

        // Nạp SWF (bytes) — player được tạo ở khung hình kế tiếp
        void LoadSwf(const Platform::Array<uint8>^ data);
        void Unload();
        // Đang chơi (hoặc đang chờ nạp). false sau khi nạp lỗi -> app C# quay về danh sách và hiện Status()
        bool IsPlaying();

        // Toạ độ chuẩn hoá 0..1 theo vùng trang (đã theo hướng). kind: 0 = move, 1 = down, 2 = up
        void Pointer(int kind, float nx, float ny);
        // Zoom/kéo 2 ngón: zoom 1..4, pan = góc trên-trái khung nhìn (chuẩn hoá trên nội dung, 0..1-1/zoom)
        void SetView(float zoom, float panX, float panY);

        // Thông báo gần nhất (lỗi nạp, lỗi GPU/âm thanh...) và dòng FPS ("60 FPS GPU")
        Platform::String^ Status();
        Platform::String^ FpsText();

        // ---- Settings (áp dụng ở khung kế tiếp)
        void SetUseGpu(bool on);              // đổi -> nạp lại SWF đang chạy
        bool UsingGpu();
        void SetMsaa(bool on);
        bool MsaaSupported();
        void SetRenderScale(float scale);     // 0 = Auto, ngược lại 0.25..1
        void SetQuality(int quality);         // 0 Low 1 Medium 2 High 3 Best
        void SetFrameLimit(int fps);          // 0 = không giới hạn
        void SetAudioEnabled(bool on);

        // Hướng trang: 0 = dọc, 1 = LandscapeLeft, 2 = LandscapeRight (Rust vẽ ngang, Present xoay 90°)
        void SetOrientation(int rotation);
        // Kích thước vùng trang (DIP, theo hướng hiện tại) -> vùng nhìn thấy (trừ dải nút ảo)
        void SetViewSize(float w, float h);
        // App bị treo/quay lại (PhoneApplicationService Deactivated/Activated): dừng/chạy âm thanh
        void SetSuspended(bool suspended);

    internal:
        HRESULT Connect(IDrawingSurfaceRuntimeHostNative* host, ID3D11Device1* device);
        void Disconnect();
        HRESULT PrepareResources(const LARGE_INTEGER* presentTargetTime, DrawingSurfaceSizeF* desiredSize);
        HRESULT Draw(ID3D11Device1* device, ID3D11DeviceContext1* context, ID3D11RenderTargetView* rtv);
        void AddLog(const char* msg);

    private:
        ~RufflePlayer80();
        void ApplyPending(UINT w, UINT h);
        void CreatePlayer(UINT w, UINT h);
        void DestroyPlayer();
        void UpdateAudio();                   // dừng engine khi không chơi / tắt tiếng / app bị treo
        void DrawCpuDirect(ID3D11DeviceContext1* context, ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc);

        Microsoft::WRL::ComPtr<IDrawingSurfaceRuntimeHostNative> m_host;

        // ---- dùng chung giữa thread UI và thread vẽ (m_lock)
        std::mutex m_lock;
        std::vector<uint8_t> m_pendingSwf;
        bool m_hasPendingSwf, m_pendingUnload;
        struct PointerEvt { int kind; float nx, ny; };
        std::vector<PointerEvt> m_pointers;
        std::string m_status;
        bool m_useGpu, m_pendingGpuChange;
        bool m_msaa;
        float m_renderScale;                  // 0 = Auto
        int m_quality, m_frameLimit;
        bool m_qualityDirty;
        bool m_audioEnabled, m_suspended;
        int m_rotation;
        float m_viewW, m_viewH;
        float m_zoom, m_panX, m_panY;
        bool m_loadFailed;

        // ---- chỉ thread vẽ
        RuffleHandle* m_ruffle;
        bool m_playing, m_ruffleGpu;
        std::vector<uint8_t> m_swf;           // giữ SWF để nạp lại (đổi renderer, device mất hẳn)
        std::unique_ptr<GpuRenderer> m_gpu;
        ID3D11Device* m_gpuDevice;            // device mà m_gpu đang dùng (so để phát hiện device mới)
        UINT m_width, m_height;               // RT hệ thống
        UINT m_contentW, m_contentH;          // cỡ player đang vẽ
        bool m_needRender;                    // texture nội dung mới/rỗng -> vẽ lại ngay
        double m_tickAcc;                     // thời gian dồn cho giới hạn FPS
        LARGE_INTEGER m_last, m_freq;
        std::vector<uint8_t> m_swizzle;       // chỉ dùng khi GPU hỏng hẳn (vẽ CPU thẳng vào RT)
        double m_fpsMs; int m_fpsFrames; float m_fps;

        // Âm thanh: thread XAudio2 gọi ruffle_audio_fill -> m_audioLock bảo vệ m_ruffle khi tạo/huỷ player
        std::unique_ptr<AudioOutput> m_audio;
        std::mutex m_audioLock;
    };
}
