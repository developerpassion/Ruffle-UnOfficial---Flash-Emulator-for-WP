#pragma once

namespace RuffleUWP
{
    // Cài đặt app, lưu ở LocalState\settings.json, ví dụ:
    //   {"gpu":true,"msaa":false,"showFps":true,"renderScale":0.75,"quality":"medium","audio":false,"frameLimit":30}
    // Thiếu khoá -> dùng mặc định (msaa thiếu = tự động: bật nếu GPU feature level >= 10_0).
    struct AppSettings
    {
        bool gpu = true;          // renderer GPU (Direct3D 11); false = CPU
        int msaa = -1;            // -1 tự động, 0 tắt, 1 bật
        bool showFps = false;     // FPS overlay
        float renderScale = 0.0f; // độ phân giải vẽ: 0 = tự động, 1 / 0.75 / 0.5
        int quality = 2;          // 0 low, 1 medium, 2 high, 3 best
        bool audio = true;        // false = không giải mã/trộn âm thanh (đỡ CPU)
        int frameLimit = 0;       // 0 = không giới hạn, 30, 20

        static AppSettings Load();
        void Save() const;
    };
}
