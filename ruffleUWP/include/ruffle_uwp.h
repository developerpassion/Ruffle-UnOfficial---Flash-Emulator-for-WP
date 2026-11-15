// C ABI của rust/ruffle_uwp — giữ đồng bộ với src/lib.rs
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RuffleHandle RuffleHandle;
typedef void (*RuffleLogCallback)(void* ctx, const char* msg);

RuffleHandle* ruffle_create(const uint8_t* data, size_t len, uint32_t width, uint32_t height,
                            double scale, RuffleLogCallback log_cb, void* log_ctx);
void ruffle_destroy(RuffleHandle* h);

/* ---- Lưu SharedObject (save game). Giữ đồng bộ src/storage.rs ---- */
typedef struct RuffleStorageHost {
    void* ctx;
    /* đọc vào buf (tối đa cap); trả kích thước thật hoặc -1 nếu không có; buf null = hỏi kích thước */
    int64_t (*get)(void* ctx, const char* name, uint8_t* buf, size_t cap);
    int32_t (*put)(void* ctx, const char* name, const uint8_t* data, size_t len); /* 1 = OK */
    void (*remove)(void* ctx, const char* name);
} RuffleStorageHost;
/* Áp dụng cho player tạo sau đó; null = lưu trong RAM */
void ruffle_set_storage(const RuffleStorageHost* host);
/* Cache web của navigator (cùng kiểu bảng hàm, key = URL); có thể gọi từ thread tải. null = tắt */
void ruffle_set_web_cache(const RuffleStorageHost* host);

/* ---- GPU renderer: Rust tessellate + duyệt lệnh, host (D3D11) thực thi. Giữ đồng bộ src/render_gpu.rs ---- */
typedef struct RuffleGpuVertex { float x, y; float color[4]; /* straight */ } RuffleGpuVertex;
enum { RUFFLE_KIND_COLOR = 0, RUFFLE_KIND_LINEAR = 1, RUFFLE_KIND_RADIAL = 2, RUFFLE_KIND_FOCAL = 3, RUFFLE_KIND_BITMAP = 4 };
enum { RUFFLE_MASK_DRAW = 0, RUFFLE_MASK_WRITE = 1, RUFFLE_MASK_CLEAR = 2 };
enum { RUFFLE_COMPOSITE_NORMAL = 0, RUFFLE_COMPOSITE_ADD, RUFFLE_COMPOSITE_SUBTRACT, RUFFLE_COMPOSITE_MULTIPLY,
       RUFFLE_COMPOSITE_SCREEN, RUFFLE_COMPOSITE_LIGHTEN, RUFFLE_COMPOSITE_DARKEN, RUFFLE_COMPOSITE_ALPHA,
       RUFFLE_COMPOSITE_ERASE, RUFFLE_COMPOSITE_INVERT };
typedef struct RuffleGpuDraw {
    uint32_t mesh, first_index, index_count, kind;
    float world[6];   /* local -> pixel: x' = w0*x + w2*y + w4, y' = w1*x + w3*y + w5 */
    float uv[6];      /* local -> uv */
    float ct_mul[4], ct_add[4];
    uint32_t texture, smooth, wrap; /* wrap: bitmap 1=lặp; gradient 0 pad 1 reflect 2 repeat */
    float focal;
    uint32_t mask_mode, mask_ref;
} RuffleGpuDraw;
typedef struct RuffleGpuHost {
    void* ctx;
    uint32_t (*create_texture)(void* ctx, uint32_t w, uint32_t h, const uint8_t* rgba /* null = trong suốt */);
    void (*update_texture)(void* ctx, uint32_t tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride);
    void (*read_texture)(void* ctx, uint32_t tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out);
    void (*destroy_texture)(void* ctx, uint32_t tex);
    uint32_t (*create_mesh)(void* ctx, const RuffleGpuVertex* v, uint32_t nv, const uint32_t* idx, uint32_t ni);
    void (*destroy_mesh)(void* ctx, uint32_t mesh);
    void (*begin_target)(void* ctx, uint32_t tex /* 0 = màn hình */, int32_t clear, const float* rgba /* premul */);
    void (*end_target)(void* ctx);
    void (*draw)(void* ctx, const RuffleGpuDraw* d);
    uint32_t (*create_layer)(void* ctx);
    void (*composite)(void* ctx, uint32_t layer, uint32_t mode, uint32_t mask_layer, uint32_t mask_ref);
} RuffleGpuHost;
/* Như ruffle_create nhưng vẽ bằng GPU. host được copy; các hàm của nó phải sống lâu hơn player. */
RuffleHandle* ruffle_create_gpu(const uint8_t* data, size_t len, uint32_t width, uint32_t height,
                                double scale, RuffleLogCallback log_cb, void* log_ctx, const RuffleGpuHost* host);
/* trả 1 nếu framebuffer vừa được vẽ lại */
int32_t ruffle_tick(RuffleHandle* h, double dt_ms);
/* vẽ lại khung hình hiện tại (không tick), vd. sau khi đổi zoom */
void ruffle_render(RuffleHandle* h);
/* Settings: tắt âm thanh cho player tạo sau đó (NullAudio, đỡ CPU); chất lượng 0 Low 1 Medium 2 High 3 Best */
void ruffle_set_audio_enabled(int32_t enabled);
void ruffle_set_quality(RuffleHandle* h, int32_t quality);
void ruffle_resize(RuffleHandle* h, uint32_t width, uint32_t height, double scale);
const uint8_t* ruffle_get_framebuffer(RuffleHandle* h, uint32_t* out_w, uint32_t* out_h, uint32_t* out_stride);
/* kind: 0=move 1=down 2=up */
void ruffle_mouse(RuffleHandle* h, int32_t kind, double x, double y);
/* kind 1=down 2=up; button 0=trái 1=phải 2=giữa. Trả 1 nếu SWF xử lý (khi đó không mở context menu). */
int32_t ruffle_mouse_button(RuffleHandle* h, int32_t kind, int32_t button, double x, double y);
/* lines > 0 = cuộn lên */
void ruffle_mouse_wheel(RuffleHandle* h, double lines);
typedef void (*RuffleMenuItemCallback)(void* ctx, uint32_t index, const char* caption_utf8,
                                       int32_t enabled, int32_t checked, int32_t separator_before);
/* Chuẩn bị context menu, gọi cb cho từng mục, trả số mục (0 = không hiện) */
uint32_t ruffle_context_menu(RuffleHandle* h, RuffleMenuItemCallback cb, void* ctx);
void ruffle_context_menu_select(RuffleHandle* h, uint32_t index);
/* Gọi khi menu đóng (dù có chọn hay không) */
void ruffle_context_menu_closed(RuffleHandle* h, double x, double y);
/* vk = Windows VirtualKey; mods bit0=Shift bit1=Ctrl; down 1/0 */
void ruffle_key(RuffleHandle* h, uint32_t vk, uint32_t mods, int32_t down);
/* ký tự text (UTF-32) */
void ruffle_char(RuffleHandle* h, uint32_t codepoint);
void ruffle_set_playing(RuffleHandle* h, int32_t playing);
uint32_t ruffle_audio_sample_rate(void);
/* stereo f32 interleaved, out có frames*2 phần tử. Gọi từ thread audio. */
void ruffle_audio_fill(RuffleHandle* h, float* out, uint32_t frames);

#ifdef __cplusplus
}
#endif
