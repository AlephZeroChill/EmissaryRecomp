#include "audio_backend.h"

#include "runtime_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

namespace {
// WIICOMPILED_AUDIO_DUMP_OUTPUT=<file>: what the output queue took, as raw 16-bit little-endian PCM at the queue's
// format (32 kHz stereo for the game: left, right), in order: the game's sound as it is played, less the blocks a full
// queue dropped and any silence the device filled in (WIICOMPILED_AUDIO_DUMP, audio.cpp, is the AI's output before the
// queue).
void DumpQueued(const void* data, int bytes) {
    static FILE* file = [] {
        const char* path = std::getenv("WIICOMPILED_AUDIO_DUMP_OUTPUT");
        return path != nullptr && *path != '\0' ? std::fopen(path, "wb") : nullptr;
    }();
    if (file == nullptr || bytes <= 0) {
        return;
    }
    std::fwrite(data, 1, static_cast<size_t>(bytes), file);
    std::fflush(file);
}

// The output queue's level control: how much sound it keeps ahead of the device, WIICOMPILED_AUDIO_BUFFER_MS
// (default 60; 0 = none, the queue as it was before: no refill, the rate fixed at 1, full at 120 ms).
//
// The game's sound comes in at the emulated time's pace, one frame's blocks at a time, while the device plays at its
// own. A frame shown late (a rollback's re-runs, a slow scene, the GPU behind) delivers its sound late by as much;
// with nothing kept ahead the device runs out and plays silence: gaps in the sound, heard as choppy audio (Brawl
// Online bug report #376: frames up to 85 ms late, the queue anywhere from 14 to 88 ms). Like Dolphin's mixer
// (Config AudioBufferSize, 80 ms), the queue keeps its level: the stream plays a little faster when it holds more
// than the target and a little slower when it holds less (up to 2%, a third of a semitone), and when the device has
// run dry the gap is made up to the target at once, so the next late frame finds the sound ahead of it again.
uint32_t TargetBufferMs() {
    static const uint32_t ms = [] {
        const char* v = std::getenv("WIICOMPILED_AUDIO_BUFFER_MS");
        if (v == nullptr || *v == '\0') return 60u;
        const long n = std::strtol(v, nullptr, 10);
        return static_cast<uint32_t>(std::clamp<long>(n, 0, 250));
    }();
    return ms;
}
constexpr double kLevelSmoothingSeconds = 0.5;  // the level the rate follows: its average over about this long
constexpr double kRateGain = 0.04;              // rate change per target's worth off the target
constexpr float kMaxRateChange = 0.02f;
constexpr float kRateStep = 0.001f;             // smaller changes are not passed on to SDL
}  // namespace

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
    // The level control starts over with each stream, the queue filled to its target before the first block.
    m_targetMs = TargetBufferMs();
    m_levelMs = -1.0;
    m_rate = 1.0f;
    m_dryReadsSeen = m_dryReads.load();
    SDL_SetAudioStreamGetCallback(stream, &AudioBackend::OnDeviceRead, this);
    if (m_targetMs != 0) {
        m_silence.assign(BytesForMsLocked(m_targetMs), 0);
        if (SDL_PutAudioStreamData(stream, m_silence.data(), static_cast<int>(m_silence.size()))) {
            m_stats.refillBytes += m_silence.size();
        }
    }
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

uint32_t AudioBackend::BytesForMsLocked(uint32_t ms) const {
    const uint64_t bytesPerSecond = static_cast<uint64_t>(m_spec.freq) *
                                    static_cast<uint64_t>(m_spec.channels) *
                                    sizeof(int16_t);
    const uint32_t frame = static_cast<uint32_t>(m_spec.channels) * sizeof(int16_t);
    const uint32_t bytes = static_cast<uint32_t>((bytesPerSecond * ms) / 1000u);
    return frame ? bytes - bytes % frame : bytes;
}

uint32_t AudioBackend::QueueLimitBytesLocked() const {
    // With the level control the rate keeps the queue near its target; the limit only catches what it cannot (sound
    // coming in much faster than it plays) and sits well above the target, so a block is dropped only then.
    return BytesForMsLocked(m_targetMs != 0 ? std::max<uint32_t>(120, m_targetMs * 2 + 60) : 120);
}

AudioBackend::Stats AudioBackend::GetStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats s = m_stats;
    s.bytesPerSecond = static_cast<uint32_t>(m_spec.freq) * static_cast<uint32_t>(m_spec.channels) * sizeof(int16_t);
    s.shortBytes = m_dryBytes.load(std::memory_order_relaxed);
    return s;
}

void AudioBackend::ResetRateRange() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats.rateMin = m_stats.rateMax = m_rate;
}

// SDL's device thread, before it takes the stream's next data (the stream's lock held: atomics only here).
// `additional` is how much of the request the queue lacks, in the stream's input format, overestimated by a few
// samples for the resampler: a shortfall over 2 ms is the device running dry.
void SDLCALL AudioBackend::OnDeviceRead(void* self, SDL_AudioStream*, int additional, int) {
    auto* backend = static_cast<AudioBackend*>(self);
    const int bytesPerMs = std::max(1, backend->m_spec.freq * backend->m_spec.channels * 2 / 1000);
    if (additional > 2 * bytesPerMs) {
        backend->m_dryReads.fetch_add(1, std::memory_order_relaxed);
        backend->m_dryBytes.fetch_add(static_cast<uint64_t>(additional), std::memory_order_relaxed);
    }
}

// Each block offered: when the device ran dry since the last one, count it once and (with the level control) fill
// the queue back to its target with silence (the gap is already heard; the sound that follows plays a target's worth
// ahead again); then move the playback rate toward keeping the target.
void AudioBackend::KeepLevelLocked(int queued) {
    const uint64_t dry = m_dryReads.load(std::memory_order_relaxed);
    if (dry != m_dryReadsSeen) {
        m_dryReadsSeen = dry;
        ++m_stats.ranDry;
        if (m_targetMs != 0) {
            const uint32_t target = BytesForMsLocked(m_targetMs);
            if (static_cast<uint32_t>(queued) < target) {
                const uint32_t frame = static_cast<uint32_t>(m_spec.channels) * sizeof(int16_t);
                uint32_t fill = target - static_cast<uint32_t>(queued);
                fill -= frame ? fill % frame : 0;
                if (m_silence.size() < fill) m_silence.assign(fill, 0);
                if (fill != 0 && SDL_PutAudioStreamData(m_stream, m_silence.data(), static_cast<int>(fill))) {
                    m_stats.refillBytes += fill;
                    DumpQueued(m_silence.data(), static_cast<int>(fill));
                    queued += static_cast<int>(fill);
                    m_levelMs = -1.0;  // the level starts over at the target
                }
            }
        }
    }
    if (m_targetMs == 0) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_last{};
    const Clock::time_point now = Clock::now();
    const double bytesPerMs = static_cast<double>(m_spec.freq) * m_spec.channels * sizeof(int16_t) / 1000.0;
    const double levelMs = bytesPerMs > 0 ? queued / bytesPerMs : 0.0;
    if (m_levelMs < 0) {
        m_levelMs = levelMs;
    } else {
        const double dt = std::chrono::duration<double>(now - s_last).count();
        const double a = 1.0 - std::exp(-std::max(0.0, dt) / kLevelSmoothingSeconds);
        m_levelMs += a * (levelMs - m_levelMs);
    }
    s_last = now;
    const double off = (m_levelMs - m_targetMs) / m_targetMs;  // above the target: play faster
    const float rate = 1.0f + std::clamp(static_cast<float>(off * kRateGain), -kMaxRateChange, kMaxRateChange);
    if (std::fabs(rate - m_rate) >= kRateStep) {
        if (SDL_SetAudioStreamFrequencyRatio(m_stream, rate)) {
            m_rate = rate;
            m_stats.rateMin = std::min(m_stats.rateMin, rate);
            m_stats.rateMax = std::max(m_stats.rateMax, rate);
        }
    }
}

bool AudioBackend::QueueHasCapacityLocked(int incomingBytes) {
    if (!m_stream) {
        return false;
    }
    int queued = SDL_GetAudioStreamQueued(m_stream);
    if (queued < 0) {
        return true;
    }
    ++m_stats.blocks;
    m_stats.queuedBytesSum += static_cast<uint64_t>(queued);
    m_stats.queuedBytesMax = std::max(m_stats.queuedBytesMax, static_cast<uint32_t>(queued));
    KeepLevelLocked(queued);
    queued = std::max(queued, SDL_GetAudioStreamQueued(m_stream));  // with a refill's silence
    const uint32_t maxQueued = QueueLimitBytesLocked();
    if (static_cast<uint64_t>(queued) + static_cast<uint64_t>(std::max(incomingBytes, 0)) > maxQueued) {
        ++m_stats.dropped;
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
    DumpQueued(m_convertBuffer.data(), lenBytes);

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
    DumpQueued(samples, lenBytes);

    return true;
}
