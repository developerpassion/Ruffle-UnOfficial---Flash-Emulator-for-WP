#pragma once
// Log ra OutputDebugString + LocalState\log.txt (để debug khi không gắn debugger, vd. trên điện thoại)
void DebugLog(const char* msg);
void DebugLogW(const wchar_t* msg);
// Chép LocalState\log.txt -> Documents\Ruffle\ruffle.log (bất đồng bộ, bỏ qua nếu lần chép trước chưa xong)
void DebugLogCopyToDocuments();
// true nếu có LocalState\debug.flag (log + autoload/frame dump chỉ chạy khi bật)
bool DebugLogEnabled();
// Đăng ký lưu SharedObject (save game) vào LocalState\SharedObjects
void RegisterStorage();
// Cache web cho navigator: LocalState\WebCache + Documents\Ruffle\web (dùng khi không có mạng)
void RegisterWebCache();
