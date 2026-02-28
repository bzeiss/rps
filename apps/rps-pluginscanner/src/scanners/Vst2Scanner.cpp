#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <rps/scanner/Vst2Scanner.hpp>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

// VST2.4 SDK headers — suppress warnings from third-party code
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Winvalid-utf8"
#endif
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif

#define VST_2_4_EXTENSIONS 1
#ifndef _WIN32
#ifndef __cdecl
#define __cdecl
#endif
#endif
#include "pluginterfaces/vst2.x/aeffect.h"
#include "pluginterfaces/vst2.x/aeffectx.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

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

    char dosHeader[64] = {};
    f.read(dosHeader, sizeof(dosHeader));
    if (!f || dosHeader[0] != 'M' || dosHeader[1] != 'Z') return;

    uint32_t peOffset = *reinterpret_cast<uint32_t*>(&dosHeader[0x3C]);
    f.seekg(peOffset);
    char peSig[4] = {};
    f.read(peSig, 4);
    if (!f || peSig[0] != 'P' || peSig[1] != 'E' || peSig[2] != 0 || peSig[3] != 0) return;

    uint16_t machine = 0;
    f.read(reinterpret_cast<char*>(&machine), 2);
    if (!f) return;

    std::string binaryArch;
    switch (machine) {
        case 0x014C: binaryArch = "x86"; break;
        case 0x8664: binaryArch = "x86_64"; break;
        case 0xAA64: binaryArch = "ARM64"; break;
        default: return; // Unknown arch — let LoadLibrary decide
    }

#if defined(_M_X64) || defined(__x86_64__)
    std::string hostArch = "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    std::string hostArch = "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    std::string hostArch = "ARM64";
#else
    return;
#endif

    if (binaryArch != hostArch) {
        throw std::runtime_error(
            "SKIP: Architecture mismatch: binary is " + binaryArch
            + " but scanner is " + hostArch
            + ": " + binaryPath.string());
    }
}
#else
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
#endif

// ---------------------------------------------------------------------------
// Minimal audioMasterCallback for plugin instantiation
// ---------------------------------------------------------------------------
static VstIntPtr VSTCALLBACK hostCallback(AEffect* /*effect*/, VstInt32 opcode,
                                           VstInt32 /*index*/, VstIntPtr /*value*/,
                                           void* /*ptr*/, float /*opt*/) {
    switch (opcode) {
        case audioMasterVersion:
            return 2400; // Report VST 2.4 host
        case audioMasterCurrentId:
            return 0; // No shell sub-plugin selected
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Map VstPlugCategory enum to human-readable string
// ---------------------------------------------------------------------------
static std::string categoryToString(VstInt32 cat) {
    switch (cat) {
        case kPlugCategEffect:          return "Effect";
        case kPlugCategSynth:           return "Instrument";
        case kPlugCategAnalysis:        return "Analyzer";
        case kPlugCategMastering:       return "Mastering";
        case kPlugCategSpacializer:     return "Spatial";
        case kPlugCategRoomFx:          return "Reverb";
        case kPlugSurroundFx:           return "Surround";
        case kPlugCategRestoration:     return "Restoration";
        case kPlugCategOfflineProcess:  return "Offline";
        case kPlugCategShell:           return "Shell";
        case kPlugCategGenerator:       return "Generator";
        default:                        return "Unknown";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// canHandle — check if this file looks like a VST2 plugin
// ---------------------------------------------------------------------------
bool Vst2Scanner::canHandle(const boost::filesystem::path& pluginPath) const {
    std::string ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#if defined(_WIN32)
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".vst";
#else
    return ext == ".so";
#endif
}

// ---------------------------------------------------------------------------
// scan — load VST2 plugin and extract metadata
// ---------------------------------------------------------------------------
rps::ipc::ScanResult Vst2Scanner::scan(const boost::filesystem::path& pluginPath,
                                         ProgressCallback progressCb) {
    std::string pluginName = pluginPath.filename().string();

    auto logStage = [&](const std::string& stage) {
        if (g_verbose) {
            std::cerr << "[vst2] " << pluginName << ": " << stage << std::endl;
        }
    };

    progressCb(5, "Checking architecture...");
    logStage("Checking binary architecture...");
    checkBinaryArchitecture(pluginPath);
    logStage("Architecture OK.");

    progressCb(10, "Loading VST2 binary...");
    logStage("Loading library...");

    // --- Platform-specific loading ---
    typedef AEffect* (VSTCALLBACK *VstEntryProc)(audioMasterCallback);
    VstEntryProc entryProc = nullptr;

#ifdef _WIN32
    HMODULE handle = LoadLibraryW(pluginPath.c_str());
    if (!handle) {
        DWORD err = GetLastError();
        throw std::runtime_error("Failed to load VST2 DLL: " + pluginPath.string()
                                 + " (Win32 error: " + std::to_string(err) + ")");
    }
    logStage("LoadLibrary succeeded.");

    entryProc = reinterpret_cast<VstEntryProc>(
        reinterpret_cast<void*>(GetProcAddress(handle, "VSTPluginMain")));
    if (!entryProc) {
        entryProc = reinterpret_cast<VstEntryProc>(
            reinterpret_cast<void*>(GetProcAddress(handle, "main")));
    }
#else
    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error("Failed to load VST2 shared library: " + pluginPath.string()
                                 + " (" + dlerror() + ")");
    }
    logStage("dlopen succeeded.");

    entryProc = reinterpret_cast<VstEntryProc>(dlsym(handle, "VSTPluginMain"));
    if (!entryProc) {
        entryProc = reinterpret_cast<VstEntryProc>(dlsym(handle, "main"));
    }
#endif

    if (!entryProc) {
        throw std::runtime_error("SKIP: No VSTPluginMain or main export found. Not a VST2 plugin: "
                                 + pluginPath.string());
    }
    logStage("Entry point found.");

    // --- Instantiate plugin ---
    progressCb(30, "Instantiating plugin...");
    logStage("Calling entry point...");
    AEffect* effect = entryProc(hostCallback);

    if (!effect) {
        throw std::runtime_error("VST2 entry point returned null for: " + pluginPath.string());
    }

    if (effect->magic != kEffectMagic) {
        throw std::runtime_error("VST2 magic number mismatch (expected 'VstP'): " + pluginPath.string());
    }
    logStage("Plugin instantiated. magic=VstP, numParams=" + std::to_string(effect->numParams)
             + ", numInputs=" + std::to_string(effect->numInputs)
             + ", numOutputs=" + std::to_string(effect->numOutputs));

    // Call effOpen to fully initialize the plugin. This may trigger license checks,
    // sample loading, etc. on some plugins, but gives more accurate metadata.
    progressCb(30, "Opening plugin...");
    logStage("Calling effOpen...");
    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
    logStage("effOpen returned.");

    // --- Extract metadata ---
    progressCb(50, "Extracting metadata...");

    rps::ipc::ScanResult result;

    // Effect name
    char nameBuf[kVstMaxEffectNameLen + 1] = {};
    effect->dispatcher(effect, effGetEffectName, 0, 0, nameBuf, 0.0f);
    result.name = nameBuf[0] ? nameBuf : pluginPath.stem().string();
    logStage("Effect name: \"" + result.name + "\"");

    // Vendor
    char vendorBuf[kVstMaxVendorStrLen + 1] = {};
    effect->dispatcher(effect, effGetVendorString, 0, 0, vendorBuf, 0.0f);
    result.vendor = vendorBuf[0] ? vendorBuf : "Unknown VST2 Vendor";
    logStage("Vendor: \"" + result.vendor + "\"");

    // Product string -> description
    char productBuf[kVstMaxProductStrLen + 1] = {};
    effect->dispatcher(effect, effGetProductString, 0, 0, productBuf, 0.0f);
    result.description = productBuf[0] ? productBuf : "";
    logStage("Product: \"" + result.description + "\"");

    // Version
    VstInt32 vendorVersion = static_cast<VstInt32>(
        effect->dispatcher(effect, effGetVendorVersion, 0, 0, nullptr, 0.0f));
    if (vendorVersion > 0) {
        int major = vendorVersion / 1000;
        int minor = (vendorVersion % 1000) / 100;
        int patch = (vendorVersion % 100) / 10;
        int build = vendorVersion % 10;
        result.version = std::to_string(major) + "." + std::to_string(minor)
                       + "." + std::to_string(patch) + "." + std::to_string(build);
    } else {
        result.version = std::to_string(effect->version);
    }
    logStage("Version: " + result.version + " (raw: " + std::to_string(vendorVersion) + ")");

    // Unique ID (4-byte integer → hex)
    char uidBuf[16];
    snprintf(uidBuf, sizeof(uidBuf), "%08X", static_cast<unsigned int>(effect->uniqueID));
    result.uid = uidBuf;
    logStage("UID: " + result.uid);

    // I/O
    result.numInputs = effect->numInputs;
    result.numOutputs = effect->numOutputs;
    logStage("I/O: " + std::to_string(result.numInputs) + " in, " + std::to_string(result.numOutputs) + " out");

    // Category
    VstInt32 plugCategory = static_cast<VstInt32>(
        effect->dispatcher(effect, effGetPlugCategory, 0, 0, nullptr, 0.0f));
    result.category = categoryToString(plugCategory);
    if (effect->flags & effFlagsIsSynth) {
        if (result.category != "Instrument") {
            result.category += "|Instrument";
        }
    }
    logStage("Category: " + result.category);

    result.format = "vst2";
    result.scanMethod = "vst2";

    // --- Shell plugin detection ---
    if (plugCategory == kPlugCategShell) {
        logStage("Shell plugin detected — enumerating sub-plugins...");
        int shellCount = 0;
        char shellName[kVstMaxProductStrLen + 1] = {};
        VstInt32 shellId;
        while ((shellId = static_cast<VstInt32>(
                    effect->dispatcher(effect, effShellGetNextPlugin, 0, 0, shellName, 0.0f))) != 0) {
            shellCount++;
            if (g_verbose) {
                std::cerr << "[vst2] " << pluginName << ":   shell[" << shellCount
                          << "] id=0x" << std::hex << shellId << std::dec
                          << " name=\"" << shellName << "\"" << std::endl;
            }
            std::memset(shellName, 0, sizeof(shellName));
        }
        logStage("Shell contains " + std::to_string(shellCount) + " sub-plugin(s).");
    }

    // --- Parameters ---
    progressCb(70, "Extracting parameters...");
    VstInt32 numParams = effect->numParams;
    logStage("Plugin reports " + std::to_string(numParams) + " parameter(s).");

    {
        auto paramStart = std::chrono::steady_clock::now();
        for (VstInt32 i = 0; i < numParams; ++i) {
            if (g_verbose && i == 0) {
                logStage("Extracting param 0/" + std::to_string(numParams) + "...");
            }

            char paramName[kVstMaxParamStrLen + 1] = {};
            effect->dispatcher(effect, effGetParamName, i, 0, paramName, 0.0f);

            float defVal = effect->getParameter(effect, i);
            // Sanitize non-finite values
            if (!std::isfinite(static_cast<double>(defVal))) defVal = 0.0f;

            result.parameters.push_back({
                static_cast<uint32_t>(i),
                paramName[0] ? paramName : ("Param " + std::to_string(i)),
                static_cast<double>(defVal)
            });

            // Log slow parameter extraction (every 10 params or if a single param takes >500ms)
            if (g_verbose) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - paramStart).count();
                if (elapsed > 2000 && i > 0) {
                    logStage("WARNING: param extraction slow — " + std::to_string(i + 1) + "/"
                             + std::to_string(numParams) + " done in " + std::to_string(elapsed) + "ms");
                    // Abort parameter extraction if it's taking too long (>5s)
                    if (elapsed > 5000) {
                        logStage("Aborting parameter extraction after " + std::to_string(i + 1)
                                 + " params (" + std::to_string(elapsed) + "ms). Plugin is too slow.");
                        break;
                    }
                }
            }
        }
        auto paramEnd = std::chrono::steady_clock::now();
        auto paramMs = std::chrono::duration_cast<std::chrono::milliseconds>(paramEnd - paramStart).count();
        logStage("Extracted " + std::to_string(result.parameters.size()) + " parameter(s) in "
                 + std::to_string(paramMs) + "ms.");
    }

    progressCb(90, "Metadata extraction complete.");
    logStage("Metadata extraction complete — returning result before cleanup.");

    // Skip effClose / FreeLibrary — same pattern as VST3 scanner.
    // Some plugins crash during cleanup. The scanner process exits immediately
    // after returning, so the OS reclaims all resources.

    progressCb(100, "Done.");
    return result;
}

} // namespace rps::scanner
