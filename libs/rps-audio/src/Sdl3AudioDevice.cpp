#include <rps/audio/Sdl3AudioDevice.hpp>
#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>

namespace rps::audio {

Sdl3AudioDevice::Sdl3AudioDevice() = default;

Sdl3AudioDevice::~Sdl3AudioDevice() {
    close();
}

std::string Sdl3AudioDevice::backendName() const {
    return "SDL3";
}

std::vector<AudioDeviceInfo> Sdl3AudioDevice::enumerateDevices() {
    std::vector<AudioDeviceInfo> devices;

    int count = 0;
    auto* ids = SDL_GetAudioPlaybackDevices(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            AudioDeviceInfo info;
            info.id = std::to_string(ids[i]);

            const char* name = SDL_GetAudioDeviceName(ids[i]);
            info.name = name ? name : "Unknown";
            info.maxOutputChannels = 2;
            info.maxInputChannels = 0;
            info.isDefault = (i == 0);
            devices.push_back(std::move(info));
        }
        SDL_free(ids);
    }

    return devices;
}

bool Sdl3AudioDevice::open(const AudioDeviceConfig& config,
                            AudioCallback callback, void* userData) {
    if (m_stream) {
        spdlog::warn("SDL3AudioDevice: already open, closing first");
        close();
    }

    m_callback = callback;
    m_userData = userData;
    m_sampleRate = config.sampleRate;
    m_blockSize = config.blockSize;
    m_numOutputChannels = config.numOutputChannels;
    m_numInputChannels = config.numInputChannels;

    // Pre-allocate callback buffer (one full block)
    const uint32_t blockFloats = m_blockSize * m_numOutputChannels;
    m_callbackBuf.resize(blockFloats, 0.0f);

    // Pre-allocate residual buffer (max one full block of leftovers)
    m_residual.resize(blockFloats, 0.0f);
    m_residualCount = 0;

    m_callbackCount.store(0, std::memory_order_relaxed);
    m_blocksProcessed.store(0, std::memory_order_relaxed);
    m_underruns.store(0, std::memory_order_relaxed);

    SDL_AudioSpec spec{};
    spec.freq = static_cast<int>(config.sampleRate);
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<int>(config.numOutputChannels);

    m_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        sdlAudioCallback,
        this
    );

    if (!m_stream) {
        spdlog::error("SDL3AudioDevice: SDL_OpenAudioDeviceStream failed: {}",
                      SDL_GetError());
        return false;
    }

    spdlog::info("SDL3AudioDevice: opened ({}Hz, {} ch, bs={})",
                 m_sampleRate, m_numOutputChannels, m_blockSize);
    return true;
}

bool Sdl3AudioDevice::start() {
    if (!m_stream) {
        spdlog::error("SDL3AudioDevice: cannot start, not open");
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(m_stream)) {
        spdlog::error("SDL3AudioDevice: SDL_ResumeAudioStreamDevice failed: {}",
                      SDL_GetError());
        return false;
    }

    m_running.store(true, std::memory_order_relaxed);
    spdlog::info("SDL3AudioDevice: started");
    return true;
}

void Sdl3AudioDevice::stop() {
    if (m_stream) {
        SDL_PauseAudioStreamDevice(m_stream);
        m_running.store(false, std::memory_order_relaxed);

        auto cbCount = m_callbackCount.load(std::memory_order_relaxed);
        auto blocks = m_blocksProcessed.load(std::memory_order_relaxed);
        auto underruns = m_underruns.load(std::memory_order_relaxed);
        spdlog::info("SDL3AudioDevice: stopped (callbacks={}, blocks_processed={}, underruns={})",
                     cbCount, blocks, underruns);
    }
}

void Sdl3AudioDevice::close() {
    if (m_stream) {
        stop();
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        spdlog::info("SDL3AudioDevice: closed");
    }
    m_callback = nullptr;
    m_userData = nullptr;
}

bool Sdl3AudioDevice::isRunning() const {
    return m_running.load(std::memory_order_relaxed);
}

uint32_t Sdl3AudioDevice::actualSampleRate() const {
    return m_sampleRate;
}

uint32_t Sdl3AudioDevice::actualBlockSize() const {
    return m_blockSize;
}

void SDLCALL Sdl3AudioDevice::sdlAudioCallback(
        void* userdata, SDL_AudioStream* /*stream*/,
        int additional_amount, int /*total_amount*/) {
    auto* self = static_cast<Sdl3AudioDevice*>(userdata);
    if (!self->m_callback || additional_amount <= 0) return;

    const uint32_t ch = self->m_numOutputChannels;
    const uint32_t bytesPerFloat = sizeof(float);
    const uint32_t bytesPerFrame = ch * bytesPerFloat;
    if (bytesPerFrame == 0) return;

    const uint32_t blockSize = self->m_blockSize;
    const uint32_t blockFloats = blockSize * ch;
    uint32_t floatsNeeded = static_cast<uint32_t>(additional_amount) / bytesPerFloat;

    self->m_callbackCount.fetch_add(1, std::memory_order_relaxed);

    // Step 1: Consume any residual samples from the previous callback
    if (self->m_residualCount > 0 && floatsNeeded > 0) {
        const uint32_t consume = std::min(self->m_residualCount, floatsNeeded);
        SDL_PutAudioStreamData(self->m_stream,
                               self->m_residual.data(),
                               static_cast<int>(consume * bytesPerFloat));
        // Shift remaining residual to front (if any)
        if (consume < self->m_residualCount) {
            std::memmove(self->m_residual.data(),
                         self->m_residual.data() + consume,
                         (self->m_residualCount - consume) * bytesPerFloat);
        }
        self->m_residualCount -= consume;
        floatsNeeded -= consume;
    }

    // Step 2: Process full blocks until we've satisfied the request
    while (floatsNeeded > 0) {
        float* buf = self->m_callbackBuf.data();
        std::memset(buf, 0, blockFloats * bytesPerFloat);

        // Always process exactly blockSize frames
        self->m_callback(nullptr, buf, blockSize, self->m_userData);
        self->m_blocksProcessed.fetch_add(1, std::memory_order_relaxed);

        if (floatsNeeded >= blockFloats) {
            // Full block fits — send directly to SDL
            SDL_PutAudioStreamData(self->m_stream,
                                   buf,
                                   static_cast<int>(blockFloats * bytesPerFloat));
            floatsNeeded -= blockFloats;
        } else {
            // Partial block — send what's needed, save the rest as residual
            SDL_PutAudioStreamData(self->m_stream,
                                   buf,
                                   static_cast<int>(floatsNeeded * bytesPerFloat));

            // Save the remainder
            self->m_residualCount = blockFloats - floatsNeeded;
            std::memcpy(self->m_residual.data(),
                        buf + floatsNeeded,
                        self->m_residualCount * bytesPerFloat);
            floatsNeeded = 0;
        }
    }
}

} // namespace rps::audio
