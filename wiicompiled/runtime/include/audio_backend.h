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
    // each offered block (the output's latency then), for the session's logs. The times the device found the queue
    // empty and how much it was short (heard as silence), the silence put in after it to fill the queue again, and the
    // playback rate's range since ResetRateRange (the queue's level control, AudioBackend.cpp).
    struct Stats {
        uint64_t blocks = 0;
        uint64_t dropped = 0;
        uint64_t queuedBytesSum = 0;
        uint32_t queuedBytesMax = 0;
        uint32_t bytesPerSecond = 0;
        uint64_t ranDry = 0;
        uint64_t shortBytes = 0;
        uint64_t refillBytes = 0;
        float rateMin = 1.0f;
        float rateMax = 1.0f;
    };
    Stats GetStats() const;
    void ResetRateRange();

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
    uint32_t BytesForMsLocked(uint32_t ms) const;
    void KeepLevelLocked(int queued);
    static void SDLCALL OnDeviceRead(void* self, SDL_AudioStream* stream, int additional, int total);
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

    // The output queue's level control (WIICOMPILED_AUDIO_BUFFER_MS, 0 = off: no refill, rate fixed at 1).
    uint32_t m_targetMs = 0;
    double m_levelMs = -1.0;            // the queue's smoothed level (below 0: none yet)
    float m_rate = 1.0f;                // the stream's frequency ratio now
    std::atomic<uint64_t> m_dryReads{0};     // device reads that found the queue short (OnDeviceRead)
    std::atomic<uint64_t> m_dryBytes{0};     // and the bytes they were short
    uint64_t m_dryReadsSeen = 0;        // m_dryReads at the last refill
    std::vector<uint8_t> m_silence;

#if defined(__SWITCH__)
    // SDL3 has no Horizon audio driver: m_stream is a device-less SDL stream (rate conversion and gain), drained into
    // libnx audout by this thread.
    void AudoutLoop();
    std::thread m_audoutThread;
    std::atomic<bool> m_audoutRunning{false};
#endif
};
