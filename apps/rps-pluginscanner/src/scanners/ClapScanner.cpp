#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <rps/scanner/ClapScanner.hpp>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <rps/core/clap/include/clap/clap.h>
#include <rps/core/clap/include/clap/ext/params.h>
#include <rps/core/clap/include/clap/ext/audio-ports.h>

extern bool g_verbose;

namespace rps::scanner {

// ---------------------------------------------------------------------------
// PE / ELF architecture check — detect arch mismatch before LoadLibrary/dlopen
// ---------------------------------------------------------------------------
namespace {
#ifdef _WIN32
void checkBinaryArchitecture(const boost::filesystem::path& binaryPath) {
    std::ifstream f(binaryPath.string(), std::ios::binary);
    if (!f) return;

    char dosHeader[64];
    f.read(dosHeader, sizeof(dosHeader));
    if (!f || dosHeader[0] != 'M' || dosHeader[1] != 'Z') return;

    uint32_t peOffset = *reinterpret_cast<uint32_t*>(&dosHeader[0x3C]);
    f.seekg(peOffset);

    char peSig[4];
    f.read(peSig, 4);
    if (!f || peSig[0] != 'P' || peSig[1] != 'E' || peSig[2] != 0 || peSig[3] != 0) return;

    uint16_t machine = 0;
    f.read(reinterpret_cast<char*>(&machine), 2);
    if (!f) return;

#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr uint16_t expectedMachine = 0xAA64;
    constexpr const char* expectedArch = "ARM64";
#elif defined(_M_X64) || defined(__x86_64__)
    constexpr uint16_t expectedMachine = 0x8664;
    constexpr const char* expectedArch = "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    constexpr uint16_t expectedMachine = 0x014C;
    constexpr const char* expectedArch = "x86";
#else
    return;
#endif

    if (machine != expectedMachine) {
        std::string binaryArch = "unknown";
        if (machine == 0x014C) binaryArch = "x86";
        else if (machine == 0x8664) binaryArch = "x86_64";
        else if (machine == 0xAA64) binaryArch = "ARM64";
        throw std::runtime_error(
            "Architecture mismatch: binary is " + binaryArch
            + " but scanner is " + expectedArch
            + ": " + binaryPath.string());
    }
}
#else
void checkBinaryArchitecture(const boost::filesystem::path& binaryPath) {
    std::ifstream f(binaryPath.string(), std::ios::binary);
    if (!f) return;

    char elfHeader[20];
    f.read(elfHeader, sizeof(elfHeader));
    if (!f || elfHeader[0] != 0x7F || elfHeader[1] != 'E' || elfHeader[2] != 'L' || elfHeader[3] != 'F')
        return;

    uint16_t eMachine = *reinterpret_cast<uint16_t*>(&elfHeader[18]);
    std::string binaryArch;
    if (eMachine == 0x3E)       binaryArch = "x86_64";
    else if (eMachine == 0xB7)  binaryArch = "aarch64";
    else if (eMachine == 0x03)  binaryArch = "x86";
    else return;

#if defined(__x86_64__)
    const char* hostArch = "x86_64";
#elif defined(__aarch64__)
    const char* hostArch = "aarch64";
#elif defined(__i386__)
    const char* hostArch = "x86";
#else
    return;
#endif

    if (binaryArch != hostArch) {
        throw std::runtime_error(
            "Architecture mismatch: binary is " + binaryArch
            + " but scanner is " + hostArch
            + ": " + binaryPath.string());
    }
}
#endif

// ---------------------------------------------------------------------------
// Minimal CLAP host — just enough to instantiate plugins for metadata extraction
// ---------------------------------------------------------------------------
const void* hostGetExtension(const clap_host*, const char*) { return nullptr; }
void hostRequestRestart(const clap_host*) {}
void hostRequestProcess(const clap_host*) {}
void hostRequestCallback(const clap_host*) {}

clap_host_t makeScannerHost() {
    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "rps-pluginscanner";
    host.vendor = "rps";
    host.url = "";
    host.version = "1.0.0";
    host.get_extension = hostGetExtension;
    host.request_restart = hostRequestRestart;
    host.request_process = hostRequestProcess;
    host.request_callback = hostRequestCallback;
    host.host_data = nullptr;
    return host;
}

// RAII wrapper for library handle cleanup
struct LibHandle {
#ifdef _WIN32
    HMODULE h = nullptr;
    void unload() { if (h) { FreeLibrary(h); h = nullptr; } }
#else
    void* h = nullptr;
    void unload() { if (h) { dlclose(h); h = nullptr; } }
#endif
    void release() { h = nullptr; } // Prevent cleanup on destruction
    ~LibHandle() { unload(); }
};

} // anonymous namespace

bool ClapScanner::canHandle(const boost::filesystem::path& pluginPath) const {
    auto ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".clap";
}

rps::ipc::ScanResult ClapScanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    auto logStage = [&](const std::string& stage) {
        if (g_verbose) {
            std::cerr << "[clap] " << pluginPath.filename().string() << ": " << stage << std::endl;
        }
    };

    // Architecture check — detect x86/x64/ARM64 mismatch before loading
    progressCb(5, "Checking binary architecture...");
    logStage("Checking binary architecture...");
    checkBinaryArchitecture(pluginPath);
    logStage("Architecture OK.");

    progressCb(10, "Loading CLAP binary...");
    logStage("Calling LoadLibrary/dlopen...");

    LibHandle lib;
#ifdef _WIN32
    lib.h = LoadLibraryW(pluginPath.c_str());
    if (!lib.h) {
        DWORD err = GetLastError();
        throw std::runtime_error("Failed to load CLAP DLL: " + pluginPath.string()
                                 + " (Win32 error: " + std::to_string(err) + ")");
    }
    logStage("LoadLibrary succeeded.");

    void* procAddress = reinterpret_cast<void*>(GetProcAddress(lib.h, "clap_entry"));
    if (!procAddress) {
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }
    const clap_plugin_entry* entry = reinterpret_cast<const clap_plugin_entry*>(procAddress);
#else
    lib.h = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib.h) {
        throw std::runtime_error(std::string("Failed to load CLAP library: ") + dlerror());
    }
    logStage("dlopen succeeded.");

    void* symAddress = dlsym(lib.h, "clap_entry");
    if (!symAddress) {
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }
    const clap_plugin_entry* entry = reinterpret_cast<const clap_plugin_entry*>(symAddress);
#endif

    if (!entry) {
        throw std::runtime_error("clap_entry returned null.");
    }

    // Verify CLAP version compatibility
    if (!clap_version_is_compatible(entry->clap_version)) {
        throw std::runtime_error("Incompatible CLAP version: "
            + std::to_string(entry->clap_version.major) + "."
            + std::to_string(entry->clap_version.minor) + "."
            + std::to_string(entry->clap_version.revision));
    }
    logStage("CLAP version " + std::to_string(entry->clap_version.major) + "."
             + std::to_string(entry->clap_version.minor) + "."
             + std::to_string(entry->clap_version.revision) + " — compatible.");

    progressCb(20, "Initializing CLAP entry point...");
    logStage("Calling entry->init()...");
    if (!entry->init(pluginPath.string().c_str())) {
        throw std::runtime_error("Failed to initialize CLAP plugin entry.");
    }
    logStage("entry->init() succeeded.");

    progressCb(30, "Querying plugin factory...");
    logStage("Querying plugin factory...");
    const void* factoryPtr = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factoryPtr) {
        throw std::runtime_error("CLAP library does not provide a plugin factory.");
    }

    const clap_plugin_factory* factory = static_cast<const clap_plugin_factory*>(factoryPtr);
    uint32_t numPlugins = factory->get_plugin_count(factory);
    logStage("Factory reports " + std::to_string(numPlugins) + " plugin(s).");

    if (numPlugins == 0) {
        throw std::runtime_error("SKIP: CLAP factory contains no plugins.");
    }

    // Scan the first plugin (multi-plugin bundles will have additional entries)
    if (numPlugins > 1) {
        logStage("Multi-plugin bundle detected. Scanning first plugin only (TODO: scan all).");
    }

    const clap_plugin_descriptor* desc = factory->get_plugin_descriptor(factory, 0);
    if (!desc) {
        throw std::runtime_error("CLAP factory returned null descriptor for index 0.");
    }

    progressCb(40, "Extracting descriptor metadata...");
    logStage("Descriptor: name=\"" + std::string(desc->name ? desc->name : "") + "\""
             + " vendor=\"" + std::string(desc->vendor ? desc->vendor : "") + "\""
             + " id=\"" + std::string(desc->id ? desc->id : "") + "\"");

    rps::ipc::ScanResult result;
    result.name = desc->name ? desc->name : "Unknown CLAP";
    result.vendor = desc->vendor ? desc->vendor : "Unknown Vendor";
    result.version = desc->version ? desc->version : "1.0.0";
    result.uid = desc->id ? desc->id : "";
    result.description = desc->description ? desc->description : "";
    result.url = desc->url ? desc->url : "";
    result.scanMethod = "clap-factory";

    // Parse features into a single comma-separated category string
    if (desc->features) {
        std::string categories;
        for (int i = 0; desc->features[i] != nullptr; ++i) {
            if (i > 0) categories += ", ";
            categories += desc->features[i];
        }
        result.category = categories;
        logStage("Features: " + categories);
    }

    // Instantiate plugin with minimal host to extract params and audio ports
    progressCb(50, "Instantiating plugin for metadata...");
    logStage("Creating plugin instance...");

    auto host = makeScannerHost();
    const clap_plugin* plugin = factory->create_plugin(factory, &host, desc->id);

    if (plugin) {
        logStage("Plugin instance created. Calling plugin->init()...");
        if (plugin->init(plugin)) {
            logStage("plugin->init() succeeded.");

            // --- Extract audio ports ---
            progressCb(60, "Querying audio ports...");
            auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
                plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));

            if (audioPorts) {
                uint32_t numInputPorts = audioPorts->count(plugin, true);
                uint32_t numOutputPorts = audioPorts->count(plugin, false);
                uint32_t totalInputChannels = 0;
                uint32_t totalOutputChannels = 0;

                for (uint32_t i = 0; i < numInputPorts; ++i) {
                    clap_audio_port_info_t info{};
                    if (audioPorts->get(plugin, i, true, &info)) {
                        totalInputChannels += info.channel_count;
                        logStage("  Input port " + std::to_string(i) + ": \""
                                 + info.name + "\" (" + std::to_string(info.channel_count) + " ch)");
                    }
                }
                for (uint32_t i = 0; i < numOutputPorts; ++i) {
                    clap_audio_port_info_t info{};
                    if (audioPorts->get(plugin, i, false, &info)) {
                        totalOutputChannels += info.channel_count;
                        logStage("  Output port " + std::to_string(i) + ": \""
                                 + info.name + "\" (" + std::to_string(info.channel_count) + " ch)");
                    }
                }
                result.numInputs = totalInputChannels;
                result.numOutputs = totalOutputChannels;
                logStage("Audio I/O: " + std::to_string(totalInputChannels) + " in, "
                         + std::to_string(totalOutputChannels) + " out.");
            } else {
                logStage("Plugin does not implement clap.audio-ports extension.");
            }

            // --- Extract parameters ---
            progressCb(70, "Querying parameters...");
            auto* params = static_cast<const clap_plugin_params_t*>(
                plugin->get_extension(plugin, CLAP_EXT_PARAMS));

            if (params) {
                uint32_t numParams = params->count(plugin);
                logStage("Plugin reports " + std::to_string(numParams) + " parameter(s).");

                for (uint32_t i = 0; i < numParams; ++i) {
                    clap_param_info_t pinfo{};
                    if (params->get_info(plugin, i, &pinfo)) {
                        // Sanitize non-finite values (NaN, Inf) — boost::json rejects them
                        double defVal = pinfo.default_value;
                        if (!std::isfinite(defVal)) defVal = 0.0;
                        result.parameters.push_back({
                            static_cast<uint32_t>(pinfo.id),
                            pinfo.name,
                            defVal
                        });
                    }
                }
                logStage("Extracted " + std::to_string(result.parameters.size()) + " parameter(s).");
            } else {
                logStage("Plugin does not implement clap.params extension.");
            }

        } else {
            logStage("plugin->init() returned false — skipping param/port extraction.");
        }
    } else {
        logStage("create_plugin returned null — skipping param/port extraction.");
    }

    progressCb(90, "Metadata extraction complete.");
    logStage("Metadata extraction complete — returning result before cleanup.");

    // IMPORTANT: Skip plugin->destroy(), entry->deinit(), and FreeLibrary.
    // Some CLAP plugins (e.g. The Usual Suspects synths) hang for minutes
    // during cleanup due to internal thread joins or resource teardown.
    // Since the scanner process exits immediately after returning, the OS
    // reclaims all resources. Skipping cleanup prevents hanging.
    lib.release(); // Prevent RAII FreeLibrary

    progressCb(100, "Done.");
    logStage("Scan complete.");
    return result;
}

} // namespace rps::scanner
