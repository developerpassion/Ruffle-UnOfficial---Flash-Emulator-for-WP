#include "RufflePlayer80.h"
#include "ruffle_uwp.h"
#include <cstdio>

using namespace RuffleWP80Native;
using namespace Microsoft::WRL;
using namespace Windows::Phone::Graphics::Interop;

namespace
{
    // Cầu nối COM mà DrawingSurfaceBackgroundGrid gọi; chuyển tiếp về RufflePlayer80.
    class ContentProvider : public RuntimeClass<RuntimeClassFlags<WinRtClassicComMix>,
                                                ABI::Windows::Phone::Graphics::Interop::IDrawingSurfaceBackgroundContentProvider,
                                                IDrawingSurfaceBackgroundContentProviderNative>
    {
    public:
        ContentProvider(RufflePlayer80^ player) : m_player(player) {}
        HRESULT STDMETHODCALLTYPE Connect(IDrawingSurfaceRuntimeHostNative* host, ID3D11Device1* device) override
        { return m_player->Connect(host, device); }
        void STDMETHODCALLTYPE Disconnect() override { m_player->Disconnect(); }
        HRESULT STDMETHODCALLTYPE PrepareResources(const LARGE_INTEGER* t, DrawingSurfaceSizeF* size) override
        { return m_player->PrepareResources(t, size); }
        HRESULT STDMETHODCALLTYPE Draw(ID3D11Device1* d, ID3D11DeviceContext1* c, ID3D11RenderTargetView* rtv) override
        { return m_player->Draw(d, c, rtv); }
    private:
        RufflePlayer80^ m_player;
    };

    void __cdecl LogCb(void*, const char* msg)
    {
        // Log của SWF (trace...) chỉ ra debug output; không hiện lên màn hình
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
    }

    Platform::String^ ToPlatform(const std::string& s)
    {
        std::wstring w(s.begin(), s.end()); // thông báo ASCII
        return ref new Platform::String(w.c_str());
    }
}

RufflePlayer80::RufflePlayer80()
    : m_hasPendingSwf(false), m_pendingUnload(false), m_useGpu(true), m_pendingGpuChange(false), m_msaa(false),
      m_renderScale(0), m_quality(2), m_frameLimit(0), m_qualityDirty(false), m_audioEnabled(true), m_suspended(false),
      m_rotation(0), m_viewW(0), m_viewH(0), m_zoom(1), m_panX(0), m_panY(0), m_loadFailed(false),
      m_ruffle(nullptr), m_playing(false), m_ruffleGpu(false), m_gpuDevice(nullptr), m_width(0), m_height(0),
      m_contentW(0), m_contentH(0), m_needRender(false), m_tickAcc(0), m_fpsMs(0), m_fpsFrames(0), m_fps(0)
{
    m_last.QuadPart = 0;
    QueryPerformanceFrequency(&m_freq);
    ruffle_set_audio_enabled(1);

    // Lambda chỉ giữ con trỏ tới thành viên (không giữ ^ của player -> không tạo vòng tham chiếu).
    // m_audio huỷ trước các thành viên này (xem destructor).
    RuffleHandle** pRuffle = &m_ruffle;
    std::mutex* pLock = &m_audioLock;
    m_audio.reset(new AudioOutput([pRuffle, pLock](float* out, uint32_t frames) {
        std::lock_guard<std::mutex> g(*pLock);
        if (*pRuffle) ruffle_audio_fill(*pRuffle, out, frames);
        else memset(out, 0, frames * 2 * sizeof(float));
    }));
    if (!m_audio->Ok())
    {
        char msg[64];
        sprintf_s(msg, "No sound: XAudio2 init failed 0x%08X", (unsigned)m_audio->InitResult());
        AddLog(msg);
    }
    UpdateAudio(); // chưa có game -> dừng engine
}

RufflePlayer80::~RufflePlayer80()
{
    m_audio.reset(); // dừng thread âm thanh trước khi huỷ player
    DestroyPlayer();
    m_gpu.reset();
}

IDrawingSurfaceBackgroundContentProvider^ RufflePlayer80::CreateContentProvider()
{
    ComPtr<ContentProvider> provider = Make<ContentProvider>(this);
    return reinterpret_cast<IDrawingSurfaceBackgroundContentProvider^>(provider.Get());
}

// ---------------------------------------------------------------- lệnh từ thread UI

void RufflePlayer80::LoadSwf(const Platform::Array<uint8>^ data)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_pendingSwf.assign(data->Data, data->Data + data->Length);
    m_hasPendingSwf = true;
    m_pendingUnload = false;
    m_loadFailed = false;
    m_zoom = 1; m_panX = 0; m_panY = 0;
    m_status.clear();
    if (m_host) m_host->RequestAdditionalFrame();
}

void RufflePlayer80::Unload()
{
    std::lock_guard<std::mutex> g(m_lock);
    m_hasPendingSwf = false;
    m_pendingUnload = true;
    m_zoom = 1; m_panX = 0; m_panY = 0;
    if (m_host) m_host->RequestAdditionalFrame();
}

bool RufflePlayer80::IsPlaying()
{
    std::lock_guard<std::mutex> g(m_lock);
    return m_hasPendingSwf || !m_loadFailed;
}

void RufflePlayer80::Pointer(int kind, float nx, float ny)
{
    std::lock_guard<std::mutex> g(m_lock);
    PointerEvt e = { kind, nx, ny };
    m_pointers.push_back(e);
}

void RufflePlayer80::SetView(float zoom, float panX, float panY)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_zoom = zoom < 1 ? 1 : (zoom > 4 ? 4 : zoom);
    float maxPan = 1 - 1 / m_zoom;
    m_panX = panX < 0 ? 0 : (panX > maxPan ? maxPan : panX);
    m_panY = panY < 0 ? 0 : (panY > maxPan ? maxPan : panY);
}

Platform::String^ RufflePlayer80::Status()
{
    std::lock_guard<std::mutex> g(m_lock);
    return ToPlatform(m_status);
}

Platform::String^ RufflePlayer80::FpsText()
{
    char buf[48];
    sprintf_s(buf, "%.0f FPS %s", m_fps, m_ruffleGpu ? "GPU" : "CPU");
    return ToPlatform(buf);
}

void RufflePlayer80::AddLog(const char* msg)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_status = msg;
    if (m_status.size() > 200) m_status.resize(200);
}

void RufflePlayer80::SetUseGpu(bool on)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_useGpu == on) return;
    m_useGpu = on;
    m_pendingGpuChange = true;
}

bool RufflePlayer80::UsingGpu() { return m_ruffleGpu; }

void RufflePlayer80::SetMsaa(bool on)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_msaa = on;
}

bool RufflePlayer80::MsaaSupported() { return m_gpu && m_gpu->Ok() && m_gpu->MsaaSupported(); }

void RufflePlayer80::SetRenderScale(float scale)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_renderScale = scale;
}

void RufflePlayer80::SetQuality(int quality)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_quality = quality;
    m_qualityDirty = true;
}

void RufflePlayer80::SetFrameLimit(int fps)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_frameLimit = fps > 0 ? fps : 0;
}

void RufflePlayer80::SetAudioEnabled(bool on)
{
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_audioEnabled = on;
    }
    UpdateAudio();
}

void RufflePlayer80::SetOrientation(int rotation)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_rotation = rotation;
    if (m_host) m_host->RequestAdditionalFrame();
}

void RufflePlayer80::SetViewSize(float w, float h)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_viewW = w; m_viewH = h;
}

void RufflePlayer80::SetSuspended(bool suspended)
{
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_suspended = suspended;
    }
    UpdateAudio();
}

void RufflePlayer80::UpdateAudio()
{
    if (!m_audio) return;
    bool paused;
    {
        std::lock_guard<std::mutex> g(m_lock);
        paused = m_suspended || !m_audioEnabled;
    }
    {
        std::lock_guard<std::mutex> g(m_audioLock);
        paused = paused || m_ruffle == nullptr;
    }
    m_audio->SetPaused(paused);
}

// ---------------------------------------------------------------- player (thread vẽ)

// Player phải huỷ TRƯỚC GpuRenderer (Rust gọi destroy_texture/mesh vào host khi huỷ)
void RufflePlayer80::DestroyPlayer()
{
    {
        std::lock_guard<std::mutex> g(m_audioLock); // thread âm thanh không được dùng player đang bị huỷ
        if (m_ruffle) ruffle_destroy(m_ruffle);
        m_ruffle = nullptr;
    }
    m_playing = false;
    m_ruffleGpu = false;
    UpdateAudio();
}

void RufflePlayer80::CreatePlayer(UINT w, UINT h)
{
    DestroyPlayer();
    if (m_swf.empty()) return;
    bool useGpu;
    int quality;
    {
        std::lock_guard<std::mutex> g(m_lock);
        useGpu = m_useGpu;
        quality = m_quality;
    }
    // pixel thật / pixel logic: theo chiều rộng máy (cạnh ngắn = 480 logic), KHÔNG theo w (w là cạnh dài khi xoay ngang)
    double scale = (m_width ? m_width : w) / 480.0;
    m_contentW = w; m_contentH = h; // player được tạo đúng cỡ này -> Draw chỉ resize khi cỡ nội dung đổi sau đó
    RuffleHandle* h2 = nullptr;
    if (useGpu && m_gpu && m_gpu->Ok())
    {
        h2 = ruffle_create_gpu(m_swf.data(), m_swf.size(), w, h, scale, &LogCb, nullptr, m_gpu->Host());
        m_ruffleGpu = h2 != nullptr;
    }
    if (!h2)
        h2 = ruffle_create(m_swf.data(), m_swf.size(), w, h, scale, &LogCb, nullptr);
    {
        std::lock_guard<std::mutex> g(m_audioLock);
        m_ruffle = h2;
    }
    m_playing = m_ruffle != nullptr;
    if (m_ruffle) ruffle_set_quality(m_ruffle, quality);
    UpdateAudio();
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_loadFailed = m_ruffle == nullptr;
        if (!m_ruffle) m_status = "Not a valid SWF file";
        else if (useGpu && m_gpu && !m_gpu->Ok()) m_status = m_gpu->Error();
    }
    m_needRender = true;
    m_tickAcc = 0;
    m_fpsMs = 0; m_fpsFrames = 0;
    QueryPerformanceCounter(&m_last);
}

// Áp các lệnh từ thread UI (gọi trên thread vẽ). w x h = cỡ nội dung hiện tại.
void RufflePlayer80::ApplyPending(UINT w, UINT h)
{
    bool load = false, unload = false, gpuChange = false, qualityDirty = false;
    int quality;
    float zoom, panX, panY;
    std::vector<PointerEvt> ptrs;
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_hasPendingSwf) { m_swf.swap(m_pendingSwf); m_pendingSwf.clear(); load = true; m_hasPendingSwf = false; }
        unload = m_pendingUnload; m_pendingUnload = false;
        gpuChange = m_pendingGpuChange; m_pendingGpuChange = false;
        qualityDirty = m_qualityDirty; m_qualityDirty = false;
        quality = m_quality;
        zoom = m_zoom; panX = m_panX; panY = m_panY;
        ptrs.swap(m_pointers);
    }
    if (unload) { DestroyPlayer(); m_swf.clear(); }
    if (load || (gpuChange && m_ruffle)) CreatePlayer(w, h);
    if (!m_ruffle) return;
    if (qualityDirty) ruffle_set_quality(m_ruffle, quality);
    for (size_t i = 0; i < ptrs.size(); ++i)
    {
        // Toạ độ trên màn (chuẩn hoá) -> trên nội dung theo zoom/kéo -> pixel nội dung
        double x = (panX + ptrs[i].nx / zoom) * w, y = (panY + ptrs[i].ny / zoom) * h;
        ruffle_mouse(m_ruffle, 0, x, y);
        if (ptrs[i].kind == 1 || ptrs[i].kind == 2) ruffle_mouse_button(m_ruffle, ptrs[i].kind, 0, x, y);
    }
}

// ---------------------------------------------------------------- DrawingSurfaceBackgroundGrid

HRESULT RufflePlayer80::Connect(IDrawingSurfaceRuntimeHostNative* host, ID3D11Device1*)
{
    m_host = host;
    QueryPerformanceCounter(&m_last);
    return S_OK;
}

void RufflePlayer80::Disconnect()
{
    m_host = nullptr;
}

HRESULT RufflePlayer80::PrepareResources(const LARGE_INTEGER*, DrawingSurfaceSizeF*)
{
    return S_OK; // giữ kích thước RT hệ thống đề xuất (độ phân giải gốc)
}

HRESULT RufflePlayer80::Draw(ID3D11Device1* device, ID3D11DeviceContext1* context, ID3D11RenderTargetView* rtv)
{
    ComPtr<ID3D11Resource> res;
    rtv->GetResource(&res);
    ComPtr<ID3D11Texture2D> target;
    res.As(&target);
    D3D11_TEXTURE2D_DESC desc;
    target->GetDesc(&desc);
    m_width = desc.Width; m_height = desc.Height;

    // Device mới (vd. quay lại từ màn chuyển app): chuyển texture/mesh sang device mới -> game chạy tiếp, không nạp lại.
    // Không chuyển được (device cũ mất hẳn) -> tạo lại renderer và nạp lại SWF sau khi biết cỡ nội dung.
    bool recreate = false;
    if (m_gpuDevice != device)
    {
        bool migrated = m_gpu && m_gpu->Ok() && m_gpu->MigrateTo(device, context);
        if (!migrated)
        {
            recreate = m_ruffle != nullptr;
            DestroyPlayer();
            m_gpu.reset(new GpuRenderer(device, context));
            if (!m_gpu->Ok()) AddLog(m_gpu->Error().c_str());
        }
        m_gpuDevice = device;
        m_needRender = true;
    }

    // Đọc cài đặt hiển thị
    int rotation, frameLimit;
    float vw, vh, renderScale, zoom, panX, panY;
    bool msaa;
    {
        std::lock_guard<std::mutex> g(m_lock);
        rotation = m_rotation; frameLimit = m_frameLimit;
        vw = m_viewW; vh = m_viewH; renderScale = m_renderScale; msaa = m_msaa;
        zoom = m_zoom; panX = m_panX; panY = m_panY;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = (now.QuadPart - m_last.QuadPart) * 1000.0 / m_freq.QuadPart;
    m_last = now;
    if (dt > 250.0) dt = 250.0;

    if (!m_gpu->Ok())
    {
        // GPU hỏng hẳn: vẽ CPU thẳng vào RT (không xoay/zoom/độ phân giải)
        ApplyPending(desc.Width, desc.Height);
        DrawCpuDirect(context, target.Get(), desc);
        if (m_host && m_ruffle) m_host->RequestAdditionalFrame();
        return S_OK;
    }

    // Vùng nhìn thấy: RT phủ cả dải nút ảo ở đáy máy (HTC One M8...). Pixel/DIP lấy theo chiều rộng máy
    // (cạnh ngắn của trang); chiều cao nhìn thấy = cạnh dài của trang * tỉ lệ đó, tính từ đỉnh RT.
    UINT visH = desc.Height;
    if (vw > 0 && vh > 0)
    {
        float shortSide = vw < vh ? vw : vh, longSide = vw < vh ? vh : vw;
        double s = desc.Width / (double)shortSide;
        UINT h = (UINT)(longSide * s + 0.5);
        if (h > 0 && h < desc.Height) visH = h;
    }
    // Độ phân giải render: Auto = tối đa 720 pixel theo chiều rộng máy (máy 1080p như M8 -> 66%)
    float scale = renderScale > 0 ? renderScale : (desc.Width > 720 ? 720.0f / desc.Width : 1.0f);
    m_gpu->SetMsaa(msaa);
    m_gpu->SetScreen(context, rtv, target.Get(), desc.Width, desc.Height, visH, rotation, scale);
    m_gpu->SetView(zoom, panX, panY);
    UINT cw = m_gpu->ContentWidth(), ch = m_gpu->ContentHeight();

    if (recreate) CreatePlayer(cw, ch);
    ApplyPending(cw, ch);

    // Cỡ nội dung đổi (xoay, dải nút ảo, độ phân giải render, quay lại app) -> resize player cho khớp
    if (m_ruffle && (cw != m_contentW || ch != m_contentH))
    {
        ruffle_resize(m_ruffle, cw, ch, desc.Width / 480.0);
        m_contentW = cw; m_contentH = ch;
        m_needRender = true;
    }

    // Giới hạn FPS: chỉ tick khi đủ thời gian; các khung còn lại vẽ lại hình cũ (Present)
    bool tick = false;
    double tickDt = 0;
    if (m_ruffle)
    {
        m_tickAcc += dt;
        double interval = frameLimit > 0 ? 1000.0 / frameLimit : 0;
        if (m_tickAcc >= interval * 0.9) { tick = true; tickDt = m_tickAcc; m_tickAcc = 0; }
    }

    if (m_ruffle && m_ruffleGpu)
    {
        bool drew = false;
        if (tick) drew = ruffle_tick(m_ruffle, tickDt) != 0;
        if (!drew && m_needRender) ruffle_render(m_ruffle); // texture nội dung mới/rỗng
        m_needRender = false;
        m_gpu->MarkGpuFrame();
        m_gpu->Present();
    }
    else if (m_ruffle)
    {
        if (tick) ruffle_tick(m_ruffle, tickDt);
        if (tick || m_needRender)
        {
            uint32_t fw = 0, fh = 0, stride = 0;
            const uint8_t* fb = ruffle_get_framebuffer(m_ruffle, &fw, &fh, &stride);
            if (fb) m_gpu->PresentCpu(fb, fw, fh, stride);
            else m_gpu->Present();
        }
        else m_gpu->Present();
        m_needRender = false;
    }
    else
    {
        const float bg[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        context->ClearRenderTargetView(rtv, bg);
    }

    if (tick)
    {
        m_fpsMs += tickDt; ++m_fpsFrames;
        if (m_fpsMs >= 1000.0) { m_fps = (float)(m_fpsFrames * 1000.0 / m_fpsMs); m_fpsMs = 0; m_fpsFrames = 0; }
    }
    if (m_host && m_ruffle) m_host->RequestAdditionalFrame(); // vẽ liên tục khi đang chơi
    return S_OK;
}

// Dự phòng khi GPU không khởi tạo được: framebuffer CPU (RGBA) chép thẳng vào RT (đảo R/B nếu RT là BGRA)
void RufflePlayer80::DrawCpuDirect(ID3D11DeviceContext1* context, ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc)
{
    if (m_ruffle)
    {
        if (desc.Width != m_contentW || desc.Height != m_contentH)
        {
            ruffle_resize(m_ruffle, desc.Width, desc.Height, desc.Width / 480.0);
            m_contentW = desc.Width; m_contentH = desc.Height;
        }
        ruffle_tick(m_ruffle, 1000.0 / 60);
        uint32_t fw = 0, fh = 0, stride = 0;
        const uint8_t* fb = ruffle_get_framebuffer(m_ruffle, &fw, &fh, &stride);
        if (fb && fw == desc.Width && fh == desc.Height && desc.SampleDesc.Count == 1)
        {
            const uint8_t* src = fb;
            UINT pitch = stride;
            if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM)
            {
                m_swizzle.resize((size_t)fw * fh * 4);
                for (uint32_t y = 0; y < fh; ++y)
                {
                    const uint8_t* s = fb + (size_t)y * stride;
                    uint8_t* d = m_swizzle.data() + (size_t)y * fw * 4;
                    for (uint32_t x = 0; x < fw; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
                }
                src = m_swizzle.data();
                pitch = fw * 4;
            }
            context->UpdateSubresource(target, 0, nullptr, src, pitch, 0);
        }
    }
}
