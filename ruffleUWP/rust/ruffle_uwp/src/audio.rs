//! Audio: AudioMixer của core. Host (XAudio2) kéo PCM stereo f32 qua `ruffle_audio_fill`.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use ruffle_core::backend::audio::{
    AudioBackend, AudioMixer, AudioMixerProxy, DecodeError, RegisterError, SoundHandle,
    SoundInstanceHandle, SoundStreamInfo, SoundTransform, swf,
};
use ruffle_core::impl_audio_mixer_backend;

pub const SAMPLE_RATE: u32 = 44100;

pub struct UwpAudio {
    mixer: AudioMixer,
    playing: Arc<AtomicBool>,
}

/// Phần chia sẻ với thread audio của host (không cần khoá Player).
#[derive(Clone)]
pub struct AudioOut {
    proxy: Arc<AudioMixerProxy>,
    playing: Arc<AtomicBool>,
}

impl UwpAudio {
    pub fn new() -> (Self, AudioOut) {
        let mixer = AudioMixer::new(2, SAMPLE_RATE);
        let playing = Arc::new(AtomicBool::new(true));
        let out = AudioOut { proxy: Arc::new(mixer.proxy()), playing: playing.clone() };
        (Self { mixer, playing }, out)
    }
}

impl AudioOut {
    /// `buf`: stereo interleaved, len = frames*2.
    pub fn fill(&self, buf: &mut [f32]) {
        if self.playing.load(Ordering::Relaxed) {
            self.proxy.mix::<f32>(buf);
        } else {
            buf.fill(0.0);
        }
    }
}

impl AudioBackend for UwpAudio {
    impl_audio_mixer_backend!(mixer);

    fn play(&mut self) {
        self.playing.store(true, Ordering::Relaxed);
    }

    fn pause(&mut self) {
        self.playing.store(false, Ordering::Relaxed);
    }
}
