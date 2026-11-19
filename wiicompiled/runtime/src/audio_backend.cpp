#include "audio_backend.h"

#include "runtime_log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

#include <SDL3/SDL_init.h>

#if defined(__SWITCH__)
#include <cstring>
#include <switch.h>

namespace {
bool g_audoutInitialized = false;  // audoutInitialize succeeded (SDL3 has no Horizon audio driver: libnx audout)
}
#endif

AudioBackend& AudioBackend::Instance() {
    // Never destroyed: the retry thread may still be waiting on m_retryWake when static destructors run.
    static AudioBackend* instance = new AudioBackend;
    return *instance;
}

float AudioBackend::EffectiveGainLocked() const {
    return m_muted ? 0.0f : m_masterVolume;
}

void AudioBackend::ApplyGainLocked() {
    if (m_stream && !SDL_SetAudioStreamGain(m_stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
    }
}

void AudioBackend::SetMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyGainLocked();
}

void AudioBackend::SetMuted(bool muted) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
    ApplyGainLocked();
}

// Opens the default playback device for this format; null on failure (reported when `report`). Touches no member
// state, so it runs without m_mutex: a probe takes milliseconds (ALSA/PipeWire configuration parsing).
SDL_AudioStream* AudioBackend::OpenDefaultStream(uint32_t sampleRate, uint32_t channels, SDL_AudioSpec& spec,
                                                 bool report) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (report) {
            RT_LOG(RT_TAG_AUDIO) << "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: " << SDL_GetError() << std::endl;
        }
        return nullptr;
    }

    spec = {};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = static_cast<int>(channels);
    spec.freq = static_cast<int>(sampleRate);

#if defined(__SWITCH__)
    // A device-less stream (rate conversion and gain) that AudoutLoop drains into audout.
    if (!g_audoutInitialized) {
        const Result rc = audoutInitialize();
        if (R_FAILED(rc)) {
            if (report) RT_LOG(RT_TAG_AUDIO) << "audoutInitialize failed: 0x" << std::hex << rc << std::dec << std::endl;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return nullptr;
        }
        g_audoutInitialized = true;
    }
    SDL_AudioSpec output{};
    output.format = SDL_AUDIO_S16LE;
    output.channels = static_cast<int>(audoutGetChannelCount());
    output.freq = static_cast<int>(audoutGetSampleRate());
    SDL_AudioStream* stream = SDL_CreateAudioStream(&spec, &output);
    if (stream) {
        RT_LOG(RT_TAG_AUDIO) << "audout: " << spec.freq << " Hz in, " << output.freq << " Hz x" << output.channels
                             << " out" << std::endl;
    }
#else
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
#endif
    if (!stream) {
        if (report) {
            RT_LOG(RT_TAG_AUDIO) << "SDL_OpenAudioDeviceStream failed: " << SDL_GetError()
                                 << " (retrying in the background, every 2 s backing off to 30 s)" << std::endl;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);  // balance this attempt's InitSubSystem
    }
    return stream;
}

// The retry thread: one for the backend's life, started by the first failed open, so a failed backend costs the
// game thread nothing after that (each probe takes milliseconds; a thread per retry cost more). While retrying it
// holds a reference to SDL's audio subsystem of its own, so the attempts' InitSubSystem/QuitSubSystem pairs only
// count references instead of unloading and reloading the audio drivers every time.
void AudioBackend::RetryMain() {
    std::unique_lock<std::mutex> lock(m_mutex);
    bool held = false;  // this thread's subsystem reference
    uint32_t delayMs = 2000;
    for (;;) {
        if (m_shutdown || !m_openFailed || m_initialized) {
            if (held) {
                SDL_QuitSubSystem(SDL_INIT_AUDIO);
                held = false;
            }
            m_retryWake.wait(lock, [this] { return !m_shutdown && m_openFailed && !m_initialized; });
            delayMs = 2000;
        }
        m_retryWake.wait_for(lock, std::chrono::milliseconds(delayMs), [this] { return m_shutdown; });
        if (m_shutdown || !m_openFailed || m_initialized) continue;
        const uint32_t rate = m_wantRate, channels = m_wantChannels;
        lock.unlock();
        if (!held) held = SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec spec{};
        SDL_AudioStream* stream = OpenDefaultStream(rate, channels, spec, false);
        lock.lock();
        if (!stream) {
            delayMs = std::min<uint32_t>(delayMs * 2, 30000);
            continue;
        }
        if (m_shutdown || m_initialized) {  // shut down meanwhile, or another open won
            SDL_DestroyAudioStream(stream);
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            continue;
        }
        if (InstallStreamLocked(stream, spec, rate, channels)) {
            m_openFailed = false;  // the stream holds its own reference; the loop drops this thread's
        } else {
            delayMs = std::min<uint32_t>(delayMs * 2, 30000);
        }
    }
}

bool AudioBackend::EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels) {
    if (m_initialized && m_sampleRate == sampleRate && m_channels == channels) {
        return true;
    }

    // A format change replaces the stream. The old one's subsystem reference is dropped after the new open (below), so
    // the audio drivers stay loaded across the switch; a failed reopen goes to the retry thread like a first failure.
    const bool replacing = m_initialized;
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    m_initialized = false;

    // Without an output device (none plugged in or all disabled) every audio block would retry the open. After the
    // first failure the retry thread (RetryMain) owns reopening, backing off from 2 s to 30 s, so a device plugged in
    // later is still picked up; the game thread (every audio block calls in) only reads the outcome. Each failed probe
    // held the game thread for milliseconds, and a thread per retry plus the subsystem's driver unload/reload cost more.
    if (m_openFailed) {
        if (m_wantRate != sampleRate || m_wantChannels != channels) {
            m_wantRate = sampleRate;
            m_wantChannels = channels;
            m_retryWake.notify_all();
        }
        return false;
    }

    SDL_AudioSpec spec{};
    SDL_AudioStream* stream = OpenDefaultStream(sampleRate, channels, spec, !m_reportedOpenFailure);
    if (replacing) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (!stream || !InstallStreamLocked(stream, spec, sampleRate, channels)) {
        m_reportedOpenFailure = true;
        m_openFailed = true;
        m_wantRate = sampleRate;
        m_wantChannels = channels;
        if (!m_retryStarted) {
            m_retryStarted = true;
            std::thread([this] { RetryMain(); }).detach();  // lives as long as the process (the backend is static)
        }
        m_retryWake.notify_all();
        return false;
    }
    return true;
}

// Takes over a stream from OpenDefaultStream and its subsystem reference; on failure destroys both.
bool AudioBackend::InstallStreamLocked(SDL_AudioStream* stream, const SDL_AudioSpec& spec, uint32_t sampleRate,
                                       uint32_t channels) {
#if !defined(__SWITCH__)
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_ResumeAudioStreamDevice failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
#endif

    if (!SDL_SetAudioStreamGain(stream, EffectiveGainLocked())) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_SetAudioStreamGain failed: " << SDL_GetError() << std::endl;
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }

    m_stream = stream;
    m_spec = spec;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
#if defined(__SWITCH__)
    if (!m_audoutRunning.exchange(true)) {
        m_audoutThread = std::thread([this] { AudoutLoop(); });
    }
#endif
    return true;
}

#if defined(__SWITCH__)
void AudioBackend::AudoutLoop() {
    constexpr size_t kFrames = 1024;                               // 21.3 ms at 48 kHz
    constexpr size_t kBytes = kFrames * 2 * sizeof(int16_t);       // 0x1000: audout's buffer size/alignment unit
    constexpr int kBuffers = 4;
    AudioOutBuffer buffers[kBuffers]{};
    const auto fill = [this](AudioOutBuffer& buffer) {
        int got = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stream) got = SDL_GetAudioStreamData(m_stream, buffer.buffer, static_cast<int>(kBytes));
        }
        if (got < 0) got = 0;
        if (static_cast<size_t>(got) < kBytes)  // underrun: pad with silence rather than replay stale samples
            std::memset(static_cast<uint8_t*>(buffer.buffer) + got, 0, kBytes - static_cast<size_t>(got));
        buffer.data_size = kBytes;
        buffer.data_offset = 0;
    };
    if (R_FAILED(audoutStartAudioOut())) {
        RT_LOG(RT_TAG_AUDIO) << "audoutStartAudioOut failed" << std::endl;
        m_audoutRunning = false;
        return;
    }
    int appended = 0;
    for (auto& buffer : buffers) {
        buffer.buffer = std::aligned_alloc(0x1000, kBytes);
        if (!buffer.buffer) break;
        buffer.buffer_size = kBytes;
        fill(buffer);
        if (R_FAILED(audoutAppendAudioOutBuffer(&buffer))) break;
        ++appended;
    }
    while (appended > 0 && m_audoutRunning.load()) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100'000'000ull)) || !released || count == 0) continue;
        fill(*released);
        audoutAppendAudioOutBuffer(released);
    }
    audoutStopAudioOut();
    for (auto& buffer : buffers) std::free(buffer.buffer);
}
#endif

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown) {
        m_shutdown = false;
        m_retryWake.notify_all();
    }
    return EnsureInitializedLocked(sampleRate, channels);
}

void AudioBackend::Shutdown() {
#if defined(__SWITCH__)
    m_audoutRunning = false;  // before taking m_mutex: the thread takes it to drain the stream
    if (m_audoutThread.joinable()) m_audoutThread.join();
#endif
    std::lock_guard<std::mutex> lock(m_mutex);
    m_shutdown = true;
    m_retryWake.notify_all();  // the retry thread drops its subsystem reference and idles
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
#if defined(__SWITCH__)
    if (g_audoutInitialized) {
        audoutExit();
        g_audoutInitialized = false;
    }
#endif
    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_reportedDroppedBlock = false;
    m_convertBuffer.clear();
}

uint32_t AudioBackend::QueueLimitBytesLocked() const {
    constexpr uint32_t queueMs = 120;

    const uint64_t bytesPerSecond = static_cast<uint64_t>(m_spec.freq) *
                                    static_cast<uint64_t>(m_spec.channels) *
                                    sizeof(int16_t);
    return static_cast<uint32_t>((bytesPerSecond * queueMs) / 1000u);
}

bool AudioBackend::QueueHasCapacityLocked(int incomingBytes) {
    if (!m_stream) {
        return false;
    }
    const int queued = SDL_GetAudioStreamQueued(m_stream);
    if (queued < 0) {
        return true;
    }
    const uint32_t maxQueued = QueueLimitBytesLocked();
    if (static_cast<uint64_t>(queued) + static_cast<uint64_t>(std::max(incomingBytes, 0)) > maxQueued) {
        // Match Dolphin's FIFO overflow behavior: preserve the continuous audio
        // already queued and discard the new block.  Clearing SDL's entire
        // stream creates an audible discontinuity (the severe crackle seen when
        // VI-batched DMA briefly outran playback).
        // A drop is audible; report the first one so it is not invisible.
        if (!m_reportedDroppedBlock) {
            m_reportedDroppedBlock = true;
            RT_LOG(RT_TAG_AUDIO) << "output queue full (" << queued << "/" << maxQueued
                      << " bytes); dropping blocks to preserve continuity" << std::endl;
        }
        return false;
    }
    return true;
}

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes) {
    if (!data || bytes == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    const size_t sampleCount = bytes / sizeof(int16_t);
    if (sampleCount == 0) {
        return false;
    }

    if (m_convertBuffer.size() < sampleCount) {
        m_convertBuffer.resize(sampleCount);
    }

    const size_t frameCount = sampleCount / 2;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t rightOffset = frame * 4;
        const size_t leftOffset = rightOffset + 2;
        const uint16_t right = static_cast<uint16_t>(data[rightOffset]) << 8 |
                               static_cast<uint16_t>(data[rightOffset + 1]);
        const uint16_t left = static_cast<uint16_t>(data[leftOffset]) << 8 |
                              static_cast<uint16_t>(data[leftOffset + 1]);
        m_convertBuffer[frame * 2] = static_cast<int16_t>(left);
        m_convertBuffer[frame * 2 + 1] = static_cast<int16_t>(right);
    }

    const int lenBytes = static_cast<int>(frameCount * 2 * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
        return true;
    }
    if (!SDL_PutAudioStreamData(m_stream, m_convertBuffer.data(), lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }

    return true;
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount) {
    if (!samples || sampleCount == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_stream) {
        return false;
    }

    const int lenBytes = static_cast<int>(sampleCount * sizeof(int16_t));
    if (!QueueHasCapacityLocked(lenBytes)) {
        return true;
    }
    if (!SDL_PutAudioStreamData(m_stream, samples, lenBytes)) {
        RT_LOG(RT_TAG_AUDIO) << "SDL_PutAudioStreamData failed: " << SDL_GetError() << std::endl;
        return false;
    }

    return true;
}
