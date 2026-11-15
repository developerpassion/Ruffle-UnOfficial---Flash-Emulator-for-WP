# PROGRESS — Nhật ký

## Trạng thái hiện tại (session 3)
- **M4 input + M5 audio: xong phần code**, đã test bàn phím trên desktop x64 (trace keyCode/charCode/shift/location đúng), XAudio2 khởi tạo OK, SWF sound chạy.
- Sửa `log.txt` (lỗi `L"\log.txt"` thiếu escape; giờ mở share-read nên đọc được khi app chạy).
- **ĐÃ CHẠY trên HP Elite x3 (W10M ARM)**: app mở được, load + render Kingdom Rush Frontiers → rủi ro raw-dylib import (#1) KHÔNG xảy ra trên máy này.

## Trạng thái cũ (cuối session 2)
- **M0, M1, M2, phần lớn M3: XONG.** App UWP build được cho **ARM** và **x64**, đã ký gói test.
- Đã chạy thật trên **desktop x64 (UWP)**: load SWF, render đúng bằng software renderer, dump frame xác nhận.
- **CHƯA chạy trên Lumia/W10M thật.** Đây là việc quan trọng nhất tiếp theo (xem "Rủi ro mở").
- Bước tiếp theo: deploy `AppPackages\...ARM...appx` lên máy, đọc `LocalState\log.txt`; sau đó M4 (input), M5 (audio).

## Cách build (đã kiểm chứng)
```powershell
.\scripts\package.ps1 -Platform ARM     # Rust thumbv7a-uwp + MSBuild VS2017 + ký
.\scripts\package.ps1 -Platform x64
.\scripts\visual-test.ps1 -Max 800      # so renderer với ảnh mẫu của bộ test Ruffle (chạy trên host)
cargo run --release --example render_png -- file.swf out.png [frames] [w] [h]   # trong rust/ruffle_uwp
```
- Test trên desktop: `Add-AppxPackage -Register x64\Release\RuffleUWP\AppxManifest.xml`, chép SWF vào
  `%LOCALAPPDATA%\Packages\RuffleUWP.Port_0zchs43c1ject\LocalState\autoload.swf` → app tự load và sau 2s ghi `frame.bmp`.

## Thông tin môi trường
- Rust: `rustc 1.102.0-nightly (f5cedb83a 2026-11-07)` + rust-src, cài qua rustup (`%USERPROFILE%\.cargo\bin`, chưa có trong PATH của shell cũ → script tự thêm).
- Target dùng: `thumbv7a-uwp-windows-msvc`, `x86_64-uwp-windows-msvc` với `-Z build-std=std,panic_abort`. Build ~7 phút/target.
- VS: 2017 Pro (v141, **có ARM**) tại `F:\Program Files\...\2017\Professional` ← dùng cái này. VS2019/2022 không có lib ARM32.
- SDK: có 10586 → 22621; project target 15063, MinVersion 10586.
- Java 26 (build_playerglobal OK).
- Chứng chỉ test: `app/RuffleUWP/RuffleUWP_TemporaryKey.pfx` (mật khẩu `ruffle`), `.cer` để cài vào máy; cert cũng nằm trong `Cert:\CurrentUser\My` (CN=RuffleUWP).

## Kết quả renderer (visual-test, 353 test có ảnh mẫu)
- ~180/353 khớp <1% pixel lệch. Phần lớn còn lại là ngoài phạm vi: Stage3D, PixelBender, filters, video, load file ngoài (Navigator null), test cần `num_ticks`.
- Lệch nhỏ 2–10% chủ yếu do **không có anti-aliasing** (cạnh răng cưa) và text/device font.
- Ví dụ đúng hoàn toàn: gradient (linear/radial/focal/LinearRGB), mask, displayobject_mask, mask_with_drawing.

## Patch / workaround
- **Không sửa upstream `ruffle/`.**
- `app/RuffleUWP/UwpStubs.cpp`: stub cho `__imp_*` mà Rust std tham chiếu nhưng không có trong WindowsApp.lib
  (CreateProcessW, env vars, LoadLibraryExA, GetModuleHandle*, CreatePipe, DeviceIoControl, NtCreateNamedPipeFile, NtSetInformationFile...).
  Trả lỗi ERROR_NOT_SUPPORTED. Không link kernel32/userenv/ntdll .lib.

## Rủi ro mở (cần kiểm trên máy W10M)
1. Rust std dùng **raw-dylib** import trực tiếp từ `kernel32.dll` (GetTimeZoneInformationForYear, WriteConsoleW, GetStdHandle,
   GetSystemTimePreciseAsFileTime, Set/GetFileInformationByHandle(Ex), GetFullPathNameW...), `ntdll.dll` (NtReadFile, NtWriteFile,
   NtQueryVolumeInformationFile, RtlNtStatusToDosError), `bcryptprimitives.dll!ProcessPrng`.
   Nếu W10M 10586 thiếu export nào → app **không mở được** (thoát ngay sau splash). Cách xử lý nếu xảy ra: patch `library/std`
   (build-std dùng rust-src của toolchain) để bỏ các import đó cho `target_vendor="uwp"`, hoặc ghim nightly cũ hơn.
   Có thể chạy WACK trên gói ARM để thấy trước.
2. `CopyResource` từ texture DYNAMIC → backbuffer: chạy OK trên desktop; trên GPU FL9_3 nếu lỗi → dùng `UpdateSubresource` vào texture DEFAULT.
3. Hiệu năng: desktop ~4–15 ms/frame ở 550x400. Lumia chậm hơn ~10–20× → có thể cần render ở 0.5x độ phân giải (M6).
4. ~~log.txt~~ đã sửa (session 3).
5. XAudio2: link `xaudio2.lib` (XAudio2 2.9). W10M có xaudio2_9.dll, nhưng chưa kiểm trên máy; nếu init lỗi app vẫn chạy (log "XAudio2 init failed").
6. Bàn phím ảo: chưa tự bật khi TextField nhận focus (cần UiBackend/ImeNotification) — hiện dùng nút Keyboard.

## Nhật ký
### 2026-11-08 — Session 1
- Phân tích repo ruffle 0.7.1; tạo PLAN/WORK/PROGRESS + skeleton Rust + C++/CX.

### 2026-11-08 — Session 2
- Cài rustup nightly + rust-src.
- Viết software renderer đầy đủ (`render_sw.rs`): tessellator → rasterizer tam giác, color/gradient/bitmap fill, color transform,
  mask stencil, alpha mask, blend modes (Add/Subtract/Multiply/Screen/Lighten/Darken/Difference/Invert/Alpha/Erase; Overlay/HardLight ≈ Normal),
  cacheAsBitmap, BitmapData.draw + sync, Alpha/Erase chỉ có hiệu lực trong layer.
- Thêm example `render_png` + `scripts/visual-test.ps1`.
- Build Rust cho x64-uwp và thumbv7a-uwp: OK.
- Build app VS2017: thêm stub link; ARM + x64 appx OK; ký bằng cert test.
- Chạy app x64 UWP: lần đầu kẹt splash (render loop giữ mutex liên tục) → thêm sleep/yield; sau đó render đúng (ảnh 1200x876 @ DPI scale).
- Thêm autoload/frame dump + DebugLog để debug không cần debugger.

### 2026-11-08 — Session 3
- Rust: `input.rs` (VirtualKey → KeyDescriptor/TextControlCode), `audio.rs` (AudioMixer + proxy), FFI `ruffle_key`, `ruffle_char`, `ruffle_set_playing`, `ruffle_audio_fill`, `ruffle_audio_sample_rate`.
- C++: `AudioOutput` (XAudio2), CoreWindow KeyDown/KeyUp/CharacterReceived, nút Back, TextBox ẩn cho bàn phím ảo, Suspending/Resuming.
- `build-rust.ps1` tự thêm cargo vào PATH. Chạy script từ bash: `powershell -File scripts/build-rust.ps1 ...` (đừng dùng `*>&1` trong PS vì stderr của cargo làm script dừng).
- Build ARM + x64 OK, đã ký.

### 2026-11-08 — Session 3 (tiếp)
- FFI: `ruffle_mouse_button` (trả handled), `ruffle_mouse_wheel`, `ruffle_context_menu` / `_select` / `_closed`.
- MainPage: xử lý nút qua `PointerUpdateKind` (cả nút bấm thêm khi đang giữ nút khác, đến qua PointerMoved), CapturePointer,
  chạm giữ 8s → nhả trái (bỏ qua lần nhấc tay) → chuột phải → MenuFlyout neo tại điểm chạm (`menuAnchor`, vì ShowAt(point) cần 1607).
- `package.ps1` chép `.cer` cạnh `.appx`.
- Chưa test tương tác menu/chạm (chỉ build OK); cần thử trên máy.

### 2026-11-08 — Session 3: test trên HP Elite x3
- App chạy, render game. Lỗi: hình phóng to ~3x, chỉ thấy góc trên-trái.
- Nguyên nhân: SwapChainPanel coi 1 pixel swapchain = 1 DIP; buffer tạo theo pixel thật nhưng thiếu `IDXGISwapChain2::SetMatrixTransform(1/scale)`.
  Desktop scale ~1 nên không lộ ra. Đã sửa + dùng `CompositionScaleX/Y` + bắt `CompositionScaleChanged`. Log in `Resize ... px`.
- Lưu ý hiệu năng: buffer giờ có thể tới 2560x1440 px → software renderer sẽ nặng; cần M6 (render 0.5x rồi để GPU kéo giãn).

### 2026-11-08 — Icon
- Icon/tile/splash từ `../logo.svg` (wordmark cam), nền tile + splash `#37528C`. Có scale-100/200/400 + targetsize.
- Tạo lại: `sh scripts/make-icons.sh` (dùng `tools/svg2png`, Rust + resvg; Edge headless không chạy được trong môi trường agent).

### 2026-11-08 — Tối ưu hiệu năng (x3 ~10 FPS)
- Không có JIT: Ruffle thông dịch AVM1/AVM2; nút thắt là software renderer.
- Render scale: tự động giới hạn ~1280x720 px (x3 2560x1440 -> 50%), GPU kéo giãn qua SetMatrixTransform. Nút app bar "Render: Auto/100/75/50%".
- Chỉ upload + Present khi `ruffle_tick` trả 1 (có frame mới); trước đây copy ~14 MB mỗi vòng lặp.
- raster_tri: chỉ duyệt đoạn x hợp lệ của từng hàng (span) thay vì cả bounding box.
- Bitmap bilinear: đường nhanh nội suy số nguyên khi 4 texel nằm trong ảnh.
- log.txt in mỗi 5s: `FPS | tick+render ms | upload+present ms | WxH`.
- Desktop x64 Kingdom Rush: 30 FPS (đúng frame rate game). visual-test: 178/353 (<1%).
- Bước tiếp nếu vẫn chậm: vẽ đa luồng theo dải ngang (4 nhân Snapdragon 820), pixel loop dùng số nguyên thay f32.

### 2026-11-08 — Log ra Documents
- Log vẫn ghi `LocalState\log.txt` (ghi đè mỗi lần mở app); mỗi 5s + khi Suspending chép sang **Documents\Ruffle
uffle.log**.
- Cần capability `documentsLibrary` + file type association `.log` (documentsLibrary chỉ cho đụng loại file đã khai báo).
- Đổi capability -> trên desktop phải `Remove-AppxPackage` rồi register lại; trên máy cài lại gói.

### 2026-11-08 — GPU renderer (Direct3D 11)
- Log trên Elite x3: tick+render 135–270 ms/frame bằng CPU -> chuyển sang GPU.
- Kiến trúc: `rust/ruffle_uwp/src/render_gpu.rs` (RenderBackend: tessellate + duyệt CommandList) gọi bảng hàm C
  `RuffleGpuHost` (include/ruffle_uwp.h); `app/RuffleUWP/GpuRenderer.cpp` thực thi bằng D3D11.
  - Shader HLSL `app/RuffleUWP/Shaders/*.hlsl` (profile 4_0_level_9_3), FxCompile -> header `g_<tên>`.
  - Mesh/texture theo id; màn hình vẽ vào RT MSAA 4x (nếu có) rồi resolve sang backbuffer.
  - Mask = stencil (equal ref; write incr / clear decr); blend mode = layer texture + composite blend state
    (Add/Subtract/Multiply/Screen/Lighten/Darken/Alpha/Erase/Invert; Overlay/HardLight/Difference ≈ Normal).
  - BitmapData.draw/cacheAsBitmap: vẽ vào texture; đọc lại qua staging khi core cần pixel.
  - Chưa: filters, Stage3D, PixelBender, smoothing gradient LinearRGB đúng chuẩn (LUT đã xử lý).
- `ruffle_create_gpu(...)`; GPU init lỗi -> tự dùng CPU. App bar: "Renderer: GPU/CPU" (nạp lại SWF), Render scale Auto = 100% khi GPU.
- Desktop x64: Kingdom Rush ~30 FPS, tick+render ~1–7 ms; test gradient/mask/bitmap khớp ảnh mẫu.
- `scripts/run-desktop.ps1 -Swf x.swf -Out frame.png`: chạy app desktop với SWF, lưu frame dump + log.

### 2026-11-08 — Màn hình xám sau PLAY (Kingdom Rush)
- Nguyên nhân chính: file `kingdom-rush-frontie-15717.swf` cũ bị hỏng (zlib lỗi ở ~92%); file tải lại thì OK.
- Sửa thêm: `NullNavigatorBackend::new()` bỏ executor -> future (Loader.loadBytes...) không bao giờ chạy.
  Giờ `src/navigator.rs` (UwpNavigator) giữ NullExecutor, `ruffle_tick` chạy executor ngoài khoá player.
- Navigator tải được `http://` (TcpStream, thread riêng); không mạng -> lỗi như cũ (game vẫn chạy).
  `RUFFLE_OFFLINE=1` để giả lập không mạng khi test.
- example `play.rs`: chạy SWF theo kịch bản (tick/real/slow/click/move/shot), in log tracing (RUST_LOG).

### 2026-11-08 — Tắt log, lưu save, cache web offline
- Đã chạy mượt trên Elite x3 với GPU renderer (theo người dùng).
- Log/autoload/frame dump mặc định TẮT; bật bằng file `LocalState\debug.flag`. `scripts/run-desktop.ps1` tự tạo/xoá cờ.
- SharedObject (save game) lưu ra `LocalState\SharedObjects\*.sol` (trước: RAM, tắt app là mất). FFI `ruffle_set_storage`.
- Cache web `ruffle_set_web_cache`: tải http thành công -> `LocalState\WebCache` (+ .swf chép ra `Documents\Ruffle\web\<host>\<path>`).
  Không mạng -> lấy từ WebCache rồi Documents. Kingdom Rush offline: cần
  `Documents\Ruffle\webgi.armorgames.comssetsgi\AGI2.swf` (Armor Games API, chạy sandbox nên không cần mạng).
  Đã kiểm bằng example play (RUFFLE_WEB_CACHE + RUFFLE_OFFLINE): offline vẫn "Connected to Armor Games".

### 2026-11-08 — Patch Kingdom Rush Frontiers chạy offline (file game, không commit)
- Công cụ: JPEXS ffdec 26.4.0 tại `../tools/ffdec` (gitignore). Decompile: `java -jar tools/ffdec/ffdec-cli.jar -export script tools/kr_src <swf>`.
- Sửa class `AgiV2Handler.loadSystem`: không nạp AGI2.swf, gán `agi` = AGI giả (khách; mua hàng/quest trả lỗi;
  storage.user dùng SharedObject `krf_offline_agi`; callback gọi trễ bằng setTimeout).
  Ghi lại: `java -jar tools/ffdec/ffdec-cli.jar -replace in.swf out.swf AgiV2Handler tools/kr_patch/AgiV2Handler.as`.
- Kết quả `kingdom-rush-frontie-15717_offline.swf`: offline vào menu, bảng save (Online = "Login to use AG Save System",
  Local save dùng được), NEW GAME -> bản đồ; không lỗi AVM2.

### 2026-11-08 — Bản ARM release chung cho Elite x3 + Lumia 650
- Gói ARM là bản chung (generic ARMv7 + NEON, opt-level 3, LTO fat, codegen-units 1, panic abort): chạy cả
  Snapdragon 820 (Elite x3) lẫn Snapdragon 212 / Cortex-A7 (Lumia 650).
- `build-rust.ps1 -TargetCpu <cpu>` (tuỳ chọn, target dir riêng): vd. `kryo` chỉ cho Snapdragon 820 — KHÔNG dùng nếu cần chạy Lumia 650.
- GPU feature level 9_x (Lumia 650: Adreno 304, FL 9_3): bitmap lặp bằng `frac()` trong PS_Bitmap (sampler luôn CLAMP)
  vì 9_x không WRAP texture không phải 2^n; texture tối đa 4096 (9_3) / 2048 (9_1).
- Chưa thử trên Lumia 650.

### 2026-11-08 — Lumia 650 (W10M 10586.404) crash khi mở
- Rủi ro mở #1 xảy ra: Rust std import raw-dylib vài hàm không có trên 10586.
- Sửa trong `UwpStubs.cpp`: định nghĩa `__imp_ProcessPrng` (-> BCryptGenRandom), `__imp_GetSystemTimePreciseAsFileTime`
  (-> GetSystemTimeAsFileTime), stub GetStdHandle/WriteConsoleW/GetTimeZoneInformationForYear/GetFullPathNameW/
  SetFileTime/Get-SetFileInformationByHandle(Ex). Import còn lại: kernel32 (CloseHandle, Get/SetLastError,
  GetSystemTimeAsFileTime, GetTickCount64), ntdll (NtReadFile/NtWriteFile/NtQueryVolumeInformationFile/RtlNtStatusToDosError).
- Cài qua USB: `WinAppDeployCmd (SDK 15063, x86) install -file <appx> -ip 127.0.0.1` (cùng version -> uninstall trước).

### 2026-11-08 — Lumia 650 (W10M 10586.404/.494): đã CHẠY
- Device Portal qua USB: http://127.0.0.1:10080 (bản 10586 chỉ có taskmanager/app, resourcemanager/processes,
  packagemanager; KHÔNG có WER/ETW/filesystem). `scripts/phone-test.sh [/p:...]`: build ARM, ký, cài qua USB,
  mở app, theo dõi tiến trình.
- Cờ chẩn đoán msbuild: SafeMode (chỉ XAML), StopStage=1..9 (dừng khởi tạo theo giai đoạn), NoGpu, NoXAudio, EnableBack.
- Đã loại trừ: import kernel32 (vẫn thay bằng TEB/ntdll, `ntdll_uwp.def` -> lib do package.ps1 tạo), XAudio2, GPU renderer,
  Rust init, swapchain, render loop.
- NGUYÊN NHÂN: đăng ký `SystemNavigationManager.BackRequested` (cả trong ctor lẫn ở Loaded) làm app chết ngay
  (native, try/catch không bắt; ApiInformation contract v3 lại báo true). Đã TẮT xử lý Back riêng (Back = thoát app).
- Thêm: Application.UnhandledException + ShowError (lỗi C++/CX hiện lên màn hình thay vì crash).

### 2026-11-08 — Zoom 2 ngón + MSAA theo GPU
- Ruffle không có API zoom (ContextMenuCallback::Zoom chưa làm). Zoom ở tầng hiển thị: ma trận swapchain
  (scale dipToBuf*zoom + translate), RufflePlayer::SetView (1..4x, kẹp để nội dung phủ kín), toạ độ chạm đổi ngược
  (ContentX/Y). Đổi zoom -> `ruffle_render` (FFI mới) vẽ lại khung hiện tại rồi present.
- MainPage: ngón thứ 2 chạm -> BeginPinch (nhả chuột trái của ngón đầu), di chuyển -> zoom quanh tâm + dịch;
  bỏ qua chạm cho tới khi nhấc hết ngón.
- MSAA 4x chỉ bật khi feature level >= 10_0 (Lumia 650 / Adreno 304 FL 9_3: tắt để đỡ giật).

### 2026-11-08 — Popup Settings
- Nút Settings (thanh dưới + dưới nút Open SWF ở màn hình chào) mở Flyout: GPU renderer, MSAA 4x, Show FPS overlay.
  Bỏ nút "Renderer: GPU/CPU" trên thanh dưới.
- Lưu `LocalState\settings.json` (Windows.Data.Json), vd. {"gpu":true,"msaa":false,"showFps":true};
  thiếu "msaa" = tự động (bật nếu GPU feature level >= 10_0). Nạp + áp dụng khi mở app (Settings.h/.cpp).
- MSAA đổi lúc chạy (GpuRenderer::SetMsaa -> tạo lại RT màn hình). FPS overlay: góc trên trái, cập nhật 0.5s
  (FPS, ms tick+vẽ+present trung bình, GPU/CPU, MSAA).

### 2026-11-08 — Settings tối ưu hiệu năng
- Thêm vào popup + settings.json: Sound (tắt = dừng engine XAudio2, KHÔNG nạp lại game; `ruffle_set_audio_enabled`/NullAudio vẫn còn nhưng app không dùng),
  Render resolution (Auto/100/75/50%, bỏ nút Render trên thanh dưới), Quality (`ruffle_set_quality`; Low = bitmap
  không làm mịn ở cả 2 renderer), Frame limit (0/30/20 FPS; render loop chờ theo mốc cố định, đo được 20.0 FPS).

- Sound: bản đầu nạp lại game khi bật/tắt (UI đơ vài giây, trông như công tắc không ăn). Đổi sang dừng/chạy engine
  XAudio2. Đã kiểm bằng móc thử (đã gỡ): bật lại -> peak âm thanh ~1.0, game chạy tiếp.
- Người dùng tự commit: không chạy git commit nữa.

### 2026-11-08 — Sửa popup Settings trên điện thoại (đã xác nhận trên Lumia 650)
- Lỗi công tắc/ComboBox "quay về giá trị cũ": so `sender == gpuSwitch` (Object^ vs ToggleSwitch^) luôn sai trong C++/CX.
  Sửa: `dynamic_cast<ToggleSwitch^>(sender)` / `dynamic_cast<ComboBox^>(sender)` rồi mới so.
- SyncSettingsUi luôn trả m_syncingUi = false; lỗi Settings hiện lên màn hình (ShowError).
- Chiều cao popup: ScrollViewer, MaxHeight = 85% cửa sổ (dọc) / chiều cao cửa sổ - 40 (ngang); tính lại khi xoay.
- UI toàn tiếng Anh.
