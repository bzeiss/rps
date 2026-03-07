#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "ClapGuiHost.hpp"

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/preset-load.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/latency.h>
#include <clap/factory/preset-discovery.h>

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <atomic>

namespace rps::scanner {

// ---------------------------------------------------------------------------
// CLAP Host Callbacks — implements the host side for GUI hosting
// ---------------------------------------------------------------------------
namespace {

// Forward declaration — we retrieve ClapGuiHost* from clap_host::host_data
ClapGuiHost* getHostFromClap(const clap_host* host) {
    return static_cast<ClapGuiHost*>(host->host_data);
}

// Host gui extension callbacks
void hostGuiResizeHintsChanged(const clap_host*) {
    spdlog::debug("hostGuiResizeHintsChanged called");
}

bool hostGuiRequestResize(const clap_host* host, uint32_t width, uint32_t height) {
    spdlog::info("hostGuiRequestResize: {}x{}", width, height);
    auto* self = getHostFromClap(host);
    if (self) {
        self->onPluginRequestResize(width, height);
    }
    return true;
}

bool hostGuiRequestShow(const clap_host*) {
    spdlog::debug("hostGuiRequestShow called");
    return true;
}

bool hostGuiRequestHide(const clap_host*) {
    spdlog::debug("hostGuiRequestHide called");
    return true;
}

void hostGuiClosed(const clap_host*, bool was_destroyed) {
    spdlog::info("hostGuiClosed called (was_destroyed={})", was_destroyed);
}

static clap_host_gui_t s_hostGui = {
    .resize_hints_changed = hostGuiResizeHintsChanged,
    .request_resize = hostGuiRequestResize,
    .request_show = hostGuiRequestShow,
    .request_hide = hostGuiRequestHide,
    .closed = hostGuiClosed,
};

// Thread-check extension — allows plugins to verify which thread they're on.
// Main thread = the thread that created the plugin (event loop thread).
// Audio thread = the thread that calls process() (SDL audio callback thread).
std::atomic<std::thread::id> g_mainThreadId{};
std::atomic<std::thread::id> g_audioThreadId{};

bool hostIsMainThread(const clap_host* /*host*/) {
    return std::this_thread::get_id() == g_mainThreadId.load(std::memory_order_relaxed);
}

bool hostIsAudioThread(const clap_host* /*host*/) {
    auto audioId = g_audioThreadId.load(std::memory_order_relaxed);
    if (audioId == std::thread::id{}) {
        // Audio thread not yet identified — accept any non-main thread.
        // This handles the case where start_processing() is called before
        // the first process() call.
        return !hostIsMainThread(nullptr);
    }
    return std::this_thread::get_id() == audioId;
}

static clap_host_thread_check_t s_hostThreadCheck = {
    .is_main_thread = hostIsMainThread,
    .is_audio_thread = hostIsAudioThread,
};

// ---------------------------------------------------------------------------
// Host extension stubs — plugins cache pointers returned by get_extension()
// during init() and may dereference them unconditionally in process().
// Returning nullptr for these causes ACCESS_VIOLATION in FabFilter plugins.
// ---------------------------------------------------------------------------

// clap.params host extension
static clap_host_params_t s_hostParams = {
    .rescan = [](const clap_host_t*, clap_param_rescan_flags) {},
    .clear = [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    .request_flush = [](const clap_host_t*) {},
};

// clap.latency host extension
static clap_host_latency_t s_hostLatency = {
    .changed = [](const clap_host_t*) {},
};

// clap.state host extension
static clap_host_state_t s_hostState = {
    .mark_dirty = [](const clap_host_t*) {},
};

// Core host callbacks
const void* hostGetExtension(const clap_host* /*host*/, const char* extensionId) {
    spdlog::debug("hostGetExtension: {}", extensionId);
    if (strcmp(extensionId, CLAP_EXT_GUI) == 0) {
        return &s_hostGui;
    }
    if (strcmp(extensionId, CLAP_EXT_THREAD_CHECK) == 0) {
        return &s_hostThreadCheck;
    }
    if (strcmp(extensionId, CLAP_EXT_PARAMS) == 0) {
        return &s_hostParams;
    }
    if (strcmp(extensionId, CLAP_EXT_LATENCY) == 0) {
        return &s_hostLatency;
    }
    if (strcmp(extensionId, CLAP_EXT_STATE) == 0) {
        return &s_hostState;
    }
    return nullptr;
}

void hostRequestRestart(const clap_host*) {
    spdlog::debug("hostRequestRestart called");
}
void hostRequestProcess(const clap_host*) {
    spdlog::debug("hostRequestProcess called");
}
void hostRequestCallback(const clap_host*) {
    spdlog::debug("hostRequestCallback called");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ClapGuiHost implementation
// ---------------------------------------------------------------------------

ClapGuiHost::ClapGuiHost() {
    // Register this thread as the "main thread" for CLAP thread-check
    g_mainThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

ClapGuiHost::~ClapGuiHost() {
    cleanup();
}

// ---------------------------------------------------------------------------
// Audio processing
// ---------------------------------------------------------------------------

std::optional<rps::gui::AudioBusLayout> ClapGuiHost::setupAudioProcessing(
    uint32_t sampleRate, uint32_t blockSize, uint32_t numChannels)
{
    spdlog::info("ClapGuiHost::setupAudioProcessing(sr={}, bs={}, ch={})",
                 sampleRate, blockSize, numChannels);

    if (!m_plugin) {
        spdlog::error("  Plugin not loaded");
        return std::nullopt;
    }

    // 1. Query audio ports extension
    m_audioPorts = reinterpret_cast<const clap_plugin_audio_ports*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_AUDIO_PORTS));
    m_latencyExt = reinterpret_cast<const clap_plugin_latency*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_LATENCY));

    // 2. Enumerate ALL audio ports (main + sidechain etc.)
    uint32_t inCh = numChannels;   // Default to requested
    uint32_t outCh = numChannels;
    m_numInputPorts = 1;
    m_numOutputPorts = 1;

    if (m_audioPorts) {
        uint32_t numIn = m_audioPorts->count(m_plugin, true);
        uint32_t numOut = m_audioPorts->count(m_plugin, false);
        spdlog::info("  Audio ports: {} input port(s), {} output port(s)", numIn, numOut);
        m_numInputPorts = std::max(numIn, 1u);
        m_numOutputPorts = std::max(numOut, 1u);

        // Log all ports
        for (uint32_t i = 0; i < numIn; ++i) {
            clap_audio_port_info_t pi{};
            if (m_audioPorts->get(m_plugin, i, true, &pi)) {
                spdlog::info("  Input port [{}] '{}': {} ch, type={}",
                             i, pi.name, pi.channel_count,
                             pi.port_type ? pi.port_type : "<null>");
                if (i == 0) inCh = pi.channel_count;
            }
        }
        for (uint32_t i = 0; i < numOut; ++i) {
            clap_audio_port_info_t pi{};
            if (m_audioPorts->get(m_plugin, i, false, &pi)) {
                spdlog::info("  Output port [{}] '{}': {} ch, type={}",
                             i, pi.name, pi.channel_count,
                             pi.port_type ? pi.port_type : "<null>");
                if (i == 0) outCh = pi.channel_count;
            }
        }
    } else {
        spdlog::info("  No audio_ports extension, assuming {} channels", numChannels);
    }

    // 3. Activate the plugin
    if (!m_plugin->activate(m_plugin, static_cast<double>(sampleRate),
                            blockSize, blockSize)) {
        spdlog::error("  activate() failed");
        return std::nullopt;
    }

    // NOTE: start_processing() is NOT called here.
    // Per CLAP spec, start_processing() must be called from the [audio-thread].
    // It is called lazily on the first processAudioBlock() call.

    // 5. Allocate de-interleaved channel buffers for the main port
    m_audioInputChannels = inCh;
    m_audioOutputChannels = outCh;
    m_audioBlockSize = blockSize;

    m_inputChannelBuffers.resize(inCh);
    m_inputPtrs.resize(inCh);
    for (uint32_t c = 0; c < inCh; ++c) {
        m_inputChannelBuffers[c].resize(blockSize, 0.0f);
        m_inputPtrs[c] = m_inputChannelBuffers[c].data();
    }

    m_outputChannelBuffers.resize(outCh);
    m_outputPtrs.resize(outCh);
    for (uint32_t c = 0; c < outCh; ++c) {
        m_outputChannelBuffers[c].resize(blockSize, 0.0f);
        m_outputPtrs[c] = m_outputChannelBuffers[c].data();
    }

    // 5b. Allocate silent buffers for extra ports (e.g., sidechain)
    // Plugins like FabFilter Pro-C access audio_inputs[1] for sidechain;
    // if we don't provide it, they crash.
    m_extraInputBuses.clear();
    m_extraInputBusPtrs.clear();
    if (m_numInputPorts > 1) {
        for (uint32_t p = 1; p < m_numInputPorts; ++p) {
            clap_audio_port_info_t pi{};
            uint32_t ch = 2; // default stereo sidechain
            if (m_audioPorts && m_audioPorts->get(m_plugin, p, true, &pi)) {
                ch = pi.channel_count;
            }
            // Allocate silent channel buffers
            std::vector<std::vector<float>> bufs(ch, std::vector<float>(blockSize, 0.0f));
            std::vector<float*> ptrs(ch);
            for (uint32_t c = 0; c < ch; ++c)
                ptrs[c] = bufs[c].data();
            m_extraInputBuses.push_back(std::move(bufs));
            m_extraInputBusPtrs.push_back(std::move(ptrs));
        }
    }
    m_extraOutputBuses.clear();
    m_extraOutputBusPtrs.clear();
    if (m_numOutputPorts > 1) {
        for (uint32_t p = 1; p < m_numOutputPorts; ++p) {
            clap_audio_port_info_t pi{};
            uint32_t ch = 2;
            if (m_audioPorts && m_audioPorts->get(m_plugin, p, false, &pi)) {
                ch = pi.channel_count;
            }
            std::vector<std::vector<float>> bufs(ch, std::vector<float>(blockSize, 0.0f));
            std::vector<float*> ptrs(ch);
            for (uint32_t c = 0; c < ch; ++c)
                ptrs[c] = bufs[c].data();
            m_extraOutputBuses.push_back(std::move(bufs));
            m_extraOutputBusPtrs.push_back(std::move(ptrs));
        }
    }

    m_audioActive = true;

    spdlog::info("  Audio processing active: {} in → {} out, bs={}, sr={}, inPorts={}, outPorts={}",
                 inCh, outCh, blockSize, sampleRate, m_numInputPorts, m_numOutputPorts);

    return rps::gui::AudioBusLayout{inCh, outCh};
}

bool ClapGuiHost::processAudioBlock(
    const float* input, float* output,
    uint32_t numInputChannels, uint32_t numOutputChannels, uint32_t numSamples,
    const std::vector<rps::gui::AutomationEvent>& /*automation*/)
{
    if (!m_audioActive || !m_plugin) return false;

    // Register the audio thread on first call (for clap.thread-check)
    static bool firstCall = true;
    auto expected = std::thread::id{};
    g_audioThreadId.compare_exchange_strong(expected, std::this_thread::get_id(),
                                            std::memory_order_relaxed);

    // Lazy start_processing() — MUST be called from the audio thread per CLAP spec.
    if (!m_processingStarted) {
        spdlog::info("  [STEP] calling start_processing() from audio thread");
        if (!m_plugin->start_processing(m_plugin)) {
            spdlog::error("  start_processing() failed");
            spdlog::default_logger()->flush();
            return false;
        }
        m_processingStarted = true;
        spdlog::info("  [STEP] start_processing() succeeded");
        spdlog::default_logger()->flush();
    }

    auto logStep = [&](const char* step) {
        if (firstCall) {
            spdlog::info("  [STEP] {}", step);
            spdlog::default_logger()->flush();
        }
    };

    if (firstCall) {
        spdlog::info("processAudioBlock: FIRST CALL in={} out={} bs={} "
                     "inputBufs={} outputBufs={} inputPtrs={} outputPtrs={} "
                     "plugin={:p}",
                     numInputChannels, numOutputChannels, numSamples,
                     m_inputChannelBuffers.size(), m_outputChannelBuffers.size(),
                     m_inputPtrs.size(), m_outputPtrs.size(),
                     static_cast<const void*>(m_plugin));
        spdlog::info("  m_plugin->process={:p}",
                     reinterpret_cast<const void*>(m_plugin->process));
        spdlog::default_logger()->flush();
    }

    logStep("de-interleave input");

    // 1. De-interleave input
    for (uint32_t s = 0; s < numSamples; ++s) {
        for (uint32_t c = 0; c < numInputChannels; ++c) {
            m_inputChannelBuffers[c][s] = input[s * numInputChannels + c];
        }
    }

    logStep("clear output buffers");

    // Clear output buffers
    for (uint32_t c = 0; c < numOutputChannels; ++c) {
        std::fill(m_outputChannelBuffers[c].begin(),
                  m_outputChannelBuffers[c].begin() + numSamples, 0.0f);
    }

    logStep("setup CLAP audio buffers");

    // 2. Set up CLAP audio buffers for ALL ports (main + sidechain etc.)
    std::vector<clap_audio_buffer_t> clapInputs(m_numInputPorts);
    // Port 0 = main input (from the interleaved input data)
    clapInputs[0] = {};
    clapInputs[0].data32 = m_inputPtrs.data();
    clapInputs[0].data64 = nullptr;
    clapInputs[0].channel_count = numInputChannels;
    clapInputs[0].latency = 0;
    clapInputs[0].constant_mask = 0;
    // Extra input ports (sidechain etc.) — silence buffers
    for (uint32_t p = 1; p < m_numInputPorts; ++p) {
        clapInputs[p] = {};
        clapInputs[p].data32 = m_extraInputBusPtrs[p - 1].data();
        clapInputs[p].data64 = nullptr;
        clapInputs[p].channel_count = static_cast<uint32_t>(m_extraInputBusPtrs[p - 1].size());
        clapInputs[p].latency = 0;
        clapInputs[p].constant_mask = 0;
    }

    std::vector<clap_audio_buffer_t> clapOutputs(m_numOutputPorts);
    clapOutputs[0] = {};
    clapOutputs[0].data32 = m_outputPtrs.data();
    clapOutputs[0].data64 = nullptr;
    clapOutputs[0].channel_count = numOutputChannels;
    clapOutputs[0].latency = 0;
    clapOutputs[0].constant_mask = 0;
    for (uint32_t p = 1; p < m_numOutputPorts; ++p) {
        clapOutputs[p] = {};
        clapOutputs[p].data32 = m_extraOutputBusPtrs[p - 1].data();
        clapOutputs[p].data64 = nullptr;
        clapOutputs[p].channel_count = static_cast<uint32_t>(m_extraOutputBusPtrs[p - 1].size());
        clapOutputs[p].latency = 0;
        clapOutputs[p].constant_mask = 0;
    }

    // 3. Set up empty event lists (Phase 1: no automation events)
    clap_input_events_t inEvents{};
    inEvents.ctx = nullptr;
    inEvents.size = [](const clap_input_events*) -> uint32_t { return 0; };
    inEvents.get = [](const clap_input_events*, uint32_t) -> const clap_event_header_t* { return nullptr; };

    clap_output_events_t outEvents{};
    outEvents.ctx = nullptr;
    outEvents.try_push = [](const clap_output_events*, const clap_event_header_t*) -> bool { return true; };

    logStep("setup process struct");

    // 4. Set up process struct
    // Use nullptr transport like the reference CLAP host (clap-host).
    // Per spec: "If null, then this is a free running host, no transport events will be provided"

    clap_process_t process{};
    process.steady_time = -1;
    process.frames_count = numSamples;
    process.transport = nullptr;
    process.audio_inputs = clapInputs.data();
    process.audio_outputs = clapOutputs.data();
    process.audio_inputs_count = m_numInputPorts;
    process.audio_outputs_count = m_numOutputPorts;
    process.in_events = &inEvents;
    process.out_events = &outEvents;

    logStep("calling m_plugin->process()");

    if (firstCall) {
        // Dump all process struct pointers for crash diagnosis
        spdlog::info("  process.frames_count={}", process.frames_count);
        spdlog::info("  process.steady_time={}", process.steady_time);
        spdlog::info("  process.audio_inputs={:p}, audio_inputs_count={}",
                     static_cast<const void*>(process.audio_inputs), process.audio_inputs_count);
        spdlog::info("  process.audio_outputs={:p}, audio_outputs_count={}",
                     static_cast<const void*>(process.audio_outputs), process.audio_outputs_count);
        spdlog::info("  process.in_events={:p} (size={:p}, get={:p})",
                     static_cast<const void*>(process.in_events),
                     reinterpret_cast<const void*>(process.in_events->size),
                     reinterpret_cast<const void*>(process.in_events->get));
        spdlog::info("  process.out_events={:p} (try_push={:p})",
                     static_cast<const void*>(process.out_events),
                     reinterpret_cast<const void*>(process.out_events->try_push));
        spdlog::info("  process.transport={:p}", static_cast<const void*>(process.transport));
        for (uint32_t p = 0; p < m_numInputPorts; ++p) {
            spdlog::info("  clapInput[{}].data32={:p}, ch={}", p,
                         static_cast<const void*>(clapInputs[p].data32), clapInputs[p].channel_count);
        }
        for (uint32_t p = 0; p < m_numOutputPorts; ++p) {
            spdlog::info("  clapOutput[{}].data32={:p}, ch={}", p,
                         static_cast<const void*>(clapOutputs[p].data32), clapOutputs[p].channel_count);
        }
        spdlog::default_logger()->flush();
    }

    // 5. Call plugin's process
    auto status = m_plugin->process(m_plugin, &process);

    logStep("process() returned");

    if (status == CLAP_PROCESS_ERROR) {
        firstCall = false;
        return false;
    }

    logStep("re-interleave output");

    // 6. Re-interleave output
    for (uint32_t s = 0; s < numSamples; ++s) {
        for (uint32_t c = 0; c < numOutputChannels; ++c) {
            output[s * numOutputChannels + c] = m_outputChannelBuffers[c][s];
        }
    }

    firstCall = false;
    return true;
}

uint32_t ClapGuiHost::getLatencySamples() const {
    if (m_latencyExt && m_plugin) {
        return m_latencyExt->get(m_plugin);
    }
    return 0;
}

void ClapGuiHost::teardownAudioProcessing() {
    if (!m_audioActive) return;

    spdlog::info("ClapGuiHost::teardownAudioProcessing()");

    if (m_plugin) {
        m_plugin->stop_processing(m_plugin);
        // Note: deactivate is called in cleanup()
    }

    m_audioActive = false;
    m_inputChannelBuffers.clear();
    m_outputChannelBuffers.clear();
    m_inputPtrs.clear();
    m_outputPtrs.clear();
}

void ClapGuiHost::cleanup() {
    spdlog::debug("ClapGuiHost::cleanup()");
    if (m_gui && m_plugin && m_guiCreated) {
        spdlog::debug("  destroying GUI");
        m_gui->destroy(m_plugin);
        m_guiCreated = false;
    }

    if (m_plugin) {
        spdlog::debug("  deactivating and destroying plugin");
        m_plugin->deactivate(m_plugin);
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
    }

    if (m_entry) {
        spdlog::debug("  deinit entry");
        m_entry->deinit();
        m_entry = nullptr;
    }

    if (m_libHandle) {
        spdlog::debug("  unloading library");
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_libHandle));
#else
        dlclose(m_libHandle);
#endif
        m_libHandle = nullptr;
    }
}

void ClapGuiHost::onPluginRequestResize(uint32_t width, uint32_t height) {
    spdlog::info("onPluginRequestResize: resizing SDL window to {}x{}", width, height);
    // Update min constraint so SDL allows the plugin to shrink below the previous minimum.
    // The plugin knows its own valid sizes, so this is always safe.
    m_window.setMinimumSize(width, height);
    m_window.resize(width, height);

    // Re-discover the actual minimum for future user-drag resizing
    if (m_canResize && m_gui && m_gui->adjust_size) {
        uint32_t minW = 1, minH = 1;
        if (m_gui->adjust_size(m_plugin, &minW, &minH)) {
            m_window.setMinimumSize(minW, minH);
        }
    }
}

rps::gui::IPluginGuiHost::OpenResult ClapGuiHost::open(const boost::filesystem::path& pluginPath) {
    spdlog::info("ClapGuiHost::open({})", pluginPath.string());

    // 1. Load the CLAP DLL
    spdlog::info("  Step 1: Loading CLAP DLL...");
#ifdef _WIN32
    HMODULE hLib = LoadLibraryW(pluginPath.c_str());
    if (!hLib) {
        DWORD err = GetLastError();
        throw std::runtime_error("Failed to load CLAP DLL: " + pluginPath.string()
                                 + " (Win32 error: " + std::to_string(err) + ")");
    }
    m_libHandle = static_cast<void*>(hLib);
    spdlog::info("  DLL loaded at {:p}", m_libHandle);

    auto* procAddr = reinterpret_cast<void*>(GetProcAddress(hLib, "clap_entry"));
    if (!procAddr) {
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }
    m_entry = reinterpret_cast<const clap_plugin_entry*>(procAddr);
    spdlog::info("  clap_entry found at {:p}", static_cast<const void*>(m_entry));
#else
    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error(std::string("Failed to load CLAP library: ") + dlerror());
    }
    m_libHandle = handle;
    spdlog::info("  DLL loaded at {:p}", m_libHandle);

    void* symAddr = dlsym(handle, "clap_entry");
    if (!symAddr) {
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }
    m_entry = reinterpret_cast<const clap_plugin_entry*>(symAddr);
    spdlog::info("  clap_entry found at {:p}", static_cast<const void*>(m_entry));
#endif

    // 2. Init entry and get factory
    spdlog::info("  Step 2: Checking CLAP version and initializing entry...");
    spdlog::info("  Plugin CLAP version: {}.{}.{}", m_entry->clap_version.major,
                 m_entry->clap_version.minor, m_entry->clap_version.revision);

    if (!clap_version_is_compatible(m_entry->clap_version)) {
        throw std::runtime_error("Incompatible CLAP version.");
    }

    spdlog::info("  Calling entry->init()...");
    if (!m_entry->init(pluginPath.string().c_str())) {
        throw std::runtime_error("Failed to initialize CLAP plugin entry.");
    }
    spdlog::info("  entry->init() succeeded");

    spdlog::info("  Step 3: Getting plugin factory...");
    const auto* factoryPtr = m_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factoryPtr) {
        throw std::runtime_error("CLAP library does not provide a plugin factory.");
    }

    auto* factory = static_cast<const clap_plugin_factory*>(factoryPtr);
    uint32_t numPlugins = factory->get_plugin_count(factory);
    spdlog::info("  Factory has {} plugin(s)", numPlugins);
    if (numPlugins == 0) {
        throw std::runtime_error("CLAP library contains no plugins.");
    }

    // 3. Get the first plugin descriptor
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    if (!desc) {
        throw std::runtime_error("Failed to get CLAP plugin descriptor.");
    }

    m_pluginName = desc->name ? desc->name : "Unknown Plugin";
    spdlog::info("  Plugin: '{}' (id: {}, vendor: {})", m_pluginName,
                 desc->id ? desc->id : "?", desc->vendor ? desc->vendor : "?");

    // 4. Create the host
    spdlog::info("  Step 4: Creating CLAP host...");
    static clap_host_t s_host{};
    s_host.clap_version = CLAP_VERSION;
    s_host.name = "rps-pluginhost-clap";
    s_host.vendor = "rps";
    s_host.url = "";
    s_host.version = "1.0.0";
    s_host.get_extension = hostGetExtension;
    s_host.request_restart = hostRequestRestart;
    s_host.request_process = hostRequestProcess;
    s_host.request_callback = hostRequestCallback;
    s_host.host_data = this;

    // 5. Create and init the plugin
    spdlog::info("  Step 5: Creating plugin instance...");
    m_plugin = factory->create_plugin(factory, &s_host, desc->id);
    if (!m_plugin) {
        throw std::runtime_error("Failed to create CLAP plugin instance.");
    }
    spdlog::info("  Plugin instance created at {:p}", static_cast<const void*>(m_plugin));

    spdlog::info("  Calling plugin->init()...");
    if (!m_plugin->init(m_plugin)) {
        throw std::runtime_error("CLAP plugin init() failed.");
    }
    spdlog::info("  plugin->init() succeeded");

    // 5b. Query params extension (optional — used for parameter streaming)
    m_params = static_cast<const clap_plugin_params*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_PARAMS));
    if (m_params) {
        spdlog::info("  Params extension found (count={} get_info={} get_value={} value_to_text={})",
                     m_params->count != nullptr, m_params->get_info != nullptr,
                     m_params->get_value != nullptr, m_params->value_to_text != nullptr);
    } else {
        spdlog::info("  No params extension — parameter streaming disabled");
    }

    // 5c. Query state extension (optional — used for state save/restore)
    m_state = static_cast<const clap_plugin_state*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_STATE));
    if (m_state) {
        spdlog::info("  State extension found (save={} load={})",
                     m_state->save != nullptr, m_state->load != nullptr);
    } else {
        spdlog::info("  No state extension — state save/restore disabled");
    }

    // 5d. Query preset-load extension (optional)
    m_presetLoad = static_cast<const clap_plugin_preset_load*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_PRESET_LOAD));
    if (m_presetLoad) {
        spdlog::info("  Preset-load extension found (from_location={})",
                     m_presetLoad->from_location != nullptr);
    } else {
        spdlog::info("  No preset-load extension");
    }

    // 5e. Discover presets via preset_discovery_factory
    discoverPresets();

    // 6. Query GUI extension
    spdlog::info("  Step 6: Querying CLAP_EXT_GUI...");
    m_gui = static_cast<const clap_plugin_gui*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_GUI));
    if (!m_gui) {
        throw std::runtime_error("Plugin does not support CLAP GUI extension.");
    }
    spdlog::info("  GUI extension found at {:p}", static_cast<const void*>(m_gui));

    // Log which function pointers are available
    spdlog::info("  GUI ext: is_api_supported={}, create={}, destroy={}, set_scale={}, "
                 "get_size={}, can_resize={}, get_resize_hints={}, adjust_size={}, "
                 "set_size={}, set_parent={}, show={}, hide={}",
                 m_gui->is_api_supported != nullptr, m_gui->create != nullptr,
                 m_gui->destroy != nullptr, m_gui->set_scale != nullptr,
                 m_gui->get_size != nullptr, m_gui->can_resize != nullptr,
                 m_gui->get_resize_hints != nullptr, m_gui->adjust_size != nullptr,
                 m_gui->set_size != nullptr, m_gui->set_parent != nullptr,
                 m_gui->show != nullptr, m_gui->hide != nullptr);

    // 7. Check API support and create GUI
#ifdef _WIN32
    const char* windowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    const char* windowApi = CLAP_WINDOW_API_COCOA;
#else
    const char* windowApi = CLAP_WINDOW_API_X11;
#endif

    spdlog::info("  Step 7: Checking API support for '{}'...", windowApi);
    if (!m_gui->is_api_supported(m_plugin, windowApi, false)) {
        throw std::runtime_error(std::string("Plugin does not support embedded ") + windowApi + " GUI.");
    }
    spdlog::info("  API '{}' is supported", windowApi);

    spdlog::info("  Calling gui->create()...");
    if (!m_gui->create(m_plugin, windowApi, false)) {
        throw std::runtime_error("clap_plugin_gui->create() failed.");
    }
    m_guiCreated = true;
    spdlog::info("  GUI created successfully");

    // 8. Query whether the plugin supports resizing
    spdlog::info("  Step 8: Querying can_resize...");
    m_canResize = m_gui->can_resize && m_gui->can_resize(m_plugin);
    spdlog::info("  can_resize = {}", m_canResize);

    // 9. Get initial size
    spdlog::info("  Step 9: Getting initial size...");
    uint32_t w = 800, h = 600;
    if (!m_gui->get_size(m_plugin, &w, &h)) {
        spdlog::warn("  get_size failed, using defaults 800x600");
        w = 800;
        h = 600;
    }
    spdlog::info("  Initial size: {}x{}", w, h);

    // 10. Create SDL3 window
    spdlog::info("  Step 10: Creating SDL3 window (resizable={})...", m_canResize);
    bool hasPresets = !m_presets.empty();
    m_window.create(m_pluginName, w, h, m_canResize, hasPresets);
    spdlog::info("  SDL3 window created");

    if (m_canResize && m_gui->adjust_size) {
        // Discover the plugin's minimum size by requesting a very small size.
        // adjust_size will snap to the nearest valid dimensions.
        uint32_t minW = 1, minH = 1;
        if (m_gui->adjust_size(m_plugin, &minW, &minH)) {
            spdlog::info("  Plugin minimum size: {}x{}", minW, minH);
            m_window.setMinimumSize(minW, minH);
        } else {
            m_window.setMinimumSize(w, h);
        }
    } else if (!m_canResize) {
        // Non-resizable: lock both min and max to the initial size
        m_window.setMinimumSize(w, h);
        m_window.setMaximumSize(w, h);
    }

    // 11. Set parent (embed plugin GUI into SDL window)
    spdlog::info("  Step 11: Setting parent window...");
    void* nativeHandle = m_window.getNativeHandle();
    spdlog::info("  Native handle: {:p}", nativeHandle);

    clap_window_t clapWindow{};
    clapWindow.api = windowApi;
#ifdef _WIN32
    clapWindow.win32 = nativeHandle;
#elif defined(__APPLE__)
    clapWindow.cocoa = nativeHandle;
#else
    clapWindow.x11 = reinterpret_cast<unsigned long>(nativeHandle);
#endif

    if (!m_gui->set_parent(m_plugin, &clapWindow)) {
        throw std::runtime_error("clap_plugin_gui->set_parent() failed.");
    }
    spdlog::info("  set_parent() succeeded");

    // 11b. Explicitly set size — some plugins require this before show()
    spdlog::info("  Step 11b: Calling set_size({}x{})...", w, h);
    if (m_gui->set_size) {
        if (!m_gui->set_size(m_plugin, w, h)) {
            spdlog::warn("  set_size() returned false (non-fatal)");
        } else {
            spdlog::info("  set_size() succeeded");
        }
    }

    // 12. Show
    spdlog::info("  Step 12: Showing GUI...");
    if (!m_gui->show(m_plugin)) {
        // Some plugins return false from show() but still render correctly.
        // Log a warning but don't treat as fatal.
        spdlog::warn("  show() returned false — plugin may still render (continuing)");
    } else {
        spdlog::info("  show() succeeded — plugin GUI is now visible");
    }


    // 12b. Initial child HWND offset for sidebar (handled by SdlWindow::repositionChildHwnd)
    m_window.repositionChildHwnd(w, h);

    // 12c. Populate sidebar presets
    if (!m_presets.empty()) {
        m_window.setPresets(m_presets);
        m_window.setPresetSelectedCallback([this](const std::string& presetId) {
            spdlog::info("Sidebar preset selected: {}", presetId);
            loadPreset(presetId);
        });
    }

    return OpenResult{m_pluginName, w, h};
}

void ClapGuiHost::runEventLoop(
    std::function<void(const std::string& reason)> closedCb,
    std::function<void(std::vector<rps::ipc::ParameterValueUpdate>)> paramChangeCb) {
    spdlog::info("ClapGuiHost::runEventLoop() starting");

    auto resizeHandler = [this](uint32_t newWidth, uint32_t newHeight) {
        if (!m_canResize || !m_gui || !m_plugin) return;

        spdlog::debug("Window resized to {}x{}, syncing with plugin...", newWidth, newHeight);

        uint32_t adjustedW = newWidth;
        uint32_t adjustedH = newHeight;

        if (m_gui->adjust_size) {
            m_gui->adjust_size(m_plugin, &adjustedW, &adjustedH);
            spdlog::debug("  adjust_size: {}x{} -> {}x{}", newWidth, newHeight, adjustedW, adjustedH);
        }

        m_gui->set_size(m_plugin, adjustedW, adjustedH);

        if (adjustedW != newWidth || adjustedH != newHeight) {
            spdlog::debug("  SDL window resize correction to {}x{}", adjustedW, adjustedH);
            m_window.resize(adjustedW, adjustedH);
        }


        // Child HWND repositioning is handled automatically by SdlWindow::handleResize()
    };
    // Register resize handler via event watcher for live resize during drag
    m_window.setResizeCallback(resizeHandler);

    auto lastParamPoll = std::chrono::steady_clock::now();
    constexpr auto kParamPollInterval = std::chrono::milliseconds(50);

    while (m_window.pollEvents()) {
        // Parameter polling at ~20Hz
        if (paramChangeCb) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastParamPoll >= kParamPollInterval) {
                auto changes = pollParameterChanges();
                if (!changes.empty()) {
                    paramChangeCb(std::move(changes));
                }
                lastParamPoll = now;
            }
        }

        // No sleep needed — SDL_WaitEventTimeout blocks efficiently
    }

    spdlog::info("ClapGuiHost::runEventLoop() ended");

    if (closedCb) {
        closedCb("user");
    }
}

void ClapGuiHost::requestClose() {
    spdlog::info("ClapGuiHost::requestClose()");
    m_window.requestClose();
}

std::vector<rps::ipc::PluginParameterInfo> ClapGuiHost::getParameters() {
    std::vector<rps::ipc::PluginParameterInfo> result;
    if (!m_params || !m_params->count || !m_params->get_info) {
        spdlog::info("getParameters: params extension not available");
        return result;
    }

    uint32_t count = m_params->count(m_plugin);
    spdlog::info("getParameters: plugin has {} parameters", count);
    result.reserve(count);
    m_cachedParams.clear();
    m_cachedParams.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!m_params->get_info(m_plugin, i, &info)) {
            spdlog::warn("  get_info({}) failed, skipping", i);
            continue;
        }

        rps::ipc::PluginParameterInfo p;
        p.id = std::to_string(info.id);
        p.index = i;
        p.name = info.name;
        p.module = info.module;
        p.minValue = info.min_value;
        p.maxValue = info.max_value;
        p.defaultValue = info.default_value;

        // Get current value
        double val = info.default_value;
        if (m_params->get_value) {
            m_params->get_value(m_plugin, info.id, &val);
        }
        p.currentValue = val;

        // Get display text
        if (m_params->value_to_text) {
            char buf[256] = {};
            if (m_params->value_to_text(m_plugin, info.id, val, buf, sizeof(buf))) {
                p.displayText = buf;
            }
        }

        // Map CLAP flags to universal flags
        uint32_t flags = 0;
        if (info.flags & CLAP_PARAM_IS_STEPPED)  flags |= rps::ipc::kParamFlagStepped;
        if (info.flags & CLAP_PARAM_IS_HIDDEN)   flags |= rps::ipc::kParamFlagHidden;
        if (info.flags & CLAP_PARAM_IS_READONLY) flags |= rps::ipc::kParamFlagReadOnly;
        if (info.flags & CLAP_PARAM_IS_BYPASS)   flags |= rps::ipc::kParamFlagBypass;
        if (info.flags & CLAP_PARAM_IS_ENUM)     flags |= rps::ipc::kParamFlagEnum;
        p.flags = flags;

        result.push_back(p);

        // Cache for polling
        m_cachedParams.push_back({p.id, val});
    }

    return result;
}

std::vector<rps::ipc::ParameterValueUpdate> ClapGuiHost::pollParameterChanges() {
    std::vector<rps::ipc::ParameterValueUpdate> updates;
    if (!m_params || !m_params->count || !m_params->get_info || !m_params->get_value) {
        return updates;
    }

    uint32_t count = m_params->count(m_plugin);

    // If the parameter count changed (e.g. preset load), reinitialize
    if (count != static_cast<uint32_t>(m_cachedParams.size())) {
        spdlog::info("pollParameterChanges: param count changed ({} -> {}), full rescan needed",
                     m_cachedParams.size(), count);
        m_cachedParams.clear();
        return updates; // The caller should call getParameters() again
    }

    for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!m_params->get_info(m_plugin, i, &info)) {
            continue;
        }

        double val = 0.0;
        if (!m_params->get_value(m_plugin, info.id, &val)) {
            continue;
        }

        // Compare with cached value (epsilon for floating-point noise)
        if (i < m_cachedParams.size() && std::abs(val - m_cachedParams[i].lastValue) > 1e-9) {
            rps::ipc::ParameterValueUpdate u;
            u.paramId = m_cachedParams[i].id;
            u.value = val;

            // Get updated display text
            if (m_params->value_to_text) {
                char buf[256] = {};
                if (m_params->value_to_text(m_plugin, info.id, val, buf, sizeof(buf))) {
                    u.displayText = buf;
                }
            }

            m_cachedParams[i].lastValue = val;
            updates.push_back(std::move(u));
        }
    }

    return updates;
}

rps::ipc::GetStateResponse ClapGuiHost::saveState() {
    rps::ipc::GetStateResponse resp;

    if (!m_state || !m_state->save) {
        resp.success = false;
        resp.error = "Plugin does not support state extension";
        spdlog::warn("saveState: state extension not available");
        return resp;
    }

    // Memory-backed output stream
    std::vector<uint8_t> buffer;
    clap_ostream_t ostream{};
    ostream.ctx = &buffer;
    ostream.write = [](const clap_ostream_t* stream, const void* data, uint64_t size) -> int64_t {
        auto* buf = static_cast<std::vector<uint8_t>*>(stream->ctx);
        auto* bytes = static_cast<const uint8_t*>(data);
        buf->insert(buf->end(), bytes, bytes + size);
        return static_cast<int64_t>(size);
    };

    spdlog::info("saveState: saving plugin state...");
    if (!m_state->save(m_plugin, &ostream)) {
        resp.success = false;
        resp.error = "Plugin save() returned false";
        spdlog::error("saveState: plugin save() failed");
        return resp;
    }

    resp.stateData = std::move(buffer);
    resp.success = true;
    spdlog::info("saveState: saved {} bytes", resp.stateData.size());
    return resp;
}

rps::ipc::SetStateResponse ClapGuiHost::loadState(const std::vector<uint8_t>& stateData) {
    rps::ipc::SetStateResponse resp;

    if (!m_state || !m_state->load) {
        resp.success = false;
        resp.error = "Plugin does not support state extension";
        spdlog::warn("loadState: state extension not available");
        return resp;
    }

    // Memory-backed input stream
    struct ReadCtx {
        const std::vector<uint8_t>* data;
        size_t pos = 0;
    };
    ReadCtx readCtx{&stateData, 0};

    clap_istream_t istream{};
    istream.ctx = &readCtx;
    istream.read = [](const clap_istream_t* stream, void* buffer, uint64_t size) -> int64_t {
        auto* ctx = static_cast<ReadCtx*>(stream->ctx);
        size_t remaining = ctx->data->size() - ctx->pos;
        size_t toRead = std::min(static_cast<size_t>(size), remaining);
        if (toRead == 0) return 0; // EOF
        std::memcpy(buffer, ctx->data->data() + ctx->pos, toRead);
        ctx->pos += toRead;
        return static_cast<int64_t>(toRead);
    };

    spdlog::info("loadState: loading {} bytes...", stateData.size());
    if (!m_state->load(m_plugin, &istream)) {
        resp.success = false;
        resp.error = "Plugin load() returned false";
        spdlog::error("loadState: plugin load() failed");
        return resp;
    }

    // Clear cached params so next poll re-queries everything
    m_cachedParams.clear();

    resp.success = true;
    spdlog::info("loadState: state restored successfully");
    return resp;
}

std::vector<rps::ipc::PresetInfo> ClapGuiHost::getPresets() {
    return m_presets;
}

rps::ipc::LoadPresetResponse ClapGuiHost::loadPreset(const std::string& presetId) {
    rps::ipc::LoadPresetResponse resp;

    if (!m_presetLoad || !m_presetLoad->from_location) {
        resp.success = false;
        resp.error = "Plugin does not support preset-load extension";
        return resp;
    }

    // Find the preset by id
    auto it = std::find_if(m_presets.begin(), m_presets.end(),
        [&presetId](const rps::ipc::PresetInfo& p) { return p.id == presetId; });
    if (it == m_presets.end()) {
        resp.success = false;
        resp.error = "Preset not found: " + presetId;
        return resp;
    }

    spdlog::info("loadPreset: loading '{}' from '{}' (key='{}')",
                 it->name, it->location, it->id);

    bool ok = m_presetLoad->from_location(
        m_plugin, it->locationKind, it->location.c_str(),
        it->id.empty() ? nullptr : it->id.c_str());

    if (!ok) {
        resp.success = false;
        resp.error = "Plugin from_location() returned false";
        spdlog::error("loadPreset: from_location() failed");
        return resp;
    }

    // Clear param cache so we re-read everything
    m_cachedParams.clear();

    resp.success = true;
    spdlog::info("loadPreset: '{}' loaded successfully", it->name);
    return resp;
}

void ClapGuiHost::discoverPresets() {
    if (!m_entry) return;

    // Get the preset discovery factory from clap_entry
    auto* discoveryFactory = static_cast<const clap_preset_discovery_factory_t*>(
        m_entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!discoveryFactory) {
        spdlog::info("  No preset discovery factory — no presets available");
        return;
    }

    uint32_t providerCount = discoveryFactory->count(discoveryFactory);
    spdlog::info("  Preset discovery: {} provider(s)", providerCount);
    if (providerCount == 0) return;

    // Storage for declared locations and file types
    struct DiscoveryContext {
        std::vector<clap_preset_discovery_location_t> locations;
        std::vector<clap_preset_discovery_filetype_t> filetypes;
    };
    DiscoveryContext ctx;

    // Create an indexer that receives declarations
    clap_preset_discovery_indexer_t indexer{};
    indexer.clap_version = CLAP_VERSION;
    indexer.name = "RPS";
    indexer.vendor = "RPS";
    indexer.url = nullptr;
    indexer.version = "1.0";
    indexer.indexer_data = &ctx;

    indexer.declare_filetype = [](const clap_preset_discovery_indexer_t* idx,
                                  const clap_preset_discovery_filetype_t* ft) -> bool {
        auto* c = static_cast<DiscoveryContext*>(idx->indexer_data);
        c->filetypes.push_back(*ft);
        spdlog::debug("  Declared filetype: '{}' ext='{}'",
                      ft->name ? ft->name : "?", ft->file_extension ? ft->file_extension : "*");
        return true;
    };

    indexer.declare_location = [](const clap_preset_discovery_indexer_t* idx,
                                  const clap_preset_discovery_location_t* loc) -> bool {
        auto* c = static_cast<DiscoveryContext*>(idx->indexer_data);
        c->locations.push_back(*loc);
        spdlog::info("  Declared location: '{}' kind={} path='{}'",
                     loc->name ? loc->name : "?", loc->kind,
                     loc->location ? loc->location : "(plugin-internal)");
        return true;
    };

    indexer.declare_soundpack = [](const clap_preset_discovery_indexer_t*,
                                   const clap_preset_discovery_soundpack_t*) -> bool {
        return true; // Acknowledge but skip soundpack metadata for now
    };

    indexer.get_extension = [](const clap_preset_discovery_indexer_t*,
                               const char*) -> const void* {
        return nullptr;
    };

    // For each provider, init + crawl
    for (uint32_t i = 0; i < providerCount; ++i) {
        auto* desc = discoveryFactory->get_descriptor(discoveryFactory, i);
        if (!desc) continue;

        spdlog::info("  Provider: '{}' (id='{}')", desc->name, desc->id);

        auto* provider = discoveryFactory->create(discoveryFactory, &indexer, desc->id);
        if (!provider) {
            spdlog::warn("  Failed to create provider '{}'", desc->id);
            continue;
        }

        if (!provider->init(provider)) {
            spdlog::warn("  Provider init failed for '{}'", desc->id);
            provider->destroy(provider);
            continue;
        }

        // Crawl each declared location
        uint32_t presetIndex = 0;
        for (const auto& loc : ctx.locations) {
            if (loc.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
                // Plugin-internal presets: location is null
                struct MetadataCtx {
                    std::vector<rps::ipc::PresetInfo>* presets;
                    uint32_t* index;
                    uint32_t locationKind;
                    std::string location;
                    uint32_t flags;
                };
                MetadataCtx mctx{&m_presets, &presetIndex,
                                 loc.kind, "", loc.flags};

                clap_preset_discovery_metadata_receiver_t receiver{};
                receiver.receiver_data = &mctx;
                receiver.on_error = [](const clap_preset_discovery_metadata_receiver_t*, int32_t, const char* msg) {
                    spdlog::warn("  Preset metadata error: {}", msg ? msg : "unknown");
                };
                receiver.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* r,
                                           const char* name, const char* load_key) -> bool {
                    auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                    rps::ipc::PresetInfo info;
                    info.id = load_key ? load_key : "";
                    info.name = name ? name : "Unnamed";
                    info.location = mc->location;
                    info.locationKind = mc->locationKind;
                    info.index = (*mc->index)++;
                    info.flags = mc->flags;
                    mc->presets->push_back(std::move(info));
                    return true;
                };
                receiver.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t*, const clap_universal_plugin_id_t*) {};
                receiver.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                receiver.set_flags = [](const clap_preset_discovery_metadata_receiver_t* r, uint32_t flags) {
                    auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                    if (!mc->presets->empty()) mc->presets->back().flags = flags;
                };
                receiver.add_creator = [](const clap_preset_discovery_metadata_receiver_t* r, const char* creator) {
                    auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                    if (!mc->presets->empty() && creator) mc->presets->back().creator = creator;
                };
                receiver.set_description = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                receiver.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
                receiver.add_feature = [](const clap_preset_discovery_metadata_receiver_t* r, const char* feature) {
                    auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                    if (!mc->presets->empty() && feature) {
                        auto& cat = mc->presets->back().category;
                        if (!cat.empty()) cat += "/";
                        cat += feature;
                    }
                };
                receiver.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};

                provider->get_metadata(provider, loc.kind, nullptr, &receiver);

            } else if (loc.kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && loc.location) {
                // File-based presets: crawl directory
                std::filesystem::path locPath(loc.location);
                if (!std::filesystem::exists(locPath)) {
                    spdlog::debug("  Location does not exist: {}", loc.location);
                    continue;
                }

                auto crawlFile = [&](const std::filesystem::path& filePath) {
                    // Check file extension against declared filetypes
                    bool extensionMatch = ctx.filetypes.empty(); // empty = match all
                    std::string ext = filePath.extension().string();
                    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                    for (const auto& ft : ctx.filetypes) {
                        if (!ft.file_extension || ft.file_extension[0] == '\0' || ext == ft.file_extension) {
                            extensionMatch = true;
                            break;
                        }
                    }
                    if (!extensionMatch) return;

                    struct MetadataCtx {
                        std::vector<rps::ipc::PresetInfo>* presets;
                        uint32_t* index;
                        uint32_t locationKind;
                        std::string location;
                        uint32_t flags;
                    };
                    MetadataCtx mctx{&m_presets, &presetIndex,
                                     loc.kind, filePath.string(), loc.flags};

                    clap_preset_discovery_metadata_receiver_t receiver{};
                    receiver.receiver_data = &mctx;
                    receiver.on_error = [](const clap_preset_discovery_metadata_receiver_t*, int32_t, const char* msg) {
                        spdlog::debug("  Preset file error: {}", msg ? msg : "unknown");
                    };
                    receiver.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* r,
                                               const char* name, const char* load_key) -> bool {
                        auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                        rps::ipc::PresetInfo info;
                        info.id = load_key ? load_key : mc->location;
                        info.name = name ? name : std::filesystem::path(mc->location).stem().string();
                        info.location = mc->location;
                        info.locationKind = mc->locationKind;
                        info.index = (*mc->index)++;
                        info.flags = mc->flags;
                        mc->presets->push_back(std::move(info));
                        return true;
                    };
                    receiver.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t*, const clap_universal_plugin_id_t*) {};
                    receiver.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                    receiver.set_flags = [](const clap_preset_discovery_metadata_receiver_t* r, uint32_t flags) {
                        auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                        if (!mc->presets->empty()) mc->presets->back().flags = flags;
                    };
                    receiver.add_creator = [](const clap_preset_discovery_metadata_receiver_t* r, const char* creator) {
                        auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                        if (!mc->presets->empty() && creator) mc->presets->back().creator = creator;
                    };
                    receiver.set_description = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
                    receiver.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
                    receiver.add_feature = [](const clap_preset_discovery_metadata_receiver_t* r, const char* feature) {
                        auto* mc = static_cast<MetadataCtx*>(r->receiver_data);
                        if (!mc->presets->empty() && feature) {
                            auto& cat = mc->presets->back().category;
                            if (!cat.empty()) cat += "/";
                            cat += feature;
                        }
                    };
                    receiver.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};

                    provider->get_metadata(provider, loc.kind, filePath.string().c_str(), &receiver);
                };

                std::error_code ec;
                if (std::filesystem::is_directory(locPath, ec)) {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(locPath, ec)) {
                        if (entry.is_regular_file()) {
                            crawlFile(entry.path());
                        }
                    }
                } else if (std::filesystem::is_regular_file(locPath, ec)) {
                    crawlFile(locPath);
                }
            }
        }

        provider->destroy(provider);
        ctx.locations.clear();
        ctx.filetypes.clear();
    }

    spdlog::info("  Preset discovery complete: {} preset(s) found", m_presets.size());
}

} // namespace rps::scanner
