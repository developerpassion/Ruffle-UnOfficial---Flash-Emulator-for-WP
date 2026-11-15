//! Kiểm tra software renderer trên desktop (không cần UWP):
//!   cargo run --example render_png -- <file.swf> <out.png> [frames] [width] [height]
use std::ffi::{c_char, c_void, CStr};

extern "C" fn log(_: *mut c_void, msg: *const c_char) {
    print!("{}", unsafe { CStr::from_ptr(msg) }.to_string_lossy());
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let swf = std::fs::read(&a[1]).expect("read swf");
    let frames: u32 = a.get(3).map(|s| s.parse().unwrap()).unwrap_or(1);
    let w: u32 = a.get(4).map(|s| s.parse().unwrap()).unwrap_or(550);
    let h: u32 = a.get(5).map(|s| s.parse().unwrap()).unwrap_or(400);

    unsafe {
        let p = ruffle_uwp::ruffle_create(swf.as_ptr(), swf.len(), w, h, 1.0, Some(log), std::ptr::null_mut());
        assert!(!p.is_null(), "ruffle_create failed");
        let t = std::time::Instant::now();
        for _ in 0..frames {
            ruffle_uwp::ruffle_tick(p, 1000.0 / 24.0);
        }
        let el = t.elapsed();
        let (mut fw, mut fh, mut st) = (0, 0, 0);
        let fb = ruffle_uwp::ruffle_get_framebuffer(p, &mut fw, &mut fh, &mut st);
        let data = std::slice::from_raw_parts(fb, (st * fh) as usize);
        let f = std::fs::File::create(&a[2]).unwrap();
        let mut enc = png::Encoder::new(std::io::BufWriter::new(f), fw, fh);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        enc.write_header().unwrap().write_image_data(data).unwrap();
        if let Some(exp) = a.get(6) {
            let mut dec = png::Decoder::new(std::io::BufReader::new(std::fs::File::open(exp).unwrap()));
            dec.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
            let mut r = dec.read_info().unwrap();
            let mut buf = vec![0; r.output_buffer_size().unwrap()];
            let info = r.next_frame(&mut buf).unwrap();
            let ch = info.line_size / info.width as usize;
            let mut bad = 0usize;
            if info.width == fw && info.height == fh {
                for i in 0..(fw * fh) as usize {
                    let px = &buf[i * ch..i * ch + ch];
                    let e = if ch >= 3 { [px[0], px[1], px[2]] } else { [px[0]; 3] };
                    let g = &data[i * 4..i * 4 + 3];
                    if (0..3).any(|k| (e[k] as i32 - g[k] as i32).abs() > 24) {
                        bad += 1;
                    }
                }
                println!("DIFF {:.2}% {}", bad as f64 * 100.0 / (fw * fh) as f64, a[1]);
            } else {
                println!("DIFF size-mismatch {}", a[1]);
            }
        }
        println!("{frames} frames in {:?} ({:.2} ms/frame) -> {}", el, el.as_secs_f64() * 1000.0 / frames as f64, a[2]);
        ruffle_uwp::ruffle_destroy(p);
    }
}
