#include "audio/audio_player.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>

// Platform-specific headers
#ifdef _WIN32
    #include <windows.h>
    #include <mmsystem.h>
    #include <mmreg.h>
    #pragma comment(lib, "winmm.lib")
#else
    // Linux - use ALSA if available
    #ifdef HAS_ALSA
        #include <alsa/asoundlib.h>
    #endif
    #include <pthread.h>
    #include <unistd.h>
#endif

namespace libswf {

// ADPCM decode tables
static const int ADPCM_INDEX_TABLE_2BIT[] = { -1, 2, -1, 2 };
static const int ADPCM_INDEX_TABLE_3BIT[] = { -1, -1, 2, 4, -1, -1, 2, 4 };
static const int ADPCM_INDEX_TABLE_4BIT[] = { -1, -1, -1, -1, 2, 4, 6, 8, 
                                               -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ADPCM_INDEX_TABLE_5BIT[] = { -1, -1, -1, -1, -1, -1, -1, -1,
                                               1, 2, 4, 6, 8, 10, 13, 16,
                                              -1, -1, -1, -1, -1, -1, -1, -1,
                                               1, 2, 4, 6, 8, 10, 13, 16 };

static const int ADPCM_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

// Platform-specific implementation
class AudioPlayer::Impl {
public:
#ifdef _WIN32
    HWAVEOUT hWaveOut = nullptr;
    WAVEFORMATEX waveFormat = {};
    bool waveOpen = false;
    
    // Wave buffers for streaming
    static const int NUM_BUFFERS = 4;
    WAVEHDR waveHeaders[NUM_BUFFERS];
    std::vector<uint8_t> bufferData[NUM_BUFFERS];
    int currentBuffer = 0;
    
    // Threading
    HANDLE updateThread = nullptr;
    bool threadRunning = false;
    AudioPlayer* player = nullptr;
    
    static DWORD WINAPI UpdateThreadProc(LPVOID param) {
        Impl* impl = static_cast<Impl*>(param);
        while (impl->threadRunning) {
            if (impl->player) {
                impl->player->update(0); // Time not used in this implementation
            }
            Sleep(10); // 10ms update interval
        }
        return 0;
    }
#else
    // ALSA implementation for Linux (or stub if ALSA not available)
    #ifdef HAS_ALSA
        snd_pcm_t* pcmHandle = nullptr;
    #else
        void* pcmHandle = nullptr;
    #endif
    std::string deviceName = "default";
    pthread_t updateThread;
    bool threadRunning = false;
    AudioPlayer* player = nullptr;
    
    static void* UpdateThreadProc(void* param) {
        Impl* impl = static_cast<Impl*>(param);
        while (impl->threadRunning) {
            if (impl->player) {
                impl->player->update(0);
            }
            usleep(10000); // 10ms
        }
        return nullptr;
    }
#endif
    
    bool initialize(const AudioConfig& config) {
#ifdef _WIN32
        // Set up wave format (44.1kHz, 16-bit, stereo)
        waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        waveFormat.nChannels = config.channels;
        waveFormat.nSamplesPerSec = config.sampleRate;
        waveFormat.wBitsPerSample = 16;
        waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
        waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
        waveFormat.cbSize = 0;
        
        // Open wave output device
        MMRESULT result = waveOutOpen(&hWaveOut, WAVE_MAPPER, &waveFormat, 
                                       0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            std::cerr << "Failed to open wave output: " << result << std::endl;
            return false;
        }
        waveOpen = true;
        
        // Prepare buffers
        for (int i = 0; i < NUM_BUFFERS; i++) {
            bufferData[i].resize(config.bufferSize * waveFormat.nBlockAlign);
            memset(&waveHeaders[i], 0, sizeof(WAVEHDR));
            waveHeaders[i].lpData = reinterpret_cast<LPSTR>(bufferData[i].data());
            waveHeaders[i].dwBufferLength = static_cast<DWORD>(bufferData[i].size());
            waveOutPrepareHeader(hWaveOut, &waveHeaders[i], sizeof(WAVEHDR));
        }
        
        // Start update thread
        threadRunning = true;
        updateThread = CreateThread(nullptr, 0, UpdateThreadProc, this, 0, nullptr);
        
        return true;
#else
        #ifdef HAS_ALSA
        // ALSA initialization
        int err = snd_pcm_open(&pcmHandle, deviceName.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0) {
            std::cerr << "Cannot open audio device: " << snd_strerror(err) << std::endl;
            return false;
        }
        
        // Configure PCM
        snd_pcm_hw_params_t* hwParams = nullptr;
        snd_pcm_hw_params_alloca(&hwParams);
        snd_pcm_hw_params_any(pcmHandle, hwParams);
        
        err = snd_pcm_hw_params_set_access(pcmHandle, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
        if (err < 0) {
            std::cerr << "Cannot set access type: " << snd_strerror(err) << std::endl;
            return false;
        }
        
        err = snd_pcm_hw_params_set_format(pcmHandle, hwParams, SND_PCM_FORMAT_S16_LE);
        if (err < 0) {
            std::cerr << "Cannot set format: " << snd_strerror(err) << std::endl;
            return false;
        }
        
        unsigned int rate = config.sampleRate;
        err = snd_pcm_hw_params_set_rate_near(pcmHandle, hwParams, &rate, 0);
        if (err < 0) {
            std::cerr << "Cannot set rate: " << snd_strerror(err) << std::endl;
            return false;
        }
        
        err = snd_pcm_hw_params_set_channels(pcmHandle, hwParams, config.channels);
        if (err < 0) {
            std::cerr << "Cannot set channels: " << snd_strerror(err) << std::endl;
            return false;
        }
        
        err = snd_pcm_hw_params(pcmHandle, hwParams);
        if (err < 0) {
            std::cerr << "Cannot set hw params: " << snd_strerror(err) << std::endl;
            return false;
        }
        
        err = snd_pcm_prepare(pcmHandle);
        if (err < 0) {
            std::cerr << "Cannot prepare PCM: " << snd_strerror(err) << std::endl;
            return false;
        }
        
        // Start update thread
        threadRunning = true;
        pthread_create(&updateThread, nullptr, UpdateThreadProc, this);
        
        return true;
        #else
        // No ALSA - create stub that just discards audio
        (void)config;
        std::cerr << "Warning: Audio playback not available (ALSA not found)" << std::endl;
        threadRunning = true;
        pthread_create(&updateThread, nullptr, UpdateThreadProc, this);
        return true; // Return true to allow silent operation
        #endif
#endif
    }
    
    void shutdown() {
#ifdef _WIN32
        threadRunning = false;
        if (updateThread) {
            WaitForSingleObject(updateThread, 1000);
            CloseHandle(updateThread);
            updateThread = nullptr;
        }
        
        if (waveOpen && hWaveOut) {
            waveOutReset(hWaveOut);
            
            for (int i = 0; i < NUM_BUFFERS; i++) {
                waveOutUnprepareHeader(hWaveOut, &waveHeaders[i], sizeof(WAVEHDR));
            }
            
            waveOutClose(hWaveOut);
            hWaveOut = nullptr;
            waveOpen = false;
        }
#else
        threadRunning = false;
        pthread_join(updateThread, nullptr);
        
        #ifdef HAS_ALSA
        if (pcmHandle) {
            snd_pcm_drain(pcmHandle);
            snd_pcm_close(pcmHandle);
            pcmHandle = nullptr;
        }
        #endif
#endif
    }
    
    bool writeAudio(const int16_t* data, size_t sampleCount) {
#ifdef _WIN32
        if (!waveOpen || !hWaveOut) return false;
        
        // Find available buffer
        WAVEHDR* header = &waveHeaders[currentBuffer];
        
        // Wait if buffer is in use
        while (header->dwFlags & WHDR_INQUEUE) {
            Sleep(1);
            currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
            header = &waveHeaders[currentBuffer];
        }
        
        // Copy data
        size_t bytesToCopy = sampleCount * waveFormat.nChannels * sizeof(int16_t);
        if (bytesToCopy > bufferData[currentBuffer].size()) {
            bytesToCopy = bufferData[currentBuffer].size();
        }
        memcpy(bufferData[currentBuffer].data(), data, bytesToCopy);
        
        header->dwBufferLength = static_cast<DWORD>(bytesToCopy);
        header->dwFlags &= ~WHDR_DONE;
        
        // Write to device
        MMRESULT result = waveOutWrite(hWaveOut, header, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            return false;
        }
        
        currentBuffer = (currentBuffer + 1) % NUM_BUFFERS;
        return true;
#else
        #ifdef HAS_ALSA
        if (!pcmHandle) return false;
        
        snd_pcm_sframes_t frames = snd_pcm_writei(pcmHandle, data, sampleCount);
        if (frames < 0) {
            frames = snd_pcm_recover(pcmHandle, static_cast<int>(frames), 0);
        }
        return frames >= 0;
        #else
        // No ALSA - just discard audio
        (void)data;
        (void)sampleCount;
        return true;
        #endif
#endif
    }
};

// Static global player
static std::unique_ptr<AudioPlayer> g_globalAudioPlayer;

AudioPlayer* GetGlobalAudioPlayer() {
    return g_globalAudioPlayer.get();
}

void SetGlobalAudioPlayer(std::unique_ptr<AudioPlayer> player) {
    g_globalAudioPlayer = std::move(player);
}

// AudioPlayer implementation
AudioPlayer::AudioPlayer() : impl_(std::make_unique<Impl>()) {
    impl_->player = this;
}

AudioPlayer::~AudioPlayer() {
    shutdown();
}

bool AudioPlayer::initialize() {
    if (initialized_) return true;
    
    AudioConfig config;
    config.sampleRate = 44100;
    config.channels = 2;
    config.format = AudioFormat::PCM_S16;
    config.bufferSize = 4096;
    
    initialized_ = impl_->initialize(config);
    return initialized_;
}

void AudioPlayer::shutdown() {
    if (!initialized_) return;
    
    stopAllSounds();
    impl_->shutdown();
    initialized_ = false;
}

bool AudioPlayer::loadSound(uint16 soundId, const SoundTag& sound) {
    if (!initialized_) {
        if (!initialize()) return false;
    }
    
    std::lock_guard<std::mutex> lock(soundsMutex_);
    
    // Decode sound
    auto buffer = decodeSound(sound);
    if (!buffer) {
        std::cerr << "Failed to decode sound " << soundId << std::endl;
        return false;
    }
    
    sounds_[soundId] = std::move(buffer);
    return true;
}

bool AudioPlayer::unloadSound(uint16 soundId) {
    std::lock_guard<std::mutex> lock(soundsMutex_);
    auto it = sounds_.find(soundId);
    if (it != sounds_.end()) {
        sounds_.erase(it);
        return true;
    }
    return false;
}

bool AudioPlayer::hasSound(uint16 soundId) const {
    std::lock_guard<std::mutex> lock(soundsMutex_);
    return sounds_.find(soundId) != sounds_.end();
}

uint32_t AudioPlayer::playSound(uint16 soundId, const SoundPlayback& params) {
    if (!initialized_) return 0;
    
    std::lock_guard<std::mutex> lock(soundsMutex_);
    if (sounds_.find(soundId) == sounds_.end()) {
        return 0; // Sound not loaded
    }
    
    std::lock_guard<std::mutex> playbackLock(playbacksMutex_);
    
    uint32_t playbackId = nextPlaybackId_++;
    SoundPlayback playback = params;
    playback.soundId = soundId;
    playback.isPlaying = true;
    
    playbacks_[playbackId] = playback;
    return playbackId;
}

void AudioPlayer::stopSound(uint32_t playbackId) {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    auto it = playbacks_.find(playbackId);
    if (it != playbacks_.end()) {
        it->second.isPlaying = false;
        playbacks_.erase(it);
    }
}

void AudioPlayer::stopAllSounds() {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    playbacks_.clear();
}

void AudioPlayer::pauseSound(uint32_t playbackId) {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    auto it = playbacks_.find(playbackId);
    if (it != playbacks_.end()) {
        it->second.isPaused = true;
    }
}

void AudioPlayer::resumeSound(uint32_t playbackId) {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    auto it = playbacks_.find(playbackId);
    if (it != playbacks_.end()) {
        it->second.isPaused = false;
    }
}

void AudioPlayer::setMasterVolume(float volume) {
    masterVolume_ = std::max(0.0f, std::min(1.0f, volume));
}

void AudioPlayer::setSoundVolume(uint32_t playbackId, float volume) {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    auto it = playbacks_.find(playbackId);
    if (it != playbacks_.end()) {
        it->second.volume = std::max(0.0f, std::min(1.0f, volume));
    }
}

void AudioPlayer::setSoundPan(uint32_t playbackId, float pan) {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    auto it = playbacks_.find(playbackId);
    if (it != playbacks_.end()) {
        it->second.pan = std::max(-1.0f, std::min(1.0f, pan));
    }
}

bool AudioPlayer::isPlaying(uint32_t playbackId) const {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    auto it = playbacks_.find(playbackId);
    if (it != playbacks_.end()) {
        return it->second.isPlaying && !it->second.isPaused;
    }
    return false;
}

bool AudioPlayer::isAnyPlaying() const {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    for (const auto& [id, playback] : playbacks_) {
        if (playback.isPlaying && !playback.isPaused) {
            return true;
        }
    }
    return false;
}

size_t AudioPlayer::getActiveCount() const {
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    return playbacks_.size();
}

void AudioPlayer::update(uint32_t currentTime) {
    if (!initialized_ || playbacks_.empty()) return;
    
    // Mix audio for all active playbacks
    const size_t MIX_BUFFER_SAMPLES = 512;
    std::vector<float> mixBuffer(MIX_BUFFER_SAMPLES * 2); // Stereo float
    
    mixAudio(mixBuffer.data(), MIX_BUFFER_SAMPLES, AudioConfig{});
    
    // Convert to int16 and write to device
    std::vector<int16_t> outputBuffer(MIX_BUFFER_SAMPLES * 2);
    for (size_t i = 0; i < mixBuffer.size(); i++) {
        float sample = mixBuffer[i] * masterVolume_;
        sample = std::max(-1.0f, std::min(1.0f, sample));
        outputBuffer[i] = static_cast<int16_t>(sample * 32767.0f);
    }
    
    impl_->writeAudio(outputBuffer.data(), MIX_BUFFER_SAMPLES);
}

void AudioPlayer::mixAudio(float* output, size_t sampleCount, const AudioConfig& config) {
    // Clear output
    memset(output, 0, sampleCount * 2 * sizeof(float));
    
    std::lock_guard<std::mutex> lock(playbacksMutex_);
    std::lock_guard<std::mutex> soundLock(soundsMutex_);
    
    for (auto& [playbackId, playback] : playbacks_) {
        if (!playback.isPlaying || playback.isPaused) continue;
        
        auto soundIt = sounds_.find(playback.soundId);
        if (soundIt == sounds_.end()) continue;
        
        const AudioBuffer* buffer = soundIt->second.get();
        if (!buffer || buffer->data.empty()) continue;
        
        const int16_t* source = reinterpret_cast<const int16_t*>(buffer->data.data());
        uint32_t sourceSamples = buffer->sampleCount * buffer->config.channels;
        
        // Apply envelope
        float leftVol = playback.volume;
        float rightVol = playback.volume;
        if (!playback.envelope.empty()) {
            applyEnvelope(playback, &leftVol, &rightVol);
        } else {
            // Apply pan
            if (playback.pan < 0) {
                rightVol *= (1.0f + playback.pan);
            } else if (playback.pan > 0) {
                leftVol *= (1.0f - playback.pan);
            }
        }
        
        // Mix samples
        for (size_t i = 0; i < sampleCount; i++) {
            if (playback.position >= sourceSamples) {
                // Sound finished
                if (playback.loopCount == 0 || playback.currentLoop < playback.loopCount - 1) {
                    playback.currentLoop++;
                    playback.position = 0;
                } else {
                    playback.isPlaying = false;
                    if (onCompleteCallback_) {
                        onCompleteCallback_(playback.soundId, playbackId);
                    }
                    break;
                }
            }
            
            if (buffer->config.channels == 1) {
                // Mono to stereo
                float sample = source[playback.position] / 32768.0f;
                output[i * 2] += sample * leftVol;
                output[i * 2 + 1] += sample * rightVol;
                playback.position++;
            } else {
                // Stereo
                output[i * 2] += source[playback.position] / 32768.0f * leftVol;
                output[i * 2 + 1] += source[playback.position + 1] / 32768.0f * rightVol;
                playback.position += 2;
            }
        }
    }
}

void AudioPlayer::applyEnvelope(SoundPlayback& playback, float* leftVol, float* rightVol) {
    if (playback.envelope.empty()) return;
    
    // Find current envelope segment
    uint32_t pos44 = playback.position * 44100 / 44100; // Convert to 44.1k reference
    
    size_t idx = 0;
    for (size_t i = 0; i < playback.envelope.size(); i++) {
        if (playback.envelope[i].mark44 > pos44) break;
        idx = i;
    }
    
    if (idx >= playback.envelope.size() - 1) {
        // Use last point
        *leftVol *= playback.envelope.back().levelLeft;
        *rightVol *= playback.envelope.back().levelRight;
    } else {
        // Interpolate between points
        const auto& p1 = playback.envelope[idx];
        const auto& p2 = playback.envelope[idx + 1];
        
        if (p2.mark44 > p1.mark44) {
            float t = static_cast<float>(pos44 - p1.mark44) / (p2.mark44 - p1.mark44);
            *leftVol *= (p1.levelLeft * (1 - t) + p2.levelLeft * t);
            *rightVol *= (p1.levelRight * (1 - t) + p2.levelRight * t);
        }
    }
}

std::unique_ptr<AudioBuffer> AudioPlayer::decodeSound(const SoundTag& sound) {
    AudioConfig config;
    
    // Convert SWF rate to sample rate
    switch (sound.rate) {
        case SoundRate::KHZ_5_5: config.sampleRate = 5512; break;
        case SoundRate::KHZ_11: config.sampleRate = 11025; break;
        case SoundRate::KHZ_22: config.sampleRate = 22050; break;
        case SoundRate::KHZ_44: config.sampleRate = 44100; break;
    }
    
    config.channels = sound.isStereo ? 2 : 1;
    config.format = AudioFormat::PCM_S16;
    
    switch (sound.format) {
        case SoundFormat::UNCOMPRESSED_NATIVE_ENDIAN:
        case SoundFormat::UNCOMPRESSED_LITTLE_ENDIAN: {
            // Raw PCM - just copy
            auto buffer = std::make_unique<AudioBuffer>();
            buffer->config = config;
            buffer->data = sound.soundData;
            buffer->sampleCount = sound.sampleCount;
            return buffer;
        }
        
        case SoundFormat::ADPCM: {
            return decodeADPCM(sound.soundData.data(), sound.soundData.size(), config, sound.isStereo);
        }
        
        case SoundFormat::MP3: {
            // For now, return empty buffer (MP3 decoding needs external library)
            std::cerr << "MP3 decoding not yet implemented" << std::endl;
            return nullptr;
        }
        
        case SoundFormat::NELLYMOSER:
        case SoundFormat::NELLYMOSER_16KHZ:
        case SoundFormat::NELLYMOSER_8KHZ:
            std::cerr << "Nellymoser decoding not yet implemented" << std::endl;
            return nullptr;
            
        case SoundFormat::SPEEX:
            std::cerr << "Speex decoding not yet implemented" << std::endl;
            return nullptr;
            
        default:
            return nullptr;
    }
}

std::unique_ptr<AudioBuffer> AudioPlayer::decodeADPCM(const uint8_t* data, size_t size, 
                                                       const AudioConfig& config, bool isStereo) {
    if (size < 2) return nullptr;
    
    auto buffer = std::make_unique<AudioBuffer>();
    buffer->config = config;
    
    // ADPCM decoding state
    struct ADPCMState {
        int predictor = 0;
        int index = 0;
    };
    
    std::vector<ADPCMState> states(config.channels);
    
    // Read initial state from header
    size_t pos = 0;
    for (int ch = 0; ch < config.channels && pos + 1 < size; ch++) {
        states[ch].predictor = static_cast<int16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        if (pos < size) {
            states[ch].index = data[pos++];
            pos++; // Skip reserved byte
        }
        if (states[ch].index > 88) states[ch].index = 88;
    }
    
    // Decode samples
    // For simplicity, assume 4-bit ADPCM (most common in SWF)
    std::vector<int16_t> samples;
    samples.reserve((size - pos) * 2 * (isStereo ? 1 : 2));
    
    int currentChannel = 0;
    while (pos < size) {
        uint8_t byte = data[pos++];
        
        // Process both nibbles
        for (int nibble = 0; nibble < 2; nibble++) {
            int code = (byte >> (nibble * 4)) & 0x0F;
            
            ADPCMState& state = states[currentChannel];
            int step = ADPCM_STEP_TABLE[state.index];
            
            // Calculate difference
            int diff = 0;
            if (code & 4) diff += step;
            if (code & 2) diff += step >> 1;
            if (code & 1) diff += step >> 2;
            diff += step >> 3;
            
            if (code & 8) {
                state.predictor -= diff;
                if (state.predictor < -32768) state.predictor = -32768;
            } else {
                state.predictor += diff;
                if (state.predictor > 32767) state.predictor = 32767;
            }
            
            // Update index
            state.index += ADPCM_INDEX_TABLE_4BIT[code];
            if (state.index < 0) state.index = 0;
            if (state.index > 88) state.index = 88;
            
            samples.push_back(static_cast<int16_t>(state.predictor));
            
            if (isStereo) {
                currentChannel = (currentChannel + 1) % 2;
            }
        }
    }
    
    // Convert to bytes
    buffer->data.resize(samples.size() * sizeof(int16_t));
    memcpy(buffer->data.data(), samples.data(), buffer->data.size());
    buffer->sampleCount = samples.size() / config.channels;
    
    return buffer;
}

std::unique_ptr<AudioBuffer> AudioPlayer::decodeMP3(const uint8_t* data, size_t size) {
    // MP3 decoding requires external library like minimp3 or mpg123
    // For now, return nullptr
    (void)data;
    (void)size;
    return nullptr;
}

void AudioPlayer::feedStream(uint16 streamId, const uint8_t* data, size_t size) {
    // Streaming sound support - feed data to a streaming playback
    // This would buffer audio data for synchronized playback with frames
    (void)streamId;
    (void)data;
    (void)size;
}

} // namespace libswf
