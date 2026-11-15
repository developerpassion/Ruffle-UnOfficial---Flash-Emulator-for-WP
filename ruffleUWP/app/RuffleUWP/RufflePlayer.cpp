#include "pch.h"
#include "RufflePlayer.h"
#include <windows.ui.xaml.media.dxinterop.h>

using namespace RuffleUWP;
using namespace Microsoft::WRL;
using namespace Windows::Foundation;
using namespace Windows::System::Threading;
using namespace Windows::Graphics::Display;

static void LogCb(void*, const char* msg) { DebugLog(msg); }

static void ThrowIfFailed(HRESULT hr) { if (FAILED(hr)) throw Platform::Exception::CreateException(hr); }

RufflePlayer::RufflePlayer(Windows::UI::Xaml::Controls::SwapChainPanel^ panel) : m_panel(panel)
{
    DebugLog("RufflePlayer ctor\n");
    m_scale = DisplayInformation::GetForCurrentView()->LogicalDpi / 96.0f;
    m_scaleX = m_scaleY = m_scale; // tạm thời; Resize sẽ lấy CompositionScale thật của panel
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE == 1
    return; // build chẩn đoán: dừng sau DPI
#endif
    CreateDevice();
    DebugLog("D3D11 device OK\n");
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE == 2
    return; // dừng sau D3D device
#endif
    {
        char msg[64];
        sprintf_s(msg, "Feature level 0x%x\n", (unsigned)m_device->GetFeatureLevel());
        DebugLog(msg);
    }
    RegisterStorage(); // save game (SharedObject) -> LocalState\SharedObjects
    RegisterWebCache(); // file tải qua mạng -> dùng lại khi offline
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE == 3
    return; // dừng sau các lệnh gọi Rust đầu tiên
#endif
#ifndef RUFFLE_NO_GPU // build chẩn đoán: msbuild /p:NoGpu=true -> chỉ renderer CPU
    m_gpu = std::make_unique<GpuRenderer>(m_device.Get(), m_context.Get());
#endif
    if (m_gpu && !m_gpu->Ok()) { m_gpu.reset(); DebugLog("GPU renderer unavailable -> CPU\n"); }
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE == 4
    return; // dừng sau GPU renderer
#endif
    m_audio = std::make_unique<AudioOutput>([this](float* out, uint32_t frames) {
        std::lock_guard<std::mutex> g(m_audioLock);
        ruffle_audio_fill(m_ruffle, out, frames); // m_ruffle null -> im lặng
    });
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE == 5
    return; // dừng sau audio
#endif
    m_loop = ThreadPool::RunAsync(ref new WorkItemHandler([this](IAsyncAction^ a) { RenderLoop(a); }),
                                  WorkItemPriority::High, WorkItemOptions::TimeSliced);
}

RufflePlayer::~RufflePlayer()
{
    m_running = false;
    if (m_loop) m_loop->Cancel();
    m_audio.reset();
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_destroy(m_ruffle);
    m_ruffle = nullptr;
    m_gpu.reset(); // sau ruffle_destroy: Rust còn trả texture/mesh khi huỷ player
}

void RufflePlayer::SetUseGpu(bool gpu)
{
    std::vector<uint8_t> swf;
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_useGpu == gpu) return;
        m_useGpu = gpu;
        swf = m_swf;
    }
    if (m_dipW > 0) Resize(m_dipW, m_dipH); // Auto render scale khác nhau giữa GPU/CPU
    if (!swf.empty()) Load(std::move(swf));
}

// Tắt âm thanh = dừng engine XAudio2, KHÔNG nạp lại game. Ruffle chỉ giải mã/trộn tiếng khi XAudio2 xin dữ liệu
// (ruffle_audio_fill), nên dừng engine cũng tiết kiệm CPU. Âm thanh stream gắn timeline: Ruffle thấy lệch thì
// tự dừng stream đó, game vẫn chạy.
void RufflePlayer::SetAudioEnabled(bool on)
{
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_audioEnabled == on) return;
        m_audioEnabled = on;
    }
    if (m_audio) m_audio->SetPaused(!on || m_suspended);
}

void RufflePlayer::SetQuality(int quality)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_quality = quality;
    if (m_ruffle)
    {
        ruffle_set_quality(m_ruffle, quality);
        m_viewDirty = true; // vẽ lại với chất lượng mới
    }
}

void RufflePlayer::KeyEvent(uint32_t vk, uint32_t mods, bool down)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_key(m_ruffle, vk, mods, down ? 1 : 0);
}

void RufflePlayer::CharEvent(uint32_t codepoint)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_char(m_ruffle, codepoint);
}

void RufflePlayer::SetSuspended(bool suspended)
{
    m_suspended = suspended;
    if (m_audio) m_audio->SetPaused(suspended || !m_audioEnabled);
    // Bỏ khoảng thời gian bị suspend để player không "đuổi" frame khi quay lại
    DebugLog(suspended ? "Suspended\n" : "Resumed\n");
}

void RufflePlayer::CreateDevice()
{
    // Lumia phần lớn chỉ có FL 9_3; giữ danh sách thấp.
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
                                   D3D_FEATURE_LEVEL_9_1 };
    ThrowIfFailed(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr, &m_context));
}

void RufflePlayer::CreateSwapChain(UINT w, UINT h)
{
    m_staging.Reset();
    if (m_swapChain)
    {
        m_context->ClearState();
        if (m_gpu) m_gpu->ReleaseScreen();
        ThrowIfFailed(m_swapChain->ResizeBuffers(2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
    }
    else
    {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = w; d.Height = h;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGIDevice1> dxgiDevice; ThrowIfFailed(m_device.As(&dxgiDevice));
        ComPtr<IDXGIAdapter> adapter;     ThrowIfFailed(dxgiDevice->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> factory;    ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&factory)));
        ThrowIfFailed(factory->CreateSwapChainForComposition(m_device.Get(), &d, nullptr, &m_swapChain));

        ComPtr<ISwapChainPanelNative> native;
        ThrowIfFailed(reinterpret_cast<IUnknown*>(m_panel.Get())->QueryInterface(IID_PPV_ARGS(&native)));
        ThrowIfFailed(native->SetSwapChain(m_swapChain.Get()));
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ThrowIfFailed(m_device->CreateTexture2D(&td, nullptr, &m_staging));

    // SwapChainPanel coi 1 pixel swapchain = 1 DIP. Buffer được tạo theo pixel thật (DIP * scale),
    // nên phải thu nhỏ lại 1/scale, nếu không hình bị phóng to đúng bằng hệ số scale (x3 trên Lumia/Elite x3).
    ApplyViewTransform();
    m_needUpload = true;
}

// Ma trận swapchain: buffer pixel -> DIP (kéo giãn theo độ phân giải vẽ) rồi zoom/dịch.
// Màn hình (DIP) = buffer * dipToBuf * zoom + (tx, ty). GPU làm việc này khi composite, gần như miễn phí.
void RufflePlayer::ApplyViewTransform()
{
    ComPtr<IDXGISwapChain2> sc2;
    if (m_swapChain && SUCCEEDED(m_swapChain.As(&sc2)))
    {
        DXGI_MATRIX_3X2_F m = {};
        m._11 = (float)dipToBufX() * m_zoom;
        m._22 = (float)dipToBufY() * m_zoom;
        m._31 = m_viewX;
        m._32 = m_viewY;
        sc2->SetMatrixTransform(&m);
    }
}

void RufflePlayer::SetView(float zoom, float tx, float ty)
{
    std::lock_guard<std::mutex> g(m_lock);
    // Giới hạn để nội dung luôn phủ kín panel (không lộ khoảng trống khi dịch)
    zoom = (std::max)(1.0f, (std::min)(zoom, 4.0f));
    tx = (std::min)(0.0f, (std::max)(tx, m_dipW - m_dipW * zoom));
    ty = (std::min)(0.0f, (std::max)(ty, m_dipH - m_dipH * zoom));
    m_zoom = zoom; m_viewX = tx; m_viewY = ty;
    ApplyViewTransform();
    m_viewDirty = true; // ma trận chỉ có hiệu lực ở lần Present kế tiếp -> render loop vẽ lại + present
}

void RufflePlayer::SetRenderScale(float s)
{
    m_renderScaleSetting = s;
    if (m_dipW > 0 && m_dipH > 0) Resize(m_dipW, m_dipH);
}

void RufflePlayer::Resize(float dipW, float dipH)
{
    std::lock_guard<std::mutex> g(m_lock);
    m_dipW = dipW; m_dipH = dipH;
    // CompositionScale = tỉ lệ pixel thật / DIP của panel (chính xác hơn LogicalDpi trên điện thoại)
    float sx = m_panel->CompositionScaleX, sy = m_panel->CompositionScaleY;
    if (sx > 0.0f && sy > 0.0f) { m_scaleX = sx; m_scaleY = sy; }
    float fullW = dipW * m_scaleX, fullH = dipH * m_scaleY;
    // Giảm độ phân giải vẽ: renderer chạy trên CPU nên chi phí tỉ lệ với số pixel.
    // Auto: tối đa ~1280x720 pixel (≈0.92 MP); màn 2560x1440 -> vẽ 1280x720 rồi GPU phóng to.
    float rs = m_renderScaleSetting;
    if (rs <= 0.0f && m_gpu && m_useGpu)
    {
        rs = 1.0f; // GPU: vẽ đủ độ phân giải
    }
    else if (rs <= 0.0f)
    {
        const float budget = 1280.0f * 720.0f;
        rs = (std::min)(1.0f, std::sqrt(budget / (std::max)(1.0f, fullW * fullH)));
    }
    m_renderScale = rs;
    m_scale = m_scaleX * rs;
    m_width = std::max<UINT>(1, (UINT)(fullW * rs + 0.5f));
    m_height = std::max<UINT>(1, (UINT)(fullH * rs + 0.5f));
    {
        char msg[160];
        sprintf_s(msg, "Resize %.1fx%.1f dip, scale %.3f, render %.0f%% -> %ux%u px\n",
                  dipW, dipH, m_scaleX, rs * 100.0f, m_width, m_height);
        DebugLog(msg);
    }
    CreateSwapChain(m_width, m_height);
    if (m_ruffle) ruffle_resize(m_ruffle, m_width, m_height, m_scale);
}


bool RufflePlayer::Load(std::vector<uint8_t> swf)
{
    DebugLog("Load SWF\n");
    std::lock_guard<std::mutex> g(m_lock);
    m_swf = swf;
    bool gpu = m_gpu && m_useGpu;
    // Huỷ player cũ trước khi tạo mới (tránh giữ 2 bộ texture GPU cùng lúc)
    {
        std::lock_guard<std::mutex> ga(m_audioLock);
        if (m_ruffle) ruffle_destroy(m_ruffle);
        m_ruffle = nullptr;
    }
    RuffleHandle* fresh = gpu
        ? ruffle_create_gpu(swf.data(), swf.size(), m_width, m_height, m_scale, &LogCb, nullptr, m_gpu->Host())
        : ruffle_create(swf.data(), swf.size(), m_width, m_height, m_scale, &LogCb, nullptr);
    {
        std::lock_guard<std::mutex> ga(m_audioLock);
        m_ruffle = fresh;
    }
    m_ruffleGpu = gpu && fresh;
    if (fresh) ruffle_set_quality(fresh, m_quality); // chất lượng từ Settings (game có thể tự đổi)
    m_needUpload = true;
    DebugLog(gpu ? "Renderer: GPU (Direct3D 11)\n" : "Renderer: CPU (software)\n");
    m_playedMs = 0;
    DebugLog(m_ruffle ? "ruffle_create OK\n" : "ruffle_create failed (invalid SWF?)\n");
    return m_ruffle != nullptr;
}

void RufflePlayer::PointerEvent(int kind, float dipX, float dipY)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_mouse(m_ruffle, kind, ContentX(dipX) * m_scale, ContentY(dipY) * m_scale);
}

bool RufflePlayer::MouseButton(int kind, int button, float dipX, float dipY)
{
    std::lock_guard<std::mutex> g(m_lock);
    return m_ruffle && ruffle_mouse_button(m_ruffle, kind, button, ContentX(dipX) * m_scale, ContentY(dipY) * m_scale) != 0;
}

void RufflePlayer::MouseWheel(double lines)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_mouse_wheel(m_ruffle, lines);
}

static void MenuItemCb(void* ctx, uint32_t, const char* caption, int32_t enabled, int32_t checked, int32_t sep)
{
    auto items = static_cast<std::vector<RufflePlayer::MenuItem>*>(ctx);
    int n = MultiByteToWideChar(CP_UTF8, 0, caption, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, caption, -1, &w[0], n);
    items->push_back({ w, enabled != 0, checked != 0, sep != 0 });
}

std::vector<RufflePlayer::MenuItem> RufflePlayer::PrepareContextMenu()
{
    std::vector<MenuItem> items;
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_context_menu(m_ruffle, &MenuItemCb, &items);
    return items;
}

void RufflePlayer::ContextMenuSelect(uint32_t index)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_context_menu_select(m_ruffle, index);
}

void RufflePlayer::ContextMenuClosed(float dipX, float dipY)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (m_ruffle) ruffle_context_menu_closed(m_ruffle, ContentX(dipX) * m_scale, ContentY(dipY) * m_scale);
}

void RufflePlayer::RenderLoop(IAsyncAction^ action)
{
    LARGE_INTEGER freq, last, now;
    LONGLONG nextAt = 0; // mốc thời gian khung kế tiếp khi giới hạn FPS
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (m_running && action->Status == AsyncStatus::Started)
    {
        QueryPerformanceCounter(&now);
        double dt = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
        // Giới hạn khung hình (Settings): chưa đủ 1000/limit ms thì chờ. Ruffle tự chạy bù nhiều frame
        // trong một lần tick (dt lớn) nên tốc độ game giữ nguyên, chỉ vẽ ít lần hơn -> đều, đỡ giật.
        // Dùng mốc cố định (nextAt += interval) để sleep quá giờ (đồng hồ Windows ~15 ms) không làm tụt FPS trung bình.
        int limit = m_frameLimit;
        if (limit > 0 && !m_suspended)
        {
            LONGLONG interval = freq.QuadPart / limit;
            if (nextAt == 0 || now.QuadPart - nextAt > interval) nextAt = now.QuadPart; // mới bật / bị chậm nhiều -> đặt lại
            if (now.QuadPart < nextAt)
            {
                double waitMs = (nextAt - now.QuadPart) * 1000.0 / freq.QuadPart;
                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs > 2.0 ? (int)waitMs - 1 : 1));
                continue;
            }
            nextAt += interval;
        }
        else nextAt = 0;
        last = now;
        if (m_suspended)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            QueryPerformanceCounter(&last);
            continue;
        }
        if (dt > 250.0) dt = 250.0; // tránh nhảy frame lớn sau khi bị chặn lâu
        bool presented = false;
        try
        {
            std::lock_guard<std::mutex> g(m_lock);
            if (m_swapChain)
            {
                LARGE_INTEGER t0, t1, t2;
                QueryPerformanceCounter(&t0);
                bool rendered = false;
                bool gpu = m_ruffle && m_ruffleGpu && m_gpu;
                if (gpu)
                {
                    // GPU: Rust vẽ thẳng vào backbuffer (qua RT MSAA) trong lúc tick
                    ComPtr<ID3D11Texture2D> back;
                    ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)));
                    m_gpu->SetScreen(back.Get(), m_width, m_height);
                }
                if (m_ruffle)
                {
                    rendered = ruffle_tick(m_ruffle, dt) != 0;
                    if (m_viewDirty && !rendered)
                    {
                        ruffle_render(m_ruffle); // zoom đổi: vẽ lại khung hiện tại để present với ma trận mới
                        rendered = true;
                    }
                    m_viewDirty = false;
                    m_playedMs += dt;
                    if (!m_dumpPath.empty() && m_playedMs > 2000.0 && (rendered || !gpu)) DumpFrame();
                }
                QueryPerformanceCounter(&t1);
                // Chỉ upload + present khi có hình mới (trước đây copy ~14 MB mỗi vòng lặp dù hình không đổi)
                if (rendered || (!gpu && (m_needUpload || !m_ruffle)))
                {
                    Present();
                    presented = true;
                    m_needUpload = false;
                }
                QueryPerformanceCounter(&t2);
                if (rendered)
                {
                    m_statFrames++;
                    m_statTickMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
                    m_statUploadMs += (t2.QuadPart - t1.QuadPart) * 1000.0 / freq.QuadPart;
                    m_ovFrames++;
                    m_ovWorkMs += (t2.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
                }
            }
        }
        catch (Platform::Exception^ ex)
        {
            DebugLogW((L"RenderLoop exception: " + std::wstring(ex->Message->Data()) + L"\n").c_str());
        }
        // Số liệu cho FPS overlay, cập nhật mỗi giây
        m_ovMs += dt;
        if (m_ovMs >= 1000.0)
        {
            m_fps = (float)(m_ovFrames * 1000.0 / m_ovMs);
            m_frameWorkMs = m_ovFrames ? (float)(m_ovWorkMs / m_ovFrames) : 0.0f;
            m_ovMs = m_ovWorkMs = 0;
            m_ovFrames = 0;
        }
        m_statMs += dt;
        if (m_statMs >= 5000.0)
        {
            if (m_statFrames > 0)
            {
                char msg[200];
                sprintf_s(msg, "FPS %.1f | tick+render %.1f ms/frame | upload+present %.1f ms/frame | %ux%u (render %.0f%%) | %s\n",
                          m_statFrames * 1000.0 / m_statMs, m_statTickMs / m_statFrames, m_statUploadMs / m_statFrames,
                          m_width, m_height, m_renderScale * 100.0f, m_ruffleGpu ? "GPU" : "CPU");
                DebugLog(msg);
            }
            DebugLogCopyToDocuments(); // Documents\Ruffle\ruffle.log
            m_statMs = m_statTickMs = m_statUploadMs = 0;
            m_statFrames = 0;
        }
        // Nhả CPU cho UI thread (std::mutex không công bằng)
        std::this_thread::sleep_for(std::chrono::milliseconds(presented ? 1 : 2));
    }
}

void RufflePlayer::DumpFrame()
{
    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t* fb = nullptr;
    std::vector<uint8_t> gpuPixels;
    if (m_ruffleGpu && m_gpu)
    {
        UINT gw = 0, gh = 0;
        if (m_gpu->ReadScreen(gpuPixels, gw, gh)) { w = gw; h = gh; stride = gw * 4; fb = gpuPixels.data(); }
    }
    else fb = ruffle_get_framebuffer(m_ruffle, &w, &h, &stride);
    std::wstring path = std::move(m_dumpPath);
    m_dumpPath.clear();
    if (!fb) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
    uint32_t rowBytes = w * 4, size = 54 + rowBytes * h;
    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, dib = 40; memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
    int32_t bw = (int32_t)w, bh = -(int32_t)h; memcpy(hdr + 18, &bw, 4); memcpy(hdr + 22, &bh, 4);
    uint16_t planes = 1, bpp = 32; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(rowBytes);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* s = fb + y * stride;
        for (uint32_t x = 0; x < w; ++x) { row[x*4] = s[x*4+2]; row[x*4+1] = s[x*4+1]; row[x*4+2] = s[x*4]; row[x*4+3] = s[x*4+3]; }
        fwrite(row.data(), 1, rowBytes, f);
    }
    fclose(f);
    OutputDebugStringW((L"Frame dumped: " + path + L"\n").c_str());
}

void RufflePlayer::Present()
{
    if (m_ruffle && m_ruffleGpu)
    {
        m_swapChain->Present(1, 0); // hình đã được vẽ vào backbuffer bởi GpuRenderer
        return;
    }
    ComPtr<ID3D11Texture2D> back;
    ThrowIfFailed(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)));

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t* fb = m_ruffle ? ruffle_get_framebuffer(m_ruffle, &w, &h, &stride) : nullptr;
    if (fb && w == m_width && h == m_height && m_staging)
    {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        {
            for (uint32_t y = 0; y < h; ++y)
                memcpy((uint8_t*)map.pData + y * map.RowPitch, fb + y * stride, w * 4);
            m_context->Unmap(m_staging.Get(), 0);
            m_context->CopyResource(back.Get(), m_staging.Get());
        }
    }
    else
    {
        ComPtr<ID3D11RenderTargetView> rtv;
        ThrowIfFailed(m_device->CreateRenderTargetView(back.Get(), nullptr, &rtv));
        const float black[4] = { 0, 0, 0, 1 };
        m_context->ClearRenderTargetView(rtv.Get(), black);
    }
    m_swapChain->Present(1, 0);
}

void RufflePlayer::SetMsaa(bool on)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (!m_gpu) return;
    m_gpu->SetMsaa(on);
    m_viewDirty = true; // vẽ lại ngay với số mẫu mới
}
