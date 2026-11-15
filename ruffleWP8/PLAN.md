# PLAN — Port Ruffle sang Windows Phone 8.0 / 8.1

> Đọc file này đầu mỗi session. Chi tiết task: `WORK.md`. Trạng thái: `PROGRESS.md`.
> Dự án anh em: `..\ruffleUWP` (Windows 10 Mobile, đã chạy trên Lumia 650). Tái dùng tối đa code ở đó.

> Hiện tại (2026-11-08): 3 solution, mỗi cái một thư mục, ưu tiên 8.0:
> - `wp80\RuffleWP80.sln` — WP 8.0 `.xap` (C# Silverlight + C++), chạy cả máy 8.0 lẫn 8.1. **Ưu tiên.**
> - `wp81sl\RuffleWP81SL.sln` — Silverlight 8.1 `.xap`, dùng chung mã với wp80 + file picker (chỉ máy 8.1).
> - `wp81\RuffleWP81.sln` — WP 8.1 `.appx` C++/CX (gần ruffleUWP; tạm dừng sau M1).

## 1. Mục tiêu
- Chạy Ruffle trên Lumia **Windows Phone 8.1** (ưu tiên) và **8.0** (sau), ARM32.
- MVP giống ruffleUWP: mở .swf local, render (CPU + GPU D3D11 FL 9_3), touch → mouse, âm thanh, zoom 2 ngón.
- **Gói UWP không cài được trên WP8.x** → cần project riêng cho từng nền tảng.

## 2. Có làm được không? — Đánh giá
| Phần | WP 8.1 (WinRT 8.1 / Store app) | WP 8.0 (Silverlight + native) |
|---|---|---|
| UI | XAML C++/CX, `SwapChainPanel` có sẵn | **Không có XAML C++**: host C# (Silverlight) + WinPRT component C++, `DrawingSurfaceBackgroundGrid` |
| D3D11 | FL 9_3 (Adreno 2xx/3xx) | FL 9_3 |
| Audio | XAudio2 | XAudio2 |
| File picker | `FileOpenPicker.PickSingleFileAndContinue` (kiểu "AndContinue") | **Không có picker cho file tuỳ ý** → nạp SWF qua thư mục app / tải về / file association |
| CRT | `msvcr120_app` (VS2013) | CRT phone VS2012 (`WPSDK\WP80`) |
| Toolchain trên máy | VS2013 + Windows Phone Kits 8.1 ✔ | VS2012 WPSDK WP80 ✔ |
| **Rust core** | **Rủi ro cao nhất** (xem §3) | Như 8.1 nhưng API còn ít hơn |

Kết luận: **8.1 khả thi nếu Rust std link được với CRT VS2013** → làm spike này TRƯỚC, nếu fail thì dừng.
8.0 khả thi thấp hơn và tốn công hơn (host C#, không có picker) → làm sau 8.1, dùng chung lõi Rust + renderer.

## 3. Rủi ro chính
1. **Rust std ↔ CRT cũ**: target `thumbv7a-uwp-windows-msvc` giả định UCRT/vcruntime140 (VS2015+). WP8.x không có UCRT.
   Hướng giải: `panic=abort`, build-std, dùng lại lớp thay thế kernel32 → ntdll/TEB của ruffleUWP (`UwpStubs.cpp`,
   `ntdll_uwp.def`), tự cung cấp các hàm CRT còn thiếu (shim C) khi link bằng `link.exe` VS2013.
2. **API app-container của WP8.x hẹp hơn W10M**: std/deps có thể gọi API không tồn tại (kiểm `dumpbin /imports`
   so với `WindowsPhoneCore.lib` 8.1/8.0). Có thể phải thêm stub.
3. **Linker cũ**: `link.exe` VS2013/VS2012 có thể không đọc được một số object do LLVM mới sinh (section/flags lạ).
   Phương án dự phòng: link bằng `lld-link` của Rust rồi chỉ dùng import lib của SDK phone.
4. **Phần cứng yếu hơn**: Lumia WP8 thường 512MB–1GB RAM, Adreno 225/305 → giới hạn RAM 150–300MB/app.
   Bắt buộc render scale thấp, frame limit, tắt MSAA.
5. **Triển khai**: cần điện thoại developer-unlocked; WP8.1 deploy bằng `AppDeploy` / `WinAppDeployCmd` 8.1 qua USB.

## 4. Kiến trúc đích
```
ruffleWP8/
  rust/                   KHÔNG copy: build lại crate ..\ruffleUWP\rust\ruffle_uwp với target/cfg phone
  shim/                   C shim: hàm CRT/Win32 thiếu trên WP8.x (thay cho phần UCRT)
  wp81/RuffleWP81.sln + RuffleWP81/  C++/CX Windows Phone 8.1 app (XAML + SwapChainPanel) — port từ ruffleUWP/app
  wp80/RuffleWP80.sln + RuffleWP80 (C#) + C# host + RuffleWP80Native (WinPRT C++, D3D + XAudio2)
  scripts/                build-rust-wp.ps1, package81.ps1, deploy81.ps1
```
Giữ ranh giới như ruffleUWP: Rust không gọi WinRT; C++ lo D3D/XAudio2/input/file. `GpuRenderer`, `RufflePlayer`,
`AudioOutput` copy từ ruffleUWP và sửa phần API khác biệt (ghi rõ trong PROGRESS).

## 5. Các giai đoạn
- **S0 Spike link (quyết định đi/dừng)**: build staticlib Rust cho ARM, link vào exe/dll WP8.1 tối giản bằng VS2013,
  liệt kê symbol thiếu, viết shim, chạy một hàm Rust trên điện thoại (log ra file).
- **M1 Skeleton 8.1**: app XAML + SwapChainPanel màn hình đen, deploy lên Lumia 8.1.
- **M2 Player**: nạp SWF (picker AndContinue + thư mục app), tick, CPU renderer.
- **M3 GPU renderer** (FL 9_3, shader 4.0_level_9_1 từ ruffleUWP).
- **M4 Input + zoom 2 ngón, M5 Audio, M6 Settings + tối ưu RAM.**
- **M7 WP 8.0** (tuỳ kết quả 8.1): host C# + WinPRT, nạp SWF không qua picker.
