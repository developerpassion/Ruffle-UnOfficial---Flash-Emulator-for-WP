//! Navigator cho UWP.
//! - Giữ executor để chạy các tác vụ bất đồng bộ của player (Loader.loadBytes, URLLoader, Sound.load...).
//!   (`NullNavigatorBackend::new()` tự tạo executor rồi bỏ đi -> future không bao giờ chạy.)
//! - Tải được URL `http://` bằng TcpStream (thread riêng, kết quả trả về qua channel).
//!   Nhiều game nạp thêm swf qua mạng, vd. Kingdom Rush nạp Armor Games API
//!   (http://agi.armorgames.com/assets/agi/AGI2.swf) sau khi bấm PLAY; tải lỗi -> đứng ở màn hình xám.
//!   `https://` chưa hỗ trợ (cần TLS) -> trả lỗi như trước.

use std::borrow::Cow;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::time::Duration;

use async_channel::{Receiver, Sender};
use encoding_rs::Encoding;
use ruffle_core::backend::navigator::{
    ErrorResponse, NavigationMethod, NavigatorBackend, NullExecutor, NullNavigatorBackend, NullSpawner,
    OwnedFuture, Request, SuccessResponse,
};
use ruffle_core::indexmap::IndexMap;
use ruffle_core::loader::Error;
use ruffle_core::socket::{SocketAction, SocketHandle};
use url::{ParseError, Url};

pub struct UwpNavigator {
    inner: NullNavigatorBackend,
    spawner: NullSpawner,
}

impl UwpNavigator {
    pub fn new(executor: &NullExecutor) -> Self {
        Self { inner: NullNavigatorBackend::new(), spawner: executor.spawner() }
    }
}

// ---------------------------------------------------------------- HTTP tối giản

struct HttpResult {
    url: String,
    status: u16,
    redirected: bool,
    body: Vec<u8>,
}

fn http_request(method: NavigationMethod, url: &str, body: Option<(Vec<u8>, String)>, headers: &IndexMap<String, String>) -> Result<HttpResult, String> {
    let mut url = Url::parse(url).map_err(|e| e.to_string())?;
    let mut redirected = false;
    let (mut method, mut body) = (method, body);
    for _ in 0..5 {
        if url.scheme() != "http" {
            return Err(format!("unsupported scheme {} (chỉ hỗ trợ http)", url.scheme()));
        }
        let host = url.host_str().ok_or("no host")?.to_string();
        let port = url.port().unwrap_or(80);
        let mut path = url.path().to_string();
        if let Some(q) = url.query() {
            path.push('?');
            path.push_str(q);
        }
        let mut stream = TcpStream::connect((host.as_str(), port)).map_err(|e| e.to_string())?;
        stream.set_read_timeout(Some(Duration::from_secs(30))).ok();
        let verb = if matches!(method, NavigationMethod::Post) { "POST" } else { "GET" };
        let mut req = format!("{verb} {path} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\nUser-Agent: RuffleUWP\r\nAccept: */*\r\n");
        for (k, v) in headers {
            req.push_str(&format!("{k}: {v}\r\n"));
        }
        if let Some((data, mime)) = &body {
            req.push_str(&format!("Content-Type: {mime}\r\nContent-Length: {}\r\n", data.len()));
        }
        req.push_str("\r\n");
        stream.write_all(req.as_bytes()).map_err(|e| e.to_string())?;
        if let Some((data, _)) = &body {
            stream.write_all(data).map_err(|e| e.to_string())?;
        }

        let mut r = BufReader::new(stream);
        let mut line = String::new();
        r.read_line(&mut line).map_err(|e| e.to_string())?;
        let status: u16 = line.split_whitespace().nth(1).and_then(|s| s.parse().ok()).ok_or("bad status line")?;
        let (mut len, mut chunked, mut location) = (None::<usize>, false, None::<String>);
        loop {
            line.clear();
            if r.read_line(&mut line).map_err(|e| e.to_string())? == 0 {
                break;
            }
            let l = line.trim_end();
            if l.is_empty() {
                break;
            }
            if let Some((k, v)) = l.split_once(':') {
                let (k, v) = (k.trim().to_ascii_lowercase(), v.trim());
                match k.as_str() {
                    "content-length" => len = v.parse().ok(),
                    "transfer-encoding" => chunked = v.eq_ignore_ascii_case("chunked"),
                    "location" => location = Some(v.to_string()),
                    _ => {}
                }
            }
        }
        if (300..400).contains(&status) {
            if let Some(loc) = location {
                url = url.join(&loc).map_err(|e| e.to_string())?;
                redirected = true;
                if status == 303 {
                    method = NavigationMethod::Get;
                    body = None;
                }
                continue;
            }
        }
        let mut data = Vec::new();
        if chunked {
            loop {
                line.clear();
                r.read_line(&mut line).map_err(|e| e.to_string())?;
                let n = usize::from_str_radix(line.trim().split(';').next().unwrap_or("0"), 16).map_err(|e| e.to_string())?;
                if n == 0 {
                    break;
                }
                let start = data.len();
                data.resize(start + n, 0);
                r.read_exact(&mut data[start..]).map_err(|e| e.to_string())?;
                line.clear();
                r.read_line(&mut line).ok(); // CRLF sau chunk
            }
        } else if let Some(n) = len {
            data.resize(n, 0);
            r.read_exact(&mut data).map_err(|e| e.to_string())?;
        } else {
            r.read_to_end(&mut data).map_err(|e| e.to_string())?;
        }
        return Ok(HttpResult { url: url.to_string(), status, redirected, body: data });
    }
    Err("too many redirects".into())
}

struct HttpResponse {
    url: String,
    status: u16,
    redirected: bool,
    body: Option<Vec<u8>>,
}

impl SuccessResponse for HttpResponse {
    fn url(&self) -> Cow<'_, str> {
        Cow::Borrowed(&self.url)
    }
    fn set_url(&mut self, url: String) {
        self.url = url;
    }
    fn body(self: Box<Self>) -> OwnedFuture<Vec<u8>, Error> {
        let b = self.body.unwrap_or_default();
        Box::pin(async move { Ok(b) })
    }
    fn text_encoding(&self) -> Option<&'static Encoding> {
        None
    }
    fn status(&self) -> u16 {
        self.status
    }
    fn redirected(&self) -> bool {
        self.redirected
    }
    fn next_chunk(&mut self) -> OwnedFuture<Option<Vec<u8>>, Error> {
        let b = self.body.take();
        Box::pin(async move { Ok(b) })
    }
    fn expected_length(&self) -> Result<Option<u64>, Error> {
        Ok(self.body.as_ref().map(|b| b.len() as u64))
    }
}

impl NavigatorBackend for UwpNavigator {
    fn navigate_to_url(&self, url: &str, target: &str, vars_method: Option<(NavigationMethod, IndexMap<String, String>)>) {
        self.inner.navigate_to_url(url, target, vars_method)
    }

    fn fetch(&self, request: Request) -> OwnedFuture<Box<dyn SuccessResponse>, ErrorResponse> {
        let url = request.url().to_string();
        if !url.starts_with("http://") && !url.starts_with("https://") {
            return self.inner.fetch(request);
        }
        // Tải trên thread riêng để không chặn game; future chờ kết quả qua channel.
        // Có mạng (http): tải rồi lưu cache. Không mạng / https / lỗi: lấy từ cache web (nếu có).
        // RUFFLE_OFFLINE=1: giả lập máy không có mạng (test trên PC).
        let (tx, rx) = async_channel::bounded(1);
        let (method, body, headers) = (request.method(), request.body().clone(), request.headers().clone());
        let thread_url = url.clone();
        std::thread::spawn(move || {
            let is_get = matches!(method, NavigationMethod::Get);
            let online = thread_url.starts_with("http://") && std::env::var_os("RUFFLE_OFFLINE").is_none();
            let mut result = if online {
                http_request(method, &thread_url, body, &headers)
            } else {
                Err("offline".to_string())
            };
            match &result {
                Ok(r) if is_get && (200..300).contains(&r.status) => crate::storage::web_put(&thread_url, &r.body),
                Ok(_) => {}
                Err(_) if is_get => {
                    if let Some(data) = crate::storage::web_get(&thread_url) {
                        result = Ok(HttpResult { url: thread_url.clone(), status: 200, redirected: false, body: data });
                    }
                }
                Err(_) => {}
            }
            let _ = tx.send_blocking(result);
        });
        Box::pin(async move {
            let result = rx.recv().await.map_err(|e| e.to_string()).and_then(|r| r);
            match result {
                Ok(r) if (200..300).contains(&r.status) => {
                    let resp: Box<dyn SuccessResponse> =
                        Box::new(HttpResponse { url: r.url, status: r.status, redirected: r.redirected, body: Some(r.body) });
                    Ok(resp)
                }
                Ok(r) => Err(ErrorResponse {
                    url: r.url.clone(),
                    error: Error::HttpNotOk(format!("HTTP {} {}", r.status, r.url), r.status, r.redirected, r.body.len() as u64),
                }),
                Err(e) => Err(ErrorResponse { url: url.clone(), error: Error::FetchError(format!("{url}: {e}")) }),
            }
        })
    }

    fn resolve_url(&self, url: &str) -> Result<Url, ParseError> {
        self.inner.resolve_url(url)
    }

    fn spawn_future(&mut self, future: OwnedFuture<(), Error>) {
        self.spawner.spawn_local(future);
    }

    fn pre_process_url(&self, url: Url) -> Url {
        self.inner.pre_process_url(url)
    }

    fn connect_socket(&mut self, host: String, port: u16, timeout: Duration, handle: SocketHandle, receiver: Receiver<Vec<u8>>, sender: Sender<SocketAction>) {
        self.inner.connect_socket(host, port, timeout, handle, receiver, sender)
    }
}
