#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rps::audio {

// ---------------------------------------------------------------------------
// Audio device types
// ---------------------------------------------------------------------------

/// Information about an available audio device.
struct AudioDeviceInfo {
    std::string id;                                ///< Backend-specific device identifier
    std::string name;                              ///< Human-readable display name
    uint32_t maxInputChannels = 0;
    uint32_t maxOutputChannels = 0;
    std::vector<uint32_t> supportedSampleRates;    ///< Empty = any rate supported
    bool isDefault = false;
};

/// Configuration for opening an audio device.
struct AudioDeviceConfig {
    std::string deviceId;          ///< Empty = use default device
    uint32_t sampleRate = 48000;
    uint32_t blockSize = 128;
    uint32_t numInputChannels = 0; ///< 0 = no input (playback only)
    uint32_t numOutputChannels = 2;
};

/// Real-time audio callback. C function pointer — no std::function overhead
/// on the hot path. Called from the audio device thread.
///
/// @param input     Interleaved input samples (nullptr if no input channels)
/// @param output    Interleaved output buffer to fill
/// @param numFrames Number of sample frames per channel (= blockSize)
/// @param userData  User context pointer passed to open()
using AudioCallback = void(*)(const float* input, float* output,
                               uint32_t numFrames, void* userData);

// ---------------------------------------------------------------------------
// IAudioDevice — abstract interface for pluggable audio backends
// ---------------------------------------------------------------------------

/// Pluggable audio device interface.
///
/// Implementations:
///   - Sdl3AudioDevice   (all platforms, Phase 1)
///   - AsioAudioDevice   (Windows, future)
///   - WasapiAudioDevice (Windows, future)
///   - PipeWireAudioDevice (Linux, future)
///   - Jack2AudioDevice  (Linux, future)
///   - CoreAudioDevice   (macOS, future)
class IAudioDevice {
public:
    virtual ~IAudioDevice() = default;

    /// Human-readable backend name: "SDL3", "ASIO", "WASAPI", etc.
    virtual std::string backendName() const = 0;

    /// Enumerate available devices for this backend.
    virtual std::vector<AudioDeviceInfo> enumerateDevices() = 0;

    /// Open a device with the given configuration.
    /// The callback will be invoked on the real-time audio thread.
    /// @return true on success.
    virtual bool open(const AudioDeviceConfig& config,
                      AudioCallback callback, void* userData) = 0;

    /// Start the audio stream (callback begins firing).
    virtual bool start() = 0;

    /// Stop the audio stream (callback stops firing).
    virtual void stop() = 0;

    /// Close the device and release all resources.
    virtual void close() = 0;

    /// Whether the device is currently open and streaming.
    virtual bool isRunning() const = 0;

    /// Actual negotiated sample rate (may differ from requested).
    virtual uint32_t actualSampleRate() const = 0;

    /// Actual negotiated block size (may differ from requested).
    virtual uint32_t actualBlockSize() const = 0;
};

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

/// Create an audio device backend by name.
/// @param backend  Backend identifier: "sdl3", "asio", "wasapi", etc.
/// @return Device instance, or nullptr if the backend is not available.
std::unique_ptr<IAudioDevice> createAudioDevice(const std::string& backend);

/// List backend names available on this platform.
/// Always includes "sdl3". Platform-specific backends are included if compiled in.
std::vector<std::string> availableAudioBackends();

} // namespace rps::audio
