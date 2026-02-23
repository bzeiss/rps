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
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <functional>
#include <vector>
#include <boost/filesystem.hpp>
#include <boost/json.hpp>

// Suppress warnings from third-party Steinberg SDK headers
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wpragma-pack"
#endif
#endif

// Include minimum VST3 COM interfaces needed to parse the factory
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wpragma-pack"
#endif
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

extern bool g_verbose;

namespace rps::scanner {

namespace {

// Resolves a .vst3 path to the actual loadable binary.
// If given a bundle directory (.vst3/), navigates into Contents/{arch}/ to find the binary.
// If given a plain file, returns it unchanged.
boost::filesystem::path resolveBinaryPath(const boost::filesystem::path& vst3Path) {
    namespace fs = boost::filesystem;

    if (fs::is_regular_file(vst3Path)) {
        return vst3Path; // Already a loadable file
    }

    if (!fs::is_directory(vst3Path)) {
        throw std::runtime_error("VST3 path is neither a file nor a directory: " + vst3Path.string());
    }

    fs::path contentsPath = vst3Path / "Contents";
    if (!fs::exists(contentsPath) || !fs::is_directory(contentsPath)) {
        throw std::runtime_error("VST3 bundle missing 'Contents' directory: " + vst3Path.string());
    }

    // Architecture subdirectory candidates in priority order
#if defined(_WIN32)
    const std::vector<std::string> archDirs = {"x86_64-win", "x86-win"};
#elif defined(__APPLE__)
    const std::vector<std::string> archDirs = {"MacOS"};
#else
    const std::vector<std::string> archDirs = {"x86_64-linux", "aarch64-linux", "i686-linux"};
#endif

    for (const auto& arch : archDirs) {
        fs::path archPath = contentsPath / arch;
        if (!fs::exists(archPath) || !fs::is_directory(archPath)) continue;

        // Look for the actual VST3 binary — must have a loadable extension
        // Per VST3 spec, the binary inside the bundle has the same stem as the bundle
        // but platform-specific extension (.vst3 on Windows, .so on Linux, no ext on macOS)
#if defined(_WIN32)
        const std::vector<std::string> validExts = {".vst3", ".dll"};
#elif defined(__APPLE__)
        const std::vector<std::string> validExts = {""};  // macOS Mach-O has no extension
#else
        const std::vector<std::string> validExts = {".so"};
#endif
        for (const auto& entry : fs::directory_iterator(archPath)) {
            if (!fs::is_regular_file(entry.path())) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            for (const auto& valid : validExts) {
                if (ext == valid) return entry.path();
            }
        }
    }

    throw std::runtime_error("SKIP: VST3 bundle contains no loadable binary for this platform: " + vst3Path.string());
}

// ---------------------------------------------------------------------------
// PE / ELF architecture check — detect arch mismatch before LoadLibrary/dlopen
// ---------------------------------------------------------------------------
#ifdef _WIN32
std::string getMachineTypeName(uint16_t machine) {
    switch (machine) {
        case 0x014C: return "x86";
        case 0x8664: return "x86_64";
        case 0xAA64: return "ARM64";
        default: {
            char buf[32];
            snprintf(buf, sizeof(buf), "unknown (0x%04X)", machine);
            return buf;
        }
    }
}

void checkBinaryArchitecture(const boost::filesystem::path& binaryPath) {
    std::ifstream f(binaryPath.string(), std::ios::binary);
    if (!f) return; // If we can't open, let LoadLibrary produce the real error

    // Read DOS header — first 2 bytes should be "MZ"
    char dosHeader[64];
    f.read(dosHeader, sizeof(dosHeader));
    if (!f || dosHeader[0] != 'M' || dosHeader[1] != 'Z') return;

    // e_lfanew is at offset 0x3C (4 bytes, little-endian)
    uint32_t peOffset = *reinterpret_cast<uint32_t*>(&dosHeader[0x3C]);
    f.seekg(peOffset);

    // PE signature: "PE\0\0"
    char peSig[4];
    f.read(peSig, 4);
    if (!f || peSig[0] != 'P' || peSig[1] != 'E' || peSig[2] != 0 || peSig[3] != 0) return;

    // IMAGE_FILE_HEADER.Machine (2 bytes)
    uint16_t machine = 0;
    f.read(reinterpret_cast<char*>(&machine), 2);
    if (!f) return;

    // Determine what this process is
#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr uint16_t expectedMachine = 0xAA64; // ARM64
    constexpr const char* expectedArch = "ARM64";
#elif defined(_M_X64) || defined(__x86_64__)
    constexpr uint16_t expectedMachine = 0x8664; // x86_64
    constexpr const char* expectedArch = "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    constexpr uint16_t expectedMachine = 0x014C; // x86
    constexpr const char* expectedArch = "x86";
#else
    return; // Unknown host arch, skip check
#endif

    if (machine != expectedMachine) {
        throw std::runtime_error(
            "Architecture mismatch: binary is " + getMachineTypeName(machine)
            + " but scanner is " + expectedArch
            + ": " + binaryPath.string());
    }
}
#else
void checkBinaryArchitecture(const boost::filesystem::path& binaryPath) {
    std::ifstream f(binaryPath.string(), std::ios::binary);
    if (!f) return;

    // ELF magic: 0x7F 'E' 'L' 'F'
    char elfHeader[20];
    f.read(elfHeader, sizeof(elfHeader));
    if (!f || elfHeader[0] != 0x7F || elfHeader[1] != 'E' || elfHeader[2] != 'L' || elfHeader[3] != 'F')
        return;

    // e_machine at offset 18 (2 bytes, little-endian for x86/ARM)
    uint16_t eMachine = *reinterpret_cast<uint16_t*>(&elfHeader[18]);

    std::string binaryArch;
    if (eMachine == 0x3E)       binaryArch = "x86_64";
    else if (eMachine == 0xB7)  binaryArch = "aarch64";
    else if (eMachine == 0x03)  binaryArch = "x86";
    else return; // Unknown, let dlopen handle it

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
// Loader-stub detection — some VST3 plugins (e.g. NotePerformer) ship a tiny
// stub DLL whose GetPluginFactory() returns null. The actual plugin DLL lives
// in Contents/Resources/ and is referenced by a .txt config file.
// Format of the config (e.g. "Loader 64.txt"):
//   Line 1: version number (e.g. "1")
//   Line 2: relative path to DLL (e.g. "/Loader 64.dat")
//   Lines 3+: metadata (URL, email, name, vendor)
// ---------------------------------------------------------------------------
#ifdef _WIN32
boost::filesystem::path detectLoaderStubDll(const boost::filesystem::path& pluginPath, bool verbose) {
    namespace fs = boost::filesystem;

    fs::path resourcesDir = pluginPath / "Contents" / "Resources";
    if (!fs::is_directory(resourcesDir)) return {};

    // Look for .txt files in Resources that reference a .dat DLL
    for (const auto& entry : fs::directory_iterator(resourcesDir)) {
        if (!fs::is_regular_file(entry.path())) continue;
        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".txt") continue;

        std::ifstream txt(entry.path().string());
        if (!txt) continue;

        std::string line1, line2;
        std::getline(txt, line1); // version
        std::getline(txt, line2); // relative path (e.g. "/Loader 64.dat")

        if (line2.empty()) continue;

        // Strip leading '/' if present
        if (line2[0] == '/') line2 = line2.substr(1);

        // Check if the referenced file exists and is a PE
        fs::path datPath = resourcesDir / line2;
        if (!fs::is_regular_file(datPath)) continue;

        // Verify it's a PE file and check architecture matches this process
        std::ifstream f(datPath.string(), std::ios::binary);
        char dosHeader[64] = {};
        f.read(dosHeader, sizeof(dosHeader));
        if (!f || dosHeader[0] != 'M' || dosHeader[1] != 'Z') continue;

        uint32_t peOffset = *reinterpret_cast<uint32_t*>(&dosHeader[0x3C]);
        f.seekg(peOffset);
        char peSig[4] = {};
        f.read(peSig, 4);
        if (!f || peSig[0] != 'P' || peSig[1] != 'E' || peSig[2] != 0 || peSig[3] != 0) continue;

        uint16_t machine = 0;
        f.read(reinterpret_cast<char*>(&machine), 2);
        if (!f) continue;

#if defined(_M_X64) || defined(__x86_64__)
        constexpr uint16_t expectedMachine = 0x8664;
#elif defined(_M_IX86) || defined(__i386__)
        constexpr uint16_t expectedMachine = 0x014C;
#elif defined(_M_ARM64) || defined(__aarch64__)
        constexpr uint16_t expectedMachine = 0xAA64;
#else
        constexpr uint16_t expectedMachine = 0;
#endif
        if (machine != expectedMachine) {
            if (verbose) {
                std::cerr << "[vst3] " << pluginPath.filename().string()
                          << ": Skipping " << datPath.filename().string()
                          << " (arch mismatch: 0x" << std::hex << machine << std::dec << ")" << std::endl;
            }
            continue;
        }

        if (verbose) {
            std::cerr << "[vst3] " << pluginPath.filename().string()
                      << ": Detected loader-stub pattern: " << entry.path().filename().string()
                      << " -> " << datPath.string() << std::endl;
        }
        return datPath;
    }
    return {};
}
#endif

// ---------------------------------------------------------------------------
// moduleinfo.json fast path — extract metadata without loading the DLL
// ---------------------------------------------------------------------------
// Returns true if moduleinfo.json was found and parsed successfully.
bool tryLoadModuleInfo(
    const boost::filesystem::path& pluginPath,
    rps::ipc::ScanResult& result,
    bool verbose,
    std::function<void(int, const std::string&)> progressCb)
{
    namespace fs = boost::filesystem;

    if (!fs::is_directory(pluginPath)) return false;

    // SDK 3.7.8+: Contents/Resources/moduleinfo.json
    // SDK 3.7.5-3.7.7: Contents/moduleinfo.json
    fs::path jsonPath = pluginPath / "Contents" / "Resources" / "moduleinfo.json";
    if (!fs::exists(jsonPath)) {
        jsonPath = pluginPath / "Contents" / "moduleinfo.json";
        if (!fs::exists(jsonPath)) return false;
    }

    if (verbose) {
        std::cerr << "[vst3] " << pluginPath.filename().string()
                  << ": Found moduleinfo.json: " << jsonPath.string() << std::endl;
    }

    // Read the file
    std::ifstream ifs(jsonPath.string());
    if (!ifs) return false;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    // moduleinfo.json uses JSON5 (allows trailing commas, comments).
    // Boost.JSON is strict, so strip trailing commas before '}' and ']'.
    {
        std::string cleaned;
        cleaned.reserve(content.size());
        bool inString = false;
        for (size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (c == '"' && (i == 0 || content[i - 1] != '\\')) inString = !inString;
            if (!inString && c == ',') {
                // Look ahead past whitespace for '}' or ']'
                size_t j = i + 1;
                while (j < content.size() && (content[j] == ' ' || content[j] == '\t' ||
                       content[j] == '\n' || content[j] == '\r')) ++j;
                if (j < content.size() && (content[j] == '}' || content[j] == ']'))
                    continue; // Skip this trailing comma
            }
            cleaned += c;
        }
        content = std::move(cleaned);
    }

    // Parse JSON
    boost::system::error_code ec;
    auto jv = boost::json::parse(content, ec);
    if (ec) {
        if (verbose) {
            std::cerr << "[vst3] " << pluginPath.filename().string()
                      << ": moduleinfo.json parse error: " << ec.message() << std::endl;
        }
        return false;
    }

    auto& root = jv.as_object();

    // Extract Factory Info (vendor, URL)
    std::string factoryVendor, factoryUrl;
    if (root.contains("Factory Info")) {
        auto& fi = root["Factory Info"].as_object();
        if (fi.contains("Vendor")) factoryVendor = fi["Vendor"].as_string().c_str();
        if (fi.contains("URL")) factoryUrl = fi["URL"].as_string().c_str();
    }

    // Find the first scannable processor class
    if (!root.contains("Classes") || !root["Classes"].is_array()) return false;

    auto& classes = root["Classes"].as_array();

    if (verbose) {
        for (size_t i = 0; i < classes.size(); ++i) {
            auto& cls = classes[i].as_object();
            std::string name = cls.contains("Name") ? cls["Name"].as_string().c_str() : "?";
            std::string cat = cls.contains("Category") ? cls["Category"].as_string().c_str() : "?";
            std::cerr << "[vst3] " << pluginPath.filename().string()
                      << ":   class[" << i << "] name=\"" << name
                      << "\" category=\"" << cat << "\" (from moduleinfo.json)" << std::endl;
        }
    }

    const boost::json::object* foundClass = nullptr;
    for (auto& cls : classes) {
        auto& obj = cls.as_object();
        if (!obj.contains("Category")) continue;
        std::string cat = obj["Category"].as_string().c_str();
        if (cat == kVstAudioEffectClass || cat == "Audio Mix Processor") {
            foundClass = &obj;
            break;
        }
    }

    if (!foundClass) return false;

    progressCb(50, "Extracting metadata from moduleinfo.json...");

    result.name = foundClass->contains("Name") ? (*foundClass).at("Name").as_string().c_str() : "";
    result.uid = foundClass->contains("CID") ? (*foundClass).at("CID").as_string().c_str() : "";

    // Version: per-class first, fall back to module-level
    if (foundClass->contains("Version"))
        result.version = (*foundClass).at("Version").as_string().c_str();
    else if (root.contains("Version"))
        result.version = root["Version"].as_string().c_str();
    else
        result.version = "1.0.0";

    // Vendor: per-class first, fall back to factory
    if (foundClass->contains("Vendor") && !(*foundClass).at("Vendor").as_string().empty())
        result.vendor = (*foundClass).at("Vendor").as_string().c_str();
    else
        result.vendor = factoryVendor;

    result.url = factoryUrl;

    // Sub Categories → category string (join with "|")
    if (foundClass->contains("Sub Categories") && (*foundClass).at("Sub Categories").is_array()) {
        auto& subs = (*foundClass).at("Sub Categories").as_array();
        std::string joined;
        for (size_t i = 0; i < subs.size(); ++i) {
            if (i > 0) joined += "|";
            joined += subs[i].as_string().c_str();
        }
        result.category = joined;
    }

    result.scanMethod = "moduleinfo.json";

    // I/O counts not available from moduleinfo.json
    result.numInputs = 0;
    result.numOutputs = 0;

    progressCb(100, "Done (from moduleinfo.json).");
    return true;
}

} // anonymous namespace

bool Vst3Scanner::canHandle(const boost::filesystem::path& pluginPath) const {
    auto ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".vst3";
}

rps::ipc::ScanResult Vst3Scanner::scan(const boost::filesystem::path& pluginPath, ProgressCallback progressCb) {
    auto logStage = [&](const std::string& stage) {
        if (g_verbose) {
            std::cerr << "[vst3] " << pluginPath.filename().string() << ": " << stage << std::endl;
        }
    };

    // Fast path: try moduleinfo.json first (no DLL loading needed)
    {
        rps::ipc::ScanResult jsonResult;
        if (tryLoadModuleInfo(pluginPath, jsonResult, g_verbose, progressCb)) {
            logStage("Metadata extracted from moduleinfo.json — skipping DLL load.");
            return jsonResult;
        }
        logStage("No moduleinfo.json — falling back to DLL load.");
    }

    progressCb(10, "Resolving VST3 binary...");
    logStage("Resolving binary path...");

    // Resolve bundle directory to actual binary (handles both bundles and single-file .vst3)
    boost::filesystem::path binaryPath = resolveBinaryPath(pluginPath);
    logStage("Resolved to: " + binaryPath.string());

    // Architecture check — detect x86/x64/ARM64 mismatch before loading
    logStage("Checking binary architecture...");
    checkBinaryArchitecture(binaryPath);
    logStage("Architecture OK.");

    progressCb(20, "Loading VST3 binary...");
    logStage("Calling LoadLibrary...");

#ifdef _WIN32
    // Set DLL search directory to the bundle root so stub/loader plugins
    // (e.g. NotePerformer) can find their dependent files (Loader 64.dat in
    // Contents/Resources/) via internal LoadLibrary calls during GetPluginFactory().
    // We keep it set until after GetPluginFactory() returns.
    auto bundleRoot = pluginPath.wstring();
    wchar_t oldDllDir[MAX_PATH] = {};
    GetDllDirectoryW(MAX_PATH, oldDllDir);
    SetDllDirectoryW(bundleRoot.c_str());
    logStage("SetDllDirectoryW -> " + pluginPath.string());

    HMODULE handle = LoadLibraryW(binaryPath.c_str());

    if (!handle) {
        SetDllDirectoryW(oldDllDir[0] ? oldDllDir : nullptr);
        DWORD err = GetLastError();
        throw std::runtime_error("Failed to load VST3 DLL: " + binaryPath.string() + " (Win32 error: " + std::to_string(err) + ")");
    }
    logStage("LoadLibrary succeeded.");

    // VST3 Module Architecture: InitDll must be called after LoadLibrary, before GetPluginFactory
    auto initDll = reinterpret_cast<bool(*)()>(
        reinterpret_cast<void*>(GetProcAddress(handle, "InitDll"))
    );
    if (initDll) {
        logStage("Calling InitDll()...");
        if (!initDll()) {
            SetDllDirectoryW(oldDllDir[0] ? oldDllDir : nullptr);
            FreeLibrary(handle);
            throw std::runtime_error("InitDll() returned false for: " + binaryPath.string());
        }
        logStage("InitDll() succeeded.");
    } else {
        logStage("No InitDll export (optional).");
    }

    auto getFactory = reinterpret_cast<Steinberg::IPluginFactory* (*)()>(
        reinterpret_cast<void*>(GetProcAddress(handle, "GetPluginFactory"))
    );
#else
    void* handle = dlopen(binaryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error(std::string("Failed to load VST3 library: ") + dlerror());
    }
    logStage("dlopen succeeded.");

    // VST3 Module Architecture: ModuleEntry must be called after dlopen, before GetPluginFactory
    auto moduleEntry = reinterpret_cast<bool(*)(void*)>(dlsym(handle, "ModuleEntry"));
    if (moduleEntry) {
        logStage("Calling ModuleEntry()...");
        if (!moduleEntry(handle)) {
            dlclose(handle);
            throw std::runtime_error("ModuleEntry() returned false for: " + binaryPath.string());
        }
        logStage("ModuleEntry() succeeded.");
    } else {
        logStage("No ModuleEntry export (optional).");
    }

    auto getFactory = reinterpret_cast<Steinberg::IPluginFactory* (*)()>(dlsym(handle, "GetPluginFactory"));
#endif

    if (!getFactory) {
#ifdef _WIN32
        SetDllDirectoryW(oldDllDir[0] ? oldDllDir : nullptr);
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("Library does not export 'GetPluginFactory'. Not a valid VST3 plugin.");
    }
    logStage("GetPluginFactory export found.");

    progressCb(30, "Getting VST3 Plugin Factory...");
    logStage("Calling GetPluginFactory()...");
    Steinberg::IPluginFactory* factory = getFactory();
    logStage("GetPluginFactory() returned.");

#ifdef _WIN32
    // Restore DLL directory now that factory call is complete
    SetDllDirectoryW(oldDllDir[0] ? oldDllDir : nullptr);
    logStage("SetDllDirectoryW restored.");
#endif

    if (!factory) {
#ifdef _WIN32
        // Fallback: detect loader-stub pattern (e.g. NotePerformer) and try
        // loading the actual DLL directly from Contents/Resources/
        logStage("GetPluginFactory returned null — checking for loader-stub pattern...");
        auto loaderDll = detectLoaderStubDll(pluginPath, g_verbose);
        if (!loaderDll.empty()) {
            logStage("Loader-stub fallback: unloading stub, loading " + loaderDll.string());
            FreeLibrary(handle);
            handle = nullptr;

            // Set DLL directory to Resources dir for the loader DLL's own dependencies
            auto resourcesDir = loaderDll.parent_path().wstring();
            SetDllDirectoryW(resourcesDir.c_str());
            logStage("SetDllDirectoryW -> " + loaderDll.parent_path().string());

            handle = LoadLibraryW(loaderDll.c_str());
            SetDllDirectoryW(oldDllDir[0] ? oldDllDir : nullptr);

            if (!handle) {
                DWORD err = GetLastError();
                throw std::runtime_error("Loader-stub fallback: failed to load " + loaderDll.string()
                                         + " (Win32 error: " + std::to_string(err) + ")");
            }
            logStage("Loader-stub fallback: LoadLibrary succeeded.");

            auto initDll2 = reinterpret_cast<bool(*)()>(
                reinterpret_cast<void*>(GetProcAddress(handle, "InitDll")));
            if (initDll2) {
                logStage("Loader-stub fallback: calling InitDll()...");
                if (!initDll2()) {
                    FreeLibrary(handle);
                    throw std::runtime_error("Loader-stub fallback: InitDll() returned false for: " + loaderDll.string());
                }
                logStage("Loader-stub fallback: InitDll() succeeded.");
            }

            auto getFactory2 = reinterpret_cast<Steinberg::IPluginFactory* (*)()>(
                reinterpret_cast<void*>(GetProcAddress(handle, "GetPluginFactory")));
            if (getFactory2) {
                logStage("Loader-stub fallback: calling GetPluginFactory()...");
                factory = getFactory2();
                logStage("Loader-stub fallback: GetPluginFactory() " +
                         std::string(factory ? "succeeded!" : "returned null again."));
            }

            if (!factory) {
                FreeLibrary(handle);
                throw std::runtime_error("Loader-stub fallback: GetPluginFactory still returned null from " + loaderDll.string());
            }
        } else {
            FreeLibrary(handle);
            throw std::runtime_error("GetPluginFactory returned null.");
        }
#else
        dlclose(handle);
        throw std::runtime_error("GetPluginFactory returned null.");
#endif
    }

    progressCb(50, "Extracting plugin metadata...");
    
    int32_t numClasses = factory->countClasses();
    logStage("Factory reports " + std::to_string(numClasses) + " class(es).");

    // Log all classes in verbose mode for diagnostics
    if (g_verbose) {
        Steinberg::PClassInfo ci;
        for (int32_t i = 0; i < numClasses; ++i) {
            if (factory->getClassInfo(i, &ci) == Steinberg::kResultOk) {
                std::cerr << "[vst3] " << pluginPath.filename().string()
                          << ":   class[" << i << "] name=\"" << ci.name
                          << "\" category=\"" << ci.category << "\"" << std::endl;
            }
        }
    }

    if (numClasses == 0) {
        factory->release();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("SKIP: VST3 factory contains no classes.");
    }

    // Find the first scannable processor class.
    // Accept standard audio effects ("Audio Module Class") and mix processors ("Audio Mix Processor").
    // Skip controller-only classes ("Component Controller Class", "Plugin Compatibility Class", "Private").
    Steinberg::PClassInfo classInfo;
    int32_t foundClassIndex = -1;
    for (int32_t i = 0; i < numClasses; ++i) {
        if (factory->getClassInfo(i, &classInfo) == Steinberg::kResultOk) {
            if (std::strcmp(classInfo.category, kVstAudioEffectClass) == 0 ||
                std::strcmp(classInfo.category, "Audio Mix Processor") == 0) {
                foundClassIndex = i;
                break;
            }
        }
    }

    if (foundClassIndex < 0) {
        factory->release();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("SKIP: VST3 factory contains no scannable processor classes.");
    }

    logStage("Found scannable class at index " + std::to_string(foundClassIndex) + ": \"" + classInfo.name + "\"");

    rps::ipc::ScanResult result;
    result.name = classInfo.name;
    result.version = "1.0.0";
    
    // Convert 16-byte FUID to hex string for UID
    char uidBuf[33];
    snprintf(uidBuf, sizeof(uidBuf), "%08X%08X%08X%08X", 
        classInfo.cid[0], classInfo.cid[1], classInfo.cid[2], classInfo.cid[3]);
    result.uid = uidBuf;
    logStage("UID: " + result.uid);

    // Extract factory-level info (vendor, URL, email) as baseline
    logStage("Calling getFactoryInfo()...");
    Steinberg::PFactoryInfo factoryInfo;
    if (factory->getFactoryInfo(&factoryInfo) == Steinberg::kResultOk) {
        result.vendor = factoryInfo.vendor;
        result.url = factoryInfo.url;
        logStage("Factory info: vendor=\"" + result.vendor + "\" url=\"" + result.url + "\"");
    } else {
        result.vendor = "Unknown VST3 Vendor";
        logStage("getFactoryInfo() failed.");
    }

    // Attempt to query IPluginFactory2 for per-class details (may override vendor/version)
    logStage("Querying IPluginFactory2...");
    Steinberg::IPluginFactory2* factory2 = nullptr;
    if (factory->queryInterface(Steinberg::IPluginFactory2::iid, reinterpret_cast<void**>(&factory2)) == Steinberg::kResultOk) {
        logStage("IPluginFactory2 supported. Calling getClassInfo2()...");
        Steinberg::PClassInfo2 classInfo2;
        if (factory2->getClassInfo2(foundClassIndex, &classInfo2) == Steinberg::kResultOk) {
            if (classInfo2.vendor[0] != '\0') result.vendor = classInfo2.vendor;
            if (classInfo2.version[0] != '\0') result.version = classInfo2.version;
            result.category = classInfo2.subCategories;
            logStage("ClassInfo2: vendor=\"" + result.vendor + "\" version=\"" + result.version + "\" subCategories=\"" + result.category + "\"");
        } else {
            logStage("getClassInfo2() failed.");
        }
        logStage("Releasing IPluginFactory2...");
        factory2->release();
        logStage("IPluginFactory2 released.");
    } else {
        logStage("IPluginFactory2 not supported (older plugin).");
    }

    result.scanMethod = "factory";

    // I/O counts and parameters require full host instantiation (IComponent::initialize,
    // getBusInfo, IEditController). Not available from factory metadata alone.
    result.numInputs = 0;
    result.numOutputs = 0;

    progressCb(90, "Metadata extraction complete.");
    logStage("Metadata extraction complete — returning result before cleanup.");

    // IMPORTANT: Return the result BEFORE cleanup. Some plugins (e.g. LX480 v4)
    // crash with ACCESS_VIOLATION during factory->release(), ExitDll(), or
    // FreeLibrary(). Since the scanner process exits immediately after returning,
    // the OS reclaims all resources. Skipping explicit cleanup is safe and
    // prevents losing a perfectly good scan result to a buggy destructor.
    //
    // The caller (main.cpp) sends the result via IPC, then the process exits.
    // On exit, Windows automatically: unloads all DLLs, frees all memory,
    // closes all handles.

    progressCb(100, "Done.");
    return result;
}

} // namespace rps::scanner
