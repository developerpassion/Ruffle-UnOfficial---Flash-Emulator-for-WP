use std::ffi::{c_char, c_void, CString};

use ruffle_core::backend::log::LogBackend;

/// Host nhận chuỗi UTF-8 kết thúc '\0' (vd. gọi OutputDebugStringA).
pub type RuffleLogCallback = extern "C" fn(ctx: *mut c_void, msg: *const c_char);

pub struct HostLog {
    cb: Option<RuffleLogCallback>,
    ctx: *mut c_void,
}

impl HostLog {
    pub fn new(cb: Option<RuffleLogCallback>, ctx: *mut c_void) -> Self {
        Self { cb, ctx }
    }
}

impl LogBackend for HostLog {
    fn avm_trace(&self, message: &str) {
        self.send("trace", message);
    }

    fn avm_warning(&self, message: &str) {
        self.send("warn", message);
    }
}

impl HostLog {
    fn send(&self, tag: &str, message: &str) {
        if let Some(cb) = self.cb {
            if let Ok(s) = CString::new(format!("[{tag}] {}\n", message.replace('\0', ""))) {
                cb(self.ctx, s.as_ptr());
            }
        }
    }
}
