#include "pch.h"
#include "MainPage.xaml.h"
#include "ruffle_uwp.h"

using namespace RuffleWP81;
using namespace Microsoft::WRL;
using namespace Windows::UI::Xaml;

// M1: kiểm tra trên máy WP 8.1 rằng (1) app nạp được (import ntdll/api-ms-win-*), (2) D3D11 + SwapChainPanel chạy,
// (3) lõi Rust chạy được: tạo Player (thread, khoá WaitOnAddress, PRNG, heap) và tick vài khung.

MainPage::MainPage()
{
    InitializeComponent();
    Loaded += ref new RoutedEventHandler(this, &MainPage::OnLoaded);
}

void MainPage::Log(Platform::String^ line)
{
    statusText->Text = statusText->Text + line + "\n";
    OutputDebugStringW((std::wstring(line->Data()) + L"\n").c_str());
}

void MainPage::OnLoaded(Platform::Object^, RoutedEventArgs^)
{
    Log("Ruffle WP 8.1 - M1");
    try { InitD3D(); }
    catch (Platform::Exception^ ex) { Log("D3D11 FAIL: " + ex->Message); }
    TestRuffle();
}

void MainPage::InitD3D()
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
                                   D3D_FEATURE_LEVEL_9_1 };
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, &fl, &m_ctx);
    if (FAILED(hr)) throw ref new Platform::COMException(hr, "D3D11CreateDevice");
    wchar_t buf[64];
    swprintf_s(buf, L"D3D11 OK, feature level %x_%x", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
    Log(ref new Platform::String(buf));

    ComPtr<IDXGIDevice1> dxgiDevice;
    m_device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    float scale = Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->RawPixelsPerViewPixel;
    UINT w = (UINT)(swapChainPanel->ActualWidth * scale), h = (UINT)(swapChainPanel->ActualHeight * scale);
    if (w == 0 || h == 0) { w = 480; h = 800; }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = w; sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForComposition(m_device.Get(), &sd, nullptr, &m_swapChain);
    if (FAILED(hr)) throw ref new Platform::COMException(hr, "CreateSwapChainForComposition");

    ComPtr<ISwapChainPanelNative> native;
    reinterpret_cast<IUnknown*>(swapChainPanel)->QueryInterface(IID_PPV_ARGS(&native));
    native->SetSwapChain(m_swapChain.Get());

    // Xoá màn hình màu xanh Ruffle rồi present: thấy màu này = swapchain chạy
    ComPtr<ID3D11Texture2D> back;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    ComPtr<ID3D11RenderTargetView> rtv;
    m_device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    const float color[4] = { 0.215f, 0.322f, 0.549f, 1.0f };
    m_ctx->ClearRenderTargetView(rtv.Get(), color);
    m_swapChain->Present(1, 0);
    swprintf_s(buf, L"SwapChain %ux%u OK", w, h);
    Log(ref new Platform::String(buf));
}

static void __cdecl RuffleLog(void* ctx, const char* msg)
{
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

void MainPage::TestRuffle()
{
    // SWF tối giản: header FWS v10, khung rỗng, 24fps, 1 frame, ShowFrame + End (17 byte)
    static const uint8_t swf[] = { 'F', 'W', 'S', 10, 17, 0, 0, 0, 0x00, 0x00, 24, 1, 0, 0x40, 0x00, 0x00, 0x00 };
    Log("Rust: sample rate = " + ruffle_audio_sample_rate().ToString());
    RuffleHandle* h = ruffle_create(swf, sizeof(swf), 320, 240, 1.0, &RuffleLog, nullptr);
    if (!h) { Log("Rust: ruffle_create FAIL"); return; }
    Log("Rust: ruffle_create OK");
    int frames = 0;
    for (int i = 0; i < 10; ++i) { ruffle_tick(h, 1000.0 / 24); ruffle_render(h); ++frames; }
    uint32_t fw = 0, fh = 0, stride = 0;
    const uint8_t* fb = ruffle_get_framebuffer(h, &fw, &fh, &stride);
    wchar_t buf[96];
    swprintf_s(buf, L"Rust: %d tick+render OK, framebuffer %ux%u %s", frames, fw, fh, fb ? L"OK" : L"null");
    Log(ref new Platform::String(buf));
    ruffle_destroy(h);
    Log("Rust: ruffle_destroy OK -> M1 PASS");
}
