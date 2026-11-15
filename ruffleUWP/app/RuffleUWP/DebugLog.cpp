#include "pch.h"
#include "DebugLog.h"
#include <share.h>
#include <unordered_map>
#include <direct.h>
#include "ruffle_uwp.h"

static std::mutex g_logLock;
static FILE* g_log = nullptr;

// Log mặc định TẮT. Bật bằng cách tạo file LocalState\debug.flag (nội dung tuỳ ý).
// Khi bật: ghi log.txt, chép ra Documents\Ruffleuffle.log, cho phép autoload.swf + frame.bmp.
static int g_enabled = -1; // -1 = chưa kiểm tra

static std::wstring LocalPath(const wchar_t* name)
{
    return std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\" + name;
}

bool DebugLogEnabled()
{
    if (g_enabled < 0)
    {
        FILE* f = nullptr;
        g_enabled = (_wfopen_s(&f, LocalPath(L"debug.flag").c_str(), L"rb") == 0 && f) ? 1 : 0;
        if (f) fclose(f);
    }
    return g_enabled == 1;
}

static FILE* LogFile()
{
    if (!g_log)
    {
        std::wstring p = std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\log.txt";
        g_log = _wfsopen(p.c_str(), L"w", _SH_DENYNO); // cho phép đọc log khi app đang chạy
    }
    return g_log;
}

// Cùng một nội dung chỉ ghi tối đa kMaxRepeat lần (vd. "[warn] Failed to place object" lặp hàng nghìn lần
// làm chậm game vì mỗi dòng là một lần ghi + fflush). Dòng FPS/Resize có số khác nhau nên không bị ảnh hưởng.
static const int kMaxRepeat = 20;
static std::unordered_map<std::string, int> g_seen;

void DebugLog(const char* msg)
{
    if (!DebugLogEnabled()) return;
    std::lock_guard<std::mutex> g(g_logLock);
    int n = ++g_seen[msg];
    if (g_seen.size() > 4096) g_seen.clear();
    if (n > kMaxRepeat) return;
    OutputDebugStringA(msg);
    if (FILE* f = LogFile())
    {
        fputs(msg, f);
        if (n == kMaxRepeat) fputs("  ^ (dòng trên lặp lại nhiều lần, ẩn các lần sau)\n", f);
        fflush(f);
    }
}

void DebugLogW(const wchar_t* msg)
{
    if (!DebugLogEnabled()) return;
    OutputDebugStringW(msg);
    std::lock_guard<std::mutex> g(g_logLock);
    if (FILE* f = LogFile()) { fputws(msg, f); fflush(f); }
}

static std::atomic<bool> g_copying{ false };

void DebugLogCopyToDocuments()
{
    using namespace Windows::Storage;
    if (!DebugLogEnabled()) return;
    if (g_copying.exchange(true)) return;
    try
    {
        auto local = ApplicationData::Current->LocalFolder;
        concurrency::create_task(KnownFolders::DocumentsLibrary->CreateFolderAsync("Ruffle", CreationCollisionOption::OpenIfExists))
            .then([local](StorageFolder^ docs) {
                return concurrency::create_task(local->GetFileAsync("log.txt")).then([docs](StorageFile^ log) {
                    return log->CopyAsync(docs, "ruffle.log", NameCollisionOption::ReplaceExisting);
                });
            })
            .then([](concurrency::task<StorageFile^> t) {
                try { t.get(); }
                catch (Platform::Exception^ ex) { OutputDebugStringW((L"Copy log failed: " + std::wstring(ex->Message->Data()) + L"\n").c_str()); }
                g_copying = false;
            });
    }
    catch (Platform::Exception^) { g_copying = false; }
}

// ---------------------------------------------------------------- SharedObject storage
// Mỗi key -> một file LocalState\SharedObjects\<key đã mã hoá>.sol
static std::wstring SolPath(const char* name)
{
    std::wstring dir = LocalPath(L"SharedObjects");
    _wmkdir(dir.c_str());
    std::wstring file;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
    {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            file += (wchar_t)c;
        else
        {
            wchar_t hex[4];
            swprintf_s(hex, L"%%%02X", c);
            file += hex;
        }
    }
    return dir + L"\\" + file + L".sol";
}

static int64_t StorageGet(void*, const char* name, uint8_t* buf, size_t cap)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, SolPath(name).c_str(), L"rb") != 0 || !f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    int64_t result = size;
    if (buf) result = (int64_t)fread(buf, 1, (std::min)((size_t)size, cap), f);
    fclose(f);
    return result;
}

static int32_t StoragePut(void*, const char* name, const uint8_t* data, size_t len)
{
    // Ghi file tạm rồi thay thế, tránh hỏng save nếu app bị tắt giữa chừng
    std::wstring path = SolPath(name), tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return 0;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (!ok) { _wremove(tmp.c_str()); return 0; }
    _wremove(path.c_str());
    return _wrename(tmp.c_str(), path.c_str()) == 0 ? 1 : 0;
}

static void StorageRemove(void*, const char* name)
{
    _wremove(SolPath(name).c_str());
}

void RegisterStorage()
{
    static RuffleStorageHost host = { nullptr, &StorageGet, &StoragePut, &StorageRemove };
    ruffle_set_storage(&host);
}

// ---------------------------------------------------------------- web cache (navigator offline)
// get: LocalState\WebCache\<url mã hoá>.bin, sau đó Documents\Ruffle\web\<host>\<path>
// put: LocalState\WebCache; nếu là .swf thì chép thêm ra Documents\Ruffle\web\... (để chép sang máy không có mạng)
// Có thể chạy trên thread tải của Rust (không phải UI thread) -> dùng .get() trên task là được.

static std::wstring EncodeName(const char* name)
{
    std::wstring file;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
    {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            file += (wchar_t)c;
        else
        {
            wchar_t hex[4];
            swprintf_s(hex, L"%%%02X", c);
            file += hex;
        }
    }
    return file;
}

static std::wstring CachePath(const char* url)
{
    std::wstring dir = LocalPath(L"WebCache");
    _wmkdir(dir.c_str());
    return dir + L"\\" + EncodeName(url) + L".bin";
}

// "http://host/a/b.swf?x" -> L"Ruffle\web\host\a\b.swf"
static std::wstring DocsRelPath(const char* url)
{
    std::string u(url);
    size_t p = u.find("://");
    if (p != std::string::npos) u = u.substr(p + 3);
    size_t q = u.find_first_of("?#");
    if (q != std::string::npos) u = u.substr(0, q);
    std::wstring w(L"Ruffle\\web\\");
    for (char c : u)
    {
        if (c == '/') w += L'\\';
        else if (c == ':' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') w += L'_';
        else w += (wchar_t)(unsigned char)c;
    }
    return w;
}

static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size : 0);
    size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(got);
    return true;
}

static bool ReadFromDocuments(const char* url, std::vector<uint8_t>& out)
{
    using namespace Windows::Storage;
    try
    {
        auto rel = ref new Platform::String(DocsRelPath(url).c_str());
        StorageFile^ file = concurrency::create_task(KnownFolders::DocumentsLibrary->GetFileAsync(rel)).get();
        auto buf = concurrency::create_task(FileIO::ReadBufferAsync(file)).get();
        out.resize(buf->Length);
        if (!out.empty())
            Streams::DataReader::FromBuffer(buf)->ReadBytes(Platform::ArrayReference<uint8_t>(out.data(), (unsigned)out.size()));
        return true;
    }
    catch (Platform::Exception^) { return false; }
}

static int64_t WebGet(void*, const char* url, uint8_t* buf, size_t cap)
{
    std::vector<uint8_t> data;
    if (!ReadWholeFile(CachePath(url), data) && !ReadFromDocuments(url, data)) return -1;
    if (buf) memcpy(buf, data.data(), (std::min)(cap, data.size()));
    return buf ? (int64_t)(std::min)(cap, data.size()) : (int64_t)data.size();
}

static int32_t WebPut(void*, const char* url, const uint8_t* data, size_t len)
{
    using namespace Windows::Storage;
    FILE* f = nullptr;
    std::wstring path = CachePath(url);
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return 0;
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    // .swf: chép thêm ra Documents\Ruffle\web\... (documentsLibrary chỉ cho các loại file đã khai báo)
    std::wstring rel = DocsRelPath(url);
    if (rel.size() > 4 && _wcsicmp(rel.c_str() + rel.size() - 4, L".swf") == 0)
    {
        try
        {
            size_t slash = rel.find_last_of(L'\\');
            auto folder = concurrency::create_task(KnownFolders::DocumentsLibrary->CreateFolderAsync(
                ref new Platform::String(rel.substr(0, slash).c_str()), CreationCollisionOption::OpenIfExists)).get();
            auto file = concurrency::create_task(folder->CreateFileAsync(
                ref new Platform::String(rel.substr(slash + 1).c_str()), CreationCollisionOption::ReplaceExisting)).get();
            auto bytes = ref new Platform::Array<uint8_t>((unsigned)len);
            memcpy(bytes->Data, data, len);
            concurrency::create_task(FileIO::WriteBytesAsync(file, bytes)).get();
        }
        catch (Platform::Exception^) {}
    }
    return ok ? 1 : 0;
}

static void WebRemove(void*, const char* url) { _wremove(CachePath(url).c_str()); }

void RegisterWebCache()
{
    static RuffleStorageHost host = { nullptr, &WebGet, &WebPut, &WebRemove };
    ruffle_set_web_cache(&host);
}
