// svg2png <in.svg> <out.png> <W> <H> <padding_fraction>
use resvg::{tiny_skia, usvg};
fn main() {
    let a: Vec<String> = std::env::args().collect();
    let (w, h): (u32, u32) = (a[3].parse().unwrap(), a[4].parse().unwrap());
    let pad: f32 = a[5].parse().unwrap();
    let data = std::fs::read(&a[1]).unwrap();
    let tree = usvg::Tree::from_data(&data, &usvg::Options::default()).unwrap();
    let s = tree.size();
    let (bw, bh) = (w as f32 * (1.0 - 2.0 * pad), h as f32 * (1.0 - 2.0 * pad));
    let k = (bw / s.width()).min(bh / s.height());
    let (dx, dy) = ((w as f32 - s.width() * k) / 2.0, (h as f32 - s.height() * k) / 2.0);
    let mut pm = tiny_skia::Pixmap::new(w, h).unwrap();
    resvg::render(&tree, tiny_skia::Transform::from_row(k, 0.0, 0.0, k, dx, dy), &mut pm.as_mut());
    pm.save_png(&a[2]).unwrap();
}
