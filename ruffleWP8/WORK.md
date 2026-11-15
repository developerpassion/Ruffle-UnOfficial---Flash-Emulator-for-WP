# WORK — Danh sách công việc

Đánh dấu `[x]` khi xong, `[~]` đang làm, `[!]` bị chặn (ghi lý do trong PROGRESS.md).

## S0 — Spike link Rust ↔ WP 8.1 (đi/dừng)
- [x] Xác định CRT/lib ARM cho WP8.1: `VC 12.0\lib\store\arm` (msvcr120_app), toolset `v120_wp81`, `x86_arm\link.exe`
- [x] Staticlib ARM: dùng luôn `..\ruffleUWP\lib\ARM\release\ruffle_uwp.lib` (thumbv7a-uwp, build-std, panic=abort)
- [x] Link DLL thử (`spike\link-spike.cmd`) bằng VS2013 — **exit 0**, `link.exe` 12.0 đọc được object LLVM
- [x] Symbol thiếu: 0 hàm CRT; 12 hàm Win32 → `shim\WpStubs.cpp` (từ UwpStubs + 4 hàm mới, PRNG qua WinRT)
- [x] ~~lld-link~~ — không cần
- [x] `dumpbin /imports`: chỉ api-ms-win-core-* (đều có trong WindowsPhoneCore.lib 8.1), ntdll, msvcr120_app, ws2_32
- [ ] Deploy lên Lumia 8.1, gọi được Rust, ghi log vào LocalFolder (cần máy WP8.1 developer-unlocked) → làm cùng M1
- [x] **Quyết định**: tiếp tục M1 (link tĩnh đã qua; rủi ro còn lại là lúc chạy: ntdll + WaitOnAddress trên 8.1)

## M1 — Skeleton app 8.1
- [x] Solution `wp81/RuffleWP81.sln` + project `wp81/RuffleWP81` (C++/CX, XAML, SwapChainPanel, manifest WP8.1, ARM, v120_wp81)
- [x] D3D11 device FL 11_1..9_1 + swapchain cho SwapChainPanel → xoá màn hình xanh; test lõi Rust (create/tick/render/destroy)
- [x] Build Release|ARM bằng MSBuild 14.0 (VS2015) → `wp81\AppPackages\RuffleWP81\RuffleWP81_0.1.0.0_ARM_Test\*.appx`
- [x] **Chạy trên Lumia 8.1** — M1 PASS (D3D11, SwapChain, lõi Rust)
- [ ] Script deploy dòng lệnh (máy không có AppDeployCmd.exe 8.1 — chỉ có AppDeploy.exe GUI)

## M2 — Player chạy
- [ ] Port `RufflePlayer` từ ruffleUWP (render thread, tick, CPU renderer blit)
- [ ] Nạp SWF: `PickSingleFileAndContinue` + xử lý `ContinuationManager` khi app quay lại
- [ ] Nạp SWF đặt sẵn trong LocalFolder (`autoload.swf`) để test nhanh
- [ ] Log → `LocalState\log.txt`

## M3 — GPU renderer
- [ ] Port `GpuRenderer` + shader (profile `4.0_level_9_1`, index 16-bit khi FL 9_1)
- [ ] Kiểm tra trên Adreno 225/305

## M4 — Input
- [ ] Touch → mouse, giữ lâu = chuột phải/context menu
- [ ] Zoom + dịch 2 ngón
- [ ] Bàn phím ảo (TextBox ẩn), nút Back (8.1: `HardwareButtons.BackPressed`)

## M5 — Audio
- [ ] XAudio2 (port `AudioOutput`), tạm dừng khi app bị suspend

## M6 — Hoàn thiện 8.1
- [ ] Settings (GPU, MSAA, render resolution, quality, frame limit, FPS, sound, version)
- [ ] Giới hạn RAM: đo peak trên máy 512MB/1GB, giảm cache texture
- [ ] SharedObject → LocalFolder

## M7 — WP 8.0 (`RuffleWP80.sln` → .xap)
- [x] Spike link lõi Rust với CRT VS2012 WP80 (`spike\link80.cmd`) — exit 0 với `shim\WpCompat80.cpp`
- [x] `wp80\RuffleWP80Native` (WinPRT C++, v110_wp80): `RuffleTest::Run()` chạy lõi Rust, trả chữ
- [x] `wp80\RuffleWP80` (C# Silverlight 8.0) hiện kết quả; build → `wp80\RuffleWP80\Bin\ARM\Release\RuffleWP80_Release_ARM.xap`
- [x] **Chạy trên máy WP 8.0** — Release .xap: M1 PASS (lõi Rust chạy trên 8.0)
- [x] Debug .xap lỗi FileNotFound (CRT debug MSVCR110D không có trên máy) → Debug dùng CRT release
- [~] **M2 8.0 (ưu tiên)**: `RufflePlayer80` — `DrawingSurfaceBackgroundGrid` + `IDrawingSurfaceBackgroundContentProviderNative`,
      CPU renderer → `UpdateSubresource` thẳng vào render target (đảo RGBA→BGRA nếu cần). Build OK, **chưa chạy trên máy**
- [~] Nạp SWF: danh sách .swf trên thẻ nhớ (`ExternalStorage`, ID_CAP_REMOVABLE_STORAGE) + mở .swf từ email/trình duyệt
      (FileTypeAssociation → `SharedStorageAccessManager`). Build OK, chưa chạy trên máy
- [~] Chạm → chuột (lớp `inputLayer`, toạ độ chuẩn hoá), Back = về danh sách
- [x] Bản Silverlight 8.1 (`wp81sl\RuffleWP81SL.sln`): cùng mã, thêm file picker. Build OK, **chưa chạy trên máy**
- [x] GPU renderer (wp80 + wp81sl, mặc định bật, lỗi -> CPU) — **mượt trên HTC One M8**
- [x] Xoay ngang (LandscapeLeft/Right) — đúng trên HTC One M8
- [x] Chạm đúng vị trí (chỉ vẽ trong vùng nhìn thấy, trừ dải nút ảo) — đúng trên HTC One M8
- [x] Âm thanh — có tiếng trên M8
- [x] Giữ Back rồi quay lại vẽ nửa màn — đã sửa (user xác nhận)
- [x] Settings (GPU, MSAA, Sound, Show FPS, render resolution, quality, frame limit, version) — chạy trên M8
- [x] Quay lại app KHÔNG nạp lại game (chuyển texture/mesh sang D3D device mới) — chạy trên M8
- [x] Zoom + kéo 2 ngón (1x..4x), "reset zoom" trên thanh app — chạy trên M8
- [x] Bỏ dòng log xanh: chỉ hiện "FPS GPU/CPU" khi bật Show FPS; lỗi nạp → về danh sách
- [ ] Test lại bản 8.0 (wp80) trên máy WP 8.0 thật
- [~] Âm thanh: `AudioOutput` (XAudio2_8, port từ ruffleUWP) cho wp80 + wp81sl, dừng khi ở danh sách/app bị treo.
      Build OK, **chưa nghe thử trên máy**
