#include <rps/audio/IAudioDevice.hpp>
#include <rps/audio/Sdl3AudioDevice.hpp>
#include <algorithm>

namespace rps::audio {

std::unique_ptr<IAudioDevice> createAudioDevice(const std::string& backend) {
    if (backend == "sdl3") {
        return std::make_unique<Sdl3AudioDevice>();
    }
    // Future backends:
    // if (backend == "asio")     return std::make_unique<AsioAudioDevice>();
    // if (backend == "wasapi")   return std::make_unique<WasapiAudioDevice>();
    // if (backend == "pipewire") return std::make_unique<PipeWireAudioDevice>();
    // if (backend == "jack2")    return std::make_unique<Jack2AudioDevice>();
    // if (backend == "coreaudio") return std::make_unique<CoreAudioDevice>();
    return nullptr;
}

std::vector<std::string> availableAudioBackends() {
    std::vector<std::string> backends;

    // SDL3 is always available (required dependency)
    backends.push_back("sdl3");

    // Future: probe for platform-specific backends at runtime
    // #ifdef _WIN32
    //   backends.push_back("wasapi");
    //   if (asioAvailable()) backends.push_back("asio");
    // #endif
    // #ifdef __linux__
    //   if (pipewireAvailable()) backends.push_back("pipewire");
    //   if (jack2Available()) backends.push_back("jack2");
    // #endif
    // #ifdef __APPLE__
    //   backends.push_back("coreaudio");
    // #endif

    return backends;
}

} // namespace rps::audio
