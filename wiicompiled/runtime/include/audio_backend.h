#pragma once

#include <condition_variable>
#include <atomic>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <SDL3/SDL_audio.h>

class AudioBackend {
public:
    static AudioBackend& Instance();

    bool Init(uint32_t sampleRate, uint32_t channels);
    void Shutdown();

    // Wii AI DMA frames are big-endian and ordered right, left. SDL expects
    // native-endian interleaved left, right samples.
    bool PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes);
    bool PushSamplesLE16(const int16_t* samples, size_t sampleCount);

    // Applied to the final host output, covering both AX and direct AI DMA.
    void SetMasterVolume(float volume);
    void SetMuted(bool muted);

    // Since the start: blocks offered, those dropped because the output queue was full, and what was queued ahead of
    // each offered block (the output's latency then), for the session's logs.
    struct Stats {
        uint64_t blocks = 0;
        uint64_t dropped = 0;
        uint64_t queuedBytesSum = 0;
        uint32_t queuedBytesMax = 0;
        uint32_t bytesPerSecond = 0;
    };
    Stats GetStats() const;

private:
    AudioBackend() = default;
    ~AudioBackend() = default;
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    bool EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels);
    static SDL_AudioStream* OpenDefaultStream(uint32_t sampleRate, uint32_t channels, SDL_AudioSpec& spec,
                                              bool report);
    void RetryMain();
    bool InstallStreamLocked(SDL_AudioStream* stream, const SDL_AudioSpec& spec, uint32_t sampleRate,
                             uint32_t channels);
    bool QueueHasCapacityLocked(int incomingBytes);
    uint32_t QueueLimitBytesLocked() const;
    float EffectiveGainLocked() const;
    void ApplyGainLocked();

    mutable std::mutex m_mutex;
    SDL_AudioStream* m_stream = nullptr;
    SDL_AudioSpec m_spec{};
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    bool m_initialized = false;
    float m_masterVolume = 1.0f;
    bool m_muted = false;
    bool m_reportedDroppedBlock = false;
    Stats m_stats;
    bool m_openFailed = false;  // no output device: the retry thread owns reopening (EnsureInitializedLocked)
    bool m_reportedOpenFailure = false;
    bool m_shutdown = false;    // Shutdown ran since the last Init: a retry that lands now must not install
    uint32_t m_wantRate = 0, m_wantChannels = 0;  // the format the retry thread opens
    bool m_retryStarted = false;                 // the retry thread: started at the first failed open, then kept
    std::condition_variable m_retryWake;
    std::vector<int16_t> m_convertBuffer;

#if defined(__SWITCH__)
    // SDL3 has no Horizon audio driver: m_stream is a device-less SDL stream (rate conversion and gain), drained into
    // libnx audout by this thread.
    void AudoutLoop();
    std::thread m_audoutThread;
    std::atomic<bool> m_audoutRunning{false};
#endif
};
