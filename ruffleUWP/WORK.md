# WORK — Danh sách công việc

Đánh dấu `[x]` khi xong, `[~]` đang làm, `[!]` bị chặn (ghi lý do trong PROGRESS.md).

## M0 — Môi trường build
- [x] Cài rustup + `rustup toolchain install nightly` + `rustup component add rust-src --toolchain nightly`
- [x] Xác định nightly hỗ trợ edition 2024 và build được std cho `thumbv7a-uwp-windows-msvc` (ghi version vào PROGRESS)
- [x] Cài Visual Studio 2019 (hoặc 2017) workload UWP + C++ UWP tools ARM + Windows SDK 10.0.10586 (và SDK mới để build)
- [x] Cài Java 11+ (cho `core/build_playerglobal`)
- [x] `scripts/build-rust.ps1 -Target x86_64-uwp-windows-msvc` build thành công `ruffle_core`
- [x] Build thành công cho `thumbv7a-uwp-windows-msvc` — ghi các dep lỗi + patch
- [~] Kiểm tra symbol import (đã liệt kê bằng dumpbin, còn raw-dylib kernel32/ntdll — xem PROGRESS) của staticlib (dumpbin /imports trên .dll test) không dùng API bị cấm

## M1 — Skeleton app
- [x] Tạo crate `rust/ruffle_uwp` (staticlib, C ABI) — skeleton
- [x] Tạo C++/CX project `app/RuffleUWP` (XAML, SwapChainPanel, manifest Mobile, MinVersion 10586) — skeleton
- [x] Mở solution trong VS, retarget SDK, build x64 Debug
- [x] Link `ruffle_uwp.lib` + `ruffle_uwp.h` (đường dẫn đã cấu hình trong vcxproj theo `$(Platform)`)
- [x] Chạy trên Desktop x64 → màn hình đen/clear color
- [x] Build ARM appx (đã ký test)
- [ ] **Deploy lên Lumia/W10M thật, kiểm tra app mở được (xem Rủi ro mở #1 trong PROGRESS)**

## M2 — Player chạy
- [x] `ruffle_create` dựng Player với backend null (code skeleton)
- [x] FileOpenPicker (.swf) → bytes → `ruffle_load_swf`
- [x] Render thread gọi `ruffle_tick(dt_ms)` theo `frame_rate`
- [x] LogBackend → `OutputDebugString` qua callback host
- [x] Xác nhận trace() của SWF test hiện trong LocalState\log.txt

## M3 — Software renderer (`render_sw.rs`)
- [x] Chọn: tiny-skia (đề xuất, pure Rust, có ARM NEON? kiểm tra) vs rasterizer tự viết
- [x] `register_shape`: convert `DistilledShape` → tiny-skia Path (hoặc dùng `ruffle_render::tessellator`)
- [x] `submit_frame`: thực thi `CommandList` (render_shape, render_bitmap, push/pop mask, blend modes cơ bản)
- [x] Color transform, gradient (linear/radial/focal), bitmap fill repeat/smooth
- [x] Mask: stencil bằng alpha mask
- [x] `register_bitmap` / `update_texture` / `render_offscreen` (BitmapData.draw)
- [x] Copy framebuffer → D3D11 texture → present (phía C++: `ruffle_get_framebuffer`)
- [ ] Quality LOW mặc định trên mobile
- [x] Anti-aliasing: MSAA 4x ở GPU renderer (CPU renderer vẫn răng cưa)

## M4 — Input
- [x] Pointer: chạm/chuột trái/phải/giữa + cuộn, CapturePointer, PointerCanceled (chưa test trên máy)
- [x] Chạm giữ 8 giây (không kéo quá 20 dip) → chuột phải → context menu Ruffle (MenuFlyout). Chuột phải thật cũng mở menu nếu SWF không xử lý
- [x] Bàn phím: CoreWindow KeyDown/Up + CharacterReceived → `ruffle_key`/`ruffle_char` (đã test desktop với key_input_location)
- [~] Bàn phím ảo: nút "Keyboard" trên app bar focus TextBox ẩn (chưa tự hiện khi focus TextField; chưa test trên điện thoại)
- [x] Nút Back: lần 1 gửi Escape cho SWF, bấm lại trong 1.5s thì thoát (chưa test trên máy)
- [~] (đã nối SizeChanged, chưa test xoay) Xoay màn hình / resize → `ruffle_resize`

## M5 — Audio
- [x] Bật feature `audio`, `mp3`, `nellymoser` trong core
- [x] `AudioMixer` của core trong `audio.rs`, xuất PCM stereo f32 44100 (`ruffle_audio_fill`)
- [x] C++: XAudio2 source voice + thread kéo PCM (`AudioOutput.cpp`). Đã khởi tạo OK trên desktop; chưa nghe thử trên máy
- [x] Pause audio + render loop khi Suspending/Resuming

## M6 — Tối ưu ARM
- [ ] profile release: lto="fat", codegen-units=1, panic="abort", opt-level=3
- [x] Scale render resolution: Auto (≤1280x720) / 100 / 75 / 50%
- [ ] Đo fps trên thiết bị, ghi PROGRESS
- [ ] Kiểm tra RAM (giới hạn app W10M ~ 390MB–1GB tuỳ máy)

## M7 — Hoàn thiện
- [x] StorageBackend → LocalState\SharedObjects (SharedObject)
- [~] Navigator: http qua TcpStream + cache web offline (Documents\Ruffle\web). Chưa: https, file phụ cùng thư mục SWF
- [ ] UI: thanh lệnh (open, fullscreen, quality), file association `.swf`
- [x] Assets: icon/tile/splash từ logo.svg
- [ ] Đóng gói appx ARM, ký cert test, chạy WACK

## Backlog
- [x] Renderer D3D11 FL 9_3 (render_gpu.rs + GpuRenderer.cpp, MSAA 4x) — cần đo trên máy
- [ ] Filters (blur/glow) trong software renderer
- [ ] Video software (VP6/Screen)
- [ ] egui-free settings page XAML
