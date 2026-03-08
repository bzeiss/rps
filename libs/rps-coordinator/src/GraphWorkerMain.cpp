#include <rps/coordinator/GraphWorkerMain.hpp>
#include <rps/coordinator/GraphSerializer.hpp>
#include <rps/coordinator/GraphExecutor.hpp>
#include <rps/coordinator/AudioBuffer.hpp>
#include <rps/coordinator/ChannelFormat.hpp>
#include <rps/coordinator/LatencyCalculator.hpp>
#include <rps/audio/SharedAudioRing.hpp>
#include <rps/core/LoggingInit.hpp>

#include <fstream>

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

#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <csignal>
#endif

namespace po = boost::program_options;

namespace rps::coordinator {

static std::atomic<bool> g_shutdown{false};

#ifndef _WIN32
static void signalHandler(int) {
    g_shutdown.store(true, std::memory_order_relaxed);
}
#endif

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

/// Format a captured SEH exception into a human-readable string.
static std::string formatSehException() {
    if (s_capturedEx.code == 0xC0000005 && s_capturedEx.numParams >= 2) {
        return fmt::format("ACCESS_VIOLATION: {} at 0x{:016X}, instruction at 0x{:016X}",
                           s_capturedEx.accessType == 0 ? "READ" : "WRITE",
                           s_capturedEx.faultAddress,
                           s_capturedEx.instruction);
    }
    return fmt::format("SEH exception 0x{:08X} at instruction 0x{:016X}",
                       s_capturedEx.code, s_capturedEx.instruction);
}
#endif

/// Print to both stderr (visible to user) and spdlog (log file).
template <typename... Args>
static void report(spdlog::level::level_enum level, fmt::format_string<Args...> fmt, Args&&... args) {
    auto msg = fmt::format(fmt, std::forward<Args>(args)...);
    spdlog::log(level, "{}", msg);

    const char* prefix = "";
    if (level == spdlog::level::err) prefix = "[ERROR] ";
    else if (level == spdlog::level::warn) prefix = "[WARN]  ";
    else if (level == spdlog::level::info) prefix = "[graph] ";
    std::cerr << prefix << msg << "\n";
}

/// Safely release all plugin hosts, catching SEH crashes per-plugin.
static void safeReleasePlugins(
    std::unordered_map<std::string, std::unique_ptr<rps::gui::IPluginGuiHost>>& hosts) {
    for (auto it = hosts.begin(); it != hosts.end(); ) {
        auto nodeId = it->first;
#ifdef _WIN32
        bool ok = true;
        [&]() {
            __try {
                // Follow proper VST3 lifecycle: stop processing before destroying
                it->second->teardownAudioProcessing();
                it->second.reset(); // Destroy the plugin host
            } __except(captureExceptionFilter(GetExceptionInformation())) {
                report(spdlog::level::warn, "Plugin '{}' crashed during teardown: {}", 
                       nodeId, formatSehException());
                ok = false;
                // The unique_ptr may be in a bad state — release ownership without destroying
                (void)it->second.release();
            }
        }();
#else
        it->second->teardownAudioProcessing();
        it->second.reset();
#endif
        it = hosts.erase(it);
    }
}

int GraphWorkerMain::run(int argc, char* argv[], HostFactory factory) {
    // Parse command-line arguments
    po::options_description desc("Plugin Graph Host Worker");
    desc.add_options()
        ("graph", po::value<std::string>(), "Graph definition (JSON string)")
        ("graph-file", po::value<std::string>(), "Path to graph definition JSON file")
        ("audio-shm", po::value<std::string>(), "Shared memory segment name for main audio I/O")
        ("validate-only", "Load graph and plugins, report status, then exit (no audio processing)")
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

    const bool validateOnly = vm.count("validate-only") > 0;

    // Set up logging
    rps::core::initLogging("PLUGINHOST_GRAPH", "rps-pluginhost.graph.log");
    if (spdlog::default_logger()->level() != spdlog::level::off) {
        spdlog::default_logger()->flush_on(spdlog::level::debug);
    }

    report(spdlog::level::info, "=== rps-pluginhost [graph mode] ===");

    // Load graph definition
    std::string graphJson;
    if (vm.count("graph")) {
        graphJson = vm["graph"].as<std::string>();
        report(spdlog::level::info, "Graph source: inline JSON ({} bytes)", graphJson.size());
    } else if (vm.count("graph-file")) {
        auto path = vm["graph-file"].as<std::string>();
        std::ifstream f(path);
        if (!f) {
            report(spdlog::level::err, "Cannot open graph file: {}", path);
            return 1;
        }
        graphJson.assign(std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>());
        report(spdlog::level::info, "Graph source: {} ({} bytes)", path, graphJson.size());
    } else {
        report(spdlog::level::err, "No graph definition provided (use --graph or --graph-file)");
        return 1;
    }

    // Parse graph
    Graph graph;
    try {
        graph = GraphSerializer::fromJson(graphJson);
        report(spdlog::level::info, "Graph parsed: {} nodes, {} edges",
               graph.nodeCount(), graph.edges().size());
    } catch (const std::exception& e) {
        report(spdlog::level::err, "Failed to parse graph JSON: {}", e.what());
        return 1;
    }

    // Validate
    auto validation = graph.validate();
    if (!validation.valid) {
        report(spdlog::level::err, "Graph validation FAILED:");
        for (const auto& err : validation.errors) {
            report(spdlog::level::err, "  - {}", err.message);
        }
        return 1;
    }
    report(spdlog::level::info, "Graph validation: OK");

    // Print graph summary
    report(spdlog::level::info, "Audio config: {}Hz, blockSize={}", 
           graph.config().sampleRate, graph.config().blockSize);
    for (const auto& [id, node] : graph.nodes()) {
        std::string typeStr(nodeTypeToString(node.type));
        if (node.type == NodeType::Plugin && node.pluginConfig) {
            report(spdlog::level::info, "  Node '{}': {} ({})", id, typeStr,
                   node.pluginConfig->pluginPath);
        } else {
            report(spdlog::level::info, "  Node '{}': {}", id, typeStr);
        }
    }
    for (const auto& edge : graph.edges()) {
        report(spdlog::level::info, "  Edge {}: {}:{} -> {}:{}", edge.id,
               edge.sourceNodeId, edge.sourcePort, edge.destNodeId, edge.destPort);
    }

    // Load plugins for each PluginNode
    std::unordered_map<std::string, std::unique_ptr<rps::gui::IPluginGuiHost>> pluginHosts;
    for (const auto& [nodeId, node] : graph.nodes()) {
        if (node.type != NodeType::Plugin) continue;
        if (!node.pluginConfig) {
            report(spdlog::level::err, "PluginNode '{}' has no plugin config", nodeId);
            return 1;
        }

        auto& cfg = *node.pluginConfig;
        report(spdlog::level::info, "Loading plugin for node '{}': {} ({})",
               nodeId, cfg.pluginPath, cfg.format);

        auto host = factory(cfg.format);
        if (!host) {
            report(spdlog::level::err, "No host available for format '{}'", cfg.format);
            return 1;
        }

        bool loadOk = true;
        try {
#ifdef _WIN32
            // Wrap plugin loading in SEH — plugins can crash during init
            [&]() {
                __try {
                    host->loadPlugin(boost::filesystem::path(cfg.pluginPath));
                } __except(captureExceptionFilter(GetExceptionInformation())) {
                    report(spdlog::level::err, "  CRASH during loadPlugin: {}", formatSehException());
                    loadOk = false;
                }
            }();
#else
            host->loadPlugin(boost::filesystem::path(cfg.pluginPath));
#endif
            if (!loadOk) {
                report(spdlog::level::err, "Plugin for node '{}' crashed during loading — skipping", nodeId);
                continue;
            }

            report(spdlog::level::info, "  Loaded: '{}'", host->getPluginName());

            // Set up audio processing for this plugin
            uint32_t channels = 2;
            if (!node.inputPorts.empty()) {
                channels = node.inputPorts[0].layout.effectiveChannelCount();
            }
            if (channels == 0) channels = 2;

#ifdef _WIN32
            std::optional<rps::gui::AudioBusLayout> layoutOpt;
            [&]() {
                __try {
                    layoutOpt = host->setupAudioProcessing(
                        graph.config().sampleRate,
                        graph.config().blockSize,
                        channels);
                } __except(captureExceptionFilter(GetExceptionInformation())) {
                    report(spdlog::level::err, "  CRASH during setupAudioProcessing: {}", formatSehException());
                    loadOk = false;
                }
            }();
#else
            auto layoutOpt = host->setupAudioProcessing(
                graph.config().sampleRate,
                graph.config().blockSize,
                channels);
#endif
            if (!loadOk) {
                report(spdlog::level::err, "Plugin for node '{}' crashed during audio setup — skipping", nodeId);
                continue;
            }

            if (layoutOpt) {
                report(spdlog::level::info, "  Audio: {} in -> {} out",
                       layoutOpt->numInputChannels, layoutOpt->numOutputChannels);
            } else {
                report(spdlog::level::warn, "  Plugin does not support audio processing");
            }

            pluginHosts[nodeId] = std::move(host);
        } catch (const std::exception& e) {
            report(spdlog::level::err, "Failed to load plugin for node '{}': {}", nodeId, e.what());
            return 1;
        }
    }

    // Compute and report latencies
    auto latencies = LatencyCalculator::compute(graph);
    for (const auto& [outputId, samples] : latencies) {
        report(spdlog::level::info, "Output '{}' latency: {} samples ({:.1f} ms at {}Hz)",
               outputId, samples,
               static_cast<double>(samples) * 1000.0 / graph.config().sampleRate,
               graph.config().sampleRate);
    }

    // Pre-allocate per-node interleaved scratch buffers for plugin callbacks.
    // These are allocated once here and reused every audio block — zero allocations
    // in the hot path. (Phase 4: Real-Time Audio Path Hardening)
    struct PluginScratch {
        std::vector<float> interleavedIn;
        std::vector<float> interleavedOut;
    };
    std::unordered_map<std::string, PluginScratch> pluginScratch;
    for (const auto& [nodeId, host] : pluginHosts) {
        const auto* node = graph.findNode(nodeId);
        if (!node) continue;
        uint32_t inCh = 2, outCh = 2;
        if (!node->inputPorts.empty())
            inCh = node->inputPorts[0].layout.effectiveChannelCount();
        if (!node->outputPorts.empty())
            outCh = node->outputPorts[0].layout.effectiveChannelCount();
        if (inCh == 0) inCh = 2;
        if (outCh == 0) outCh = 2;
        uint32_t bs = graph.config().blockSize;
        pluginScratch[nodeId] = {
            std::vector<float>(inCh * bs),
            std::vector<float>(outCh * bs)
        };
    }

    // Set up the graph executor with plugin callbacks
    GraphExecutor executor;
    executor.prepare(graph, [&pluginHosts, &pluginScratch](
        const std::string& nodeId,
        const AudioBuffer& input,
        AudioBuffer& output) -> bool {

        auto it = pluginHosts.find(nodeId);
        if (it == pluginHosts.end()) return false;

        auto& host = it->second;
        uint32_t inCh = input.numChannels();
        uint32_t outCh = output.numChannels();
        uint32_t bs = input.blockSize();

        // Use pre-allocated scratch buffers (no allocation in hot path)
        auto scratchIt = pluginScratch.find(nodeId);
        if (scratchIt == pluginScratch.end()) return false;
        auto& scratch = scratchIt->second;
        input.interleaveTo(scratch.interleavedIn.data());
        std::fill(scratch.interleavedOut.begin(), scratch.interleavedOut.end(), 0.0f);

        // Process through the plugin (with SEH protection)
        bool processOk = true;
#ifdef _WIN32
        [&]() {
            __try {
                host->processAudioBlock(scratch.interleavedIn.data(), scratch.interleavedOut.data(),
                                        inCh, outCh, bs);
            } __except(captureExceptionFilter(GetExceptionInformation())) {
                spdlog::error("Plugin '{}' CRASH during process: {}", nodeId, formatSehException());
                processOk = false;
            }
        }();
#else
        host->processAudioBlock(scratch.interleavedIn.data(), scratch.interleavedOut.data(),
                                inCh, outCh, bs);
#endif
        if (!processOk) return false; // Fallback to passthrough

        // Convert back to deinterleaved
        output.deinterleaveFrom(scratch.interleavedOut.data(), outCh, bs);
        return true;
    });

    report(spdlog::level::info, "Graph executor ready (processing order: {} nodes)",
           executor.processingOrder().size());
    for (const auto& id : executor.processingOrder()) {
        spdlog::info("  -> {}", id);
    }

    // --- Validate-only mode: report success and exit ---
    if (validateOnly) {
        report(spdlog::level::info, "");
        report(spdlog::level::info, "=== VALIDATE-ONLY: All checks passed, {} plugin(s) loaded ===",
               pluginHosts.size());
        safeReleasePlugins(pluginHosts);
        return 0;
    }

    // --- Audio processing ---

    // Set up signal handling for graceful shutdown
#ifndef _WIN32
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#else
    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        g_shutdown.store(true, std::memory_order_relaxed);
        return TRUE;
    }, TRUE);
#endif

    // Open shared memory for main audio I/O
    std::unique_ptr<rps::audio::SharedAudioRing> audioRing;
    if (vm.count("audio-shm")) {
        const auto shmName = vm["audio-shm"].as<std::string>();
        report(spdlog::level::info, "Opening audio shared memory: {}", shmName);
        try {
            audioRing = rps::audio::SharedAudioRing::open(shmName);
        } catch (const std::exception& e) {
            report(spdlog::level::err, "Failed to open audio shared memory: {}", e.what());
            return 1;
        }
    }

    // Audio processing loop
    if (audioRing) {
        const auto& hdr = audioRing->header();
        uint32_t inChannels = hdr.numChannels;
        uint32_t blockSize = hdr.blockSize;

        report(spdlog::level::info, "Starting audio processing loop ({}Hz, bs={}, {}ch)",
               hdr.sampleRate, blockSize, inChannels);

        std::vector<float> inputInterleaved(inChannels * blockSize);
        std::vector<float> outputInterleaved(inChannels * blockSize);

        // Find input and output node IDs
        std::string inputNodeId;
        std::string outputNodeId;
        for (const auto& [id, node] : graph.nodes()) {
            if (node.type == NodeType::Input && inputNodeId.empty()) inputNodeId = id;
            if (node.type == NodeType::Output && outputNodeId.empty()) outputNodeId = id;
        }

        if (inputNodeId.empty() || outputNodeId.empty()) {
            report(spdlog::level::err, "Graph must have at least one InputNode and one OutputNode");
            return 1;
        }

        report(spdlog::level::info, "Audio I/O: input='{}', output='{}'", inputNodeId, outputNodeId);
        report(spdlog::level::info, "Processing... (Ctrl+C to stop)");

        // Pre-allocate I/O buffers and maps ONCE (Phase 4: zero allocations in hot path)
        AudioBuffer inputBuf(inChannels, blockSize);
        std::unordered_map<std::string, AudioBuffer> inputs;
        inputs.emplace(inputNodeId, AudioBuffer(inChannels, blockSize));
        std::unordered_map<std::string, AudioBuffer> outputs;
        // Determine output channel count from the graph
        uint32_t outChannels = inChannels; // fallback
        {
            const auto* outNode = graph.findNode(outputNodeId);
            if (outNode && !outNode->inputPorts.empty()) {
                uint32_t ec = outNode->inputPorts[0].layout.effectiveChannelCount();
                if (ec > 0) outChannels = ec;
            }
        }
        outputs.emplace(outputNodeId, AudioBuffer(outChannels, blockSize));

        while (!g_shutdown.load(std::memory_order_relaxed)) {
            if (!audioRing->waitForInput(std::chrono::milliseconds(10))) continue;
            if (!audioRing->readInputBlock(inputInterleaved.data())) continue;

            // Deinterleave input into pre-allocated buffer
            inputs[inputNodeId].deinterleaveFrom(inputInterleaved.data(), inChannels, blockSize);

            // Clear output buffer for reuse
            outputs[outputNodeId].clear();

            // Process the graph
            executor.processBlock(inputs, outputs);

            // Interleave output
            outputs[outputNodeId].interleaveTo(outputInterleaved.data());

            // Write output
            while (!g_shutdown.load(std::memory_order_relaxed)) {
                if (audioRing->writeOutputBlock(outputInterleaved.data())) break;
                std::this_thread::yield();
            }
        }

        report(spdlog::level::info, "Audio processing loop ended");
    } else {
        report(spdlog::level::warn, "No --audio-shm provided. Use --validate-only to test without audio, "
               "or provide --audio-shm for live processing.");
        report(spdlog::level::info, "Waiting for shutdown... (Ctrl+C to stop)");
        while (!g_shutdown.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Cleanup
    report(spdlog::level::info, "Graph worker shutting down...");
    safeReleasePlugins(pluginHosts);
    report(spdlog::level::info, "Graph worker exiting");

    return 0;
}

} // namespace rps::coordinator

