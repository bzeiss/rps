#include <rps/scanner/LadspaScanner.hpp>

#ifdef __linux__
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>

#include <rps/core/ladspa/src/ladspa.h>

extern bool g_verbose;

namespace rps::scanner {

namespace {

void checkBinaryArchitecture(const boost::filesystem::path& binaryPath) {
    std::ifstream f(binaryPath.string(), std::ios::binary);
    if (!f) return;

    unsigned char elfIdent[16] = {};
    f.read(reinterpret_cast<char*>(elfIdent), sizeof(elfIdent));
    if (!f || elfIdent[0] != 0x7F || elfIdent[1] != 'E' || elfIdent[2] != 'L' || elfIdent[3] != 'F') return;

    uint8_t elfClass = elfIdent[4]; // 1=32-bit, 2=64-bit
    std::string binaryArch = (elfClass == 2) ? "64-bit" : "32-bit";

#if defined(__x86_64__) || defined(__aarch64__)
    std::string hostArch = "64-bit";
#else
    std::string hostArch = "32-bit";
#endif

    if (binaryArch != hostArch) {
        throw std::runtime_error(
            "SKIP: Architecture mismatch: binary is " + binaryArch
            + " but scanner is " + hostArch
            + ": " + binaryPath.string());
    }
}

// RAII wrapper for library handle cleanup
struct LibHandle {
    void* h = nullptr;
    void unload() { if (h) { dlclose(h); h = nullptr; } }
    void release() { h = nullptr; } // Prevent cleanup on destruction
    ~LibHandle() { unload(); }
};

} // namespace

bool LadspaScanner::canHandle(const boost::filesystem::path& pluginPath) const {
    std::string ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".so";
}

rps::ipc::ScanResult LadspaScanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    auto logStage = [&](const std::string& stage) {
        if (g_verbose) {
            std::cerr << "[ladspa] " << pluginPath.filename().string() << ": " << stage << std::endl;
        }
    };

    progressCb(5, "Checking binary architecture...");
    logStage("Checking binary architecture...");
    checkBinaryArchitecture(pluginPath);
    logStage("Architecture OK.");

    progressCb(10, "Loading LADSPA binary...");
    logStage("Calling dlopen...");

    LibHandle lib;
    lib.h = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib.h) {
        throw std::runtime_error(std::string("Failed to load LADSPA library: ") + dlerror());
    }
    logStage("dlopen succeeded.");

    void* symAddress = dlsym(lib.h, "ladspa_descriptor");
    if (!symAddress) {
        throw std::runtime_error("SKIP: Library does not export 'ladspa_descriptor'. Not a valid LADSPA plugin.");
    }
    LADSPA_Descriptor_Function descriptorFunc = reinterpret_cast<LADSPA_Descriptor_Function>(symAddress);

    progressCb(30, "Querying plugin descriptor...");
    logStage("Querying plugin descriptor 0...");
    const LADSPA_Descriptor* desc = descriptorFunc(0);
    if (!desc) {
        throw std::runtime_error("SKIP: LADSPA library returned null descriptor for index 0.");
    }

    // Check if there are more
    unsigned long numPlugins = 0;
    while (descriptorFunc(numPlugins) != nullptr) {
        numPlugins++;
    }
    logStage("Library contains " + std::to_string(numPlugins) + " plugin(s).");

    if (numPlugins > 1) {
        logStage("Multi-plugin bundle detected. Scanning first plugin only (TODO: scan all).");
    }

    progressCb(40, "Extracting descriptor metadata...");
    
    rps::ipc::ScanResult result;
    result.name = desc->Name ? desc->Name : "Unknown LADSPA";
    result.vendor = desc->Maker ? desc->Maker : "Unknown Vendor";
    result.version = "1.0"; // LADSPA doesn't provide version
    result.uid = std::to_string(desc->UniqueID);
    result.description = "";
    result.url = "";
    result.format = "ladspa";
    result.scanMethod = "ladspa_descriptor";
    result.category = "Effect";

    logStage(std::string("Descriptor: name=\"") + result.name + "\" vendor=\"" + result.vendor + "\" uid=\"" + result.uid + "\"");

    progressCb(60, "Querying ports and parameters...");
    
    uint32_t numInputs = 0;
    uint32_t numOutputs = 0;

    for (unsigned long i = 0; i < desc->PortCount; ++i) {
        LADSPA_PortDescriptor pdesc = desc->PortDescriptors[i];
        
        if (LADSPA_IS_PORT_AUDIO(pdesc)) {
            if (LADSPA_IS_PORT_INPUT(pdesc)) {
                numInputs++;
            } else if (LADSPA_IS_PORT_OUTPUT(pdesc)) {
                numOutputs++;
            }
        } else if (LADSPA_IS_PORT_CONTROL(pdesc) && LADSPA_IS_PORT_INPUT(pdesc)) {
            // It's a parameter
            LADSPA_PortRangeHint hint = desc->PortRangeHints[i];
            double defVal = 0.0;
            
            if (LADSPA_IS_HINT_HAS_DEFAULT(hint.HintDescriptor)) {
                if (LADSPA_IS_HINT_DEFAULT_0(hint.HintDescriptor)) defVal = 0.0;
                else if (LADSPA_IS_HINT_DEFAULT_1(hint.HintDescriptor)) defVal = 1.0;
                else if (LADSPA_IS_HINT_DEFAULT_100(hint.HintDescriptor)) defVal = 100.0;
                else if (LADSPA_IS_HINT_DEFAULT_440(hint.HintDescriptor)) defVal = 440.0;
                else if (LADSPA_IS_HINT_DEFAULT_MINIMUM(hint.HintDescriptor)) defVal = hint.LowerBound;
                else if (LADSPA_IS_HINT_DEFAULT_MAXIMUM(hint.HintDescriptor)) defVal = hint.UpperBound;
                else if (LADSPA_IS_HINT_DEFAULT_LOW(hint.HintDescriptor)) {
                    if (LADSPA_IS_HINT_LOGARITHMIC(hint.HintDescriptor)) {
                        defVal = std::exp(std::log(std::max(1e-6f, hint.LowerBound))*0.75 + std::log(std::max(1e-6f, hint.UpperBound))*0.25);
                    } else {
                        defVal = hint.LowerBound*0.75 + hint.UpperBound*0.25;
                    }
                }
                else if (LADSPA_IS_HINT_DEFAULT_MIDDLE(hint.HintDescriptor)) {
                    if (LADSPA_IS_HINT_LOGARITHMIC(hint.HintDescriptor)) {
                        defVal = std::exp(std::log(std::max(1e-6f, hint.LowerBound))*0.5 + std::log(std::max(1e-6f, hint.UpperBound))*0.5);
                    } else {
                        defVal = hint.LowerBound*0.5 + hint.UpperBound*0.5;
                    }
                }
                else if (LADSPA_IS_HINT_DEFAULT_HIGH(hint.HintDescriptor)) {
                    if (LADSPA_IS_HINT_LOGARITHMIC(hint.HintDescriptor)) {
                        defVal = std::exp(std::log(std::max(1e-6f, hint.LowerBound))*0.25 + std::log(std::max(1e-6f, hint.UpperBound))*0.75);
                    } else {
                        defVal = hint.LowerBound*0.25 + hint.UpperBound*0.75;
                    }
                }
            }

            if (!std::isfinite(defVal)) defVal = 0.0;

            std::string paramName = desc->PortNames[i] ? desc->PortNames[i] : ("Param " + std::to_string(i));
            
            result.parameters.push_back({
                static_cast<uint32_t>(i),
                paramName,
                defVal
            });
        }
    }

    result.numInputs = numInputs;
    result.numOutputs = numOutputs;
    logStage("Audio I/O: " + std::to_string(numInputs) + " in, " + std::to_string(numOutputs) + " out.");
    logStage("Extracted " + std::to_string(result.parameters.size()) + " parameter(s).");

    progressCb(90, "Metadata extraction complete.");
    
    // Release library without unloading, in case of cleanup hangs
    lib.release();
    
    progressCb(100, "Done.");
    logStage("Scan complete.");

    return result;
}

} // namespace rps::scanner

#endif // __linux__