//! Software renderer cho Ruffle (CPU) — dùng ShapeTessellator của ruffle_render
//! rồi rasterize tam giác vào buffer RGBA8 premultiplied.
//!
//! Hỗ trợ: fill màu/gradient (linear/radial/focal, pad/reflect/repeat)/bitmap (repeat, smooth),
//! render_bitmap, color transform, mask (stencil), alpha mask, blend modes cơ bản,
//! cacheAsBitmap (không filter), BitmapData.draw (render_offscreen + sync).
//! Chưa: anti-aliasing, filters, Stage3D, PixelBender, gradient LinearRGB.

use std::any::Any;
use std::borrow::Cow;
use std::cell::RefCell;
use std::fmt;
use std::num::NonZeroU32;
use std::sync::Arc;

use ruffle_render::backend::{
    BitmapCacheEntry, Context3D, Context3DProfile, PixelBenderOutput, PixelBenderTarget,
    RenderBackend, ShapeHandle, ShapeHandleImpl, ViewportDimensions,
};
use ruffle_render::bitmap::{
    Bitmap, BitmapHandle, BitmapHandleImpl, BitmapSource, PixelRegion, PixelSnapping, RgbaBufRead,
    SyncHandle,
};
use ruffle_render::commands::{CommandHandler, CommandList, RenderBlendMode};
use ruffle_render::error::Error;
use ruffle_render::matrix::Matrix;
use ruffle_render::pixel_bender::{PixelBenderShader, PixelBenderShaderHandle};
use ruffle_render::pixel_bender_support::PixelBenderShaderArgument;
use ruffle_render::quality::StageQuality;
use ruffle_render::shape_utils::{DistilledShape, GradientType};
use ruffle_render::tessellator::{DrawType, ShapeTessellator};
use ruffle_render::transform::Transform;
use swf::{BlendMode, Color, ColorTransform, GradientSpread};

// ---------------------------------------------------------------- affine
/// x' = a*x + c*y + tx ; y' = b*x + d*y + ty   ([a,b,c,d,tx,ty])
pub(crate) type Aff = [f32; 6];

pub(crate) fn aff_from_matrix(m: &Matrix) -> Aff {
    [m.a, m.b, m.c, m.d, m.tx.to_pixels() as f32, m.ty.to_pixels() as f32]
}
/// Ma trận GL 3x3 column-major (tessellator) -> Aff
pub(crate) fn aff_from_gl(g: &[[f32; 3]; 3]) -> Aff {
    [g[0][0], g[0][1], g[1][0], g[1][1], g[2][0], g[2][1]]
}
/// p ∘ q (áp dụng q trước)
pub(crate) fn aff_mul(p: &Aff, q: &Aff) -> Aff {
    [
        p[0] * q[0] + p[2] * q[1],
        p[1] * q[0] + p[3] * q[1],
        p[0] * q[2] + p[2] * q[3],
        p[1] * q[2] + p[3] * q[3],
        p[0] * q[4] + p[2] * q[5] + p[4],
        p[1] * q[4] + p[3] * q[5] + p[5],
    ]
}
pub(crate) fn aff_inv(m: &Aff) -> Option<Aff> {
    let det = m[0] * m[3] - m[2] * m[1];
    if det.abs() < 1e-12 {
        return None;
    }
    let (a, b, c, d) = (m[3] / det, -m[1] / det, -m[2] / det, m[0] / det);
    Some([a, b, c, d, -(a * m[4] + c * m[5]), -(b * m[4] + d * m[5])])
}
#[inline]
pub(crate) fn aff_apply(m: &Aff, x: f32, y: f32) -> (f32, f32) {
    (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])
}

// ---------------------------------------------------------------- color
#[derive(Clone, Copy)]
struct Ct {
    mul: [f32; 4],
    add: [f32; 4],
    identity: bool,
}
impl Ct {
    fn new(c: &ColorTransform) -> Self {
        let mul = [
            c.r_multiply.to_f32(),
            c.g_multiply.to_f32(),
            c.b_multiply.to_f32(),
            c.a_multiply.to_f32(),
        ];
        let add = [
            c.r_add as f32 / 255.0,
            c.g_add as f32 / 255.0,
            c.b_add as f32 / 255.0,
            c.a_add as f32 / 255.0,
        ];
        let identity = mul == [1.0; 4] && add == [0.0; 4];
        Self { mul, add, identity }
    }
    /// straight -> premultiplied, sau color transform
    #[inline]
    fn apply_straight(&self, c: [f32; 4]) -> [f32; 4] {
        let mut o = [0.0; 4];
        for i in 0..4 {
            o[i] = (c[i] * self.mul[i] + self.add[i]).clamp(0.0, 1.0);
        }
        [o[0] * o[3], o[1] * o[3], o[2] * o[3], o[3]]
    }
    /// premultiplied -> premultiplied
    #[inline]
    fn apply_premul(&self, c: [f32; 4]) -> [f32; 4] {
        if self.identity {
            return c;
        }
        let a = c[3];
        let s = if a > 0.0 {
            [c[0] / a, c[1] / a, c[2] / a, a]
        } else {
            [0.0, 0.0, 0.0, 0.0]
        };
        self.apply_straight(s)
    }
}

pub(crate) fn color_f(c: &Color) -> [f32; 4] {
    [
        c.r as f32 / 255.0,
        c.g as f32 / 255.0,
        c.b as f32 / 255.0,
        c.a as f32 / 255.0,
    ]
}

// ---------------------------------------------------------------- canvas
struct Canvas {
    w: u32,
    h: u32,
    px: Vec<u8>,
}
impl Canvas {
    fn new(w: u32, h: u32) -> Self {
        Self { w, h, px: vec![0; (w as usize) * (h as usize) * 4] }
    }
    fn clear(&mut self, c: [f32; 4]) {
        let p = [
            (c[0] * c[3] * 255.0) as u8,
            (c[1] * c[3] * 255.0) as u8,
            (c[2] * c[3] * 255.0) as u8,
            (c[3] * 255.0) as u8,
        ];
        for px in self.px.chunks_exact_mut(4) {
            px.copy_from_slice(&p);
        }
    }
}

#[inline]
fn load(p: &[u8]) -> [f32; 4] {
    [
        p[0] as f32 * (1.0 / 255.0),
        p[1] as f32 * (1.0 / 255.0),
        p[2] as f32 * (1.0 / 255.0),
        p[3] as f32 * (1.0 / 255.0),
    ]
}
#[inline]
fn store(p: &mut [u8], c: [f32; 4]) {
    for i in 0..4 {
        p[i] = (c[i].clamp(0.0, 1.0) * 255.0 + 0.5) as u8;
    }
}
#[inline]
fn over(dst: &mut [u8], s: [f32; 4]) {
    if s[3] >= 0.999 {
        store(dst, s);
        return;
    }
    if s[3] <= 0.0 && s[0] <= 0.0 && s[1] <= 0.0 && s[2] <= 0.0 {
        return;
    }
    let d = load(dst);
    let k = 1.0 - s[3];
    store(dst, [s[0] + d[0] * k, s[1] + d[1] * k, s[2] + d[2] * k, s[3] + d[3] * k]);
}

fn blend_px(mode: BlendMode, d: [f32; 4], s: [f32; 4]) -> [f32; 4] {
    let (sa, da) = (s[3], d[3]);
    let mut o = [0.0; 4];
    match mode {
        BlendMode::Add => {
            for i in 0..3 {
                o[i] = (s[i] + d[i]).min(1.0);
            }
            o[3] = sa + da * (1.0 - sa);
        }
        BlendMode::Subtract => {
            for i in 0..3 {
                o[i] = (d[i] - s[i]).max(0.0);
            }
            o[3] = da;
        }
        BlendMode::Multiply => {
            for i in 0..3 {
                o[i] = s[i] * d[i] + s[i] * (1.0 - da) + d[i] * (1.0 - sa);
            }
            o[3] = sa + da * (1.0 - sa);
        }
        BlendMode::Screen => {
            for i in 0..3 {
                o[i] = s[i] + d[i] - s[i] * d[i];
            }
            o[3] = sa + da * (1.0 - sa);
        }
        BlendMode::Lighten | BlendMode::Darken | BlendMode::Difference => {
            for i in 0..3 {
                let (x, y) = (s[i] * da, d[i] * sa);
                let base = s[i] * (1.0 - da) + d[i] * (1.0 - sa);
                o[i] = match mode {
                    BlendMode::Lighten => base + x.max(y),
                    BlendMode::Darken => base + x.min(y),
                    _ => s[i] + d[i] - 2.0 * x.min(y),
                };
            }
            o[3] = sa + da * (1.0 - sa);
        }
        BlendMode::Invert => {
            for i in 0..3 {
                o[i] = (da - d[i]) * sa + d[i] * (1.0 - sa);
            }
            o[3] = da;
        }
        BlendMode::Alpha => {
            for i in 0..4 {
                o[i] = d[i] * sa;
            }
        }
        BlendMode::Erase => {
            for i in 0..4 {
                o[i] = d[i] * (1.0 - sa);
            }
        }
        // Normal, Layer, Overlay, HardLight (xấp xỉ bằng normal)
        _ => {
            let k = 1.0 - sa;
            for i in 0..4 {
                o[i] = s[i] + d[i] * k;
            }
        }
    }
    o
}

// ---------------------------------------------------------------- handles
struct SwBitmap {
    w: u32,
    h: u32,
    data: RefCell<Vec<u8>>, // RGBA premultiplied
}
impl fmt::Debug for SwBitmap {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SwBitmap({}x{})", self.w, self.h)
    }
}
impl BitmapHandleImpl for SwBitmap {}

fn as_sw(h: &BitmapHandle) -> Option<&SwBitmap> {
    let any: &dyn Any = &*h.0;
    any.downcast_ref::<SwBitmap>()
}

#[derive(Clone, Copy)]
enum GradKind {
    Linear,
    Radial,
    Focal(f32),
}

enum SwFill {
    Color,
    Gradient {
        uv: Aff, // local -> gradient space
        lut: Box<[[f32; 4]; 256]>,
        kind: GradKind,
        spread: GradientSpread,
    },
    Bitmap {
        uv: Aff, // local -> [0,1]
        handle: Option<BitmapHandle>,
        smooth: bool,
        repeat: bool,
    },
}

struct SwDraw {
    fill: SwFill,
    pos: Vec<[f32; 2]>,
    col: Vec<[f32; 4]>, // straight
    idx: Vec<u32>,
}

struct SwShape {
    draws: Vec<SwDraw>,
}
impl fmt::Debug for SwShape {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SwShape({} draws)", self.draws.len())
    }
}
impl ShapeHandleImpl for SwShape {}

struct SwSync {
    handle: BitmapHandle,
    bounds: PixelRegion,
}
impl fmt::Debug for SwSync {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SwSync")
    }
}
impl SyncHandle for SwSync {}

fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 { c / 12.92 } else { ((c + 0.055) / 1.055).powf(2.4) }
}
fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.0031308 { c * 12.92 } else { 1.055 * c.powf(1.0 / 2.4) - 0.055 }
}

pub(crate) fn build_lut(g: &ruffle_render::tessellator::Gradient) -> Box<[[f32; 4]; 256]> {
    let mut lut = Box::new([[0.0f32; 4]; 256]);
    let recs = &g.records;
    let linear = matches!(g.interpolation, swf::GradientInterpolation::LinearRgb);
    if recs.is_empty() {
        return lut;
    }
    for (i, slot) in lut.iter_mut().enumerate() {
        let r = i as f32;
        let mut c = color_f(&recs[recs.len() - 1].color);
        if r <= recs[0].ratio as f32 {
            c = color_f(&recs[0].color);
        } else {
            for w in recs.windows(2) {
                let (r0, r1) = (w[0].ratio as f32, w[1].ratio as f32);
                if r >= r0 && r <= r1 {
                    let t = if r1 > r0 { (r - r0) / (r1 - r0) } else { 0.0 };
                    let (mut a, mut b) = (color_f(&w[0].color), color_f(&w[1].color));
                    if linear {
                        for k in 0..3 {
                            a[k] = srgb_to_linear(a[k]);
                            b[k] = srgb_to_linear(b[k]);
                        }
                    }
                    for k in 0..4 {
                        c[k] = a[k] + (b[k] - a[k]) * t;
                    }
                    if linear {
                        for k in 0..3 {
                            c[k] = linear_to_srgb(c[k]);
                        }
                    }
                    break;
                }
            }
        }
        *slot = c;
    }
    lut
}

// ---------------------------------------------------------------- rasterizer
/// Gọi `f(x, y, l0, l1, l2)` cho mỗi pixel có tâm nằm trong tam giác (top-left rule nhất quán).
#[inline]
fn raster_tri(w: u32, h: u32, p: [[f32; 2]; 3], mut f: impl FnMut(u32, u32, f32, f32, f32)) {
    let edge = |a: [f32; 2], b: [f32; 2], x: f32, y: f32| (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
    let (p0, mut p1, mut p2) = (p[0], p[1], p[2]);
    let mut area = edge(p0, p1, p2[0], p2[1]);
    let mut swapped = false;
    if area.abs() < 1e-9 || !area.is_finite() {
        return;
    }
    if area < 0.0 {
        std::mem::swap(&mut p1, &mut p2);
        area = -area;
        swapped = true;
    }
    let inc = |a: [f32; 2], b: [f32; 2]| {
        let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
        dy > 0.0 || (dy == 0.0 && dx > 0.0)
    };
    // e0 đối diện p0 (cạnh p1->p2), e1 cạnh p2->p0, e2 cạnh p0->p1
    let (i0, i1, i2) = (inc(p1, p2), inc(p2, p0), inc(p0, p1));
    let minx = p0[0].min(p1[0]).min(p2[0]).floor().max(0.0) as i64;
    let maxx = (p0[0].max(p1[0]).max(p2[0]).ceil() as i64).min(w as i64);
    let miny = p0[1].min(p1[1]).min(p2[1]).floor().max(0.0) as i64;
    let maxy = (p0[1].max(p1[1]).max(p2[1]).ceil() as i64).min(h as i64);
    if minx >= maxx || miny >= maxy {
        return;
    }
    let inv = 1.0 / area;
    // Mỗi cạnh e(x) = ax*x + c(y). Với từng hàng, tính đoạn x mà cả 3 cạnh >= 0 rồi chỉ duyệt đoạn đó
    // (thay vì cả bounding box). Biên ±1 pixel vẫn kiểm tra chính xác bằng quy tắc top-left.
    let edges = [(p1, p2), (p2, p0), (p0, p1)];
    for y in miny..maxy {
        let py = y as f32 + 0.5;
        let (mut lo, mut hi) = (minx as f32, maxx as f32);
        for &(a, b) in &edges {
            let ax = -(b[1] - a[1]);
            let c = (b[0] - a[0]) * (py - a[1]) + (b[1] - a[1]) * a[0];
            if ax > 0.0 {
                lo = lo.max(-c / ax - 0.5);
            } else if ax < 0.0 {
                hi = hi.min(-c / ax - 0.5);
            } else if c < 0.0 {
                hi = lo - 1.0; // cạnh ngang, cả hàng nằm ngoài
            }
        }
        if !(hi >= lo - 1.0) {
            continue;
        }
        let x0 = ((lo.floor() as i64) - 1).max(minx);
        let x1 = ((hi.ceil() as i64) + 2).min(maxx);
        for x in x0..x1 {
            let px = x as f32 + 0.5;
            let e0 = edge(p1, p2, px, py);
            let e1 = edge(p2, p0, px, py);
            let e2 = edge(p0, p1, px, py);
            if (e0 > 0.0 || (e0 == 0.0 && i0)) && (e1 > 0.0 || (e1 == 0.0 && i1)) && (e2 > 0.0 || (e2 == 0.0 && i2)) {
                let (l0, l1, l2) = (e0 * inv, e1 * inv, e2 * inv);
                if swapped {
                    f(x as u32, y as u32, l0, l2, l1);
                } else {
                    f(x as u32, y as u32, l0, l1, l2);
                }
            }
        }
    }
}

#[inline]
fn wrap_t(t: f32, spread: GradientSpread) -> f32 {
    match spread {
        GradientSpread::Reflect => {
            let t = t.abs();
            let i = t.floor();
            let f = t - i;
            if (i as i64) & 1 == 0 { f } else { 1.0 - f }
        }
        GradientSpread::Repeat => t - t.floor(),
        _ => t.clamp(0.0, 1.0),
    }
}

#[inline]
fn texel(data: &[u8], w: u32, h: u32, x: i64, y: i64, repeat: bool) -> [f32; 4] {
    let (x, y) = if repeat {
        (x.rem_euclid(w as i64), y.rem_euclid(h as i64))
    } else {
        (x.clamp(0, w as i64 - 1), y.clamp(0, h as i64 - 1))
    };
    let i = ((y as usize) * (w as usize) + x as usize) * 4;
    match data.get(i..i + 4) {
        Some(p) => load(p),
        None => [0.0; 4],
    }
}

fn sample(bm: &SwBitmap, data: &[u8], u: f32, v: f32, smooth: bool, repeat: bool) -> [f32; 4] {
    let smooth = smooth && !crate::low_quality();
    if bm.w == 0 || bm.h == 0 {
        return [0.0; 4];
    }
    let fx = u * bm.w as f32;
    let fy = v * bm.h as f32;
    if !smooth {
        return texel(data, bm.w, bm.h, fx.floor() as i64, fy.floor() as i64, repeat);
    }
    let (x, y) = (fx - 0.5, fy - 0.5);
    let (x0, y0) = (x.floor(), y.floor());
    let (tx, ty) = (x - x0, y - y0);
    let (x0, y0) = (x0 as i64, y0 as i64);
    // Đường nhanh: cả 4 texel nằm trong ảnh -> đọc trực tiếp, nội suy bằng số nguyên 8-bit
    if x0 >= 0 && y0 >= 0 && x0 + 1 < bm.w as i64 && y0 + 1 < bm.h as i64 {
        let stride = bm.w as usize * 4;
        let i = y0 as usize * stride + x0 as usize * 4;
        if let (Some(r0), Some(r1)) = (data.get(i..i + 8), data.get(i + stride..i + stride + 8)) {
            let fx = (tx * 256.0) as u32;
            let fy = (ty * 256.0) as u32;
            let mut o = [0.0; 4];
            for k in 0..4 {
                let top = r0[k] as u32 * (256 - fx) + r0[k + 4] as u32 * fx;
                let bot = r1[k] as u32 * (256 - fx) + r1[k + 4] as u32 * fx;
                let v = top * (256 - fy) + bot * fy; // thang 255 * 65536
                o[k] = v as f32 * (1.0 / (255.0 * 65536.0));
            }
            return o;
        }
    }
    let a = texel(data, bm.w, bm.h, x0, y0, repeat);
    let b = texel(data, bm.w, bm.h, x0 + 1, y0, repeat);
    let c = texel(data, bm.w, bm.h, x0, y0 + 1, repeat);
    let d = texel(data, bm.w, bm.h, x0 + 1, y0 + 1, repeat);
    let mut o = [0.0; 4];
    for i in 0..4 {
        let top = a[i] + (b[i] - a[i]) * tx;
        let bot = c[i] + (d[i] - c[i]) * tx;
        o[i] = top + (bot - top) * ty;
    }
    o
}

// ---------------------------------------------------------------- render pass
struct Pass<'a> {
    canvas: &'a mut Canvas,
    masks: Vec<Vec<u8>>,
    drawing_mask: bool,
    skip: u32,
    /// Đang ở trong layer (BlendMode::Layer hoặc blend khác) — Alpha/Erase chỉ có tác dụng khi true.
    in_layer: bool,
}

/// Ghi một pixel (premultiplied) vào canvas có xét mask.
#[inline]
fn emit(canvas: &mut Canvas, masks: &mut [Vec<u8>], drawing_mask: bool, idx: usize, s: [f32; 4]) {
    if drawing_mask {
        let n = masks.len();
        if s[3] > 0.0 && (n < 2 || masks[n - 2][idx] > 0) {
            masks[n - 1][idx] = 255;
        }
        return;
    }
    if let Some(m) = masks.last() {
        if m[idx] == 0 {
            return;
        }
    }
    over(&mut canvas.px[idx * 4..idx * 4 + 4], s);
}

impl<'a> Pass<'a> {
    fn new(canvas: &'a mut Canvas) -> Self {
        Self { canvas, masks: Vec::new(), drawing_mask: false, skip: 0, in_layer: false }
    }

    fn run(mut self, commands: CommandList) {
        commands.execute(&mut self);
    }

    fn draw_shape(&mut self, shape: &SwShape, transform: &Transform) {
        if self.skip > 0 {
            return;
        }
        let m = aff_from_matrix(&transform.matrix);
        let inv = aff_inv(&m);
        let ct = Ct::new(&transform.color_transform);
        let (w, h) = (self.canvas.w, self.canvas.h);
        for d in &shape.draws {
            let pts: Vec<[f32; 2]> = d.pos.iter().map(|p| {
                let (x, y) = aff_apply(&m, p[0], p[1]);
                [x, y]
            }).collect();
            match &d.fill {
                SwFill::Color => {
                    let cols: Vec<[f32; 4]> = d.col.iter().map(|c| ct.apply_straight(*c)).collect();
                    for t in d.idx.chunks_exact(3) {
                        let (a, b, c) = (t[0] as usize, t[1] as usize, t[2] as usize);
                        let (ca, cb, cc) = (cols[a], cols[b], cols[c]);
                        let uniform = ca == cb && cb == cc;
                        let (canvas, masks, dm) = (&mut *self.canvas, &mut self.masks, self.drawing_mask);
                        raster_tri(w, h, [pts[a], pts[b], pts[c]], |x, y, l0, l1, l2| {
                            let s = if uniform {
                                ca
                            } else {
                                let mut s = [0.0; 4];
                                for i in 0..4 {
                                    s[i] = ca[i] * l0 + cb[i] * l1 + cc[i] * l2;
                                }
                                s
                            };
                            emit(canvas, masks, dm, (y * w + x) as usize, s);
                        });
                    }
                }
                SwFill::Gradient { uv, lut, kind, spread } => {
                    let Some(inv) = inv else { continue };
                    let g = aff_mul(uv, &inv); // screen -> gradient
                    // áp color transform lên LUT 1 lần
                    let lut: Vec<[f32; 4]> = lut.iter().map(|c| ct.apply_straight(*c)).collect();
                    let (kind, spread) = (*kind, *spread);
                    for t in d.idx.chunks_exact(3) {
                        let tri = [pts[t[0] as usize], pts[t[1] as usize], pts[t[2] as usize]];
                        let (canvas, masks, dm) = (&mut *self.canvas, &mut self.masks, self.drawing_mask);
                        raster_tri(w, h, tri, |x, y, _, _, _| {
                            let (u, v) = aff_apply(&g, x as f32 + 0.5, y as f32 + 0.5);
                            let tt = match kind {
                                GradKind::Linear => u,
                                GradKind::Radial => {
                                    let (a, b) = (u * 2.0 - 1.0, v * 2.0 - 1.0);
                                    (a * a + b * b).sqrt()
                                }
                                GradKind::Focal(fp) => {
                                    let (a, b) = (u * 2.0 - 1.0, v * 2.0 - 1.0);
                                    let (mut dx, mut dy) = (fp - a, -b);
                                    let l = (dx * dx + dy * dy).sqrt();
                                    if l > 0.0 {
                                        dx /= l;
                                        dy /= l;
                                    }
                                    l / ((1.0 - fp * fp * dy * dy).max(0.0).sqrt() + fp * dx)
                                }
                            };
                            let tt = wrap_t(tt, spread);
                            let s = lut[((tt * 255.0 + 0.5) as usize).min(255)];
                            emit(canvas, masks, dm, (y * w + x) as usize, s);
                        });
                    }
                }
                SwFill::Bitmap { uv, handle, smooth, repeat } => {
                    let Some(inv) = inv else { continue };
                    let Some(handle) = handle else { continue };
                    let Some(bm) = as_sw(handle) else { continue };
                    let g = aff_mul(uv, &inv);
                    let data = bm.data.borrow();
                    let (smooth, repeat) = (*smooth, *repeat);
                    for t in d.idx.chunks_exact(3) {
                        let tri = [pts[t[0] as usize], pts[t[1] as usize], pts[t[2] as usize]];
                        let (canvas, masks, dm) = (&mut *self.canvas, &mut self.masks, self.drawing_mask);
                        raster_tri(w, h, tri, |x, y, _, _, _| {
                            let (u, v) = aff_apply(&g, x as f32 + 0.5, y as f32 + 0.5);
                            let s = ct.apply_premul(sample(bm, &data, u, v, smooth, repeat));
                            emit(canvas, masks, dm, (y * w + x) as usize, s);
                        });
                    }
                }
            }
        }
    }

    /// Vẽ hình chữ nhật [0,rw]x[0,rh] (local px) qua `m`, mỗi pixel tô bằng `shade(u,v)` (u,v local px)
    fn draw_quad(&mut self, m: &Aff, rw: f32, rh: f32, mut shade: impl FnMut(f32, f32) -> [f32; 4]) {
        if self.skip > 0 {
            return;
        }
        let Some(inv) = aff_inv(m) else { return };
        let corners = [(0.0, 0.0), (rw, 0.0), (rw, rh), (0.0, rh)].map(|(x, y)| {
            let (a, b) = aff_apply(m, x, y);
            [a, b]
        });
        let (w, h) = (self.canvas.w, self.canvas.h);
        for tri in [[corners[0], corners[1], corners[2]], [corners[0], corners[2], corners[3]]] {
            let (canvas, masks, dm) = (&mut *self.canvas, &mut self.masks, self.drawing_mask);
            raster_tri(w, h, tri, |x, y, _, _, _| {
                let (u, v) = aff_apply(&inv, x as f32 + 0.5, y as f32 + 0.5);
                emit(canvas, masks, dm, (y * w + x) as usize, shade(u, v));
            });
        }
    }

    fn draw_bitmap_region(&mut self, bitmap: &BitmapHandle, transform: &Transform, smooth: bool, region: PixelRegion) {
        let Some(bm) = as_sw(bitmap) else { return };
        let m = aff_from_matrix(&transform.matrix);
        let ct = Ct::new(&transform.color_transform);
        let data = bm.data.borrow();
        let (ox, oy) = (region.x_min as f32, region.y_min as f32);
        let (bw, bh) = (bm.w as f32, bm.h as f32);
        self.draw_quad(&m, region.width() as f32, region.height() as f32, |u, v| {
            ct.apply_premul(sample(bm, &data, (u + ox) / bw, (v + oy) / bh, smooth, false))
        });
    }

    /// Render `commands` vào layer mới cùng kích thước rồi trả về.
    fn render_layer(&self, commands: CommandList, in_layer: bool) -> Canvas {
        let mut layer = Canvas::new(self.canvas.w, self.canvas.h);
        let mut pass = Pass::new(&mut layer);
        pass.in_layer = in_layer;
        pass.run(commands);
        layer
    }

    fn composite(&mut self, layer: &Canvas, mode: BlendMode) {
        if self.skip > 0 {
            return;
        }
        let n = (layer.w * layer.h) as usize;
        for i in 0..n {
            let s = load(&layer.px[i * 4..i * 4 + 4]);
            if self.drawing_mask {
                emit(self.canvas, &mut self.masks, true, i, s);
                continue;
            }
            if let Some(m) = self.masks.last() {
                if m[i] == 0 {
                    continue;
                }
            }
            if s[3] == 0.0 && !matches!(mode, BlendMode::Alpha) {
                continue;
            }
            let dst = &mut self.canvas.px[i * 4..i * 4 + 4];
            let d = load(dst);
            store(dst, blend_px(mode, d, s));
        }
    }
}

impl CommandHandler for Pass<'_> {
    fn render_bitmap(&mut self, bitmap: BitmapHandle, mut transform: Transform, smoothing: bool, pixel_snapping: PixelSnapping, region: PixelRegion) {
        pixel_snapping.apply(&mut transform.matrix);
        self.draw_bitmap_region(&bitmap, &transform, smoothing, region);
    }

    fn render_stage3d(&mut self, bitmap: BitmapHandle, transform: Transform) {
        if let Some(bm) = as_sw(&bitmap) {
            let r = PixelRegion::for_whole_size(bm.w, bm.h);
            self.draw_bitmap_region(&bitmap, &transform, false, r);
        }
    }

    fn render_shape(&mut self, shape: ShapeHandle, transform: Transform) {
        let any: &dyn Any = &*shape.0;
        if let Some(s) = any.downcast_ref::<SwShape>() {
            self.draw_shape(s, &transform);
        }
    }

    fn render_alpha_mask(&mut self, maskee_commands: CommandList, mask_commands: CommandList) {
        if self.skip > 0 {
            return;
        }
        let mut maskee = self.render_layer(maskee_commands, self.in_layer);
        let mask = self.render_layer(mask_commands, false);
        for (p, m) in maskee.px.chunks_exact_mut(4).zip(mask.px.chunks_exact(4)) {
            let a = m[3] as u32;
            for c in p.iter_mut() {
                *c = ((*c as u32 * a + 127) / 255) as u8;
            }
        }
        self.composite(&maskee, BlendMode::Normal);
    }

    fn draw_rect(&mut self, color: Color, matrix: Matrix) {
        let c = Ct::new(&ColorTransform::IDENTITY).apply_straight(color_f(&color));
        let m = aff_from_matrix(&matrix);
        self.draw_quad(&m, 1.0, 1.0, |_, _| c);
    }

    fn draw_line(&mut self, color: Color, matrix: Matrix) {
        ruffle_render::lines::emulate_line(self, color, matrix);
    }

    fn draw_line_rect(&mut self, color: Color, matrix: Matrix) {
        ruffle_render::lines::emulate_line_rect(self, color, matrix);
    }

    fn push_mask(&mut self) {
        let n = (self.canvas.w * self.canvas.h) as usize;
        self.masks.push(vec![0u8; n]);
        self.drawing_mask = true;
    }

    fn activate_mask(&mut self) {
        self.drawing_mask = false;
    }

    fn deactivate_mask(&mut self) {
        self.skip += 1;
    }

    fn pop_mask(&mut self) {
        if self.skip > 0 {
            self.skip -= 1;
        }
        self.masks.pop();
        self.drawing_mask = false;
    }

    fn blend(&mut self, commands: CommandList, blend_mode: RenderBlendMode) {
        if self.skip > 0 {
            return;
        }
        let mut mode = match blend_mode {
            RenderBlendMode::Builtin(m) => m,
            RenderBlendMode::Shader(_) => BlendMode::Normal,
        };
        if matches!(mode, BlendMode::Alpha | BlendMode::Erase) && !self.in_layer {
            mode = BlendMode::Normal;
        }
        if matches!(mode, BlendMode::Normal) && !self.drawing_mask {
            commands.execute(self);
            return;
        }
        let layer = self.render_layer(commands, true);
        self.composite(&layer, mode);
    }
}

// ---------------------------------------------------------------- backend
pub struct SoftwareRenderer {
    dims: ViewportDimensions,
    frame: Canvas,
    tess: ShapeTessellator,
}

impl SoftwareRenderer {
    pub fn new(width: u32, height: u32, scale: f64) -> Self {
        Self {
            dims: ViewportDimensions { width, height, scale_factor: scale },
            frame: Canvas::new(width.max(1), height.max(1)),
            tess: ShapeTessellator::new(),
        }
    }

    pub fn framebuffer(&self) -> (u32, u32, *const u8) {
        (self.frame.w, self.frame.h, self.frame.px.as_ptr())
    }

    /// Render vào bitmap `handle` (lấy data ra làm canvas tạm, rồi trả lại).
    fn render_into_bitmap(&mut self, handle: &BitmapHandle, clear: Option<[f32; 4]>, commands: CommandList) {
        let Some(bm) = as_sw(handle) else { return };
        let px = std::mem::take(&mut *bm.data.borrow_mut());
        let mut canvas = Canvas { w: bm.w, h: bm.h, px };
        if canvas.px.len() != (bm.w * bm.h * 4) as usize {
            canvas.px = vec![0; (bm.w * bm.h * 4) as usize];
        }
        if let Some(c) = clear {
            canvas.clear(c);
        }
        Pass::new(&mut canvas).run(commands);
        *bm.data.borrow_mut() = canvas.px;
    }
}

impl RenderBackend for SoftwareRenderer {
    fn viewport_dimensions(&self) -> ViewportDimensions {
        self.dims
    }

    fn set_viewport_dimensions(&mut self, d: ViewportDimensions) {
        self.dims = d;
        self.frame = Canvas::new(d.width.max(1), d.height.max(1));
    }

    fn register_shape(&mut self, shape: DistilledShape, bitmap_source: &dyn BitmapSource) -> ShapeHandle {
        let mesh = self.tess.tessellate_shape(shape, bitmap_source);
        let luts: Vec<_> = mesh.gradients.iter().map(build_lut).collect();
        let mut draws = Vec::with_capacity(mesh.draws.len());
        for d in mesh.draws {
            let fill = match d.draw_type {
                DrawType::Color => SwFill::Color,
                DrawType::Gradient { matrix, gradient } => {
                    let g = &mesh.gradients[gradient];
                    let kind = match g.gradient_type {
                        GradientType::Linear => GradKind::Linear,
                        GradientType::Radial => GradKind::Radial,
                        GradientType::Focal => GradKind::Focal(g.focal_point.to_f32().clamp(-0.99, 0.99)),
                    };
                    SwFill::Gradient { uv: aff_from_gl(&matrix), lut: luts[gradient].clone(), kind, spread: g.repeat_mode }
                }
                DrawType::Bitmap(b) => SwFill::Bitmap {
                    uv: aff_from_gl(&b.matrix),
                    handle: bitmap_source.bitmap_handle(b.bitmap_id, self),
                    smooth: b.is_smoothed,
                    repeat: b.is_repeating,
                },
            };
            draws.push(SwDraw {
                fill,
                pos: d.vertices.iter().map(|v| [v.x, v.y]).collect(),
                col: d.vertices.iter().map(|v| color_f(&v.color)).collect(),
                idx: d.indices,
            });
        }
        ShapeHandle(Arc::new(SwShape { draws }))
    }

    fn render_offscreen(&mut self, handle: BitmapHandle, commands: CommandList, _quality: StageQuality, bounds: PixelRegion) -> Option<Box<dyn SyncHandle>> {
        self.render_into_bitmap(&handle, None, commands);
        Some(Box::new(SwSync { handle, bounds }))
    }

    fn is_offscreen_supported(&self) -> bool {
        true
    }

    fn submit_frame(&mut self, clear: Color, commands: CommandList, cache_entries: Vec<BitmapCacheEntry>) {
        for entry in cache_entries {
            let c = color_f(&entry.clear);
            self.render_into_bitmap(&entry.handle, Some(c), entry.commands);
            // TODO: entry.filters
        }
        if std::env::var("SW_DEBUG").is_ok() { eprintln!("submit_frame clear={clear:?} cmds={}", commands.commands.len()); }
        self.frame.clear(color_f(&clear));
        Pass::new(&mut self.frame).run(commands);
    }

    fn create_empty_texture(&mut self, width: NonZeroU32, height: NonZeroU32) -> Result<BitmapHandle, Error> {
        let (w, h) = (width.get(), height.get());
        Ok(BitmapHandle(Arc::new(SwBitmap { w, h, data: RefCell::new(vec![0; (w * h * 4) as usize]) })))
    }

    fn register_bitmap(&mut self, bitmap: Bitmap<'_>) -> Result<BitmapHandle, Error> {
        let bitmap = bitmap.to_rgba();
        let (w, h) = (bitmap.width(), bitmap.height());
        Ok(BitmapHandle(Arc::new(SwBitmap { w, h, data: RefCell::new(bitmap.data().to_vec()) })))
    }

    fn update_texture(&mut self, handle: &BitmapHandle, bitmap: Bitmap<'_>, region: PixelRegion) -> Result<(), Error> {
        let Some(bm) = as_sw(handle) else {
            return Err(Error::UnknownHandle(handle.clone()));
        };
        let bitmap = bitmap.to_rgba();
        let src = bitmap.data();
        let sw = bitmap.width() as usize;
        let mut data = bm.data.borrow_mut();
        let dw = bm.w as usize;
        let x0 = region.x_min.min(bm.w) as usize;
        let x1 = region.x_max.min(bm.w).min(bitmap.width()) as usize;
        for y in region.y_min..region.y_max.min(bm.h).min(bitmap.height()) {
            let y = y as usize;
            if x1 <= x0 {
                break;
            }
            let s = &src[(y * sw + x0) * 4..(y * sw + x1) * 4];
            data[(y * dw + x0) * 4..(y * dw + x1) * 4].copy_from_slice(s);
        }
        Ok(())
    }

    fn create_context3d(&mut self, _profile: Context3DProfile) -> Result<Box<dyn Context3D>, Error> {
        Err(Error::Unimplemented("createContext3D (software renderer)".into()))
    }

    fn debug_info(&self) -> Cow<'static, str> {
        Cow::Borrowed("Renderer: UWP software")
    }

    fn name(&self) -> &'static str {
        "uwp-software"
    }

    fn set_quality(&mut self, quality: StageQuality) {
        crate::LOW_QUALITY.store(matches!(quality, StageQuality::Low), std::sync::atomic::Ordering::Relaxed);
    }

    fn compile_pixelbender_shader(&mut self, _shader: PixelBenderShader) -> Result<PixelBenderShaderHandle, Error> {
        Err(Error::Unimplemented("PixelBender (software renderer)".into()))
    }

    fn run_pixelbender_shader(&mut self, _handle: PixelBenderShaderHandle, _arguments: &[PixelBenderShaderArgument], _target: &PixelBenderTarget) -> Result<PixelBenderOutput, Error> {
        Err(Error::Unimplemented("PixelBender (software renderer)".into()))
    }

    fn resolve_sync_handle(&mut self, handle: Box<dyn SyncHandle>, with_rgba: RgbaBufRead) -> Result<(), Error> {
        let any: Box<dyn Any> = handle;
        let Ok(sync) = any.downcast::<SwSync>() else {
            return Err(Error::Unimplemented("foreign sync handle".into()));
        };
        let Some(bm) = as_sw(&sync.handle) else {
            return Err(Error::UnknownHandle(sync.handle.clone()));
        };
        let mut r = sync.bounds;
        r.clamp(bm.w, bm.h);
        let (rw, rh) = (r.width() as usize, r.height() as usize);
        let data = bm.data.borrow();
        let mut out = Vec::with_capacity(rw * rh * 4);
        for y in r.y_min as usize..r.y_min as usize + rh {
            let start = (y * bm.w as usize + r.x_min as usize) * 4;
            out.extend_from_slice(&data[start..start + rw * 4]);
        }
        with_rgba(&out, (rw * 4) as u32);
        Ok(())
    }
}
