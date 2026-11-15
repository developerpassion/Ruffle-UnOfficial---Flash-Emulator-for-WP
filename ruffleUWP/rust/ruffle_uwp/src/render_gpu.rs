//! GPU renderer cho Ruffle qua host (C++ / Direct3D 11).
//!
//! Rust tessellate shape (ShapeTessellator của ruffle_render) và duyệt CommandList;
//! mọi việc GPU (texture, mesh, render target, stencil, blend) do host thực hiện qua bảng hàm C
//! `RuffleGpuHost`. Rust không đụng tới D3D/WinRT.
//!
//! Mask = stencil (giống renderer wgpu của Ruffle): push_mask -> vẽ mask với stencil "equal depth, incr",
//! activate -> depth+1, deactivate -> vẽ lại mask với "equal depth, decr", pop -> depth-1.
//! Blend mode khác Normal -> vẽ vào layer (texture tạm) rồi host composite bằng blend state.

use std::any::Any;
use std::borrow::Cow;
use std::ffi::c_void;
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

use crate::render_sw::{aff_from_gl, aff_from_matrix, aff_mul, build_lut, color_f, Aff};

// ---------------------------------------------------------------- C ABI (giữ đồng bộ include/ruffle_uwp.h)

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GpuVertex {
    pub x: f32,
    pub y: f32,
    /// màu straight (chưa premultiply), 0..1
    pub color: [f32; 4],
}

pub const KIND_COLOR: u32 = 0;
pub const KIND_LINEAR: u32 = 1;
pub const KIND_RADIAL: u32 = 2;
pub const KIND_FOCAL: u32 = 3;
pub const KIND_BITMAP: u32 = 4;

pub const MASK_DRAW: u32 = 0; // vẽ bình thường, stencil test == mask_ref
pub const MASK_WRITE: u32 = 1; // ghi mask: test == ref, incr, không ghi màu
pub const MASK_CLEAR: u32 = 2; // xoá mask: test == ref, decr, không ghi màu

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GpuDraw {
    pub mesh: u32,
    pub first_index: u32,
    pub index_count: u32,
    pub kind: u32,
    /// local -> pixel của target: x' = w[0]*x + w[2]*y + w[4], y' = w[1]*x + w[3]*y + w[5]
    pub world: [f32; 6],
    /// local -> uv (gradient space hoặc toạ độ texture 0..1)
    pub uv: [f32; 6],
    pub ct_mul: [f32; 4],
    pub ct_add: [f32; 4],
    /// LUT gradient (256x1, straight) hoặc bitmap (premultiplied)
    pub texture: u32,
    /// bitmap: 1 = lọc tuyến tính
    pub smooth: u32,
    /// bitmap: 1 = lặp; gradient: 0 pad, 1 reflect, 2 repeat
    pub wrap: u32,
    pub focal: f32,
    pub mask_mode: u32,
    pub mask_ref: u32,
}

pub const COMPOSITE_NORMAL: u32 = 0;
pub const COMPOSITE_ADD: u32 = 1;
pub const COMPOSITE_SUBTRACT: u32 = 2;
pub const COMPOSITE_MULTIPLY: u32 = 3;
pub const COMPOSITE_SCREEN: u32 = 4;
pub const COMPOSITE_LIGHTEN: u32 = 5;
pub const COMPOSITE_DARKEN: u32 = 6;
pub const COMPOSITE_ALPHA: u32 = 7;
pub const COMPOSITE_ERASE: u32 = 8;
pub const COMPOSITE_INVERT: u32 = 9;

pub type HostCreateTexture = unsafe extern "C" fn(ctx: *mut c_void, w: u32, h: u32, rgba: *const u8) -> u32;
pub type HostUpdateTexture =
    unsafe extern "C" fn(ctx: *mut c_void, tex: u32, x: u32, y: u32, w: u32, h: u32, rgba: *const u8, stride: u32);
pub type HostReadTexture = unsafe extern "C" fn(ctx: *mut c_void, tex: u32, x: u32, y: u32, w: u32, h: u32, out: *mut u8);
pub type HostDestroy = unsafe extern "C" fn(ctx: *mut c_void, id: u32);
pub type HostCreateMesh =
    unsafe extern "C" fn(ctx: *mut c_void, v: *const GpuVertex, nv: u32, idx: *const u32, ni: u32) -> u32;
pub type HostBeginTarget = unsafe extern "C" fn(ctx: *mut c_void, tex: u32, clear: i32, rgba: *const f32);
pub type HostEndTarget = unsafe extern "C" fn(ctx: *mut c_void);
pub type HostDraw = unsafe extern "C" fn(ctx: *mut c_void, d: *const GpuDraw);
pub type HostCreateLayer = unsafe extern "C" fn(ctx: *mut c_void) -> u32;
pub type HostComposite =
    unsafe extern "C" fn(ctx: *mut c_void, layer: u32, mode: u32, mask_layer: u32, mask_ref: u32);

/// Bảng hàm host cung cấp. Mọi hàm gọi trên thread đang gọi ruffle_* (host tự đảm bảo tuần tự).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RuffleGpuHost {
    pub ctx: *mut c_void,
    /// Tạo texture RGBA8 (dùng được làm render target). `rgba` null = trong suốt. Trả 0 nếu lỗi.
    pub create_texture: HostCreateTexture,
    pub update_texture: HostUpdateTexture,
    pub read_texture: HostReadTexture,
    pub destroy_texture: HostDestroy,
    pub create_mesh: HostCreateMesh,
    pub destroy_mesh: HostDestroy,
    /// tex = 0: màn hình. Đẩy target lên chồng.
    pub begin_target: HostBeginTarget,
    /// Bỏ target khỏi chồng (màn hình: resolve MSAA).
    pub end_target: HostEndTarget,
    pub draw: HostDraw,
    /// Texture tạm cỡ target hiện tại (huỷ bằng destroy_texture).
    pub create_layer: HostCreateLayer,
    /// Vẽ layer lên target hiện tại với blend `mode`; `mask_layer` != 0: nhân alpha của mask.
    pub composite: HostComposite,
}

#[derive(Clone)]
struct Host(Arc<RuffleGpuHost>);
// Host chỉ dùng trên thread của player (C++ khoá mutex); handle cần Send/Sync? không — chỉ Any + Debug.

impl Host {
    fn tex(&self, w: u32, h: u32, rgba: Option<&[u8]>) -> u32 {
        unsafe { (self.0.create_texture)(self.0.ctx, w, h, rgba.map_or(std::ptr::null(), |d| d.as_ptr())) }
    }
    fn draw(&self, d: &GpuDraw) {
        unsafe { (self.0.draw)(self.0.ctx, d) }
    }
    fn begin(&self, tex: u32, clear: Option<[f32; 4]>) {
        let c = clear.unwrap_or([0.0; 4]);
        unsafe { (self.0.begin_target)(self.0.ctx, tex, clear.is_some() as i32, c.as_ptr()) }
    }
    fn end(&self) {
        unsafe { (self.0.end_target)(self.0.ctx) }
    }
}

// ---------------------------------------------------------------- handles

struct GpuTexture {
    host: Host,
    id: u32,
    w: u32,
    h: u32,
}
impl Drop for GpuTexture {
    fn drop(&mut self) {
        if self.id != 0 {
            unsafe { (self.host.0.destroy_texture)(self.host.0.ctx, self.id) }
        }
    }
}
impl fmt::Debug for GpuTexture {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "GpuTexture#{}({}x{})", self.id, self.w, self.h)
    }
}
impl BitmapHandleImpl for GpuTexture {}

fn as_gpu(h: &BitmapHandle) -> Option<&GpuTexture> {
    let any: &dyn Any = &*h.0;
    any.downcast_ref::<GpuTexture>()
}

enum GFill {
    Color,
    Gradient { uv: Aff, lut: Arc<GpuTexture>, kind: u32, focal: f32, wrap: u32 },
    Bitmap { uv: Aff, handle: Option<BitmapHandle>, smooth: bool, repeat: bool },
}

struct GDraw {
    fill: GFill,
    first: u32,
    count: u32,
}

struct GpuShape {
    host: Host,
    mesh: u32,
    draws: Vec<GDraw>,
}
impl Drop for GpuShape {
    fn drop(&mut self) {
        if self.mesh != 0 {
            unsafe { (self.host.0.destroy_mesh)(self.host.0.ctx, self.mesh) }
        }
    }
}
impl fmt::Debug for GpuShape {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "GpuShape(mesh {}, {} draws)", self.mesh, self.draws.len())
    }
}
impl ShapeHandleImpl for GpuShape {}

struct GpuSync {
    handle: BitmapHandle,
    bounds: PixelRegion,
}
impl fmt::Debug for GpuSync {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "GpuSync")
    }
}
impl SyncHandle for GpuSync {}

fn ct_vecs(c: &ColorTransform) -> ([f32; 4], [f32; 4]) {
    (
        [c.r_multiply.to_f32(), c.g_multiply.to_f32(), c.b_multiply.to_f32(), c.a_multiply.to_f32()],
        [
            c.r_add as f32 / 255.0,
            c.g_add as f32 / 255.0,
            c.b_add as f32 / 255.0,
            c.a_add as f32 / 255.0,
        ],
    )
}

/// x,y (0..1 của quad) -> scale (sx, sy)
fn aff_scale(sx: f32, sy: f32) -> Aff {
    [sx, 0.0, 0.0, sy, 0.0, 0.0]
}

// ---------------------------------------------------------------- render pass

#[derive(Clone, Copy, PartialEq)]
enum MaskState {
    Draw,
    Write,
    Clear,
}

struct Pass<'a> {
    r: &'a GpuRenderer,
    depth: u32,
    state: MaskState,
    in_layer: bool,
}

impl<'a> Pass<'a> {
    fn new(r: &'a GpuRenderer, in_layer: bool) -> Self {
        Self { r, depth: 0, state: MaskState::Draw, in_layer }
    }

    fn mask_fields(&self) -> (u32, u32) {
        match self.state {
            MaskState::Draw => (MASK_DRAW, self.depth),
            MaskState::Write => (MASK_WRITE, self.depth),
            MaskState::Clear => (MASK_CLEAR, self.depth),
        }
    }

    fn base(&self, mesh: u32, first: u32, count: u32, world: Aff, ct: &ColorTransform) -> GpuDraw {
        let (ct_mul, ct_add) = ct_vecs(ct);
        let (mask_mode, mask_ref) = self.mask_fields();
        GpuDraw {
            mesh,
            first_index: first,
            index_count: count,
            kind: KIND_COLOR,
            world,
            uv: [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
            ct_mul,
            ct_add,
            texture: 0,
            smooth: 0,
            wrap: 0,
            focal: 0.0,
            mask_mode,
            mask_ref,
        }
    }

    fn draw_shape(&mut self, shape: &GpuShape, transform: &Transform) {
        let world = aff_from_matrix(&transform.matrix);
        for d in &shape.draws {
            let mut g = self.base(shape.mesh, d.first, d.count, world, &transform.color_transform);
            match &d.fill {
                GFill::Color => {}
                GFill::Gradient { uv, lut, kind, focal, wrap } => {
                    g.kind = *kind;
                    g.uv = *uv;
                    g.texture = lut.id;
                    g.focal = *focal;
                    g.wrap = *wrap;
                    g.smooth = 1;
                }
                GFill::Bitmap { uv, handle, smooth, repeat } => {
                    let Some(t) = handle.as_ref().and_then(as_gpu) else { continue };
                    g.kind = KIND_BITMAP;
                    g.uv = *uv;
                    g.texture = t.id;
                    g.smooth = (*smooth && !crate::low_quality()) as u32;
                    g.wrap = *repeat as u32;
                }
            }
            self.r.host.draw(&g);
        }
    }

    fn draw_bitmap_region(&mut self, bitmap: &BitmapHandle, transform: &Transform, smooth: bool, region: PixelRegion) {
        let Some(t) = as_gpu(bitmap) else { return };
        let (rw, rh) = (region.width() as f32, region.height() as f32);
        if rw <= 0.0 || rh <= 0.0 || t.w == 0 || t.h == 0 {
            return;
        }
        let m = aff_from_matrix(&transform.matrix);
        let world = aff_mul(&m, &aff_scale(rw, rh));
        let mut g = self.base(self.r.quad_mesh, 0, 6, world, &transform.color_transform);
        g.kind = KIND_BITMAP;
        // quad 0..1 -> uv của vùng region trong texture
        let (bw, bh) = (t.w as f32, t.h as f32);
        g.uv = [rw / bw, 0.0, 0.0, rh / bh, region.x_min as f32 / bw, region.y_min as f32 / bh];
        g.texture = t.id;
        g.smooth = (smooth && !crate::low_quality()) as u32;
        self.r.host.draw(&g);
    }

    /// Render `commands` vào layer mới (cỡ target hiện tại), trả id texture.
    fn render_layer(&self, commands: CommandList, in_layer: bool) -> u32 {
        let host = &self.r.host;
        let layer = unsafe { (host.0.create_layer)(host.0.ctx) };
        if layer == 0 {
            return 0;
        }
        host.begin(layer, Some([0.0; 4]));
        let mut p = Pass::new(self.r, in_layer);
        commands.execute(&mut p);
        host.end();
        layer
    }

    fn composite(&self, layer: u32, mode: u32, mask_layer: u32) {
        let host = &self.r.host;
        if layer != 0 {
            unsafe { (host.0.composite)(host.0.ctx, layer, mode, mask_layer, self.depth) };
            unsafe { (host.0.destroy_texture)(host.0.ctx, layer) };
        }
        if mask_layer != 0 {
            unsafe { (host.0.destroy_texture)(host.0.ctx, mask_layer) };
        }
    }
}

fn composite_mode(m: BlendMode) -> u32 {
    match m {
        BlendMode::Add => COMPOSITE_ADD,
        BlendMode::Subtract => COMPOSITE_SUBTRACT,
        BlendMode::Multiply => COMPOSITE_MULTIPLY,
        BlendMode::Screen => COMPOSITE_SCREEN,
        BlendMode::Lighten => COMPOSITE_LIGHTEN,
        BlendMode::Darken => COMPOSITE_DARKEN,
        BlendMode::Alpha => COMPOSITE_ALPHA,
        BlendMode::Erase => COMPOSITE_ERASE,
        BlendMode::Invert => COMPOSITE_INVERT,
        // Overlay/HardLight/Difference: chưa có -> Normal
        _ => COMPOSITE_NORMAL,
    }
}

impl CommandHandler for Pass<'_> {
    fn render_bitmap(&mut self, bitmap: BitmapHandle, mut transform: Transform, smoothing: bool, pixel_snapping: PixelSnapping, region: PixelRegion) {
        pixel_snapping.apply(&mut transform.matrix);
        self.draw_bitmap_region(&bitmap, &transform, smoothing, region);
    }

    fn render_stage3d(&mut self, bitmap: BitmapHandle, transform: Transform) {
        if let Some(t) = as_gpu(&bitmap) {
            let r = PixelRegion::for_whole_size(t.w, t.h);
            self.draw_bitmap_region(&bitmap, &transform, false, r);
        }
    }

    fn render_shape(&mut self, shape: ShapeHandle, transform: Transform) {
        let any: &dyn Any = &*shape.0;
        if let Some(s) = any.downcast_ref::<GpuShape>() {
            self.draw_shape(s, &transform);
        }
    }

    fn render_alpha_mask(&mut self, maskee_commands: CommandList, mask_commands: CommandList) {
        if self.state != MaskState::Draw {
            maskee_commands.execute(self);
            return;
        }
        let maskee = self.render_layer(maskee_commands, self.in_layer);
        let mask = self.render_layer(mask_commands, false);
        self.composite(maskee, COMPOSITE_NORMAL, mask);
    }

    fn draw_rect(&mut self, color: Color, matrix: Matrix) {
        let world = aff_from_matrix(&matrix);
        let mut g = self.base(self.r.quad_mesh, 0, 6, world, &ColorTransform::IDENTITY);
        g.ct_mul = color_f(&color); // quad màu trắng * màu
        self.r.host.draw(&g);
    }

    fn draw_line(&mut self, color: Color, matrix: Matrix) {
        ruffle_render::lines::emulate_line(self, color, matrix);
    }

    fn draw_line_rect(&mut self, color: Color, matrix: Matrix) {
        ruffle_render::lines::emulate_line_rect(self, color, matrix);
    }

    fn push_mask(&mut self) {
        self.state = MaskState::Write;
    }

    fn activate_mask(&mut self) {
        self.depth += 1;
        self.state = MaskState::Draw;
    }

    fn deactivate_mask(&mut self) {
        self.state = MaskState::Clear;
    }

    fn pop_mask(&mut self) {
        self.depth = self.depth.saturating_sub(1);
        self.state = MaskState::Draw;
    }

    fn blend(&mut self, commands: CommandList, blend_mode: RenderBlendMode) {
        let mut mode = match blend_mode {
            RenderBlendMode::Builtin(m) => m,
            RenderBlendMode::Shader(_) => BlendMode::Normal,
        };
        // Alpha/Erase chỉ có tác dụng trong layer (giống Flash)
        if matches!(mode, BlendMode::Alpha | BlendMode::Erase) && !self.in_layer {
            mode = BlendMode::Normal;
        }
        let cm = composite_mode(mode);
        if self.state != MaskState::Draw || (cm == COMPOSITE_NORMAL && !matches!(mode, BlendMode::Layer)) {
            commands.execute(self);
            return;
        }
        let layer = self.render_layer(commands, true);
        self.composite(layer, cm, 0);
    }
}

// ---------------------------------------------------------------- backend

pub struct GpuRenderer {
    host: Host,
    dims: ViewportDimensions,
    tess: ShapeTessellator,
    quad_mesh: u32,
}

impl GpuRenderer {
    /// # Safety
    /// Các con trỏ hàm trong `host` phải hợp lệ suốt đời renderer (và mọi handle nó tạo).
    pub unsafe fn new(host: RuffleGpuHost, width: u32, height: u32, scale: f64) -> Self {
        let host = Host(Arc::new(host));
        let white = [1.0f32; 4];
        let v = [
            GpuVertex { x: 0.0, y: 0.0, color: white },
            GpuVertex { x: 1.0, y: 0.0, color: white },
            GpuVertex { x: 1.0, y: 1.0, color: white },
            GpuVertex { x: 0.0, y: 1.0, color: white },
        ];
        let idx = [0u32, 1, 2, 0, 2, 3];
        let quad_mesh = unsafe { (host.0.create_mesh)(host.0.ctx, v.as_ptr(), 4, idx.as_ptr(), 6) };
        Self {
            host,
            dims: ViewportDimensions { width, height, scale_factor: scale },
            tess: ShapeTessellator::new(),
            quad_mesh,
        }
    }

    fn texture_handle(&self, w: u32, h: u32, rgba: Option<&[u8]>) -> Result<BitmapHandle, Error> {
        let id = self.host.tex(w, h, rgba);
        if id == 0 {
            return Err(Error::Unimplemented(format!("GPU texture {w}x{h} failed").into()));
        }
        Ok(BitmapHandle(Arc::new(GpuTexture { host: self.host.clone(), id, w, h })))
    }

    fn render_into(&self, handle: &BitmapHandle, clear: Option<[f32; 4]>, commands: CommandList) {
        let Some(t) = as_gpu(handle) else { return };
        self.host.begin(t.id, clear);
        let mut p = Pass::new(self, false);
        commands.execute(&mut p);
        self.host.end();
    }
}

impl Drop for GpuRenderer {
    fn drop(&mut self) {
        if self.quad_mesh != 0 {
            unsafe { (self.host.0.destroy_mesh)(self.host.0.ctx, self.quad_mesh) }
        }
    }
}

impl RenderBackend for GpuRenderer {
    fn viewport_dimensions(&self) -> ViewportDimensions {
        self.dims
    }

    fn set_viewport_dimensions(&mut self, d: ViewportDimensions) {
        self.dims = d;
    }

    fn register_shape(&mut self, shape: DistilledShape, bitmap_source: &dyn BitmapSource) -> ShapeHandle {
        let mesh = self.tess.tessellate_shape(shape, bitmap_source);
        // LUT gradient -> texture 256x1 (straight RGBA8)
        let luts: Vec<Arc<GpuTexture>> = mesh
            .gradients
            .iter()
            .map(|g| {
                let lut = build_lut(g);
                let mut px = Vec::with_capacity(256 * 4);
                for c in lut.iter() {
                    for k in 0..4 {
                        px.push((c[k].clamp(0.0, 1.0) * 255.0 + 0.5) as u8);
                    }
                }
                let id = self.host.tex(256, 1, Some(&px));
                Arc::new(GpuTexture { host: self.host.clone(), id, w: 256, h: 1 })
            })
            .collect();

        let mut verts: Vec<GpuVertex> = Vec::new();
        let mut idx: Vec<u32> = Vec::new();
        let mut draws = Vec::with_capacity(mesh.draws.len());
        for d in mesh.draws {
            let base = verts.len() as u32;
            let first = idx.len() as u32;
            verts.extend(d.vertices.iter().map(|v| GpuVertex { x: v.x, y: v.y, color: color_f(&v.color) }));
            idx.extend(d.indices.iter().map(|i| i + base));
            let fill = match d.draw_type {
                DrawType::Color => GFill::Color,
                DrawType::Gradient { matrix, gradient } => {
                    let g = &mesh.gradients[gradient];
                    let (kind, focal) = match g.gradient_type {
                        GradientType::Linear => (KIND_LINEAR, 0.0),
                        GradientType::Radial => (KIND_RADIAL, 0.0),
                        GradientType::Focal => (KIND_FOCAL, g.focal_point.to_f32().clamp(-0.99, 0.99)),
                    };
                    let wrap = match g.repeat_mode {
                        GradientSpread::Reflect => 1,
                        GradientSpread::Repeat => 2,
                        _ => 0,
                    };
                    GFill::Gradient { uv: aff_from_gl(&matrix), lut: luts[gradient].clone(), kind, focal, wrap }
                }
                DrawType::Bitmap(b) => GFill::Bitmap {
                    uv: aff_from_gl(&b.matrix),
                    handle: bitmap_source.bitmap_handle(b.bitmap_id, self),
                    smooth: b.is_smoothed,
                    repeat: b.is_repeating,
                },
            };
            draws.push(GDraw { fill, first, count: d.indices.len() as u32 });
        }
        let mesh_id = if idx.is_empty() {
            0
        } else {
            unsafe {
                (self.host.0.create_mesh)(self.host.0.ctx, verts.as_ptr(), verts.len() as u32, idx.as_ptr(), idx.len() as u32)
            }
        };
        if mesh_id == 0 {
            draws.clear();
        }
        ShapeHandle(Arc::new(GpuShape { host: self.host.clone(), mesh: mesh_id, draws }))
    }

    fn render_offscreen(&mut self, handle: BitmapHandle, commands: CommandList, _quality: StageQuality, bounds: PixelRegion) -> Option<Box<dyn SyncHandle>> {
        self.render_into(&handle, None, commands);
        Some(Box::new(GpuSync { handle, bounds }))
    }

    fn is_offscreen_supported(&self) -> bool {
        true
    }

    fn submit_frame(&mut self, clear: Color, commands: CommandList, cache_entries: Vec<BitmapCacheEntry>) {
        for entry in cache_entries {
            self.render_into(&entry.handle, Some(color_f(&entry.clear)), entry.commands);
            // TODO: entry.filters
        }
        let c = color_f(&clear);
        // màu clear -> premultiplied
        self.host.begin(0, Some([c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]]));
        let mut p = Pass::new(self, false);
        commands.execute(&mut p);
        self.host.end();
    }

    fn create_empty_texture(&mut self, width: NonZeroU32, height: NonZeroU32) -> Result<BitmapHandle, Error> {
        self.texture_handle(width.get(), height.get(), None)
    }

    fn register_bitmap(&mut self, bitmap: Bitmap<'_>) -> Result<BitmapHandle, Error> {
        let bitmap = bitmap.to_rgba();
        self.texture_handle(bitmap.width(), bitmap.height(), Some(bitmap.data()))
    }

    fn update_texture(&mut self, handle: &BitmapHandle, bitmap: Bitmap<'_>, region: PixelRegion) -> Result<(), Error> {
        let Some(t) = as_gpu(handle) else {
            return Err(Error::UnknownHandle(handle.clone()));
        };
        let bitmap = bitmap.to_rgba();
        let mut r = region;
        r.clamp(t.w.min(bitmap.width()), t.h.min(bitmap.height()));
        if r.width() == 0 || r.height() == 0 {
            return Ok(());
        }
        let stride = bitmap.width() * 4;
        let off = (r.y_min * stride + r.x_min * 4) as usize;
        let data = &bitmap.data()[off..];
        unsafe {
            (self.host.0.update_texture)(self.host.0.ctx, t.id, r.x_min, r.y_min, r.width(), r.height(), data.as_ptr(), stride)
        };
        Ok(())
    }

    fn create_context3d(&mut self, _profile: Context3DProfile) -> Result<Box<dyn Context3D>, Error> {
        Err(Error::Unimplemented("createContext3D (UWP GPU renderer)".into()))
    }

    fn debug_info(&self) -> Cow<'static, str> {
        Cow::Borrowed("Renderer: UWP Direct3D 11")
    }

    fn name(&self) -> &'static str {
        "uwp-d3d11"
    }

    fn set_quality(&mut self, quality: StageQuality) {
        crate::LOW_QUALITY.store(matches!(quality, StageQuality::Low), std::sync::atomic::Ordering::Relaxed);
    }

    fn compile_pixelbender_shader(&mut self, _shader: PixelBenderShader) -> Result<PixelBenderShaderHandle, Error> {
        Err(Error::Unimplemented("PixelBender (UWP GPU renderer)".into()))
    }

    fn run_pixelbender_shader(&mut self, _handle: PixelBenderShaderHandle, _arguments: &[PixelBenderShaderArgument], _target: &PixelBenderTarget) -> Result<PixelBenderOutput, Error> {
        Err(Error::Unimplemented("PixelBender (UWP GPU renderer)".into()))
    }

    fn resolve_sync_handle(&mut self, handle: Box<dyn SyncHandle>, with_rgba: RgbaBufRead) -> Result<(), Error> {
        let any: Box<dyn Any> = handle;
        let Ok(sync) = any.downcast::<GpuSync>() else {
            return Err(Error::Unimplemented("foreign sync handle".into()));
        };
        let Some(t) = as_gpu(&sync.handle) else {
            return Err(Error::UnknownHandle(sync.handle.clone()));
        };
        let mut r = sync.bounds;
        r.clamp(t.w, t.h);
        let (rw, rh) = (r.width(), r.height());
        let mut out = vec![0u8; (rw * rh * 4) as usize];
        if rw > 0 && rh > 0 {
            unsafe { (self.host.0.read_texture)(self.host.0.ctx, t.id, r.x_min, r.y_min, rw, rh, out.as_mut_ptr()) };
        }
        with_rgba(&out, rw * 4);
        Ok(())
    }
}
