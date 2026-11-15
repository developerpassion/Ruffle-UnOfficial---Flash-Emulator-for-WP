//! Chạy SWF theo kịch bản để tái hiện lỗi trên desktop (không cần UWP):
//!   cargo run --release --example play -- <file.swf> <script.txt> [width] [height]
//! Mỗi dòng của script:
//!   tick <n>          chạy n frame (1/30 s mỗi frame)
//!   click <x> <y>     click chuột trái (toạ độ pixel của viewport)
//!   move <x> <y>
//!   shot <out.png>    lưu framebuffer
use std::ffi::{c_char, c_void, CStr};

extern "C" fn log(_: *mut c_void, msg: *const c_char) {
    print!("{}", unsafe { CStr::from_ptr(msg) }.to_string_lossy());
}

fn shot(p: *mut ruffle_uwp::RuffleHandle, path: &str) {
    unsafe {
        let (mut fw, mut fh, mut st) = (0, 0, 0);
        let fb = ruffle_uwp::ruffle_get_framebuffer(p, &mut fw, &mut fh, &mut st);
        let data = std::slice::from_raw_parts(fb, (st * fh) as usize);
        let f = std::fs::File::create(path).unwrap();
        let mut enc = png::Encoder::new(std::io::BufWriter::new(f), fw, fh);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        enc.write_header().unwrap().write_image_data(data).unwrap();
    }
    println!("-> {path}");
}

// Cache web đơn giản cho test (thư mục RUFFLE_WEB_CACHE): giống LocalState\WebCache của app
fn cache_file(name: *const c_char) -> Option<std::path::PathBuf> {
    let dir = std::env::var_os("RUFFLE_WEB_CACHE")?;
    let n = unsafe { CStr::from_ptr(name) }.to_string_lossy().replace(|c: char| !c.is_ascii_alphanumeric() && c != '.', "_");
    Some(std::path::Path::new(&dir).join(n))
}
unsafe extern "C" fn web_get(_: *mut c_void, name: *const c_char, buf: *mut u8, cap: usize) -> i64 {
    let Some(d) = cache_file(name).and_then(|p| std::fs::read(p).ok()) else { return -1 };
    if !buf.is_null() {
        let n = d.len().min(cap);
        unsafe { std::ptr::copy_nonoverlapping(d.as_ptr(), buf, n) };
        return n as i64;
    }
    d.len() as i64
}
unsafe extern "C" fn web_put(_: *mut c_void, name: *const c_char, data: *const u8, len: usize) -> i32 {
    let Some(p) = cache_file(name) else { return 0 };
    std::fs::write(p, unsafe { std::slice::from_raw_parts(data, len) }).is_ok() as i32
}
unsafe extern "C" fn web_remove(_: *mut c_void, _: *const c_char) {}

fn main() {
    // Hiện lỗi/cảnh báo nội bộ của Ruffle (RUST_LOG=warn mặc định)
    let _ = tracing_subscriber::fmt().with_env_filter(tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "warn".into())).with_writer(std::io::stdout).try_init();
    let a: Vec<String> = std::env::args().collect();
    let swf = std::fs::read(&a[1]).expect("read swf");
    let script = std::fs::read_to_string(&a[2]).expect("read script");
    let w: u32 = a.get(3).map(|s| s.parse().unwrap()).unwrap_or(800);
    let h: u32 = a.get(4).map(|s| s.parse().unwrap()).unwrap_or(600);
    if std::env::var_os("RUFFLE_WEB_CACHE").is_some() {
        let host = ruffle_uwp::storage::RuffleStorageHost { ctx: std::ptr::null_mut(), get: web_get, put: web_put, remove: web_remove };
        unsafe { ruffle_uwp::ruffle_set_web_cache(&host) };
    }
    unsafe {
        let p = ruffle_uwp::ruffle_create(swf.as_ptr(), swf.len(), w, h, 1.0, Some(log), std::ptr::null_mut());
        assert!(!p.is_null(), "ruffle_create failed");
        for line in script.lines() {
            let t: Vec<&str> = line.split_whitespace().collect();
            match t.as_slice() {
                ["tick", n] => {
                    for _ in 0..n.parse::<u32>().unwrap() {
                        ruffle_uwp::ruffle_tick(p, 1000.0 / 30.0);
                    }
                }
                ["real", n] => {
                    // như tick nhưng chờ thời gian thực (game dùng getTimer/Timer/Loader bất đồng bộ)
                    for _ in 0..n.parse::<u32>().unwrap() {
                        ruffle_uwp::ruffle_tick(p, 1000.0 / 30.0);
                        std::thread::sleep(std::time::Duration::from_millis(33));
                    }
                }
                ["slow", n, ms] => {
                    // tick với dt lớn (giả lập máy chậm, vd. điện thoại ~200 ms/tick)
                    let ms: f64 = ms.parse().unwrap();
                    for _ in 0..n.parse::<u32>().unwrap() {
                        ruffle_uwp::ruffle_tick(p, ms);
                    }
                }
                ["move", x, y] => ruffle_uwp::ruffle_mouse(p, 0, x.parse().unwrap(), y.parse().unwrap()),
                ["click", x, y] => {
                    let (x, y) = (x.parse().unwrap(), y.parse().unwrap());
                    ruffle_uwp::ruffle_mouse(p, 0, x, y);
                    ruffle_uwp::ruffle_tick(p, 1000.0 / 30.0);
                    ruffle_uwp::ruffle_mouse_button(p, 1, 0, x, y);
                    ruffle_uwp::ruffle_tick(p, 1000.0 / 30.0);
                    ruffle_uwp::ruffle_mouse_button(p, 2, 0, x, y);
                }
                ["shot", path] => shot(p, path),
                _ => {}
            }
        }
        ruffle_uwp::ruffle_destroy(p);
    }
}
