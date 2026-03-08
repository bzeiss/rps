#include <rps/gui/GuiWorkerMain.hpp>
#include <rps/gui/SdlWindow.hpp>
#include <rps/ipc/Connection.hpp>
#include <rps/ipc/Messages.hpp>
#include <rps/audio/SharedAudioRing.hpp>
#include <rps/audio/IAudioDevice.hpp>
#include <SDL3/SDL.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#endif
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "Avrt.lib")
#endif

namespace po = boost::program_options;

namespace rps::gui {

#ifdef _WIN32
/// Thread-local storage for exception info copied during SEH filter phase.
struct CapturedExceptionInfo {
    DWORD code = 0;
    ULONG_PTR instruction = 0;
    ULONG_PTR accessType = 0; // 0=read, 1=write (for ACCESS_VIOLATION)
    ULONG_PTR faultAddress = 0;
    DWORD numParams = 0;
};
static thread_local CapturedExceptionInfo s_capturedEx;

/// SEH filter that copies exception data during filter phase (before unwinding).
static LONG captureExceptionFilter(EXCEPTION_POINTERS* ep) {
    auto* rec = ep->ExceptionRecord;
    s_capturedEx.code = rec->ExceptionCode;
    s_capturedEx.instruction = reinterpret_cast<ULONG_PTR>(rec->ExceptionAddress);
    s_capturedEx.numParams = rec->NumberParameters;
    if (rec->NumberParameters >= 1) s_capturedEx.accessType = rec->ExceptionInformation[0];
    if (rec->NumberParameters >= 2) s_capturedEx.faultAddress = rec->ExceptionInformation[1];
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/// Context passed to the IAudioDevice callback (real-time path).
/// The SDL callback reads from playbackRing (pre-processed audio).
/// A separate worker thread does: readInputBlock → processAudioBlock → write playbackRing.
struct AudioDeviceContext {
    uint32_t blockSize = 0;
    uint32_t outChannels = 0;

    // Lock-free SPSC playback ring (written by worker, read by SDL callback).
    // Fixed-size circular buffer of interleaved float blocks.
    static constexpr uint32_t kPlaybackRingBlocks = 16;
    std::vector<float> playbackRing;       // kPlaybackRingBlocks * blockSize * outChannels
    std::atomic<uint32_t> playbackWrite{0};
    std::atomic<uint32_t> playbackRead{0};

    // Debug counters
    std::atomic<uint64_t> callbackCount{0};
    std::atomic<uint64_t> blocksConsumed{0};
    std::atomic<uint64_t> underruns{0};
};

// --- Toolbar-controlled audio processing flags (set from UI thread) ---
static std::atomic<bool> g_bypassActive{false};
static std::atomic<bool> g_deltaActive{false};

/// Simple circular delay buffer for latency compensation in delta mode.
/// Delays an interleaved float buffer by a fixed number of samples.
class DelaySamples {
public:
    void resize(uint32_t delaySamples, uint32_t numChannels) {
        m_numChannels = numChannels;
        m_delaySamples = delaySamples;
        m_totalFloats = delaySamples * numChannels;
        if (m_totalFloats > 0) {
            m_buffer.resize(m_totalFloats, 0.0f);
        } else {
            m_buffer.clear();
        }
        m_writePos = 0;
    }

    /// Push one block of interleaved samples into the delay, returning the delayed output.
    /// @param input  Current block (numSamples * numChannels interleaved floats)
    /// @param output Delayed block (same size)
    /// @param numSamples Number of audio frames in this block
    void process(const float* input, float* output, uint32_t numSamples) {
        if (m_totalFloats == 0) {
            // Zero delay — just copy
            std::memcpy(output, input, numSamples * m_numChannels * sizeof(float));
            return;
        }
        for (uint32_t i = 0; i < numSamples * m_numChannels; ++i) {
            output[i] = m_buffer[m_writePos];
            m_buffer[m_writePos] = input[i];
            m_writePos = (m_writePos + 1) % m_totalFloats;
        }
    }

private:
    std::vector<float> m_buffer;
    uint32_t m_numChannels = 0;
    uint32_t m_delaySamples = 0;
    uint32_t m_totalFloats = 0;
    uint32_t m_writePos = 0;
};

/// Real-time audio device callback.
/// Only reads pre-processed audio from the playback ring — NO plugin processing here.
static void audioDeviceCallback(const float* /*input*/, float* output,
                                 uint32_t numFrames, void* userData) {
    auto* ctx = static_cast<AudioDeviceContext*>(userData);
    if (!ctx || !output) return;

    const uint32_t blockFloats = ctx->blockSize * ctx->outChannels;
    const uint32_t outFloats = numFrames * ctx->outChannels;
    ctx->callbackCount.fetch_add(1, std::memory_order_relaxed);

    // Try to read one block from the playback ring
    uint32_t rd = ctx->playbackRead.load(std::memory_order_acquire);
    uint32_t wr = ctx->playbackWrite.load(std::memory_order_acquire);

    if (rd != wr) {
        // Data available — copy to output
        uint32_t slot = rd % AudioDeviceContext::kPlaybackRingBlocks;
        const float* src = ctx->playbackRing.data() + slot * blockFloats;
        const uint32_t framesToCopy = std::min(numFrames, ctx->blockSize);
        std::memcpy(output, src, framesToCopy * ctx->outChannels * sizeof(float));
        // Zero any remaining frames if SDL requested more than blockSize
        if (framesToCopy < numFrames) {
            std::memset(output + framesToCopy * ctx->outChannels, 0,
                        (numFrames - framesToCopy) * ctx->outChannels * sizeof(float));
        }
        ctx->playbackRead.store(rd + 1, std::memory_order_release);
        ctx->blocksConsumed.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Underrun — output silence
        std::memset(output, 0, outFloats * sizeof(float));
        ctx->underruns.fetch_add(1, std::memory_order_relaxed);
    }
}

int GuiWorkerMain::run(int argc, char* argv[], std::unique_ptr<IPluginGuiHost> host) {
    // Parse command-line arguments
    po::options_description desc("Plugin GUI Host Worker");
    desc.add_options()
        ("ipc-id", po::value<std::string>(), "IPC queue identifier")
        ("plugin-path", po::value<std::string>(), "Path to plugin binary")
        ("format", po::value<std::string>()->default_value("clap"), "Plugin format")
        ("audio-shm", po::value<std::string>(), "Shared memory segment name for audio processing")
        ("audio-device", po::value<std::string>(), "Audio device backend (e.g. sdl3)")
        ("help,h", "Show help");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error parsing arguments: " << e.what() << "\n";
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

    if (!vm.count("ipc-id") || !vm.count("plugin-path")) {
        std::cerr << "Error: --ipc-id and --plugin-path are required.\n";
        return 1;
    }

    const auto ipcId = vm["ipc-id"].as<std::string>();
    const auto pluginPath = vm["plugin-path"].as<std::string>();
    const auto format = vm["format"].as<std::string>();

    // Set up logging — both console and file for crash diagnostics
    try {
        auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            "rps-pluginhost.log", true);
        auto logger = std::make_shared<spdlog::logger>(
            "pluginhost", spdlog::sinks_init_list{consoleSink, fileSink});
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::debug);  // Flush every message for crash diagnostics
        // Include format in every log line for easy identification
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [" + format + "] %v");
        spdlog::set_default_logger(logger);
    } catch (...) {
        // Fall back to default logger
        spdlog::set_level(spdlog::level::debug);
    }

    spdlog::info("=== rps-pluginhost [{}] starting ===", format);
    spdlog::info("  ipc-id: {}", ipcId);
    spdlog::info("  plugin-path: {}", pluginPath);

    // Connect to IPC queue
    spdlog::info("Connecting to IPC queue...");
    auto connection = rps::ipc::MessageQueueConnection::createClient(ipcId);
    if (!connection) {
        spdlog::error("Failed to connect to IPC queue: {}", ipcId);
        return 1;
    }
    spdlog::info("IPC connected");

    // Initialize SDL3 — only audio subsystem if needed; VIDEO deferred to ShowGui
    const std::string audioDeviceBackend = vm.count("audio-device")
        ? vm["audio-device"].as<std::string>() : "";

    if (audioDeviceBackend == "sdl3") {
        spdlog::info("Initializing SDL3 audio subsystem...");
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            spdlog::error("SDL_Init(AUDIO) failed: {}", SDL_GetError());
        } else {
            spdlog::info("SDL3 audio initialized");
        }
    }

    try {
        // Load the plugin headlessly (no GUI, no window)
        spdlog::info("Loading plugin (headless)...");
        host->loadPlugin(boost::filesystem::path(pluginPath));
        spdlog::info("Plugin loaded: '{}'", host->getPluginName());

        // Send PluginLoadedEvent
        rps::ipc::Message loadedMsg;
        loadedMsg.type = rps::ipc::MessageType::PluginLoadedEvent;
        loadedMsg.payload = rps::ipc::PluginLoadedEvent{host->getPluginName(), "", true};
        connection->sendMessage(loadedMsg);

        // Query and send full parameter list
        auto params = host->getParameters();
        if (!params.empty()) {
            spdlog::info("Sending ParameterListEvent ({} params)", params.size());
            rps::ipc::Message paramListMsg;
            paramListMsg.type = rps::ipc::MessageType::ParameterListEvent;
            paramListMsg.payload = rps::ipc::ParameterListEvent{std::move(params)};
            connection->sendMessage(paramListMsg);
        }

        // Query and send preset list
        auto presets = host->getPresets();
        if (!presets.empty()) {
            spdlog::info("Sending PresetListEvent ({} presets)", presets.size());
            rps::ipc::Message presetListMsg;
            presetListMsg.type = rps::ipc::MessageType::PresetListEvent;
            presetListMsg.payload = rps::ipc::PresetListEvent{std::move(presets)};
            connection->sendMessage(presetListMsg);
        }

        // === Audio processing setup ===
        std::unique_ptr<rps::audio::SharedAudioRing> audioRing;
        std::atomic<bool> audioShutdown{false};
        std::thread audioThread;
        rps::gui::AudioBusLayout audioLayout{};
        std::unique_ptr<rps::audio::IAudioDevice> audioDevice;
        AudioDeviceContext deviceCtx;

        if (vm.count("audio-shm")) {
            const auto shmName = vm["audio-shm"].as<std::string>();
            spdlog::info("Opening audio shared memory: {}", shmName);

            try {
                audioRing = rps::audio::SharedAudioRing::open(shmName);
            } catch (const std::exception& e) {
                spdlog::error("Failed to open audio shared memory '{}': {}", shmName, e.what());
                audioRing.reset();
            }

            if (audioRing && host->supportsAudioProcessing()) {
                const auto& hdr = audioRing->header();
                auto layoutOpt = host->setupAudioProcessing(
                    hdr.sampleRate, hdr.blockSize, hdr.numChannels);

                if (layoutOpt) {
                    audioLayout = *layoutOpt;
                    spdlog::info("Audio processing setup: {} in -> {} out",
                                 audioLayout.numInputChannels, audioLayout.numOutputChannels);

                    const uint32_t inFloats = hdr.blockSize * audioLayout.numInputChannels;
                    const uint32_t outFloats = hdr.blockSize * audioLayout.numOutputChannels;

                    // ---- Real-time device mode ----
                    if (!audioDeviceBackend.empty()) {
                        audioDevice = rps::audio::createAudioDevice(audioDeviceBackend);
                        if (audioDevice) {
                            // Set up device context for the callback (playback ring only)
                            const uint32_t blockFloats = hdr.blockSize * audioLayout.numOutputChannels;
                            deviceCtx.blockSize = hdr.blockSize;
                            deviceCtx.outChannels = audioLayout.numOutputChannels;
                            deviceCtx.playbackRing.resize(
                                AudioDeviceContext::kPlaybackRingBlocks * blockFloats, 0.0f);

                            rps::audio::AudioDeviceConfig devConfig;
                            devConfig.sampleRate = hdr.sampleRate;
                            devConfig.blockSize = hdr.blockSize;
                            devConfig.numOutputChannels = audioLayout.numOutputChannels;

                            if (audioDevice->open(devConfig, audioDeviceCallback, &deviceCtx)) {
                                audioDevice->start();
                                spdlog::info("Audio device started: {} ({}Hz, bs={})",
                                             audioDevice->backendName(),
                                             audioDevice->actualSampleRate(),
                                             audioDevice->actualBlockSize());

                                // Start a worker thread that reads from input ring,
                                // processes audio via the plugin, and writes to both
                                // the playback ring (for SDL) and the output ring (for Python).
                                // Use 8MB stack — plugins like FabFilter need more than the
                                // default 1MB for heavy DSP (oversampling, lookahead, etc.).
                                struct WorkerArgs {
                                    std::unique_ptr<rps::audio::SharedAudioRing>* audioRing;
                                    std::unique_ptr<rps::gui::IPluginGuiHost>* host;
                                    std::atomic<bool>* audioShutdown;
                                    AudioDeviceContext* deviceCtx;
                                    uint32_t inFloats, outFloats, bs, inCh, outCh;
                                    uint32_t latencySamples;
                                };
                                auto* workerArgs = new WorkerArgs{
                                    &audioRing, &host, &audioShutdown, &deviceCtx,
                                    inFloats, outFloats, hdr.blockSize,
                                    audioLayout.numInputChannels, audioLayout.numOutputChannels,
                                    host->getLatencySamples()
                                };

                                // Worker body — captureless, castable to LPTHREAD_START_ROUTINE.
                                // On x64 Windows __stdcall == __cdecl, so the cast is safe.
                                auto workerBody = [](LPVOID param) -> DWORD {
                                    auto* args = static_cast<WorkerArgs*>(param);
                                    auto& audioRing = *args->audioRing;
                                    auto& host = *args->host;
                                    auto& audioShutdown = *args->audioShutdown;
                                    auto& deviceCtx = *args->deviceCtx;
                                    const uint32_t inFloats = args->inFloats;
                                    const uint32_t outFloats = args->outFloats;
                                    const uint32_t bs = args->bs;
                                    const uint32_t inCh = args->inCh;
                                    const uint32_t outCh = args->outCh;
                                    const uint32_t latencySamples = args->latencySamples;
                                    delete args;

                                    spdlog::info("Audio worker thread started (device mode, 8MB stack)");

                                    DWORD taskIndex = 0;
                                    auto hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
                                    if (hTask) {
                                        spdlog::info("Audio worker thread elevated to Pro Audio priority");
                                    }

                                    // Delay buffer for delta mode latency compensation
                                    DelaySamples deltaDelay;
                                    deltaDelay.resize(latencySamples, inCh);
                                    spdlog::info("Delta delay buffer: {} samples", latencySamples);

                                    const uint32_t blockFloats = bs * outCh;
                                    std::vector<float> inputBuf(inFloats);
                                    std::vector<float> outputBuf(outFloats);
                                    std::vector<float> dryCopy(inFloats);   // for bypass/delta
                                    std::vector<float> dryDelayed(inFloats); // delay-compensated dry
                                    bool processingOk = true;

                                    while (!audioShutdown.load(std::memory_order_relaxed)) {
                                        if (!processingOk) {
                                            // After a crash, just drain input and output silence
                                            if (audioRing->waitForInput(std::chrono::milliseconds(10))) {
                                                if (audioRing->readInputBlock(inputBuf.data())) {
                                                    std::fill(outputBuf.begin(), outputBuf.end(), 0.0f);
                                                    audioRing->writeOutputBlock(outputBuf.data());
                                                }
                                            }
                                            continue;
                                        }
                                        if (audioRing->waitForInput(std::chrono::milliseconds(10))) {
                                            if (audioRing->readInputBlock(inputBuf.data())) {
                                                bool bypass = g_bypassActive.load(std::memory_order_relaxed);
                                                bool delta = g_deltaActive.load(std::memory_order_relaxed);

                                                // Save dry copy for bypass/delta
                                                if (bypass || delta) {
                                                    std::memcpy(dryCopy.data(), inputBuf.data(), inFloats * sizeof(float));
                                                }

                                                if (bypass) {
                                                    // Send silence to plugin (keep it alive)
                                                    std::vector<float> silenceBuf(inFloats, 0.0f);
                                                    [&]() {
                                                        __try {
                                                            host->processAudioBlock(
                                                                silenceBuf.data(), outputBuf.data(),
                                                                inCh, outCh, bs);
                                                        } __except(captureExceptionFilter(GetExceptionInformation())) {
                                                            spdlog::error("SEH exception 0x{:08X} during bypass process",
                                                                          s_capturedEx.code);
                                                            processingOk = false;
                                                        }
                                                    }();
                                                    // Output = dry input (bypass)
                                                    // Copy input channels to output (handles in != out channel count)
                                                    for (uint32_t s = 0; s < bs; ++s) {
                                                        for (uint32_t c = 0; c < outCh; ++c) {
                                                            outputBuf[s * outCh + c] = (c < inCh)
                                                                ? dryCopy[s * inCh + c] : 0.0f;
                                                        }
                                                    }
                                                } else {
                                                    // Normal processing
                                                    [&]() {
                                                        __try {
                                                            host->processAudioBlock(
                                                                inputBuf.data(), outputBuf.data(),
                                                                inCh, outCh, bs);
                                                        } __except(captureExceptionFilter(GetExceptionInformation())) {
                                                            if (s_capturedEx.code == 0xC0000005 && s_capturedEx.numParams >= 2) {
                                                                spdlog::error(
                                                                    "ACCESS_VIOLATION: {} at address 0x{:016X}, instruction at 0x{:016X}",
                                                                    s_capturedEx.accessType == 0 ? "READ" : "WRITE",
                                                                    s_capturedEx.faultAddress,
                                                                    s_capturedEx.instruction);
                                                            } else {
                                                                spdlog::error(
                                                                    "SEH exception 0x{:08X} at instruction 0x{:016X}",
                                                                    s_capturedEx.code,
                                                                    s_capturedEx.instruction);
                                                            }
                                                            spdlog::default_logger()->flush();
                                                            processingOk = false;
                                                            std::fill(outputBuf.begin(), outputBuf.end(), 0.0f);
                                                        }
                                                    }();

                                                    // Delta: output = wet - dry_delayed
                                                    if (delta && processingOk) {
                                                        deltaDelay.process(dryCopy.data(), dryDelayed.data(), bs);
                                                        for (uint32_t s = 0; s < bs; ++s) {
                                                            for (uint32_t c = 0; c < outCh; ++c) {
                                                                float dry = (c < inCh)
                                                                    ? dryDelayed[s * inCh + c] : 0.0f;
                                                                outputBuf[s * outCh + c] -= dry;
                                                            }
                                                        }
                                                    }
                                                }

                                                // Write to output ring (for Python collection)
                                                audioRing->writeOutputBlock(outputBuf.data());

                                                // Write to playback ring (for SDL callback)
                                                uint32_t wr = deviceCtx.playbackWrite.load(std::memory_order_relaxed);
                                                uint32_t rd = deviceCtx.playbackRead.load(std::memory_order_acquire);
                                                if (wr - rd < AudioDeviceContext::kPlaybackRingBlocks) {
                                                    uint32_t slot = wr % AudioDeviceContext::kPlaybackRingBlocks;
                                                    std::memcpy(
                                                        deviceCtx.playbackRing.data() + slot * blockFloats,
                                                        outputBuf.data(),
                                                        blockFloats * sizeof(float));
                                                    deviceCtx.playbackWrite.store(wr + 1, std::memory_order_release);
                                                }
                                                // else: playback ring full, skip (SDL will under-run gracefully)
                                            }
                                        }
                                    }

                                    spdlog::info("Audio worker thread exiting");
                                    return 0;
                                };

                                // Launch with 8MB stack via CreateThread
                                constexpr SIZE_T kWorkerStackSize = 8 * 1024 * 1024; // 8MB
                                HANDLE hWorker = CreateThread(
                                    nullptr, kWorkerStackSize,
                                    static_cast<LPTHREAD_START_ROUTINE>(+workerBody),
                                    workerArgs, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
                                if (hWorker) {
                                    spdlog::info("Audio worker thread created with 8MB stack");
                                    // Wrap the native handle so we can join later
                                    audioThread = std::thread([hWorker]() {
                                        WaitForSingleObject(hWorker, INFINITE);
                                        CloseHandle(hWorker);
                                    });
                                } else {
                                    spdlog::error("Failed to create audio worker thread");
                                    delete workerArgs;
                                }
                            } else {
                                spdlog::error("Failed to open audio device: {}",
                                              audioDeviceBackend);
                                audioDevice.reset();
                            }
                        } else {
                            spdlog::error("Unknown audio device backend: {}",
                                          audioDeviceBackend);
                        }
                    }

                    // ---- Offline polling mode (no device, or device failed) ----
                    if (!audioDevice) {
                        // Capture latency before thread starts
                        uint32_t latSamples = host->getLatencySamples();
                        audioThread = std::thread([&audioRing, &host, &audioShutdown,
                                                   inFloats, outFloats,
                                                   bs = hdr.blockSize,
                                                   inCh = audioLayout.numInputChannels,
                                                   outCh = audioLayout.numOutputChannels,
                                                   latSamples]() {
                            spdlog::info("Audio thread started (polling mode)");

#ifdef _WIN32
                            // AVRT for Pro Audio thread priority
                            DWORD taskIndex = 0;
                            auto hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
                            if (hTask) {
                                spdlog::info("Audio thread elevated to Pro Audio priority");
                            }
#endif

                            // Delay buffer for delta mode latency compensation
                            DelaySamples deltaDelay;
                            deltaDelay.resize(latSamples, inCh);

                            std::vector<float> inputBuf(inFloats);
                            std::vector<float> outputBuf(outFloats);
                            std::vector<float> dryCopy(inFloats);
                            std::vector<float> dryDelayed(inFloats);

                            while (!audioShutdown.load(std::memory_order_relaxed)) {
                                if (audioRing->waitForInput(std::chrono::milliseconds(10))) {
                                    if (audioRing->readInputBlock(inputBuf.data())) {
                                        bool bypass = g_bypassActive.load(std::memory_order_relaxed);
                                        bool delta = g_deltaActive.load(std::memory_order_relaxed);

                                        if (bypass || delta) {
                                            std::memcpy(dryCopy.data(), inputBuf.data(), inFloats * sizeof(float));
                                        }

                                        if (bypass) {
                                            // Send silence to plugin, output dry
                                            std::vector<float> silenceBuf(inFloats, 0.0f);
                                            host->processAudioBlock(
                                                silenceBuf.data(), outputBuf.data(),
                                                inCh, outCh, bs);
                                            for (uint32_t s = 0; s < bs; ++s) {
                                                for (uint32_t c = 0; c < outCh; ++c) {
                                                    outputBuf[s * outCh + c] = (c < inCh)
                                                        ? dryCopy[s * inCh + c] : 0.0f;
                                                }
                                            }
                                        } else {
                                            host->processAudioBlock(
                                                inputBuf.data(), outputBuf.data(),
                                                inCh, outCh, bs);

                                            if (delta) {
                                                deltaDelay.process(dryCopy.data(), dryDelayed.data(), bs);
                                                for (uint32_t s = 0; s < bs; ++s) {
                                                    for (uint32_t c = 0; c < outCh; ++c) {
                                                        float dry = (c < inCh)
                                                            ? dryDelayed[s * inCh + c] : 0.0f;
                                                        outputBuf[s * outCh + c] -= dry;
                                                    }
                                                }
                                            }
                                        }

                                        // Spin-wait until output ring has space
                                        while (!audioShutdown.load(std::memory_order_relaxed)) {
                                            if (audioRing->writeOutputBlock(outputBuf.data())) {
                                                break;
                                            }
                                            std::this_thread::yield();
                                        }
                                    }
                                }
                            }

                            spdlog::info("Audio thread exiting");
                        });
                    }
                } else {
                    spdlog::warn("Plugin does not support audio processing");
                }
            }
        }

        // =================================================================
        // Headless IPC command loop — process stays alive here.
        // GUI is opened on demand via ShowGuiRequest.
        // =================================================================
        spdlog::info("Entering headless IPC command loop...");
        bool sessionRunning = true;

        while (sessionRunning) {
            auto msg = connection->receiveMessage(200);

            // Check for async preset enrichment completion
            if (host->hasEnrichedPresets()) {
                auto enriched = host->getEnrichedPresets();
                spdlog::info("Sending enriched PresetListEvent ({} presets)", enriched.size());
                rps::ipc::Message presetMsg;
                presetMsg.type = rps::ipc::MessageType::PresetListEvent;
                presetMsg.payload = rps::ipc::PresetListEvent{std::move(enriched)};
                connection->sendMessage(presetMsg);
            }

            if (!msg) continue;

            // --- ShowGuiRequest: open window, run event loop, return here ---
            if (msg->type == rps::ipc::MessageType::ShowGuiRequest) {
                spdlog::info("Received ShowGuiRequest — opening GUI window");

                // Initialize SDL VIDEO if not already done
                if (!SDL_WasInit(SDL_INIT_VIDEO)) {
                    if (!SDL_Init(SDL_INIT_VIDEO)) {
                        spdlog::error("SDL_Init(VIDEO) failed: {}", SDL_GetError());
                        rps::ipc::Message errMsg;
                        errMsg.type = rps::ipc::MessageType::GuiClosedEvent;
                        errMsg.payload = rps::ipc::GuiClosedEvent{"sdl_init_failed"};
                        connection->sendMessage(errMsg);
                        continue;
                    }
                }

                try {
                    auto result = host->open(boost::filesystem::path(pluginPath));
                    spdlog::info("GUI opened: '{}' ({}x{})", result.name, result.width, result.height);

                    // Send GuiOpenedEvent
                    rps::ipc::Message openedMsg;
                    openedMsg.type = rps::ipc::MessageType::GuiOpenedEvent;
                    openedMsg.payload = rps::ipc::GuiOpenedEvent{result.name, result.width, result.height};
                    connection->sendMessage(openedMsg);

                    // IPC listener thread for commands while GUI is open
                    std::atomic<bool> guiIpcDone{false};
                    std::thread guiIpcThread([&]() {
                        while (!guiIpcDone.load(std::memory_order_relaxed)) {
                            auto guiMsg = connection->receiveMessage(200);
                            if (!guiMsg) continue;

                            if (guiMsg->type == rps::ipc::MessageType::CloseGuiRequest) {
                                spdlog::info("Received CloseGuiRequest via IPC");
                                host->requestClose();
                                break;
                            }
                            if (guiMsg->type == rps::ipc::MessageType::CloseSessionRequest) {
                                spdlog::info("Received CloseSessionRequest via IPC (during GUI)");
                                host->requestClose();
                                sessionRunning = false;
                                break;
                            }
                            if (guiMsg->type == rps::ipc::MessageType::GetStateRequest) {
                                auto resp = host->saveState();
                                rps::ipc::Message respMsg;
                                respMsg.type = rps::ipc::MessageType::GetStateResponse;
                                respMsg.payload = std::move(resp);
                                connection->sendMessage(respMsg);
                            }
                            if (guiMsg->type == rps::ipc::MessageType::SetStateRequest) {
                                auto& req = std::get<rps::ipc::SetStateRequest>(guiMsg->payload);
                                auto resp = host->loadState(req.stateData);
                                rps::ipc::Message respMsg;
                                respMsg.type = rps::ipc::MessageType::SetStateResponse;
                                respMsg.payload = std::move(resp);
                                connection->sendMessage(respMsg);
                                if (resp.success) {
                                    auto params = host->getParameters();
                                    rps::ipc::Message paramMsg;
                                    paramMsg.type = rps::ipc::MessageType::ParameterListEvent;
                                    paramMsg.payload = rps::ipc::ParameterListEvent{std::move(params)};
                                    connection->sendMessage(paramMsg);
                                }
                            }
                            if (guiMsg->type == rps::ipc::MessageType::LoadPresetRequest) {
                                auto& req = std::get<rps::ipc::LoadPresetRequest>(guiMsg->payload);
                                auto resp = host->loadPreset(req.presetId);
                                rps::ipc::Message respMsg;
                                respMsg.type = rps::ipc::MessageType::LoadPresetResponse;
                                respMsg.payload = std::move(resp);
                                connection->sendMessage(respMsg);
                                if (resp.success) {
                                    auto params = host->getParameters();
                                    rps::ipc::Message paramMsg;
                                    paramMsg.type = rps::ipc::MessageType::ParameterListEvent;
                                    paramMsg.payload = rps::ipc::ParameterListEvent{std::move(params)};
                                    connection->sendMessage(paramMsg);
                                }
                            }
                        }
                    });

                    // Wire toolbar callbacks
                    host->setToolbarCallbacks(rps::gui::ToolbarCallbacks{
                        .onBypassChanged = [](bool active) {
                            g_bypassActive.store(active, std::memory_order_relaxed);
                            spdlog::info("Toolbar: bypass {}", active ? "ON" : "OFF");
                        },
                        .onDeltaChanged = [](bool active) {
                            g_deltaActive.store(active, std::memory_order_relaxed);
                            spdlog::info("Toolbar: delta {}", active ? "ON" : "OFF");
                        }
                    });

                    // Parameter change callback
                    auto paramChangeCb = [&connection](std::vector<rps::ipc::ParameterValueUpdate> changes) {
                        rps::ipc::Message pmsg;
                        pmsg.type = rps::ipc::MessageType::ParameterValuesEvent;
                        pmsg.payload = rps::ipc::ParameterValuesEvent{std::move(changes)};
                        connection->sendMessage(pmsg);
                    };

                    std::string closeReason;
                    host->runEventLoop(
                        [&](const std::string& reason) { closeReason = reason; },
                        paramChangeCb
                    );

                    spdlog::info("GUI event loop exited (reason: {})", closeReason.empty() ? "user" : closeReason);

                    guiIpcDone.store(true, std::memory_order_relaxed);
                    if (guiIpcThread.joinable()) guiIpcThread.join();

                    // Tear down GUI resources (window + plugin GUI extension)
                    // but keep the plugin loaded for headless operation
                    host->destroyGui();

                    // Send GuiClosedEvent — but process stays alive
                    rps::ipc::Message closedMsg;
                    closedMsg.type = rps::ipc::MessageType::GuiClosedEvent;
                    closedMsg.payload = rps::ipc::GuiClosedEvent{closeReason.empty() ? "user" : closeReason};
                    connection->sendMessage(closedMsg);

                    spdlog::info("GUI closed, returning to headless command loop");

                } catch (const std::exception& e) {
                    spdlog::error("GUI open error: {}", e.what());
                    rps::ipc::Message errMsg;
                    errMsg.type = rps::ipc::MessageType::GuiClosedEvent;
                    errMsg.payload = rps::ipc::GuiClosedEvent{"crash"};
                    connection->sendMessage(errMsg);
                }
                continue;
            }

            // --- CloseSessionRequest: terminate the process ---
            if (msg->type == rps::ipc::MessageType::CloseSessionRequest ||
                msg->type == rps::ipc::MessageType::CloseGuiRequest) {
                spdlog::info("Received session close request via IPC");
                sessionRunning = false;
                break;
            }

            // --- State/preset commands (headless, no GUI needed) ---
            if (msg->type == rps::ipc::MessageType::GetStateRequest) {
                spdlog::info("Received GetStateRequest via IPC (headless)");
                auto resp = host->saveState();
                rps::ipc::Message respMsg;
                respMsg.type = rps::ipc::MessageType::GetStateResponse;
                respMsg.payload = std::move(resp);
                connection->sendMessage(respMsg);
                continue;
            }

            if (msg->type == rps::ipc::MessageType::SetStateRequest) {
                spdlog::info("Received SetStateRequest via IPC (headless)");
                auto& req = std::get<rps::ipc::SetStateRequest>(msg->payload);
                auto resp = host->loadState(req.stateData);
                rps::ipc::Message respMsg;
                respMsg.type = rps::ipc::MessageType::SetStateResponse;
                respMsg.payload = std::move(resp);
                connection->sendMessage(respMsg);
                if (resp.success) {
                    auto params = host->getParameters();
                    rps::ipc::Message paramMsg;
                    paramMsg.type = rps::ipc::MessageType::ParameterListEvent;
                    paramMsg.payload = rps::ipc::ParameterListEvent{std::move(params)};
                    connection->sendMessage(paramMsg);
                }
                continue;
            }

            if (msg->type == rps::ipc::MessageType::LoadPresetRequest) {
                spdlog::info("Received LoadPresetRequest via IPC (headless)");
                auto& req = std::get<rps::ipc::LoadPresetRequest>(msg->payload);
                auto resp = host->loadPreset(req.presetId);
                rps::ipc::Message respMsg;
                respMsg.type = rps::ipc::MessageType::LoadPresetResponse;
                respMsg.payload = std::move(resp);
                connection->sendMessage(respMsg);
                if (resp.success) {
                    auto params = host->getParameters();
                    rps::ipc::Message paramMsg;
                    paramMsg.type = rps::ipc::MessageType::ParameterListEvent;
                    paramMsg.payload = rps::ipc::ParameterListEvent{std::move(params)};
                    connection->sendMessage(paramMsg);
                }
                continue;
            }
        }

        // Shut down audio
        audioShutdown.store(true, std::memory_order_relaxed);
        if (audioDevice) {
            spdlog::info("Audio device stats: callbacks={}, consumed={}, underruns={}",
                         deviceCtx.callbackCount.load(std::memory_order_relaxed),
                         deviceCtx.blocksConsumed.load(std::memory_order_relaxed),
                         deviceCtx.underruns.load(std::memory_order_relaxed));
            audioDevice->stop();
            audioDevice->close();
            audioDevice.reset();
        }
        if (audioThread.joinable()) {
            audioThread.join();
        }
        if (host->supportsAudioProcessing()) {
            host->teardownAudioProcessing();
        }

        // Send final session closed event
        rps::ipc::Message closedMsg;
        closedMsg.type = rps::ipc::MessageType::GuiClosedEvent;
        closedMsg.payload = rps::ipc::GuiClosedEvent{"session_ended"};
        connection->sendMessage(closedMsg);

    } catch (const std::exception& e) {
        spdlog::error("Plugin GUI error: {}", e.what());

        // Send error event
        rps::ipc::Message errMsg;
        errMsg.type = rps::ipc::MessageType::GuiClosedEvent;
        errMsg.payload = rps::ipc::GuiClosedEvent{"crash"};
        connection->sendMessage(errMsg);
    }

    spdlog::info("=== rps-pluginhost shutting down ===");
    SDL_Quit();
    return 0;
}

} // namespace rps::gui
