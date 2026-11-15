# PROGRESS — Ruffle cho Windows Phone 8.0 / 8.1

## Trạng thái hiện tại (2026-11-08)
- Phạm vi: **WP 8.1 và WP 8.0**. Máy build chỉ có **VS2015** (+ toolset v120_wp81 / v110_wp80).
- **S0 qua phần link tĩnh**: lõi Rust (lib ARM của ruffleUWP) + `shim\WpStubs.cpp` link thành DLL ARM bằng VS2013
  toolset, không thiếu symbol, import toàn API có trên WP 8.1. **Chưa chạy trên máy WP 8.1.**
- Hai solution riêng, mỗi cái một thư mục (mở bằng VS2015). `shim\`, `scripts\`, `spike\` và tài liệu dùng chung ở gốc.
  - `wp81\RuffleWP81.sln` → WP 8.1 `.appx` (C++/CX XAML) ở `wp81\AppPackages\`. M1 build xong.
  - `wp80\RuffleWP80.sln` → WP 8.0 `.xap` (C# Silverlight + component C++) ở `wp80\RuffleWP80\Bin\ARM\<cfg>\`.
    (chỉ test lõi Rust, chưa vẽ D3D).
- **M1 PASS trên máy thật cả 8.1 (.appx) và 8.0 (.xap, Release).**
- **Ưu tiên WP 8.0** (user chốt). Player Silverlight (wp80 + wp81sl) chạy trên HTC One M8 (8.1): GPU mượt,
  xoay ngang và chạm đúng, âm thanh, Settings, zoom 2 ngón, giữ game khi ra nền (đã xác nhận 2026-11-09).
  Còn: test lại bản 8.0 trên máy WP 8.0 thật; bàn phím ảo, menu chuột phải (giữ lâu) như ruffleUWP.
  - `wp81sl\RuffleWP81SL.sln` → **Silverlight 8.1 `.xap`** (chỉ máy 8.1): cùng mã C#/C++ với wp80 + file picker
    (Documents/Downloads/OneDrive). Build xong, **chưa chạy trên máy**.

## Môi trường trên máy build (đã kiểm tra)
- Visual Studio 2012 (11.0) có `VC\WPSDK\WP80` (CRT + `x86_arm\link.exe`) → WP 8.0.
- Visual Studio 2013 (12.0) có `VC\bin\x86_arm\link.exe`, `VC\lib\arm` → WP 8.1 (cần xác nhận thư mục CRT phone 8.1).
- Visual Studio 2015 (14.0), VS2017 (F:\, dùng cho ruffleUWP), VS2019.
- `Microsoft SDKs\Windows Phone\v8.0, v8.1`, `Microsoft SDKs\WindowsPhoneApp\v8.1`.
- `Windows Phone Kits\8.0\lib\arm` và `8.1\lib\arm`: có `d3d11.lib`, `dxgi.lib`, `XAudio2.lib`, `WindowsPhoneCore.lib`,
  `PhoneAppModelHost.lib`, `runtimeobject.lib` (8.0), `ws2_32.lib` (8.0).
- Rust: `nightly-x86_64-pc-windows-msvc` (cùng toolchain ruffleUWP dùng).

## Ghi chú từ ruffleUWP (áp dụng lại)
- Lõi Rust đã build cho `thumbv7a-uwp-windows-msvc` với `-Z build-std`; import kernel32 đã thay bằng TEB/ntdll
  (`UwpStubs.cpp`, `ntdll_uwp.def`). Đây là điểm xuất phát cho shim CRT của WP8.x.
- GPU renderer chạy mọi FL 9_1..11_1 (shader `4.0_level_9_1`, index 16-bit ở 9_1) → dùng lại nguyên.
- Lumia 650 (W10M) chỉ có FL 9_3; máy WP8 (Adreno 225/305) cũng là 9_3.
- Popup Settings không được gắn vào AppBarButton (ComboBox làm CommandBar đóng → Flyout đóng theo).

## Cách build spike (đã kiểm chứng)
- Chạy qua `cmd` (từ Git Bash `c2.dll` không chạy được; cần `vcvarsx86_arm.bat`):
  `spike\build-shim.cmd` (cl /ZW WpStubs.cpp) rồi `spike\link-spike.cmd` (link DLL + `dumpbin /imports`).
- Include WP 8.1: `Windows Phone Kits\8.1\Include` + `\minwin` + `\mincore` + `\abi`.
- `/ZW` cần `/AI` tới `WindowsPhoneApp\v8.1\ExtensionSDKs\Microsoft.VCLibs\12.0\References\CommonConfiguration\neutral`
  (platform.winmd) và `Windows Phone Kits\8.1\References\CommonConfiguration\Neutral` (Windows.winmd).
- Lib: `msvcrt.lib vccorlib.lib WindowsPhoneCore.lib runtimeobject.lib ws2_32.lib` + `ntdll_uwp.lib` của ruffleUWP.

## Rủi ro mở
1. ~~ntdll.dll import trên 8.1/8.0~~ — chạy được (M1 PASS cả hai).
2. ~~WaitOnAddress trên 8.1/8.0~~ — chạy được.
3. RAM thấp (512MB) trên máy WP 8.x đời đầu.
4. WP 8.0: Fls* giả (callback huỷ TLS không chạy) — rò nhỏ khi thread thoát; theo dõi khi có nhiều thread.

## Nhật ký
### 2026-11-08 — Khởi tạo
- Tạo `PLAN.md`, `WORK.md`, `PROGRESS.md`. Đánh giá: 8.1 khả thi nếu qua được S0; 8.0 khó hơn, làm sau.

### 2026-11-08 — S0 spike link
- User chốt: chỉ hỗ trợ WP 8.1, chỉ có VS2015.
- Link `ruffle_uwp.lib` (ARM, ~184MB) với `VC 12.0\lib\store\arm` + `Windows Phone Kits\8.1\lib\arm`:
  lần đầu thiếu 37 symbol (Winsock + Win32), **0 symbol CRT**. Thêm `ws2_32.lib` + `ntdll_uwp.lib` → còn 12.
- `shim\WpStubs.cpp` = UwpStubs của ruffleUWP + `GetCommandLineW`, `Get/SetCurrentDirectoryW` (stub lỗi),
  `WaitForMultipleObjects` → `WaitForMultipleObjectsEx`. WP 8.1 không có `bcrypt.h` → `ProcessPrng` dùng
  `CryptographicBuffer::GenerateRandom` (WinRT, build `/ZW`).
- Kết quả: `spike.dll` 5.4MB, link exit 0. Import: api-ms-win-core-{com,errorhandling,fibers,heap,libraryloader,
  localization,processthreads,profile,string,synch,util,winrt-string}, ntdll, msvcr120_app, WS2_32. Không còn kernel32.

### 2026-11-08 — M1 project + build
- `RuffleWP8.sln` (nay là `wp81\RuffleWP81.sln`) + `wp81\RuffleWP81\RuffleWP81.vcxproj`: ToolsVersion 14.0, `ApplicationType=Windows Phone`,
  `ApplicationTypeRevision=8.1`, `PlatformToolset=v120_wp81`. MSBuild 14.0 build được trực tiếp (không cần MSBuild 12).
- Manifest WP 8.1 (namespace 2010 + m2/m3 + mp), OSMinVersion 6.3.1, Identity `RuffleWP81.Port`, ký bằng
  `RuffleWP81_TemporaryKey.pfx` (copy từ ruffleUWP, CN=RuffleUWP). Icon/splash: `scripts\make-assets.ps1`.
- `shim\WpStubs.cpp` được build trong project (không dùng pch, /ZW).
- MainPage (M1 test): D3D11 + swapchain xoá màn hình xanh, rồi `ruffle_create` với SWF 17 byte, 10 lần tick+render,
  đọc framebuffer, destroy. Kết quả in lên màn hình.
- Build Release|ARM: thành công, 1 warning C4244 (vô hại).
- Deploy: máy không có `AppDeployCmd.exe` 8.1; dùng VS2015 (Device) hoặc GUI `Tools\AppDeploy\AppDeploy.exe`.

### 2026-11-08 — WP 8.0 (.xap)
- User muốn thêm WP 8.0 và tách 2 solution: `RuffleWP8.sln` đổi tên → `RuffleWP81.sln`; thêm `RuffleWP80.sln`.
- Spike link 8.0 (`spike\link80.cmd`, CRT `VC 11.0\WPSDK\WP80\lib\arm` = MSVCR110, `Windows Phone Kits\8.0\lib\arm`):
  - Không có `CryptographicBuffer` → `WpStubs.cpp` dùng `rand_s` khi `RUFFLE_WP80`.
  - Thiếu 19 hàm toán C99 (round, trunc, rint, fma, log2, exp2, log1p, cbrt, acosh, asinh + bản f) và
    `FlsAlloc/FlsFree/FlsSetValue/IsThreadAFiber`. WP 8.0 cũng KHÔNG có `Tls*` → `shim\WpCompat80.cpp`:
    toán tự cài, Fls* = bảng 64 slot trong `__declspec(thread)` (callback huỷ TLS không được gọi: rò nhỏ khi thread thoát).
  - Link exit 0; import: ntdll, MSVCR110, api-ms-win-core-* (8.0), WS2_32.
- `wp80\RuffleWP80Native\RuffleWP80Native.vcxproj`: WinMDAssembly, `TargetPlatformIdentifier=Windows Phone`, `TargetPlatformVersion=8.0`,
  `PlatformToolset=v110_wp80`, import `Microsoft.Cpp.WindowsPhone.8.0.targets`, bỏ `ole32.lib` mặc định.
- `wp80\RuffleWP80\RuffleWP80.csproj`: Silverlight WP 8.0 (ProjectTypeGuids C089C8C0…), ProjectReference tới component;
  `WMAppManifest.xml` khai `ActivatableClasses` cho `RuffleWP80Native.RuffleTest`. Asset: `scripts\make-assets.ps1`.
- Build `RuffleWP80.sln` Release|ARM bằng MSBuild 14.0: OK → `.xap` 3.0MB nén (5.5MB khi giải nén; RuffleWP80Native.dll + .winmd bên trong).

### 2026-11-08 — WP 8.1: M1 PASS trên máy thật
- `wp81` Release ARM chạy trên Lumia 8.1: D3D11 + SwapChain + lõi Rust (create/tick/render/destroy) đều OK.
  → Import thẳng `ntdll.dll` và `WaitOnAddress` chạy được trên WP 8.1 (rủi ro mở #1, #2 đã đóng cho 8.1).

### 2026-11-08 — WP 8.0: "Native FAIL: FileNotFoundException" trên máy
- Tạo `RuffleWP80Native.RuffleTest` lỗi FileNotFound = nạp DLL thất bại (thiếu DLL hệ thống phụ thuộc).
- Import của bản đầy đủ: ntdll, WS2_32, d3d11, MSVCR110/MSVCP110/vccorlib110, 13 api-ms-win-core-* (xem wp80\imports.txt).
- Chẩn đoán: `spike\probe80\make-probes.py` + `build-probes.cmd` sinh `probe_NN.dll` (mỗi cái import đúng các hàm từ
  một DLL). Build `wp80\RuffleWP80.sln /p:Probe=true` → `RuffleWP80_Probe_ARM.xap` (không có Rust, chạy
  `RuffleTest::Probe()`: LoadPackagedLibrary từng probe, 126 = thiếu DLL, 127 = thiếu hàm). **Chờ kết quả trên máy.**
- Nghi nhất: `ntdll.dll` (Rust std raw-dylib) và `api-ms-win-core-synch-l1-2-0` (WaitOnAddress).
- Lỗi debug VS2015 "communication resource (port) is already in use": IpOverUsb bị chiếm (Lumia W10M còn cắm,
  AppDeploy/TShell/VS khác đang mở) → rút máy khác, đóng tool, restart service IpOverUsbSvc.

### 2026-11-08 — WP 8.0: M1 PASS, nguyên nhân FileNotFound
- Release .xap chạy trên máy WP 8.0: **M1 PASS** (lõi Rust create/tick/render/destroy OK; ntdll + WaitOnAddress chạy được).
- FileNotFound lúc đầu là bản **Debug** (F5 từ VS): component import `MSVCR110D/MSVCP110D/vccorlib110d` — máy không có.
  Sửa: Debug của RuffleWP80Native dùng CRT release (`UseDebugLibraries=false`, `RuntimeLibrary=MultiThreadedDLL`,
  tắt tối ưu), không định nghĩa `_DEBUG`.
- Bản probe cũng lỗi: `LoadPackagedLibrary` KHÔNG có trong `WindowsPhoneCore.lib` 8.0 → probe component không nạp được.
  Probe (`/p:Probe=true`) không dùng được trên 8.0 như hiện tại; giữ lại để tham khảo.

### 2026-11-08 — M2 WP 8.0: player thật (build OK)
- `wp80\RuffleWP80Native\RufflePlayer80.{h,cpp}`: ref class kích hoạt được, `CreateContentProvider()` trả WRL
  `ContentProvider` (IDrawingSurfaceBackgroundContentProvider + ...Native). Mọi lời gọi Rust trên thread vẽ (`Draw`);
  UI thread chỉ đẩy lệnh (LoadSwf/Unload/Pointer) qua mutex.
- Vẽ: Ruffle CPU render đúng kích thước render target hệ thống → `UpdateSubresource` vào texture của RTV (không shader);
  đảo RGBA→BGRA khi RT là B8G8R8A8; MSAA/khác cỡ → chỉ clear đen. `RequestAdditionalFrame()` mỗi khung khi đang chơi.
- scale = rộng RT / 480 (pixel logic WP). Âm thanh tắt (`ruffle_set_audio_enabled(0)`).
- C#: `DrawingSurfaceBackgroundGrid` là gốc trang (namespace mặc định, KHÔNG phải `phone:`); danh sách .swf thẻ nhớ
  (ExternalStorage chỉ thấy loại file đã đăng ký), `AssociationUriMapper` cho `/FileTypeAssociation?fileToken=`,
  chạm qua `Rectangle inputLayer` trong suốt, Back = Unload. Khoá Portrait (grid nền không tự xoay).
- VS2012: không có khởi tạo thành viên trong class (C2864) → gán trong constructor.

### 2026-11-09 — Settings, giữ game khi ra nền, zoom 2 ngón, bỏ log xanh (wp80 + wp81sl) — user xác nhận chạy hết trên M8
- User xác nhận: có âm thanh; lỗi nửa màn hình sau khi giữ Back đã hết.
- GpuRenderer: Rust LUÔN vẽ vào texture riêng `m_off` (cỡ nội dung = vùng nhìn thấy x độ phân giải render);
  `Present()` vẽ m_off lên RT hệ thống: phóng (lọc tuyến tính khi < 100% hoặc zoom), xoay, zoom/kéo
  (uv = pan + toạ độ màn / zoom). Khung bị bỏ qua do giới hạn FPS chỉ Present lại hình cũ (không cần ruffle_render).
  CPU renderer: `PresentCpu` → `m_cpuTex`; cờ `m_lastIsCpu` chọn nguồn (không giữ con trỏ thô tới SRV).
- Giữ game khi quay lại app: `GpuRenderer::MigrateTo(device, ctx)` đọc mọi texture (ReadTexture) và mesh (buffer staging)
  từ device CŨ (ta vẫn giữ tham chiếu), gọi lại `InitStates()` trên device mới, tạo lại texture/mesh CÙNG id → player Rust
  giữ nguyên. Device cũ đã mất (`GetDeviceRemovedReason`) hoặc lỗi → như cũ: tạo lại renderer + nạp lại SWF.
  Lưu ý: nếu hệ điều hành kết thúc app khi thiếu RAM (tombstone) thì vẫn phải mở lại từ đầu.
- Settings (C# `AppSettings`, IsolatedStorageSettings): GPU (đổi → nạp lại game), MSAA 4x, Sound (dừng engine XAudio2),
  Show FPS, Render resolution Auto/100/75/50% (Auto = tối đa 720px theo chiều rộng máy → M8 ≈ 66%), Quality
  (`ruffle_set_quality`), Frame limit Unlimited/30/20 (dồn dt, tick khi đủ khoảng), Version (đọc WMAppManifest).
  Nút bấm xoay vòng giá trị (SDK 8.0 không có ComboBox/ListPicker). Mở từ nút "Settings" ở danh sách hoặc
  ApplicationBar thu nhỏ (mờ 0.6) khi chơi: settings / reset zoom / back to list.
- Zoom 2 ngón: C# theo dõi mọi ngón (Touch.FrameReported); ngón 2 chạm → nhả chuột ngón 1, zoom 1..4 quanh tâm 2 ngón
  + kéo theo tâm; native đổi toạ độ chạm: (pan + n/zoom) * cỡ nội dung.
- Bỏ dòng log xanh: chỉ `FpsText()` ("60 FPS GPU") khi bật Show FPS; trace của SWF chỉ ra debug output;
  nạp lỗi → `IsPlaying()` false → về danh sách và hiện `Status()`.
- `RufflePlayer80.{h,cpp}` viết lại toàn bộ cho các thay đổi trên; `AppSettings.cs` mới (wp81sl liên kết).

### 2026-11-09 — Sửa: giữ Back (màn chuyển app) rồi quay lại chỉ vẽ nửa màn hình
- Quay lại app → hệ thống đưa D3D device mới → tạo lại GpuRenderer và player. Lỗi: player được tạo ngay với cỡ RT dọc
  đầy đủ, trước khi tính cỡ nội dung (xoay + vùng nhìn thấy); `m_contentW/H` cũ vẫn khớp nên không `ruffle_resize`
  → Ruffle vẽ sai cỡ, chỉ phủ một phần màn hình.
- Sửa: tạo lại player SAU khi biết cỡ nội dung; `CreatePlayer` ghi `m_contentW/H` = cỡ tạo; Draw chỉ cập nhật khi đã resize.
- Kèm: scale DPI trong `CreatePlayer` dùng chiều rộng RT (cạnh ngắn)/480 như `ruffle_resize` (trước dùng cạnh dài khi ngang).
- Build wp80 + wp81sl OK, chưa xác nhận trên máy.

### 2026-11-08 — Âm thanh (wp80 + wp81sl, build OK, chưa nghe thử)
- `wp80\RuffleWP80Native\AudioOutput.{h,cpp}`: port ruffleUWP cho VS2012 — XAudio2 (`xaudio2.lib` → `XAudio2_8.dll`,
  có trên WP 8.0/8.1), source voice stereo f32 ở `ruffle_audio_sample_rate()`, thread riêng gọi fill, 4 x 1024 frame.
- `RufflePlayer80`: `ruffle_set_audio_enabled(1)`; lambda fill chỉ giữ con trỏ `&m_ruffle`/`&m_audioLock` (không giữ ^
  → không vòng tham chiếu); tạo/huỷ player dưới `m_audioLock`; engine dừng khi không có game hoặc app bị treo
  (`SetSuspended` từ PhoneApplicationService Deactivated/Closing/Activated). Lỗi XAudio2 → dòng trạng thái "No sound".
- Cả hai project native thêm `AudioOutput.cpp` + `xaudio2.lib`.

### 2026-11-08 — Sửa xoay ngược + chạm lệch (HTC One M8, 8.1)
- Test M8: xoay ngang bị NGƯỢC → đổi chỗ công thức uv: LandscapeLeft = (y, 1-x), LandscapeRight = (1-y, x).
- Chạm lệch nhẹ: RT hệ thống phủ cả dải nút ảo ở đáy máy, còn toạ độ chạm chuẩn hoá theo vùng trang nhìn thấy.
  Sửa: chỉ vẽ trong vùng nhìn thấy — `visH = cạnh dài trang (DIP) * (RT.Width / cạnh ngắn trang)`, tính từ đỉnh RT
  (dải nút luôn ở đáy máy vật lý kể cả khi xoay). Nội dung = W x visH (dọc) hoặc visH x W (ngang); Blit phủ đúng vùng đó.
  Máy không có nút ảo: visH = RT.Height → không đổi gì.
- Build cả wp80 và wp81sl: OK. **User xác nhận trên M8: xoay và chạm đều đúng.**

### 2026-11-08 — Xoay ngang + chẩn đoán chạm lệch (build OK, chưa chạy trên máy)
- GPU: user xác nhận đã mượt trên HTC One M8. Còn: chạm bị LỆCH (không phải mất), không xoay được, chưa có âm thanh.
- Xoay: trang `PortraitOrLandscape`, `OnOrientationChanged` → `SetOrientation(0/1/2)`. RT hệ thống luôn dọc;
  khi ngang, Rust vẽ vào texture ngang `m_off` (cỡ h x w) và `Present()` vẽ quad xoay bằng PS_Layer:
  uv LandscapeLeft = (1-y, x), LandscapeRight = (y, 1-x). Nếu ngược chiều thì đổi 2 công thức này.
- CPU renderer cũng đi qua GPU (`PresentCpu`: UpdateSubresource vào texture RGBA rồi Blit) → bỏ đảo màu CPU.
- Chạm: bỏ Rectangle + Mouse*, dùng `Touch.FrameReported` (ngón chính, chuẩn hoá theo ActualWidth/Height của grid).
  Dòng trạng thái in: RT, content, rot, view (DIP), tap chuẩn hoá -> pixel, để tìm nguyên nhân lệch.
- `.gitignore`: thêm output ruffleWP8 (ARM/, Bin/, obj/, AppPackages/, Generated Files/, *.xap/*.appx, spike *.obj/*.dll/*.txt);
  bỏ theo dõi (git rm --cached) 10 file build trong `ruffleWP8/spike` đã lỡ commit (file vẫn còn trên đĩa).

### 2026-11-08 — GPU renderer cho wp80 + wp81sl (build OK, chưa chạy trên máy)
- Bản 8.1 SL chạy trên HTC One M8 (1080x1920) nhưng giật: CPU raster full-res + đảo màu CPU ~8MB/khung.
- `wp80\RuffleWP80Native\GpuRenderer.{h,cpp}`: port từ ruffleUWP cho VS2012 (không NSDMI, không `= {...}`,
  lambda mặc định → `MakeBlend`, lambda → hàm tĩnh `H*`). Vẽ vào RTV của DrawingSurfaceBackgroundGrid
  (`SetScreen` mỗi khung, context có thể đổi); MSAA dùng format của RT, mặc định tắt.
- Shader: FxCompile thẳng file `..\ruffleUWP\app\RuffleUWP\Shaders\*.hlsl` (4.0_level_9_1, header g_<tên> trong $(IntDir)).
- `RufflePlayer80`: mặc định `ruffle_create_gpu`; GPU lỗi → `ruffle_create` (CPU). RT hệ thống không giữ nội dung
  → khung không có frame mới gọi `ruffle_render`. Device mới (Connect lại) → tạo lại GpuRenderer + nạp lại SWF từ `m_swf`.
  `SetUseGpu()` cho Settings sau. Dòng trạng thái ghi "GPU"/"CPU".

### 2026-11-08 — Silverlight 8.1 (.xap cho máy 8.1, có file picker)
- Lý do: WP 8.0 không cho app đọc Documents/Downloads và không có file picker; Silverlight 8.1 có
  `FileOpenPicker.PickSingleFileAndContinue` mà vẫn giữ kiến trúc C# + DrawingSurfaceBackgroundGrid + component C++.
- `wp81sl\RuffleWP81SLNative.vcxproj`: `ApplicationType=Windows Phone Silverlight`, `ApplicationTypeRevision=8.1`,
  `PlatformToolset=v120`, biên dịch file nguồn của `wp80\RuffleWP80Native` + `shim\WpStubs.cpp` (không WpCompat80);
  `TargetName=RuffleWP80Native` để .winmd khớp namespace → C# dùng chung không đổi.
- `wp81sl\RuffleWP81SL.csproj`: `TargetFrameworkVersion=v8.1`, LIÊN KẾT App.xaml/MainPage.xaml(+.cs) từ wp80, hằng `WP81SL`
  bật nút "Choose file..." + `PhoneApplicationService.ContractActivated` (FileOpenPickerContinuationEventArgs).
- Cần cả `WMAppManifest.xml` (AppPlatformVersion 8.1, namespace 2014) và `Package.appxmanifest` (AGHost.exe; component
  đăng ký trong `windows.activatableClass.inProcessServer`; capability `removableStorage`).
- Trình đóng gói .xap KHÔNG đọc ảnh liên kết (`<Link>`) → `make-assets.ps1` chép thật ảnh vào wp81sl.
- Build Release|ARM: OK → `wp81sl\RuffleWP81SL\Bin\ARM\Release\RuffleWP81SL_Release_ARM.xap` (3.2MB).

### 2026-11-08 — Tách thư mục
- `app81\` → `wp81\` (kèm `RuffleWP81.sln`), `app80\` → `wp80\` (kèm `RuffleWP80.sln`). Độ sâu project không đổi nên
  đường dẫn tương đối tới `..\..\shim` và `..\..\..\ruffleUWP` giữ nguyên. Output build giờ nằm trong từng thư mục.
