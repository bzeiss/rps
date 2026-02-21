#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <rps/scanner/Vst3Scanner.hpp>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

// Include minimum VST3 COM interfaces needed to parse the factory
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

// Disable GCC format warnings for the Steinberg SDK file which triggers -Werror
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#endif

#include <pluginterfaces/base/funknown.cpp>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace Steinberg {
    const FUID IPluginFactory::iid(0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
    const FUID IPluginFactory2::iid(0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
    const FUID IPluginFactory3::iid(0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931);
}

namespace rps::scanner {

bool Vst3Scanner::canHandle(const boost::filesystem::path& pluginPath) const {
    auto ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".vst3";
}

rps::ipc::ScanResult Vst3Scanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    progressCb(10, "Loading VST3 binary...");

#ifdef _WIN32
    HMODULE handle = LoadLibraryW(pluginPath.c_str());
    if (!handle) {
        throw std::runtime_error("Failed to load VST3 DLL: " + pluginPath.string());
    }

    auto getFactory = reinterpret_cast<Steinberg::IPluginFactory* (*)()>(
        reinterpret_cast<void*>(GetProcAddress(handle, "GetPluginFactory"))
    );
#else
    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error(std::string("Failed to load VST3 library: ") + dlerror());
    }

    auto getFactory = reinterpret_cast<Steinberg::IPluginFactory* (*)()>(dlsym(handle, "GetPluginFactory"));
#endif

    if (!getFactory) {
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("Library does not export 'GetPluginFactory'. Not a valid VST3 plugin.");
    }

    progressCb(30, "Getting VST3 Plugin Factory...");
    Steinberg::IPluginFactory* factory = getFactory();
    if (!factory) {
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("GetPluginFactory returned null.");
    }

    progressCb(50, "Extracting plugin metadata...");
    
    int32_t numClasses = factory->countClasses();
    if (numClasses == 0) {
        factory->release();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("VST3 factory contains no classes.");
    }

    // Find the first Audio Module class
    Steinberg::PClassInfo classInfo;
    bool foundAudioModule = false;
    for (int32_t i = 0; i < numClasses; ++i) {
        if (factory->getClassInfo(i, &classInfo) == Steinberg::kResultOk) {
            if (std::strcmp(classInfo.category, kVstAudioEffectClass) == 0) {
                foundAudioModule = true;
                break;
            }
        }
    }

    if (!foundAudioModule) {
        factory->release();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("VST3 factory contains no audio effects.");
    }

    rps::ipc::ScanResult result;
    result.name = classInfo.name;
    result.vendor = "Unknown VST3 Vendor"; // Need IPluginFactory2/3 for vendor details
    result.version = "1.0.0";

    // Attempt to query IPluginFactory2 for vendor string
    Steinberg::IPluginFactory2* factory2 = nullptr;
    if (factory->queryInterface(Steinberg::IPluginFactory2::iid, reinterpret_cast<void**>(&factory2)) == Steinberg::kResultOk) {
        Steinberg::PClassInfo2 classInfo2;
        if (factory2->getClassInfo2(0, &classInfo2) == Steinberg::kResultOk) {
            result.vendor = classInfo2.vendor;
            result.version = classInfo2.version;
        }
        factory2->release();
    }

    progressCb(80, "Extracting features...");
    result.numInputs = 2;   // Faked without full host instantiation
    result.numOutputs = 2;  // Faked without full host instantiation
    result.parameters.push_back({0, "VST3 Param 1 (Mock)", 0.5});

    progressCb(90, "Releasing factory...");
    factory->release();

#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif

    progressCb(100, "Done.");
    return result;
}

} // namespace rps::scanner
