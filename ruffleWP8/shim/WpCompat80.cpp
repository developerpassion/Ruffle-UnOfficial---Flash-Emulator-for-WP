// Chỉ build cho Windows Phone 8.0 (CRT VS2012 = msvcr110 phone).
// 1) Hàm toán C99 mà Rust core/libm gọi nhưng CRT VS2012 chưa có.
// 2) Fls* (fiber local storage) không có trên WP 8.0 -> dùng Tls*. Callback huỷ (destructor TLS của Rust)
//    KHÔNG được gọi khi thread kết thúc: chỉ rò ít bộ nhớ của thread đã thoát (Ruffle có rất ít thread).
#include <windows.h>
#include <math.h>
#include <float.h>

extern "C" {

double trunc(double x) { double i; modf(x, &i); return i; }
float truncf(float x) { return (float)trunc(x); }

double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }
float roundf(float x) { return (float)round(x); }

// Làm tròn tới số nguyên gần nhất, hoà thì về số chẵn (giống chế độ làm tròn mặc định)
double rint(double x)
{
    const double two52 = 4503599627370496.0;
    if (!(fabs(x) < two52)) return x; // đã là số nguyên, hoặc NaN/inf
    double r = x >= 0 ? (x + two52) - two52 : (x - two52) + two52;
    return r == 0 ? _copysign(0.0, x) : r;
}
float rintf(float x) { return (float)rint(x); }

double fma(double a, double b, double c) { return a * b + c; } // không fused, đủ cho renderer/AVM
float fmaf(float a, float b, float c) { return (float)((double)a * b + c); }

double log2(double x) { return log(x) * 1.4426950408889634073599; }
float log2f(float x) { return (float)log2(x); }

double exp2(double x) { return pow(2.0, x); }
float exp2f(float x) { return (float)exp2(x); }

double log1p(double x)
{
    double u = 1.0 + x;
    if (u == 1.0) return x;
    return log(u) * x / (u - 1.0); // bù sai số làm tròn của 1+x
}
float log1pf(float x) { return (float)log1p(x); }

double cbrt(double x)
{
    if (x == 0 || !_finite(x)) return x;
    double y = _copysign(pow(fabs(x), 1.0 / 3.0), x);
    return y - (y * y * y - x) / (3.0 * y * y); // 1 bước Newton
}

double acosh(double x) { return log(x + sqrt(x * x - 1.0)); }
float acoshf(float x) { return (float)acosh(x); }

double asinh(double x)
{
    double a = fabs(x);
    double r = a < 1e-4 ? a : log(a + sqrt(a * a + 1.0));
    return _copysign(r, x);
}
float asinhf(float x) { return (float)asinh(x); }

} // extern "C"

// ---- Fls*: WP 8.0 không có cả Tls* -> bảng slot trong static TLS (__declspec(thread)).
// Rust std chỉ dùng FlsAlloc/FlsSetValue để đăng ký callback huỷ TLS; giá trị đọc qua #[thread_local] riêng.
static const DWORD kSlots = 64;
static __declspec(thread) PVOID t_slots[kSlots];
static volatile LONG g_nextSlot = 0;
static DWORD __cdecl Stub_FlsAlloc(PFLS_CALLBACK_FUNCTION)
{
    LONG i = InterlockedIncrement(&g_nextSlot) - 1;
    return i < (LONG)kSlots ? (DWORD)i : FLS_OUT_OF_INDEXES;
}
static BOOL __cdecl Stub_FlsFree(DWORD) { return TRUE; }
static BOOL __cdecl Stub_FlsSetValue(DWORD i, PVOID v)
{
    if (i >= kSlots) return FALSE;
    t_slots[i] = v;
    return TRUE;
}
static BOOL __cdecl Stub_IsThreadAFiber() { return FALSE; }
extern "C" void* __imp_FlsAlloc = (void*)&Stub_FlsAlloc;
extern "C" void* __imp_FlsFree = (void*)&Stub_FlsFree;
extern "C" void* __imp_FlsSetValue = (void*)&Stub_FlsSetValue;
extern "C" void* __imp_IsThreadAFiber = (void*)&Stub_IsThreadAFiber;
