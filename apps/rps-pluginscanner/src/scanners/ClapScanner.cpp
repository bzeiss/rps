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
#include <stdexcept>
#include <cstring>
#include <rps/core/clap/include/clap/clap.h>

extern bool g_verbose;

namespace rps::scanner {

bool ClapScanner::canHandle(const boost::filesystem::path& pluginPath) const {
    auto ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".clap";
}

rps::ipc::ScanResult ClapScanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    if (g_verbose) std::cerr << "[DEBUG] ClapScanner::scan called for " << pluginPath.string() << "\n";
    progressCb(10, "Loading CLAP binary...");

#ifdef _WIN32
    if (g_verbose) std::cerr << "[DEBUG] Calling LoadLibraryW\n";
    HMODULE handle = LoadLibraryW(pluginPath.c_str());
    if (!handle) {
        throw std::runtime_error("Failed to load CLAP DLL: " + pluginPath.string());
    }
    
    if (g_verbose) std::cerr << "[DEBUG] Calling GetProcAddress\n";
    void* procAddress = reinterpret_cast<void*>(GetProcAddress(handle, "clap_entry"));
    if (g_verbose) std::cerr << "[DEBUG] GetProcAddress returned: " << procAddress << "\n";
    
    if (!procAddress) {
        FreeLibrary(handle);
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }

    const clap_plugin_entry* entry = reinterpret_cast<const clap_plugin_entry*>(procAddress);
    if (g_verbose) std::cerr << "[DEBUG] entry pointer: " << entry << "\n";
#else
    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error(std::string("Failed to load CLAP library: ") + dlerror());
    }

    void* symAddress = dlsym(handle, "clap_entry");

    if (!symAddress) {
        dlclose(handle);
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }

    const clap_plugin_entry* entry = reinterpret_cast<const clap_plugin_entry*>(symAddress);
#endif

    if (!entry) {
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("clap_entry returned null.");
    }

    progressCb(30, "Initializing CLAP entry point...");
    if (g_verbose) std::cerr << "[DEBUG] Calling entry->init()\n";
    if (!entry->init(pluginPath.string().c_str())) {
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("Failed to initialize CLAP plugin entry.");
    }

    if (g_verbose) std::cerr << "[DEBUG] entry->init() successful. Calling get_factory()\n";
    const void* factoryPtr = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factoryPtr) {
        entry->deinit();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("CLAP library does not provide a plugin factory.");
    }

    const clap_plugin_factory* factory = static_cast<const clap_plugin_factory*>(factoryPtr);
    
    uint32_t numPlugins = factory->get_plugin_count(factory);
    if (numPlugins == 0) {
        entry->deinit();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("CLAP factory contains no plugins.");
    }

    // For now, just grab the first plugin in the bundle.
    const clap_plugin_descriptor* desc = factory->get_plugin_descriptor(factory, 0);
    if (!desc) {
        entry->deinit();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("CLAP factory returned null descriptor.");
    }
    
    rps::ipc::ScanResult result;
    result.name = desc->name ? desc->name : "Unknown CLAP";
    result.vendor = desc->vendor ? desc->vendor : "Unknown Vendor";
    result.version = desc->version ? desc->version : "1.0.0";
    
    progressCb(80, "Extracting features...");
    result.numInputs = 2;   // Faked without full host instantiation
    result.numOutputs = 2;  // Faked without full host instantiation
    
    result.parameters.push_back({0, "CLAP Param 1 (Mock)", 0.5});

    progressCb(90, "Deinitializing CLAP plugin...");
    entry->deinit();

#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif

    progressCb(100, "Done.");
    return result;
}

} // namespace rps::scanner
