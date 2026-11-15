#include "AudioOutput.h"
#include "ruffle_uwp.h"
#include <cstdio>

using namespace RuffleWP80Native;

AudioOutput::AudioOutput(FillFn fill)
    : m_fill(fill), m_master(nullptr), m_voice(nullptr), m_initHr(S_OK), m_event(nullptr), m_paused(false)
{
    m_running = true;
    m_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    HRESULT hr = XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
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
        char msg[64];
        sprintf_s(msg, "XAudio2 init failed 0x%08X\n", (unsigned)m_initHr);
        OutputDebugStringA(msg);
        m_voice = nullptr;
        return;
    }
    for (int i = 0; i < kBuffers; ++i) m_bufs[i].assign(kFrames * 2, 0.0f);
    m_voice->Start(0);
    m_thread = std::thread([this] { Loop(); });
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

// Dừng engine = không còn xin PCM -> Ruffle không giải mã/trộn tiếng (đỡ CPU) khi tắt tiếng, ở danh sách, hoặc app bị treo
void AudioOutput::SetPaused(bool paused)
{
    if (!m_xaudio || paused == m_paused) return;
    m_paused = paused;
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
        while (m_running && st.BuffersQueued < (UINT32)kBuffers)
        {
            std::vector<float>& b = m_bufs[next];
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
