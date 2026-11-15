#pragma once
// Âm thanh cho WP 8.0 / Silverlight 8.1: port từ ruffleUWP\app\RuffleUWP\AudioOutput (XAudio2 2.8 có trên WP 8.0).
// Viết lại cho VS2012: không khởi tạo thành viên trong class.
#include <windows.h>
#include <xaudio2.h>
#include <wrl.h>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>

namespace RuffleWP80Native
{
    // XAudio2 source voice (stereo f32). Thread riêng kéo PCM qua hàm fill, giữ vài buffer trong hàng đợi.
    class AudioOutput : public IXAudio2VoiceCallback
    {
    public:
        typedef std::function<void(float* out, uint32_t frames)> FillFn;
        explicit AudioOutput(FillFn fill);
        ~AudioOutput();
        bool Ok() const { return m_voice != nullptr; }
        HRESULT InitResult() const { return m_initHr; }
        void SetPaused(bool paused);

        // IXAudio2VoiceCallback
        void STDMETHODCALLTYPE OnBufferEnd(void*) override { SetEvent(m_event); }
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}

    private:
        void Loop();
        static const int kBuffers = 4;
        static const uint32_t kFrames = 1024; // ~23 ms @ 44.1 kHz

        FillFn m_fill;
        Microsoft::WRL::ComPtr<IXAudio2> m_xaudio;
        IXAudio2MasteringVoice* m_master;
        IXAudio2SourceVoice* m_voice;
        HRESULT m_initHr;
        HANDLE m_event;
        bool m_paused;
        std::vector<float> m_bufs[kBuffers];
        std::thread m_thread;
        std::atomic<bool> m_running;
    };
}
