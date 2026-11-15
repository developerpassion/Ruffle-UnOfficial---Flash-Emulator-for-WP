#pragma once
#include <xaudio2.h>

namespace RuffleUWP
{
    // XAudio2 source voice (stereo f32). Thread riêng kéo PCM qua hàm fill, giữ vài buffer trong hàng đợi.
    class AudioOutput : public IXAudio2VoiceCallback
    {
    public:
        using FillFn = std::function<void(float* out, uint32_t frames)>;
        explicit AudioOutput(FillFn fill);
        ~AudioOutput();
        bool Ok() const { return m_voice != nullptr; }
        HRESULT InitResult() const { return m_initHr; } // lỗi khởi tạo (để hiện lên màn hình)
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
        IXAudio2MasteringVoice* m_master = nullptr;
        IXAudio2SourceVoice* m_voice = nullptr;
        HRESULT m_initHr = S_OK;
        HANDLE m_event = nullptr;
        std::vector<float> m_bufs[kBuffers];
        std::thread m_thread;
        std::atomic<bool> m_running{ true };
    };
}
