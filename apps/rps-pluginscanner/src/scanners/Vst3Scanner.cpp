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
#include <map>
#include <cstdint>
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

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996) // Unsafe C functions
#endif

// Include minimum VST3 COM interfaces needed to parse the factory
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/base/iplugincompatibility.h>
#include <cmath>

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

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Steinberg {
    const FUID IPluginFactory::iid(0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
    const FUID IPluginFactory2::iid(0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
    const FUID IPluginFactory3::iid(0x4555A2AB, 0xC1234E57, 0x9B122910, 0x36878931);
    const FUID FUnknown::iid(0x00000000, 0x00000000, 0xC0000000, 0x00000046);
    const FUID IBStream::iid(0xC3BF6EA2, 0x30994752, 0x9B6BF990, 0x1EE33E9B);
    const FUID IPluginCompatibility::iid(0x4AFD4B6A, 0x35D7C240, 0xA5C31414, 0xFB7D15E6);
    namespace Vst {
        const FUID IComponent::iid(0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
        const FUID IEditController::iid(0xDCD7BBE3, 0x7742448D, 0xA874AACC, 0x979C759E);
        const FUID IComponentHandler::iid(0x93A0BEA3, 0x0BD045DB, 0x8E890B0C, 0xC1E46AC6);
        const FUID IHostApplication::iid(0x58E595CC, 0xDB2D4969, 0x8B6AAF8C, 0x36A664E5);
    }
}

extern bool g_verbose;

namespace rps::scanner {

namespace {

// ---------------------------------------------------------------------------
// UTF-16 (String128) to UTF-8 converter — needed for ParameterInfo.title, BusInfo.name
// ---------------------------------------------------------------------------
static std::string utf16ToUtf8(const Steinberg::Vst::TChar* src) {
    if (!src || src[0] == 0) return {};
    std::string out;
    for (size_t i = 0; src[i] != 0; ++i) {
        char16_t c = static_cast<char16_t>(src[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Minimal host context for IComponent::initialize / IEditController::initialize
// ---------------------------------------------------------------------------
class ScannerHostContext : public Steinberg::Vst::IHostApplication {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid.toTUID()) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::Vst::IHostApplication::iid.toTUID())) {
            addRef();
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        const char16_t src[] = u"rps-pluginscanner";
        std::memcpy(name, src, sizeof(src));
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID /*cid*/, Steinberg::TUID /*_iid*/,
                                                  void** obj) override {
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
};

static ScannerHostContext g_hostContext;

// ---------------------------------------------------------------------------
// Minimal IBStream implementation for IPluginCompatibility::getCompatibilityJSON
// ---------------------------------------------------------------------------
class MemoryStream : public Steinberg::IBStream {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid.toTUID()) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::IBStream::iid.toTUID())) {
            addRef();
            *obj = static_cast<Steinberg::IBStream*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        if (!buffer) return Steinberg::kInvalidArgument;
        Steinberg::int32 avail = static_cast<Steinberg::int32>(m_data.size()) - static_cast<Steinberg::int32>(m_pos);
        if (avail <= 0) { if (numBytesRead) *numBytesRead = 0; return Steinberg::kResultOk; }
        Steinberg::int32 toRead = (numBytes < avail) ? numBytes : avail;
        std::memcpy(buffer, m_data.data() + m_pos, toRead);
        m_pos += toRead;
        if (numBytesRead) *numBytesRead = toRead;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
        if (!buffer || numBytes <= 0) { if (numBytesWritten) *numBytesWritten = 0; return Steinberg::kResultOk; }
        auto* src = static_cast<const char*>(buffer);
        if (m_pos == m_data.size()) {
            m_data.insert(m_data.end(), src, src + numBytes);
        } else {
            size_t end = m_pos + numBytes;
            if (end > m_data.size()) m_data.resize(end);
            std::memcpy(m_data.data() + m_pos, src, numBytes);
        }
        m_pos += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override {
        Steinberg::int64 newPos = 0;
        if (mode == kIBSeekSet) newPos = pos;
        else if (mode == kIBSeekCur) newPos = static_cast<Steinberg::int64>(m_pos) + pos;
        else if (mode == kIBSeekEnd) newPos = static_cast<Steinberg::int64>(m_data.size()) + pos;
        if (newPos < 0) newPos = 0;
        m_pos = static_cast<size_t>(newPos);
        if (result) *result = static_cast<Steinberg::int64>(m_pos);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (pos) *pos = static_cast<Steinberg::int64>(m_pos);
        return Steinberg::kResultOk;
    }

    std::string str() const { return std::string(m_data.begin(), m_data.end()); }
private:
    std::vector<char> m_data;
    size_t m_pos = 0;
};

// ---------------------------------------------------------------------------
// Helper: Convert TUID (16 bytes) to 32-char uppercase hex string
// On Windows (COM_COMPATIBLE=1), the first 8 bytes are stored in GUID layout:
//   Data1 (4 bytes LE uint32) + Data2 (2 bytes LE uint16) + Data3 (2 bytes LE uint16)
// followed by 8 raw bytes. The canonical hex string re-interprets them accordingly.
// ---------------------------------------------------------------------------
static std::string tuidToHex(const Steinberg::TUID cid) {
    char buf[33];
#if COM_COMPATIBLE
    auto* b = reinterpret_cast<const unsigned char*>(cid);
    uint32_t d1 = b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    uint16_t d2 = b[4] | (uint16_t(b[5]) << 8);
    uint16_t d3 = b[6] | (uint16_t(b[7]) << 8);
    snprintf(buf, sizeof(buf), "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
             d1, d2, d3, b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
#else
    for (int i = 0; i < 16; ++i) {
        snprintf(buf + i * 2, 3, "%02X", static_cast<unsigned char>(cid[i]));
    }
    buf[32] = '\0';
#endif
    return buf;
}

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
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    for (const auto& valid : validExts) {                if (ext == valid) return entry.path();
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
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

    // Extract Factory Info (vendor, URL, email, flags)
    std::string factoryVendor, factoryUrl, factoryEmail;
    int32_t factoryFlags = 0;
    if (root.contains("Factory Info")) {
        auto& fi = root["Factory Info"].as_object();
        if (fi.contains("Vendor")) factoryVendor = fi["Vendor"].as_string().c_str();
        if (fi.contains("URL")) factoryUrl = fi["URL"].as_string().c_str();
        if (fi.contains("E-Mail")) factoryEmail = fi["E-Mail"].as_string().c_str();
        if (fi.contains("Flags") && fi["Flags"].is_object()) {
            auto& flags = fi["Flags"].as_object();
            if (flags.contains("Unicode") && flags["Unicode"].is_bool() && flags["Unicode"].as_bool())
                factoryFlags |= 1; // kUnicode
            if (flags.contains("Classes Discardable") && flags["Classes Discardable"].is_bool() && flags["Classes Discardable"].as_bool())
                factoryFlags |= 2; // kClassesDiscardable
            if (flags.contains("Component Non Discardable") && flags["Component Non Discardable"].is_bool() && flags["Component Non Discardable"].as_bool())
                factoryFlags |= 8; // kComponentNonDiscardable
        }
    }

    // Enumerate all classes from moduleinfo.json
    if (!root.contains("Classes") || !root["Classes"].is_array()) return false;

    auto& classes = root["Classes"].as_array();

    std::string moduleVersion;
    if (root.contains("Version"))
        moduleVersion = root["Version"].as_string().c_str();

    // Iterate ALL processor classes, pack into extraData, first becomes main result
    const boost::json::object* foundClass = nullptr;
    int processorCount = 0;

    for (size_t i = 0; i < classes.size(); ++i) {
        auto& obj = classes[i].as_object();
        std::string name = obj.contains("Name") ? obj["Name"].as_string().c_str() : "";
        std::string cat = obj.contains("Category") ? obj["Category"].as_string().c_str() : "";

        if (verbose) {
            std::cerr << "[vst3] " << pluginPath.filename().string()
                      << ":   class[" << i << "] name=\"" << name
                      << "\" category=\"" << cat << "\" (from moduleinfo.json)" << std::endl;
        }

        bool isProcessor = (cat == kVstAudioEffectClass || cat == "Audio Mix Processor");
        if (!isProcessor) continue;

        std::string uid = obj.contains("CID") ? obj["CID"].as_string().c_str() : "";
        std::string classVendor = (obj.contains("Vendor") && !obj["Vendor"].as_string().empty())
            ? std::string(obj["Vendor"].as_string().c_str()) : factoryVendor;
        std::string classVersion = obj.contains("Version")
            ? std::string(obj["Version"].as_string().c_str()) : moduleVersion;

        std::string classCategory;
        if (obj.contains("Sub Categories") && obj["Sub Categories"].is_array()) {
            auto& subs = obj["Sub Categories"].as_array();
            for (size_t j = 0; j < subs.size(); ++j) {
                if (j > 0) classCategory += "|";
                classCategory += subs[j].as_string().c_str();
            }
        }

        // Pack into extraData
        std::string prefix = "vst3_c" + std::to_string(processorCount) + "_";
        result.extraData[prefix + "name"] = name;
        result.extraData[prefix + "uid"] = uid;
        result.extraData[prefix + "category"] = classCategory;
        result.extraData[prefix + "vendor"] = classVendor;
        result.extraData[prefix + "version"] = classVersion;

        // First processor class becomes the main result
        if (!foundClass) {
            foundClass = &obj;
            result.name = name;
            result.uid = uid;
            result.vendor = classVendor.empty() ? factoryVendor : classVendor;
            result.version = classVersion.empty() ? (moduleVersion.empty() ? "1.0.0" : moduleVersion) : classVersion;
            result.url = factoryUrl;
            result.category = classCategory;
        }

        processorCount++;
    }

    if (!foundClass) return false;

    result.extraData["vst3_class_count"] = std::to_string(processorCount);

    progressCb(50, "Extracting metadata from moduleinfo.json...");

    result.format = "vst3";
    result.scanMethod = "moduleinfo.json";

    // I/O counts not available from moduleinfo.json
    result.numInputs = 0;
    result.numOutputs = 0;

    // -----------------------------------------------------------------------
    // Store factory-level email and flags for vstscannermaster XML output
    // -----------------------------------------------------------------------
    result.extraData["vst3_factory_email"] = factoryEmail;
    result.extraData["vst3_factory_flags"] = std::to_string(factoryFlags);

    // -----------------------------------------------------------------------
    // Second pass: capture ALL classes for vstscannermaster XML output
    // -----------------------------------------------------------------------
    // Parse compatibility map first: New CID → list of Old CIDs
    std::map<std::string, std::vector<std::string>> compatMap;
    if (root.contains("Compatibility") && root["Compatibility"].is_array()) {
        for (auto& entry : root["Compatibility"].as_array()) {
            if (!entry.is_object()) continue;
            auto& obj = entry.as_object();
            std::string newUid;
            if (obj.contains("New") && obj["New"].is_string())
                newUid = std::string(obj["New"].as_string());
            if (obj.contains("Old") && obj["Old"].is_array()) {
                for (auto& old : obj["Old"].as_array()) {
                    if (old.is_string())
                        compatMap[newUid].push_back(std::string(old.as_string()));
                }
            }
        }
    }

    for (size_t i = 0; i < classes.size(); ++i) {
        auto& obj = classes[i].as_object();
        std::string prefix = "vst3_all_c" + std::to_string(i) + "_";

        std::string cid = obj.contains("CID") ? obj["CID"].as_string().c_str() : "";
        result.extraData[prefix + "cid"] = cid;
        result.extraData[prefix + "name"] = obj.contains("Name") ? obj["Name"].as_string().c_str() : "";
        result.extraData[prefix + "category"] = obj.contains("Category") ? obj["Category"].as_string().c_str() : "";

        int64_t cardinality = 0x7FFFFFFF; // default kManyInstances
        if (obj.contains("Cardinality") && obj["Cardinality"].is_int64())
            cardinality = obj["Cardinality"].as_int64();
        result.extraData[prefix + "cardinality"] = std::to_string(cardinality);

        uint32_t classFlags = 0;
        if (obj.contains("Class Flags") && obj["Class Flags"].is_int64())
            classFlags = static_cast<uint32_t>(obj["Class Flags"].as_int64());
        result.extraData[prefix + "classFlags"] = std::to_string(classFlags);

        std::string subCats;
        if (obj.contains("Sub Categories") && obj["Sub Categories"].is_array()) {
            auto& subs = obj["Sub Categories"].as_array();
            for (size_t j = 0; j < subs.size(); ++j) {
                if (j > 0) subCats += "|";
                subCats += subs[j].as_string().c_str();
            }
        }
        result.extraData[prefix + "subCategories"] = subCats;

        std::string cv = (obj.contains("Vendor") && !obj["Vendor"].as_string().empty())
            ? std::string(obj["Vendor"].as_string().c_str()) : factoryVendor;
        result.extraData[prefix + "vendor"] = cv;
        result.extraData[prefix + "version"] = obj.contains("Version") ? obj["Version"].as_string().c_str() : "";
        result.extraData[prefix + "sdkVersion"] = obj.contains("SDKVersion") ? obj["SDKVersion"].as_string().c_str() : "";

        // Attach compatibility UIDs if this class's CID matches a compat entry
        auto compatIt = compatMap.find(cid);
        if (compatIt != compatMap.end()) {
            int compatIdx = 0;
            for (auto& oldUid : compatIt->second) {
                std::string cPrefix = prefix + "compat_" + std::to_string(compatIdx) + "_";
                result.extraData[cPrefix + "new"] = cid;
                result.extraData[cPrefix + "old"] = oldUid;
                compatIdx++;
            }
            result.extraData[prefix + "compat_count"] = std::to_string(compatIdx);
        }
    }
    result.extraData["vst3_all_class_count"] = std::to_string(classes.size());

    progressCb(100, "Done (from moduleinfo.json).");
    return true;
}

} // anonymous namespace

bool Vst3Scanner::canHandle(const boost::filesystem::path& pluginPath) const {
    auto ext = pluginPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

    if (numClasses == 0) {
        factory->release();
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        throw std::runtime_error("SKIP: VST3 factory contains no classes.");
    }

    // Extract factory-level info (vendor, URL) as baseline
    logStage("Calling getFactoryInfo()...");
    Steinberg::PFactoryInfo factoryInfo;
    std::string factoryVendor, factoryUrl;
    if (factory->getFactoryInfo(&factoryInfo) == Steinberg::kResultOk) {
        factoryVendor = factoryInfo.vendor;
        factoryUrl = factoryInfo.url;
        logStage("Factory info: vendor=\"" + factoryVendor + "\" url=\"" + factoryUrl + "\"");
    }

    // Query IPluginFactory2 for per-class details (vendor/version/subCategories)
    logStage("Querying IPluginFactory2...");
    Steinberg::IPluginFactory2* factory2 = nullptr;
    bool hasFactory2 = (factory->queryInterface(Steinberg::IPluginFactory2::iid,
        reinterpret_cast<void**>(&factory2)) == Steinberg::kResultOk && factory2);
    if (hasFactory2) {
        logStage("IPluginFactory2 supported.");
    } else {
        logStage("IPluginFactory2 not supported (older plugin).");
    }

    // Enumerate ALL processor classes and pack into extraData.
    // The first processor class becomes the "main" result.
    Steinberg::PClassInfo classInfo;
    int32_t foundClassIndex = -1;
    int processorCount = 0;

    rps::ipc::ScanResult result;
    result.version = "1.0.0";
    result.vendor = factoryVendor.empty() ? "Unknown VST3 Vendor" : factoryVendor;
    result.url = factoryUrl;

    for (int32_t i = 0; i < numClasses; ++i) {
        if (factory->getClassInfo(i, &classInfo) != Steinberg::kResultOk) continue;

        bool isProcessor = (std::strcmp(classInfo.category, kVstAudioEffectClass) == 0 ||
                            std::strcmp(classInfo.category, "Audio Mix Processor") == 0);

        if (g_verbose) {
            std::cerr << "[vst3] " << pluginPath.filename().string()
                      << ":   class[" << i << "] name=\"" << classInfo.name
                      << "\" category=\"" << classInfo.category << "\"" << std::endl;
        }

        if (!isProcessor) continue;

        // Convert 16-byte FUID to hex string
        std::string uidStr = tuidToHex(classInfo.cid);

        std::string classVendor = factoryVendor;
        std::string classVersion;
        std::string classCategory;

        if (hasFactory2) {
            Steinberg::PClassInfo2 ci2;
            if (factory2->getClassInfo2(i, &ci2) == Steinberg::kResultOk) {
                if (ci2.vendor[0] != '\0') classVendor = ci2.vendor;
                if (ci2.version[0] != '\0') classVersion = ci2.version;
                classCategory = ci2.subCategories;
            }
        }

        // Pack into extraData
        std::string prefix = "vst3_c" + std::to_string(processorCount) + "_";
        result.extraData[prefix + "name"] = classInfo.name;
        result.extraData[prefix + "uid"] = uidStr;
        result.extraData[prefix + "category"] = classCategory;
        result.extraData[prefix + "vendor"] = classVendor;
        result.extraData[prefix + "version"] = classVersion;

        // First processor class becomes the main result
        if (foundClassIndex < 0) {
            foundClassIndex = i;
            result.name = classInfo.name;
            result.uid = uidStr;
            if (!classVendor.empty()) result.vendor = classVendor;
            if (!classVersion.empty()) result.version = classVersion;
            result.category = classCategory;
            logStage("Found scannable class at index " + std::to_string(i) + ": \"" + classInfo.name + "\"");
            logStage("UID: " + result.uid);
            if (hasFactory2) {
                logStage("ClassInfo2: vendor=\"" + result.vendor + "\" version=\"" + result.version + "\" subCategories=\"" + result.category + "\"");
            }
        }

        processorCount++;
    }

    result.extraData["vst3_class_count"] = std::to_string(processorCount);
    logStage("Total processor classes: " + std::to_string(processorCount));

    // -----------------------------------------------------------------------
    // Store factory-level email and flags for vstscannermaster XML output
    // -----------------------------------------------------------------------
    result.extraData["vst3_factory_email"] = factoryInfo.email;
    result.extraData["vst3_factory_flags"] = std::to_string(factoryInfo.flags);

    // -----------------------------------------------------------------------
    // Second pass: capture ALL classes (processors + controllers + compat + ARA)
    // for the vstscannermaster XML output using vst3_all_c{N}_ prefix.
    // -----------------------------------------------------------------------
    logStage("Enumerating ALL classes for vstscannermaster...");

    // Query IPluginFactory3 for PClassInfoW (sdkVersion, classFlags, unicode names)
    Steinberg::IPluginFactory3* factory3 = nullptr;
    bool hasFactory3 = (factory->queryInterface(Steinberg::IPluginFactory3::iid,
        reinterpret_cast<void**>(&factory3)) == Steinberg::kResultOk && factory3);
    if (hasFactory3) {
        logStage("IPluginFactory3 supported.");
    }

    for (int32_t i = 0; i < numClasses; ++i) {
        Steinberg::PClassInfo ci;
        if (factory->getClassInfo(i, &ci) != Steinberg::kResultOk) continue;

        std::string prefix = "vst3_all_c" + std::to_string(i) + "_";
        result.extraData[prefix + "cid"] = tuidToHex(ci.cid);
        result.extraData[prefix + "name"] = ci.name;
        result.extraData[prefix + "category"] = ci.category;
        result.extraData[prefix + "cardinality"] = std::to_string(ci.cardinality);

        // Try to get extended info from Factory3 (best), Factory2 (fallback)
        std::string classVendor, classVersion, sdkVersion, subCategories;
        uint32_t classFlags = 0;

        if (hasFactory3) {
            Steinberg::PClassInfoW ciw;
            if (factory3->getClassInfoUnicode(i, &ciw) == Steinberg::kResultOk) {
                classVendor = utf16ToUtf8(ciw.vendor);
                classVersion = utf16ToUtf8(ciw.version);
                sdkVersion = utf16ToUtf8(ciw.sdkVersion);
                subCategories = ciw.subCategories;
                classFlags = ciw.classFlags;
                // Prefer unicode name if available
                std::string uniName = utf16ToUtf8(ciw.name);
                if (!uniName.empty()) result.extraData[prefix + "name"] = uniName;
            }
        } else if (hasFactory2) {
            Steinberg::PClassInfo2 ci2;
            if (factory2->getClassInfo2(i, &ci2) == Steinberg::kResultOk) {
                classVendor = ci2.vendor;
                classVersion = ci2.version;
                sdkVersion = ci2.sdkVersion;
                subCategories = ci2.subCategories;
                classFlags = ci2.classFlags;
            }
        }

        // Fall back to factory vendor if per-class vendor is empty
        if (classVendor.empty()) classVendor = factoryInfo.vendor;
        result.extraData[prefix + "vendor"] = classVendor;
        result.extraData[prefix + "version"] = classVersion;
        result.extraData[prefix + "sdkVersion"] = sdkVersion;
        result.extraData[prefix + "subCategories"] = subCategories;
        result.extraData[prefix + "classFlags"] = std::to_string(classFlags);

        // For "Plugin Compatibility Class" entries, extract compatibility UIDs
        if (std::strcmp(ci.category, "Plugin Compatibility Class") == 0) {
            logStage("Found Plugin Compatibility Class at index " + std::to_string(i)
                     + " CID=" + tuidToHex(ci.cid) + ", querying compat UIDs...");
            Steinberg::IPluginCompatibility* compat = nullptr;
            Steinberg::tresult compatRes = factory->createInstance(
                ci.cid,
                Steinberg::IPluginCompatibility::iid.toTUID(),
                reinterpret_cast<void**>(&compat));
            if (compatRes == Steinberg::kResultOk && compat) {
                MemoryStream stream;
                Steinberg::tresult jsonRes = compat->getCompatibilityJSON(&stream);
                if (jsonRes == Steinberg::kResultTrue) {
                    std::string json = stream.str();
                    logStage("Compat JSON (" + std::to_string(json.size()) + " bytes): " + json);
                    // Strip trailing commas (some plugins emit JSON5-style output)
                    {
                        std::string cleaned;
                        cleaned.reserve(json.size());
                        bool inStr = false;
                        for (size_t charIdx = 0; charIdx < json.size(); ++charIdx) {
                            char ch = json[charIdx];
                            if (ch == '"' && (charIdx == 0 || json[charIdx - 1] != '\\')) inStr = !inStr;
                            if (!inStr && ch == ',') {
                                size_t nj = charIdx + 1;
                                while (nj < json.size() && (json[nj] == ' ' || json[nj] == '\t' ||
                                       json[nj] == '\n' || json[nj] == '\r')) ++nj;
                                if (nj < json.size() && (json[nj] == '}' || json[nj] == ']'))
                                    continue;
                            }
                            cleaned += ch;
                        }
                        json = std::move(cleaned);
                    }
                    // Parse the JSON array to extract Old UIDs
                    // Format: [{"New":"HEXUID","Old":["HEXUID1","HEXUID2"]}]
                    try {
                        auto jv = boost::json::parse(json);
                        if (jv.is_array()) {
                            int compatIdx = 0;
                            for (auto& entry : jv.as_array()) {
                                if (!entry.is_object()) continue;
                                auto& obj = entry.as_object();
                                std::string newUid;
                                if (obj.contains("New") && obj["New"].is_string())
                                    newUid = std::string(obj["New"].as_string());
                                if (obj.contains("Old") && obj["Old"].is_array()) {
                                    for (auto& old : obj["Old"].as_array()) {
                                        if (old.is_string()) {
                                            std::string cPrefix = prefix + "compat_" + std::to_string(compatIdx) + "_";
                                            result.extraData[cPrefix + "new"] = newUid;
                                            result.extraData[cPrefix + "old"] = std::string(old.as_string());
                                            compatIdx++;
                                        }
                                    }
                                }
                            }
                            result.extraData[prefix + "compat_count"] = std::to_string(compatIdx);
                            logStage("Extracted " + std::to_string(compatIdx) + " compat UIDs");
                        } else {
                            logStage("Compat JSON is not an array");
                        }
                    } catch (const std::exception& e) {
                        logStage("Failed to parse compat JSON: " + std::string(e.what()));
                    }
                } else {
                    logStage("getCompatibilityJSON returned " + std::to_string(jsonRes));
                }
                compat->release();
            } else {
                logStage("createInstance for IPluginCompatibility failed: " + std::to_string(compatRes));
            }
        }
    }
    result.extraData["vst3_all_class_count"] = std::to_string(numClasses);
    logStage("Total ALL classes captured: " + std::to_string(numClasses));

    if (hasFactory3) {
        factory3->release();
    }
    if (hasFactory2) {
        factory2->release();
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

    result.format = "vst3";
    result.scanMethod = "factory";
    result.numInputs = 0;
    result.numOutputs = 0;

    // -----------------------------------------------------------------------
    // Instantiate IComponent to get bus info (I/O channels) and IEditController
    // for parameter extraction. This is optional — if it fails we still return
    // the factory-level metadata above.
    // -----------------------------------------------------------------------
    progressCb(70, "Instantiating component...");
    logStage("Creating IComponent instance...");

    Steinberg::Vst::IComponent* component = nullptr;
    Steinberg::tresult createRes = factory->createInstance(
        classInfo.cid,
        Steinberg::Vst::IComponent::iid.toTUID(),
        reinterpret_cast<void**>(&component));

    if (createRes == Steinberg::kResultOk && component) {
        logStage("IComponent created. Calling initialize()...");
        Steinberg::tresult initRes = component->initialize(
            static_cast<Steinberg::FUnknown*>(&g_hostContext));

        if (initRes == Steinberg::kResultOk) {
            logStage("IComponent initialized.");
            result.scanMethod = "component";

            // --- Extract audio bus info (I/O channel counts) ---
            progressCb(75, "Querying audio buses...");
            {
                using namespace Steinberg::Vst;
                Steinberg::int32 numInputBuses = component->getBusCount(kAudio, kInput);
                Steinberg::int32 numOutputBuses = component->getBusCount(kAudio, kOutput);
                logStage("Audio buses: " + std::to_string(numInputBuses) + " input, "
                         + std::to_string(numOutputBuses) + " output.");

                Steinberg::int32 totalInputChannels = 0;
                for (Steinberg::int32 i = 0; i < numInputBuses; ++i) {
                    BusInfo bus{};
                    if (component->getBusInfo(kAudio, kInput, i, bus) == Steinberg::kResultOk) {
                        totalInputChannels += bus.channelCount;
                        if (g_verbose) {
                            std::cerr << "[vst3] " << pluginPath.filename().string()
                                      << ":   Input bus " << i << ": \""
                                      << utf16ToUtf8(bus.name) << "\" ("
                                      << bus.channelCount << " ch, "
                                      << (bus.busType == kMain ? "main" : "aux") << ")"
                                      << std::endl;
                        }
                    }
                }

                Steinberg::int32 totalOutputChannels = 0;
                for (Steinberg::int32 i = 0; i < numOutputBuses; ++i) {
                    BusInfo bus{};
                    if (component->getBusInfo(kAudio, kOutput, i, bus) == Steinberg::kResultOk) {
                        totalOutputChannels += bus.channelCount;
                        if (g_verbose) {
                            std::cerr << "[vst3] " << pluginPath.filename().string()
                                      << ":   Output bus " << i << ": \""
                                      << utf16ToUtf8(bus.name) << "\" ("
                                      << bus.channelCount << " ch, "
                                      << (bus.busType == kMain ? "main" : "aux") << ")"
                                      << std::endl;
                        }
                    }
                }

                result.numInputs = static_cast<uint32_t>(totalInputChannels);
                result.numOutputs = static_cast<uint32_t>(totalOutputChannels);
                logStage("Total audio I/O: " + std::to_string(result.numInputs) + " in, "
                         + std::to_string(result.numOutputs) + " out.");
            }

            // --- Extract parameters via IEditController ---
            progressCb(80, "Querying parameters...");
            logStage("Attempting to get IEditController...");

            Steinberg::Vst::IEditController* controller = nullptr;

            // Strategy 1: Query IEditController directly from the component
            // (single-component design where processor and controller are unified)
            if (component->queryInterface(Steinberg::Vst::IEditController::iid.toTUID(),
                                          reinterpret_cast<void**>(&controller)) == Steinberg::kResultOk
                && controller) {
                logStage("IEditController obtained via queryInterface (single-component).");
            } else {
                // Strategy 2: Get separate controller class ID and create it from factory
                controller = nullptr;
                Steinberg::TUID controllerCid;
                if (component->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
                    logStage("Separate controller class found. Creating instance...");
                    Steinberg::tresult ctrlRes = factory->createInstance(
                        reinterpret_cast<Steinberg::FIDString>(controllerCid),
                        Steinberg::Vst::IEditController::iid.toTUID(),
                        reinterpret_cast<void**>(&controller));
                    if (ctrlRes == Steinberg::kResultOk && controller) {
                        logStage("Separate IEditController created. Calling initialize()...");
                        controller->initialize(
                            static_cast<Steinberg::FUnknown*>(&g_hostContext));
                        logStage("Separate IEditController initialized.");
                    } else {
                        controller = nullptr;
                        logStage("Failed to create separate IEditController.");
                    }
                } else {
                    logStage("No controller class ID available.");
                }
            }

            if (controller) {
                Steinberg::int32 paramCount = controller->getParameterCount();
                logStage("IEditController reports " + std::to_string(paramCount) + " parameter(s).");

                for (Steinberg::int32 i = 0; i < paramCount; ++i) {
                    Steinberg::Vst::ParameterInfo pinfo{};
                    if (controller->getParameterInfo(i, pinfo) == Steinberg::kResultOk) {
                        std::string paramName = utf16ToUtf8(pinfo.title);
                        double defVal = pinfo.defaultNormalizedValue;
                        if (!std::isfinite(defVal)) defVal = 0.0;

                        result.parameters.push_back({
                            static_cast<uint32_t>(pinfo.id),
                            paramName.empty() ? ("Param " + std::to_string(pinfo.id)) : paramName,
                            defVal
                        });
                    }
                }
                logStage("Extracted " + std::to_string(result.parameters.size()) + " parameter(s).");
            } else {
                logStage("No IEditController available — skipping parameter extraction.");
            }
        } else {
            logStage("IComponent::initialize() failed (tresult=" + std::to_string(initRes)
                     + "). Using factory-only metadata.");
        }
    } else {
        logStage("createInstance for IComponent failed (tresult=" + std::to_string(createRes)
                 + "). Using factory-only metadata.");
    }

    progressCb(90, "Metadata extraction complete.");
    logStage("Metadata extraction complete — returning result before cleanup.");

    // IMPORTANT: Skip all cleanup (component->terminate, factory->release, ExitDll,
    // FreeLibrary). Some plugins (e.g. LX480 v4) crash with ACCESS_VIOLATION during
    // cleanup. The scanner process exits immediately after returning — the OS reclaims
    // all resources.

    progressCb(100, "Done.");
    return result;
}

} // namespace rps::scanner
