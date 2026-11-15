#include "pch.h"
#include "AudioOutput.h"
#include "ruffle_uwp.h"

using namespace RuffleUWP;

AudioOutput::AudioOutput(FillFn fill) : m_fill(std::move(fill))
{
    m_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
#ifdef RUFFLE_NO_XAUDIO
    HRESULT hr = E_NOTIMPL; // build thử không phụ thuộc XAudio2_9.dll (msbuild /p:NoXAudio=true)
#else
    HRESULT hr = XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
#endif
    if (SUCCEEDED(hr)) hr = m_xaudio->CreateMasteringVoice(&m_master);
    if (SUCCEEDED(hr))
    {
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = ruffle_audio_sample_rate();
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
        hr = m_xaudio->CreateSourceVoice(&m_voice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
    }
    if (FAILED(hr) || !m_voice)
    {
        m_initHr = FAILED(hr) ? hr : E_FAIL;
        char msg[64]; sprintf_s(msg, "XAudio2 init failed 0x%08X\n", (unsigned)hr);
        DebugLog(msg);
        m_voice = nullptr;
        return;
    }
    for (auto& b : m_bufs) b.assign(kFrames * 2, 0.0f);
    m_voice->Start(0);
    m_thread = std::thread([this] { Loop(); });
    DebugLog("XAudio2 OK\n");
}

AudioOutput::~AudioOutput()
{
    m_running = false;
    if (m_event) SetEvent(m_event);
    if (m_thread.joinable()) m_thread.join();
    if (m_voice) { m_voice->Stop(0); m_voice->DestroyVoice(); }
    if (m_master) m_master->DestroyVoice();
    m_xaudio.Reset();
    if (m_event) CloseHandle(m_event);
}

void AudioOutput::SetPaused(bool paused)
{
    if (!m_xaudio) return;
    if (paused) m_xaudio->StopEngine();
    else m_xaudio->StartEngine();
}

void AudioOutput::Loop()
{
    int next = 0;
    while (m_running)
    {
        XAUDIO2_VOICE_STATE st;
        m_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (m_running && st.BuffersQueued < kBuffers)
        {
            auto& b = m_bufs[next];
            m_fill(b.data(), kFrames);
            XAUDIO2_BUFFER xb = {};
            xb.AudioBytes = (UINT32)(b.size() * sizeof(float));
            xb.pAudioData = reinterpret_cast<const BYTE*>(b.data());
            if (FAILED(m_voice->SubmitSourceBuffer(&xb))) break;
            next = (next + 1) % kBuffers;
            ++st.BuffersQueued;
        }
        WaitForSingleObjectEx(m_event, 100, FALSE);
    }
}
