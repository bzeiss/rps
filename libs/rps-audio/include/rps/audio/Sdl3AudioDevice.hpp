#pragma once

#include <rps/audio/IAudioDevice.hpp>
#include <atomic>
#include <vector>
#include <SDL3/SDL.h>

namespace rps::audio {

/// SDL3-based audio device backend. Works on all SDL3-supported platforms.
///
/// Uses an internal linear buffer for reblocking: the user callback is always
/// invoked with exactly `blockSize` frames. A residual buffer accumulates
/// leftover samples between SDL callback invocations.
class Sdl3AudioDevice final : public IAudioDevice {
public:
    Sdl3AudioDevice();
    ~Sdl3AudioDevice() override;

    std::string backendName() const override;
    std::vector<AudioDeviceInfo> enumerateDevices() override;
    bool open(const AudioDeviceConfig& config,
              AudioCallback callback, void* userData) override;
    bool start() override;
    void stop() override;
    void close() override;
    bool isRunning() const override;
    uint32_t actualSampleRate() const override;
    uint32_t actualBlockSize() const override;

private:
    SDL_AudioStream* m_stream = nullptr;
    AudioCallback m_callback = nullptr;
    void* m_userData = nullptr;
    uint32_t m_sampleRate = 0;
    uint32_t m_blockSize = 0;
    uint32_t m_numOutputChannels = 0;
    uint32_t m_numInputChannels = 0;
    std::atomic<bool> m_running{false};

    // Pre-allocated buffer for one full block from the user callback
    std::vector<float> m_callbackBuf;

    // Residual buffer: leftover processed samples from the previous callback.
    // These are consumed first when SDL requests more data.
    std::vector<float> m_residual;
    uint32_t m_residualCount = 0;  // number of valid floats in m_residual

    // Debug counters
    std::atomic<uint64_t> m_callbackCount{0};
    std::atomic<uint64_t> m_blocksProcessed{0};
    std::atomic<uint64_t> m_underruns{0};

    static void SDLCALL sdlAudioCallback(void* userdata,
                                          SDL_AudioStream* stream,
                                          int additional_amount,
                                          int total_amount);
};

} // namespace rps::audio
