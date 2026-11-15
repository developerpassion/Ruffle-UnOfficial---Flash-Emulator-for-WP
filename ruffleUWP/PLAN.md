# PLAN — Port Ruffle sang UWP cho Windows 10 Mobile

> Đọc file này đầu mỗi session. Chi tiết task: `WORK.md`. Trạng thái: `PROGRESS.md`.

## 1. Mục tiêu
- Chạy Ruffle (Flash emulator, Rust) như app UWP trên **Windows 10 Mobile**, thiết bị Lumia ARM32.
- Target build: user ghi "10524". **Không có build W10M 10524 công khai** — gần nhất là **10.0.10586 (TH2, 1511)**. Plan dùng
  `TargetPlatformMinVersion = 10.0.10586.0` (đổi trong `.vcxproj` + manifest nếu thực sự cần 10240).
- MVP: mở file .swf local (FileOpenPicker), render, touch → mouse, âm thanh. Không hỗ trợ ban đầu: AVM2 Stage3D, video H.264, network, PixelBender.

## 2. Phân tích Ruffle (repo `E:\Projects\Ruffle\ruffle`, v0.7.1, edition 2024)
| Crate | Vai trò | Dùng được trên UWP ARM? |
|---|---|---|
| `core` | Player, AVM1/AVM2 | Có, cần build-std. `build_playerglobal` chạy trên **host** (cần Java) → OK |
| `swf`, `flv`, `wstr` | parse | Có |
| `render` (+tessellator, lyon) | trait `RenderBackend` | Có |
| `render/wgpu` | GPU renderer | **Không thực tế**: wgpu đã bỏ DX11; DX12 trên Lumia hạn chế; target tier-3 |
| `render/canvas`, `webgl` | web | Không |
| `video/software` | VP6/Screen video | Có thể (feature từng phần) |
| `desktop` | winit + egui + cpal + rfd | **Không** — viết frontend mới |
| `frontend-utils` | tokio/reqwest/cpal | Bỏ, hoặc dùng chọn lọc |

Backend cần cung cấp cho `PlayerBuilder` (core/src/player.rs:3065): Render, Audio, Navigator, Storage, Ui, Log, Video.
Null impl có sẵn: `NullRenderer`, `NullAudioBackend`, `NullNavigatorBackend`, `MemoryStorageBackend`, `NullUiBackend`, `NullVideoBackend`.

## 3. Rủi ro chính
1. **Toolchain Rust**: target `thumbv7a-uwp-windows-msvc` là **tier 3** → cần nightly + `-Z build-std`. Ruffle cần Rust rất mới (edition 2024) nên phải dùng nightly mới; std uwp có thể vỡ → có thể phải patch.
2. **Windows SDK/VS**: compile C++/CX ARM cho 10586 cần VS2017/VS2019 + Windows SDK 10.0.10586 (hoặc SDK mới hơn với MinVersion 10586). Link Rust staticlib với CRT UWP (`vcruntime140_app`).
3. **API cấm trong UWP**: crate dùng Win32 không thuộc app-container (getaddrinfo ok, nhưng `GetModuleHandle`, registry, v.v.) sẽ fail WACK. Kiểm tra từng dep (`chrono` localtime, `rand` sys_rng → `BCryptGenRandom` OK).
4. **Renderer**: phải viết renderer riêng. Chọn **software renderer** (tiny-skia hoặc rasterizer tự viết dùng mesh từ tessellator) → blit vào D3D11 SwapChainPanel. Sau MVP có thể làm D3D11 FL9_3 renderer.
5. **Hiệu năng/RAM**: Lumia 1–3GB RAM, CPU Snapdragon 400–810. Cần giảm quality, scale viewport, giới hạn fps.
6. Java (playerglobal) cần có trên máy build.

## 4. Kiến trúc đích
```
ruffleUWP/
  rust/ruffle_uwp/        crate Rust (staticlib) — C ABI: ruffle_create/load/tick/render/input/destroy
     src/lib.rs           FFI + PlayerBuilder
     src/render_sw.rs     SoftwareRenderBackend (RenderBackend) → RGBA buffer
     src/audio.rs         AudioBackend: mixer ruffle core → ring buffer, host kéo PCM
     src/navigator.rs     Navigator: fetch file local qua callback host
     src/storage.rs, ui.rs, log.rs
  app/RuffleUWP/          C++/CX UWP app (XAML + SwapChainPanel)
     App.xaml(.h/.cpp), MainPage.xaml(.h/.cpp)
     RufflePlayer.h/.cpp  vòng lặp render thread, D3D11 swapchain, XAudio2, input
     Package.appxmanifest (Mobile family, MinVersion 10586)
  scripts/build-rust.ps1  build-std cho thumbv7a-uwp-windows-msvc / x86_64-uwp
```
Ranh giới: Rust KHÔNG gọi WinRT; mọi WinRT (file picker, XAudio2, D3D11, input) nằm phía C++/CX. Rust chỉ nhận bytes + trả framebuffer + PCM. Giảm rủi ro uwp target.

## 5. Các giai đoạn (milestone)
- **M0 Môi trường**: cài nightly, rust-src, VS + SDK, Java; build `ruffle_core` cho `x86_64-uwp-windows-msvc` rồi `thumbv7a-uwp-windows-msvc`.
- **M1 Skeleton**: app UWP chạy trên emulator/x64 desktop, link Rust staticlib, hiển thị khung đen. (Desktop UWP x64 để debug nhanh.)
- **M2 Load SWF + NullRenderer**: player chạy tick, log ra Debug output.
- **M3 Software renderer**: shapes (fill solid/gradient/bitmap), bitmaps, transform, color transform, mask cơ bản.
- **M4 Input**: touch→mouse, bàn phím ảo, nút Back.
- **M5 Audio**: mixer core (`audio` + `mp3` feature) → XAudio2.
- **M6 Tối ưu ARM**: release LTO, opt-level 3, giảm resolution, fps cap; test Lumia thật.
- **M7 Hoàn thiện**: storage SharedObject vào LocalFolder, settings, đóng gói appx/appxbundle ARM, WACK.
- (Sau) D3D11 renderer, filters, video.

## 6. Quy ước làm việc giữa các session
- Mỗi session: đọc PLAN → PROGRESS → chọn task `[ ]` đầu tiên chưa xong trong WORK.md.
- Kết thúc session: cập nhật checkbox WORK.md, thêm entry nhật ký trong PROGRESS.md (ngày, làm gì, lỗi, bước tiếp).
- Không sửa repo `ruffle/` trừ khi cần; nếu sửa thì ghi patch vào `PROGRESS.md` mục "Patch vào upstream".
