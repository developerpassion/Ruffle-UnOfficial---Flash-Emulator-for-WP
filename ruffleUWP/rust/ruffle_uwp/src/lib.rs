//! C ABI cho host UWP (C++/CX). Rust không gọi WinRT; host lo file/D3D/audio/input.
//! Xem ../../include/ruffle_uwp.h — phải giữ đồng bộ.

mod audio;
mod input;
mod log;
mod navigator;
pub mod render_gpu;
mod render_sw;
pub mod storage;

use std::ffi::c_void;
use std::sync::{Arc, Mutex};

use ruffle_core::backend::navigator::NullExecutor;
use ruffle_core::backend::ui::NullUiBackend;
use ruffle_core::events::{MouseButton, PlayerEvent};
use ruffle_core::tag_utils::SwfMovieData;
use ruffle_core::{FloatDuration, Player, PlayerBuilder};
use ruffle_video::null::NullVideoBackend;

pub use log::RuffleLogCallback;
use render_sw::SoftwareRenderer;

pub struct RuffleHandle {
    player: Arc<Mutex<Player>>,
    audio: Option<audio::AudioOut>, // None khi tắt âm thanh
    /// Chạy future của player (loadBytes...). Phải chạy NGOÀI khoá player (future tự khoá).
    executor: std::cell::RefCell<NullExecutor>,
}

/// Tạo player từ bytes SWF. Trả null nếu lỗi.
/// # Safety
/// `data` phải trỏ tới `len` byte hợp lệ.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_create(
    data: *const u8,
    len: usize,
    width: u32,
    height: u32,
    scale: f64,
    log_cb: Option<RuffleLogCallback>,
    log_ctx: *mut c_void,
) -> *mut RuffleHandle {
    unsafe { create_with(data, len, width, height, scale, log_cb, log_ctx, SoftwareRenderer::new(width, height, scale)) }
}

/// Như `ruffle_create` nhưng vẽ bằng GPU qua `host` (Direct3D 11 phía C++).
/// Host phải sống lâu hơn player (gọi ruffle_destroy trước khi huỷ host).
/// # Safety
/// `data` trỏ tới `len` byte; `host` trỏ tới bảng hàm hợp lệ (được copy).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_create_gpu(
    data: *const u8,
    len: usize,
    width: u32,
    height: u32,
    scale: f64,
    log_cb: Option<RuffleLogCallback>,
    log_ctx: *mut c_void,
    host: *const render_gpu::RuffleGpuHost,
) -> *mut RuffleHandle {
    let Some(host) = (unsafe { host.as_ref() }) else { return std::ptr::null_mut() };
    let renderer = unsafe { render_gpu::GpuRenderer::new(*host, width, height, scale) };
    unsafe { create_with(data, len, width, height, scale, log_cb, log_ctx, renderer) }
}

#[allow(clippy::too_many_arguments)]
unsafe fn create_with(
    data: *const u8,
    len: usize,
    width: u32,
    height: u32,
    scale: f64,
    log_cb: Option<RuffleLogCallback>,
    log_ctx: *mut c_void,
    renderer: impl ruffle_render::backend::RenderBackend + 'static,
) -> *mut RuffleHandle {
    if data.is_null() {
        return std::ptr::null_mut();
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };
    let movie = match SwfMovieData::from_data(bytes, "file:///movie.swf".into(), None, None) {
        Ok(m) => m,
        Err(_) => return std::ptr::null_mut(),
    };

    let executor = NullExecutor::new();
    let builder = PlayerBuilder::new().with_renderer(renderer);
    // Tắt âm thanh (Settings): NullAudioBackend -> không giải mã/trộn tiếng (đỡ CPU), timeline vẫn đúng nhịp
    let (builder, audio_out) = if AUDIO_ENABLED.load(std::sync::atomic::Ordering::Relaxed) {
        let (backend, out) = audio::UwpAudio::new();
        (builder.with_audio(backend), Some(out))
    } else {
        (builder.with_audio(ruffle_core::backend::audio::NullAudioBackend::new()), None)
    };
    let player = builder
        .with_navigator(navigator::UwpNavigator::new(&executor))
        .with_storage(storage::backend())
        .with_ui(NullUiBackend::new())
        .with_video(NullVideoBackend::new())
        .with_log(log::HostLog::new(log_cb, log_ctx))
        .with_viewport_dimensions(width, height, scale)
        // Phủ đen phần ngoài sân khấu (màn hình rộng hơn game): không lộ vật thể game đặt ngoài khung
        .with_letterbox(ruffle_core::config::Letterbox::On)
        .with_autoplay(true)
        .with_movie(movie)
        .build();

    Box::into_raw(Box::new(RuffleHandle { player, audio: audio_out, executor: std::cell::RefCell::new(executor) }))
}

/// # Safety
/// `h` từ `ruffle_create`, chỉ gọi một lần.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_destroy(h: *mut RuffleHandle) {
    if !h.is_null() {
        drop(unsafe { Box::from_raw(h) });
    }
}

/// Chạy các future đang chờ (không giữ khoá player).
fn run_executor(h: *mut RuffleHandle) {
    if let Some(h) = unsafe { h.as_ref() } {
        if let Ok(mut ex) = h.executor.try_borrow_mut() {
            ex.run();
        }
    }
}

fn with_player<R>(h: *mut RuffleHandle, f: impl FnOnce(&mut Player) -> R) -> Option<R> {
    let h = unsafe { h.as_ref()? };
    let mut p = h.player.lock().ok()?;
    Some(f(&mut p))
}

/// Tiến thời gian `dt_ms` mili giây và render vào framebuffer nội bộ.
#[unsafe(no_mangle)]
/// Trả 1 nếu framebuffer vừa được vẽ lại (host mới cần upload lên GPU).
pub extern "C" fn ruffle_tick(h: *mut RuffleHandle, dt_ms: f64) -> i32 {
    with_player(h, |p| p.tick(FloatDuration::from_secs(dt_ms / 1000.0)));
    run_executor(h);
    with_player(h, |p| {
        if p.needs_render() {
            p.render();
            1
        } else {
            0
        }
    })
    .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn ruffle_resize(h: *mut RuffleHandle, width: u32, height: u32, scale: f64) {
    with_player(h, |p| {
        p.set_viewport_dimensions(ruffle_render::backend::ViewportDimensions {
            width,
            height,
            scale_factor: scale,
        });
    });
}

/// Lấy framebuffer RGBA8 (premultiplied). Con trỏ hợp lệ đến lần tick/resize kế tiếp.
/// # Safety
/// out_* phải là con trỏ hợp lệ.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_get_framebuffer(
    h: *mut RuffleHandle,
    out_w: *mut u32,
    out_h: *mut u32,
    out_stride: *mut u32,
) -> *const u8 {
    with_player(h, |p| {
        let any: &dyn std::any::Any = p.renderer();
        match any.downcast_ref::<SoftwareRenderer>() {
            Some(r) => {
                let (w, hgt, ptr) = r.framebuffer();
                unsafe {
                    *out_w = w;
                    *out_h = hgt;
                    *out_stride = w * 4;
                }
                ptr
            }
            None => std::ptr::null(),
        }
    })
    .unwrap_or(std::ptr::null())
}

/// kind: 0=move, 1=down, 2=up. Toạ độ pixel thiết bị.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_mouse(h: *mut RuffleHandle, kind: i32, x: f64, y: f64) {
    let ev = match kind {
        1 => PlayerEvent::MouseDown { x, y, button: MouseButton::Left, index: None },
        2 => PlayerEvent::MouseUp { x, y, button: MouseButton::Left },
        _ => PlayerEvent::MouseMove { x, y },
    };
    with_player(h, |p| {
        p.handle_event(ev);
    });
}

fn mouse_button(b: i32) -> MouseButton {
    match b {
        0 => MouseButton::Left,
        1 => MouseButton::Right,
        2 => MouseButton::Middle,
        _ => MouseButton::Unknown,
    }
}

/// Nút chuột. kind: 1=down 2=up; button: 0=trái 1=phải 2=giữa. Trả 1 nếu SWF đã xử lý sự kiện.
/// Host nên mở context menu khi chuột phải down mà trả 0 (giống desktop Ruffle).
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_mouse_button(h: *mut RuffleHandle, kind: i32, button: i32, x: f64, y: f64) -> i32 {
    let button = mouse_button(button);
    let ev = if kind == 1 {
        PlayerEvent::MouseDown { x, y, button, index: None }
    } else {
        PlayerEvent::MouseUp { x, y, button }
    };
    with_player(h, |p| p.handle_event(ev) as i32).unwrap_or(0)
}

/// Cuộn chuột, `lines` > 0 = cuộn lên.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_mouse_wheel(h: *mut RuffleHandle, lines: f64) {
    with_player(h, |p| {
        p.handle_event(PlayerEvent::MouseWheel {
            delta: ruffle_core::events::MouseWheelDelta::Lines(lines),
        });
    });
}

/// Callback liệt kê mục context menu. `caption` là UTF-8 kết thúc bằng 0, chỉ hợp lệ trong lời gọi.
pub type RuffleMenuItemCallback = unsafe extern "C" fn(
    ctx: *mut c_void,
    index: u32,
    caption: *const std::ffi::c_char,
    enabled: i32,
    checked: i32,
    separator_before: i32,
);

/// Chuẩn bị context menu (gọi sau chuột phải down). Gọi `cb` cho từng mục, trả số mục (0 = không hiện menu).
/// # Safety
/// `cb` hợp lệ.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_context_menu(
    h: *mut RuffleHandle,
    cb: Option<RuffleMenuItemCallback>,
    ctx: *mut c_void,
) -> u32 {
    let items = with_player(h, |p| p.prepare_context_menu()).unwrap_or_default();
    if let Some(cb) = cb {
        for (i, it) in items.iter().enumerate() {
            let caption = std::ffi::CString::new(it.caption.replace('\0', "")).unwrap_or_default();
            unsafe {
                cb(ctx, i as u32, caption.as_ptr(), it.enabled as i32, it.checked as i32, it.separator_before as i32)
            };
        }
    }
    items.len() as u32
}

/// Chạy mục context menu đã chọn.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_context_menu_select(h: *mut RuffleHandle, index: u32) {
    with_player(h, |p| p.run_context_menu_callback(index as usize));
}

/// Menu đóng: gửi MouseUp chuột phải (bị nuốt khi menu đang mở) và dọn menu.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_context_menu_closed(h: *mut RuffleHandle, x: f64, y: f64) {
    with_player(h, |p| {
        p.handle_event(PlayerEvent::MouseUp { x, y, button: MouseButton::Right });
        p.clear_custom_menu_items();
    });
}

/// Phím bấm. `vk` = Windows VirtualKey, `mods` bit0=Shift bit1=Ctrl, `down` 1/0.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_key(h: *mut RuffleHandle, vk: u32, mods: u32, down: i32) {
    let key = input::key_descriptor(vk, mods);
    with_player(h, |p| {
        if down != 0 {
            p.handle_event(PlayerEvent::KeyDown { key });
            if let Some(code) = input::text_control(vk, mods) {
                p.handle_event(PlayerEvent::TextControl { code });
            }
        } else {
            p.handle_event(PlayerEvent::KeyUp { key });
        }
    });
}

/// Ký tự đã nhập (codepoint UTF-32) cho TextField. Ký tự điều khiển bị bỏ (đã gửi qua TextControl).
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_char(h: *mut RuffleHandle, codepoint: u32) {
    let Some(c) = char::from_u32(codepoint) else { return };
    if c.is_control() {
        return;
    }
    with_player(h, |p| {
        p.handle_event(PlayerEvent::TextInput { codepoint: c });
    });
}

/// Pause/resume (Suspending/Resuming).
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_set_playing(h: *mut RuffleHandle, playing: i32) {
    with_player(h, |p| p.set_is_playing(playing != 0));
}

/// Tần số mẫu audio (Hz); PCM stereo f32 interleaved.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_audio_sample_rate() -> u32 {
    audio::SAMPLE_RATE
}

/// Trộn `frames` khung stereo f32 vào `out` (len = frames*2). Gọi từ thread audio, không khoá Player.
/// # Safety
/// `out` trỏ tới `frames*2` float; `h` còn sống trong suốt lời gọi.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_audio_fill(h: *mut RuffleHandle, out: *mut f32, frames: u32) {
    if out.is_null() {
        return;
    }
    let buf = unsafe { std::slice::from_raw_parts_mut(out, frames as usize * 2) };
    match unsafe { h.as_ref() } {
        Some(RuffleHandle { audio: Some(a), .. }) => a.fill(buf),
        _ => buf.fill(0.0),
    }
}

/// Đăng ký nơi lưu SharedObject (save game) cho các player tạo sau đó. null = lưu trong RAM.
/// # Safety
/// `host` null hoặc trỏ tới bảng hàm hợp lệ (được copy); hàm phải sống lâu hơn mọi player.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_set_storage(host: *const storage::RuffleStorageHost) {
    storage::set_host(unsafe { host.as_ref() }.copied());
}

/// Đăng ký cache web cho navigator (lưu file tải được; trả lại khi không có mạng). null = tắt.
/// Hàm của host có thể được gọi từ thread khác (thread tải mạng).
/// # Safety
/// `host` null hoặc trỏ tới bảng hàm hợp lệ (được copy).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ruffle_set_web_cache(host: *const storage::RuffleStorageHost) {
    storage::set_web_host(unsafe { host.as_ref() }.copied());
}

/// Vẽ lại khung hình hiện tại ngay (không tiến thời gian). Dùng khi host đổi cách hiển thị (vd. zoom)
/// và cần present lại với nội dung mới nhất.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_render(h: *mut RuffleHandle) {
    with_player(h, |p| p.render());
}

/// Bật/tắt âm thanh cho các player tạo sau đó (tắt = NullAudioBackend, tiết kiệm CPU).
static AUDIO_ENABLED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(true);

#[unsafe(no_mangle)]
pub extern "C" fn ruffle_set_audio_enabled(enabled: i32) {
    AUDIO_ENABLED.store(enabled != 0, std::sync::atomic::Ordering::Relaxed);
}

/// Chất lượng hình: 0 Low (không làm mịn bitmap), 1 Medium, 2 High, 3 Best.
/// Lưu ý: SWF có thể tự đổi bằng stage.quality.
#[unsafe(no_mangle)]
pub extern "C" fn ruffle_set_quality(h: *mut RuffleHandle, quality: i32) {
    use ruffle_render::quality::StageQuality as Q;
    let q = match quality {
        0 => Q::Low,
        1 => Q::Medium,
        3 => Q::Best,
        _ => Q::High,
    };
    with_player(h, |p| p.set_quality(q));
}

/// true khi chất lượng Low: renderer bỏ làm mịn bitmap (đọc trong render_sw / render_gpu).
pub(crate) static LOW_QUALITY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub(crate) fn low_quality() -> bool {
    LOW_QUALITY.load(std::sync::atomic::Ordering::Relaxed)
}
