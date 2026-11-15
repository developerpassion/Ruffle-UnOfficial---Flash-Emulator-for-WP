//! SharedObject (save game) lưu qua host (C++ ghi file trong LocalState\SharedObjects).
//! Trước đây dùng MemoryStorageBackend -> tắt app là mất save.
//! Rust std::fs không dùng được trên UWP ở đây (CreateFileW bị stub), nên host làm I/O.

use std::ffi::{c_char, c_void, CString};
use std::sync::Mutex;

use ruffle_core::backend::storage::{MemoryStorageBackend, StorageBackend};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RuffleStorageHost {
    pub ctx: *mut c_void,
    /// Đọc key `name` vào `buf` (tối đa `cap` byte). Trả kích thước thật, hoặc -1 nếu không có.
    /// Gọi với buf = null để hỏi kích thước.
    pub get: unsafe extern "C" fn(ctx: *mut c_void, name: *const c_char, buf: *mut u8, cap: usize) -> i64,
    /// Ghi; trả 1 nếu thành công.
    pub put: unsafe extern "C" fn(ctx: *mut c_void, name: *const c_char, data: *const u8, len: usize) -> i32,
    pub remove: unsafe extern "C" fn(ctx: *mut c_void, name: *const c_char),
}

// Con trỏ ctx do host quản lý; chỉ dùng trên thread của player.
unsafe impl Send for RuffleStorageHost {}

static HOST: Mutex<Option<RuffleStorageHost>> = Mutex::new(None);

pub fn set_host(h: Option<RuffleStorageHost>) {
    *HOST.lock().unwrap() = h;
}

/// Backend cho player mới: host nếu đã đăng ký, không thì bộ nhớ.
pub fn backend() -> Box<dyn StorageBackend> {
    match *HOST.lock().unwrap() {
        Some(h) => Box::new(HostStorage(h)),
        None => Box::new(MemoryStorageBackend::new()),
    }
}

struct HostStorage(RuffleStorageHost);

impl StorageBackend for HostStorage {
    fn get(&self, name: &str) -> Option<Vec<u8>> {
        let n = CString::new(name).ok()?;
        let h = &self.0;
        let size = unsafe { (h.get)(h.ctx, n.as_ptr(), std::ptr::null_mut(), 0) };
        if size < 0 {
            return None;
        }
        let mut buf = vec![0u8; size as usize];
        let got = unsafe { (h.get)(h.ctx, n.as_ptr(), buf.as_mut_ptr(), buf.len()) };
        if got < 0 {
            return None;
        }
        buf.truncate(got as usize);
        Some(buf)
    }

    fn put(&mut self, name: &str, value: &[u8]) -> bool {
        let Ok(n) = CString::new(name) else { return false };
        let h = &self.0;
        unsafe { (h.put)(h.ctx, n.as_ptr(), value.as_ptr(), value.len()) != 0 }
    }

    fn remove_key(&mut self, name: &str) {
        if let Ok(n) = CString::new(name) {
            let h = &self.0;
            unsafe { (h.remove)(h.ctx, n.as_ptr()) }
        }
    }
}

// ---------------------------------------------------------------- web cache (dùng cùng kiểu bảng hàm)
// Khi không có mạng, navigator lấy file đã tải trước đó (LocalState\WebCache) hoặc file người dùng chép vào
// Documents\Ruffle\web\<host>\<path>. Key = URL đầy đủ. Hàm có thể bị gọi từ thread tải (không phải thread player).

static WEB: Mutex<Option<RuffleStorageHost>> = Mutex::new(None);

pub fn set_web_host(h: Option<RuffleStorageHost>) {
    *WEB.lock().unwrap() = h;
}

pub fn web_get(url: &str) -> Option<Vec<u8>> {
    let h = (*WEB.lock().unwrap())?;
    HostStorage(h).get(url)
}

pub fn web_put(url: &str, data: &[u8]) {
    if let Some(h) = *WEB.lock().unwrap() {
        HostStorage(h).put(url, data);
    }
}
