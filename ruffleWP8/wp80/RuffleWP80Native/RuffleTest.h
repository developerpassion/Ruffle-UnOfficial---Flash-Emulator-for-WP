#pragma once

namespace RuffleWP80Native
{
    // M1 (WP 8.0): chạy lõi Rust trên máy và trả kết quả dạng chữ cho app C# hiển thị.
    public ref class RuffleTest sealed
    {
    public:
        static Platform::String^ Run();
        // Chẩn đoán (build /p:Probe=true): nạp từng probe_NN.dll, báo DLL/hàm hệ thống nào thiếu trên máy
        static Platform::String^ Probe();
    };
}
