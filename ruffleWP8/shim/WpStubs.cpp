// Stub cho các hàm Win32 mà Rust std tham chiếu nhưng không có trong WindowsApp.lib
// (spawn process, biến môi trường, LoadLibrary...). Ruffle không gọi chúng trên UWP;
// nếu bị gọi thì trả lỗi ERROR_NOT_SUPPORTED thay vì crash lúc nạp app.
// KHÔNG link kernel32.lib: trên Windows 10 Mobile các hàm này không tồn tại -> app không khởi động.
//
// Chỉ đúng cho x64 / ARM / ARM64 (không có name decoration stdcall). Win32 x86 cần thêm @N.
// RUFFLE_WP80: build cho Windows Phone 8.0 (không có CryptographicBuffer -> dùng rand_s của CRT).
#define _CRT_RAND_S
#include <stdlib.h>
#include <windows.h>
#include <wchar.h>
#include <intrin.h>

#define FAIL_STUB(name, ret)                                            \
    static ret __cdecl Stub_##name()                         \
    { SetLastError(ERROR_NOT_SUPPORTED); return (ret)0; }               \
    extern "C" void* __imp_##name = (void*)&Stub_##name;

FAIL_STUB(FreeEnvironmentStringsW, BOOL)
FAIL_STUB(LoadLibraryExA, HMODULE)
FAIL_STUB(GetModuleHandleA, HMODULE)
FAIL_STUB(CreateProcessW, BOOL)
FAIL_STUB(GetEnvironmentStringsW, LPWCH)
FAIL_STUB(InitializeProcThreadAttributeList, BOOL)
FAIL_STUB(UpdateProcThreadAttribute, BOOL)
FAIL_STUB(DeviceIoControl, BOOL)
FAIL_STUB(GetEnvironmentVariableW, DWORD)
FAIL_STUB(SetEnvironmentVariableW, BOOL)
FAIL_STUB(CreatePipe, BOOL)
FAIL_STUB(GetModuleHandleExW, BOOL)
FAIL_STUB(GetModuleHandleW, HMODULE)
FAIL_STUB(DeleteProcThreadAttributeList, int)

static int __cdecl Stub_lstrlenW(const wchar_t* s) { return s ? (int)wcslen(s) : 0; }
extern "C" void* __imp_lstrlenW = (void*)&Stub_lstrlenW;

static void __cdecl Stub_ExitProcess(UINT) { __fastfail(7 /*FAST_FAIL_FATAL_APP_EXIT*/); }
extern "C" void* __imp_ExitProcess = (void*)&Stub_ExitProcess;

static HANDLE __cdecl Stub_CreateFileW() { SetLastError(ERROR_NOT_SUPPORTED); return INVALID_HANDLE_VALUE; }
extern "C" void* __imp_CreateFileW = (void*)&Stub_CreateFileW;

// ntdll (không có trong WindowsApp.lib): std dùng cho anonymous pipe / file rename-delete.
static LONG __cdecl Stub_NtStatusNotSupported() { return (LONG)0xC00000BBL; /* STATUS_NOT_SUPPORTED */ }
extern "C" void* __imp_NtCreateNamedPipeFile = (void*)&Stub_NtStatusNotSupported;
extern "C" void* __imp_NtSetInformationFile = (void*)&Stub_NtStatusNotSupported;

// ---------------------------------------------------------------------------------------------
// Windows 10 Mobile 10586 (vd. Lumia 650): Rust std import thẳng (raw-dylib) vài hàm mà bản này có thể
// không có -> app không khởi động được. Định nghĩa __imp_ ở đây để linker dùng bản này thay vì import DLL.
// bcryptprimitives!ProcessPrng (Rust std: HashMap seed, rand). WP 8.1 không có bcrypt ->
// dùng WinRT CryptographicBuffer (file này build với /ZW).
static BOOL __cdecl Stub_ProcessPrng(PBYTE data, SIZE_T len)
{
#ifdef RUFFLE_WP80
    // WP 8.0: không có CryptographicBuffer/bcrypt -> rand_s (nguồn ngẫu nhiên của hệ thống qua CRT)
    while (len > 0)
    {
        unsigned int v;
        if (rand_s(&v) != 0) return FALSE;
        size_t n = len < sizeof(v) ? len : sizeof(v);
        memcpy(data, &v, n);
        data += n; len -= n;
    }
    return TRUE;
#else
    using Windows::Security::Cryptography::CryptographicBuffer;
    while (len > 0)
    {
        unsigned chunk = len > 0x100000 ? 0x100000 : (unsigned)len;
        Platform::Array<unsigned char>^ arr = nullptr;
        CryptographicBuffer::CopyToByteArray(CryptographicBuffer::GenerateRandom(chunk), &arr);
        memcpy(data, arr->Data, chunk);
        data += chunk; len -= chunk;
    }
    return TRUE;
#endif
}
extern "C" void* __imp_ProcessPrng = (void*)&Stub_ProcessPrng;

// kernel32!GetSystemTimePreciseAsFileTime -> GetSystemTimeAsFileTime (độ phân giải thấp hơn, đủ dùng)
static void __cdecl Stub_GetSystemTimePreciseAsFileTime(LPFILETIME ft) { GetSystemTimeAsFileTime(ft); }
extern "C" void* __imp_GetSystemTimePreciseAsFileTime = (void*)&Stub_GetSystemTimePreciseAsFileTime;

// Console / file metadata: app không dùng (không có console; std::fs không dùng trên UWP ở đây)
static HANDLE __cdecl Stub_GetStdHandle(DWORD) { return nullptr; } // null = không có stdout, std bỏ qua
extern "C" void* __imp_GetStdHandle = (void*)&Stub_GetStdHandle;
FAIL_STUB(WriteConsoleW, BOOL)
FAIL_STUB(GetTimeZoneInformationForYear, BOOL)
FAIL_STUB(GetFullPathNameW, DWORD)
FAIL_STUB(SetFileTime, BOOL)
FAIL_STUB(GetFileInformationByHandleEx, BOOL)
FAIL_STUB(SetFileInformationByHandle, BOOL)

// ---------------------------------------------------------------------------------------------
// Windows 10 Mobile 10586: app không nạp được khi import thẳng kernel32.dll (Rust std raw-dylib).
// Cài lại 5 hàm kernel32 mà không dùng kernel32: TEB (mã lỗi), ntdll (NtClose, NtQuerySystemTime),
// QueryPerformanceCounter (api-ms-win-core-profile). Áp dụng cho mọi code trong exe (C++ lẫn Rust).
extern "C" __declspec(dllimport) LONG NTAPI NtClose(HANDLE h);
extern "C" __declspec(dllimport) LONG NTAPI NtQuerySystemTime(PLARGE_INTEGER t);
extern "C" __declspec(dllimport) ULONG NTAPI RtlNtStatusToDosError(LONG status);

static DWORD* LastErrorSlot()
{
    // TEB.LastErrorValue: 0x34 (32-bit), 0x68 (64-bit)
    return (DWORD*)((BYTE*)NtCurrentTeb() + (sizeof(void*) == 8 ? 0x68 : 0x34));
}
static DWORD __cdecl Stub_GetLastError() { return *LastErrorSlot(); }
static void __cdecl Stub_SetLastError(DWORD e) { *LastErrorSlot() = e; }
extern "C" void* __imp_GetLastError = (void*)&Stub_GetLastError;
extern "C" void* __imp_SetLastError = (void*)&Stub_SetLastError;

static BOOL __cdecl Stub_CloseHandle(HANDLE h)
{
    LONG st = NtClose(h);
    if (st < 0) { *LastErrorSlot() = RtlNtStatusToDosError(st); return FALSE; }
    return TRUE;
}
extern "C" void* __imp_CloseHandle = (void*)&Stub_CloseHandle;

static void __cdecl Stub_GetSystemTimeAsFileTime(LPFILETIME ft)
{
    LARGE_INTEGER t;
    NtQuerySystemTime(&t); // cùng đơn vị/epoch với FILETIME (100ns từ 1601)
    ft->dwLowDateTime = t.LowPart;
    ft->dwHighDateTime = (DWORD)t.HighPart;
}
extern "C" void* __imp_GetSystemTimeAsFileTime = (void*)&Stub_GetSystemTimeAsFileTime;

static ULONGLONG __cdecl Stub_GetTickCount64()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart / (f.QuadPart / 1000));
}
extern "C" void* __imp_GetTickCount64 = (void*)&Stub_GetTickCount64;


// ---------------------------------------------------------------------------------------------
// Windows Phone 8.1: các hàm Rust std tham chiếu nhưng WindowsPhoneCore.lib không có.
// Ruffle không dùng thư mục hiện hành / command line; WaitForMultipleObjects -> bản Ex (có trên 8.1).
static LPWSTR __cdecl Stub_GetCommandLineW() { static wchar_t empty[1] = { 0 }; return empty; }
extern "C" void* __imp_GetCommandLineW = (void*)&Stub_GetCommandLineW;
FAIL_STUB(GetCurrentDirectoryW, DWORD)
FAIL_STUB(SetCurrentDirectoryW, BOOL)
static DWORD __cdecl Stub_WaitForMultipleObjects(DWORD n, const HANDLE* h, BOOL all, DWORD ms)
{ return WaitForMultipleObjectsEx(n, h, all, ms, FALSE); }
extern "C" void* __imp_WaitForMultipleObjects = (void*)&Stub_WaitForMultipleObjects;
