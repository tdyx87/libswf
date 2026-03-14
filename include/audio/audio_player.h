#ifndef LIBSWF_AUDIO_AUDIO_PLAYER_H
#define LIBSWF_AUDIO_AUDIO_PLAYER_H

#include "parser/swftag.h"
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace libswf {

// Audio format conversion
enum class AudioFormat {
    PCM_U8,      // Unsigned 8-bit PCM
    PCM_S16,     // Signed 16-bit PCM (native endian)
    PCM_S24,     // Signed 24-bit PCM
    PCM_S32,     // Signed 32-bit PCM
    PCM_FLOAT,   // 32-bit float
    MP3,         // MPEG Layer 3
    ADPCM        // Adaptive DPCM
};

// Audio stream configuration
struct AudioConfig {
    uint32_t sampleRate = 44100;
    uint8_t channels = 2;      // 1 = mono, 2 = stereo
    AudioFormat format = AudioFormat::PCM_S16;
    uint32_t bufferSize = 4096; // Buffer size in samples
};

// Decoded audio data
struct AudioBuffer {
    std::vector<uint8_t> data;
    AudioConfig config;
    uint32_t sampleCount = 0;
    
    // Get duration in milliseconds
    uint32_t getDurationMs() const {
        if (config.sampleRate == 0) return 0;
        return (sampleCount * 1000) / config.sampleRate;
    }
};

// Sound playback state
struct SoundPlayback {
    uint16 soundId = 0;
    uint32_t startTime = 0;      // Start time in milliseconds
    uint32_t position = 0;       // Current position in samples
    uint32_t loopCount = 0;      // Number of loops (0 = infinite)
    uint32_t currentLoop = 0;    // Current loop index
    float volume = 1.0f;         // 0.0 - 1.0
    float pan = 0.0f;            // -1.0 (left) to 1.0 (right)
    bool isPlaying = false;
    bool isPaused = false;
    bool isStream = false;       // Streaming sound (synced with frames)
    
    // Envelope control (for SWF sound envelopes)
    struct EnvelopePoint {
        uint32_t mark44;         // Position in 44.1kHz samples
        float levelLeft = 1.0f;  // Left channel level (0-1)
        float levelRight = 1.0f; // Right channel level (0-1)
    };
    std::vector<EnvelopePoint> envelope;
};

// Platform-independent audio player interface
class AudioPlayer {
public:
    AudioPlayer();
    virtual ~AudioPlayer();
    
    // Initialize audio system
    bool initialize();
    void shutdown();
    bool isInitialized() const { return initialized_; }
    
    // Load sound data
    bool loadSound(uint16 soundId, const SoundTag& sound);
    bool unloadSound(uint16 soundId);
    bool hasSound(uint16 soundId) const;
    
    // Playback control
    uint32_t playSound(uint16 soundId, const SoundPlayback& params);
    void stopSound(uint32_t playbackId);
    void stopAllSounds();
    void pauseSound(uint32_t playbackId);
    void resumeSound(uint32_t playbackId);
    
    // Volume control
    void setMasterVolume(float volume);
    float getMasterVolume() const { return masterVolume_; }
    void setSoundVolume(uint32_t playbackId, float volume);
    void setSoundPan(uint32_t playbackId, float pan);
    
    // Update (call regularly, e.g., each frame)
    void update(uint32_t currentTime);
    
    // Query state
    bool isPlaying(uint32_t playbackId) const;
    bool isAnyPlaying() const;
    size_t getActiveCount() const;
    
    // Stream sound data (for streaming sounds)
    void feedStream(uint16 streamId, const uint8_t* data, size_t size);
    
    // Callback for when sound finishes
    using SoundCompleteCallback = std::function<void(uint16 soundId, uint32_t playbackId)>;
    void setOnCompleteCallback(SoundCompleteCallback callback) { onCompleteCallback_ = callback; }

private:
    bool initialized_ = false;
    float masterVolume_ = 1.0f;
    uint32_t nextPlaybackId_ = 1;
    
    // Sound bank (decoded audio data)
    std::unordered_map<uint16, std::unique_ptr<AudioBuffer>> sounds_;
    mutable std::mutex soundsMutex_;
    
    // Active playbacks
    std::unordered_map<uint32_t, SoundPlayback> playbacks_;
    mutable std::mutex playbacksMutex_;
    
    // Callback
    SoundCompleteCallback onCompleteCallback_;
    
    // Platform-specific implementation
    class Impl;
    std::unique_ptr<Impl> impl_;
    
    // Decode SWF sound format to PCM
    std::unique_ptr<AudioBuffer> decodeSound(const SoundTag& sound);
    std::unique_ptr<AudioBuffer> decodeADPCM(const uint8_t* data, size_t size, 
                                              const AudioConfig& config, bool isStereo);
    std::unique_ptr<AudioBuffer> decodeMP3(const uint8_t* data, size_t size);
    
    // Convert to target format
    void convertToOutputFormat(AudioBuffer& buffer, const AudioConfig& targetConfig);
    
    // Mix audio for playback
    void mixAudio(float* output, size_t sampleCount, const AudioConfig& config);
    
    // Apply envelope
    void applyEnvelope(SoundPlayback& playback, float* leftVol, float* rightVol);
};

// Global audio player instance (singleton-like access)
AudioPlayer* GetGlobalAudioPlayer();
void SetGlobalAudioPlayer(std::unique_ptr<AudioPlayer> player);

} // namespace libswf

#endif // LIBSWF_AUDIO_AUDIO_PLAYER_H
