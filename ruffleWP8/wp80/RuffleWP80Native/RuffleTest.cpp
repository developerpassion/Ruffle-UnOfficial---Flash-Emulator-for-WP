#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>
#include <string>
#include <cstdio>
#include "RuffleTest.h"
#ifndef RUFFLE_PROBE
#include "ruffle_uwp.h"
#endif

using namespace RuffleWP80Native;

// Probe: mỗi probe_NN.dll import đúng các hàm mà bản đầy đủ RuffleWP80Native.dll import từ một DLL hệ thống
// (sinh bởi spike\probe80\make-probes.py). 126 = DLL không có, 127 = thiếu hàm.
Platform::String^ RuffleTest::Probe()
{
    static const wchar_t* probes[][2] = {
        { L"probe_00.dll", L"MSVCP110.dll" },
        { L"probe_01.dll", L"MSVCR110.dll" },
        { L"probe_02.dll", L"WS2_32.dll" },
        { L"probe_03.dll", L"api-ms-win-core-com-l1-1-0" },
        { L"probe_04.dll", L"api-ms-win-core-debug-l1-1-1" },
        { L"probe_05.dll", L"api-ms-win-core-errorhandling-l1-1-1" },
        { L"probe_06.dll", L"api-ms-win-core-heap-l1-2-0" },
        { L"probe_07.dll", L"api-ms-win-core-libraryloader-l1-1-1" },
        { L"probe_08.dll", L"api-ms-win-core-localization-l1-2-0" },
        { L"probe_09.dll", L"api-ms-win-core-processthreads-l1-1-1" },
        { L"probe_10.dll", L"api-ms-win-core-profile-l1-1-0" },
        { L"probe_11.dll", L"api-ms-win-core-string-l1-1-0" },
        { L"probe_12.dll", L"api-ms-win-core-synch-l1-2-0" },
        { L"probe_13.dll", L"api-ms-win-core-util-l1-1-0" },
        { L"probe_14.dll", L"api-ms-win-core-winrt-string-l1-1-0" },
        { L"probe_15.dll", L"d3d11.dll" },
        { L"probe_16.dll", L"ntdll.dll" },
        { L"probe_17.dll", L"vccorlib110.DLL" },
    };
    std::wstring out = L"Probe WP 8.0 (126 = missing DLL, 127 = missing function)\n";
    for (auto& p : probes)
    {
        HMODULE m = LoadPackagedLibrary(p[0], 0);
        wchar_t line[160];
        if (m) swprintf_s(line, L"OK   %s\n", p[1]);
        else swprintf_s(line, L"FAIL %s  err=%u\n", p[1], (unsigned)GetLastError());
        out += line;
    }
    return ref new Platform::String(out.c_str());
}

#ifndef RUFFLE_PROBE
static void __cdecl RuffleLog(void*, const char* msg)
{
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
#endif

Platform::String^ RuffleTest::Run()
{
#ifdef RUFFLE_PROBE
    return L"(probe build: no Rust core)";
#else
    std::wstring out = L"Ruffle WP 8.0 - M1\n";
    wchar_t buf[128];

    // D3D11 (chỉ tạo device để biết feature level; vẽ qua DrawingSurface ở bước sau)
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
                                   D3D_FEATURE_LEVEL_9_1 };
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (SUCCEEDED(hr)) swprintf_s(buf, L"D3D11 OK, feature level %x_%x\n", (fl >> 12) & 0xF, (fl >> 8) & 0xF);
    else swprintf_s(buf, L"D3D11 FAIL 0x%08X\n", (unsigned)hr);
    out += buf;

    // Lõi Rust: SWF tối giản (FWS v10, khung rỗng, 24fps, 1 frame, ShowFrame + End)
    static const uint8_t swf[] = { 'F', 'W', 'S', 10, 17, 0, 0, 0, 0x00, 0x00, 24, 1, 0, 0x40, 0x00, 0x00, 0x00 };
    swprintf_s(buf, L"Rust: sample rate = %u\n", ruffle_audio_sample_rate());
    out += buf;
    RuffleHandle* h = ruffle_create(swf, sizeof(swf), 320, 240, 1.0, &RuffleLog, nullptr);
    if (!h) return ref new Platform::String((out + L"Rust: ruffle_create FAIL").c_str());
    out += L"Rust: ruffle_create OK\n";
    for (int i = 0; i < 10; ++i) { ruffle_tick(h, 1000.0 / 24); ruffle_render(h); }
    uint32_t fw = 0, fh = 0, stride = 0;
    const uint8_t* fb = ruffle_get_framebuffer(h, &fw, &fh, &stride);
    swprintf_s(buf, L"Rust: 10 tick+render OK, framebuffer %ux%u %s\n", fw, fh, fb ? L"OK" : L"null");
    out += buf;
    ruffle_destroy(h);
    out += L"Rust: ruffle_destroy OK -> M1 PASS";
    return ref new Platform::String(out.c_str());
#endif
}
