#include <rps/core/FormatTraits.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#else
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace rps::core {

namespace fs = boost::filesystem;

#if defined(__APPLE__) || defined(__linux__)
// --- Helper for getting home directory ---
static fs::path getHomeDir() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path))) {
        return fs::path(path);
    }
    return fs::path("C:\\");
#else
    const char* home = getenv("HOME");
    if (home) return fs::path(home);
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return fs::path(pw->pw_dir);
    return fs::path("/");
#endif
}
#endif

// --- VST3 Traits ---
class Vst3Traits : public IFormatTraits {
public:
    PluginFormat getFormat() const override { return PluginFormat::VST3; }
    std::string getName() const override { return "vst3"; }
    std::string getExtension() const override { return ".vst3"; }

    std::vector<fs::path> getDefaultPaths() const override {
        std::vector<fs::path> paths;
#if defined(_WIN32)
        paths.push_back("C:\\Program Files\\Common Files\\VST3");
        paths.push_back("C:\\Program Files (x86)\\Common Files\\VST3");
#elif defined(__APPLE__)
        paths.push_back("/Library/Audio/Plug-Ins/VST3");
        paths.push_back(getHomeDir() / "Library/Audio/Plug-Ins/VST3");
#else // Linux
        paths.push_back("/usr/lib/vst3");
        paths.push_back("/usr/local/lib/vst3");
        paths.push_back(getHomeDir() / ".vst3");
#endif
        return paths;
    }

    bool isBundleDirectory() const override {
#if defined(__APPLE__)
        return true; // Always bundles on Mac
#elif defined(_WIN32)
        return false; // Can be bundles on Windows, but the host loads the .vst3 file directly, 
                      // or the bundle contains a specific architecture folder. For simplicity, we usually scan files.
                      // Actually, VST3 on Win/Linux CAN be bundles (folder with .vst3 extension containing Contents/x86_64-win/...)
                      // We'll treat it as a file/bundle hybrid in discovery. For now, false so we recurse inside it to find the actual .vst3 file if needed.
        return true; // Let's treat them as bundles on Win/Linux too, the scanner will handle finding the binary.
#else
        return true; 
#endif
    }

    bool isPluginPath(const fs::path& path) const override {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".vst3";
    }
};

// --- CLAP Traits ---
class ClapTraits : public IFormatTraits {
public:
    PluginFormat getFormat() const override { return PluginFormat::CLAP; }
    std::string getName() const override { return "clap"; }
    std::string getExtension() const override { return ".clap"; }

    std::vector<fs::path> getDefaultPaths() const override {
        std::vector<fs::path> paths;
#if defined(_WIN32)
        paths.push_back("C:\\Program Files\\Common Files\\CLAP");
        paths.push_back("C:\\Program Files (x86)\\Common Files\\CLAP");
#elif defined(__APPLE__)
        paths.push_back("/Library/Audio/Plug-Ins/CLAP");
        paths.push_back(getHomeDir() / "Library/Audio/Plug-Ins/CLAP");
#else
        paths.push_back("/usr/lib/clap");
        paths.push_back("/usr/local/lib/clap");
        paths.push_back(getHomeDir() / ".clap");
#endif
        return paths;
    }

    bool isBundleDirectory() const override {
#if defined(__APPLE__)
        return true;
#else
        return false;
#endif
    }

    bool isPluginPath(const fs::path& path) const override {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".clap" && (isBundleDirectory() ? fs::is_directory(path) : fs::is_regular_file(path));
    }
};

// --- VST2 Traits ---
class Vst2Traits : public IFormatTraits {
public:
    PluginFormat getFormat() const override { return PluginFormat::VST2; }
    std::string getName() const override { return "vst2"; }
    std::string getExtension() const override { 
#if defined(_WIN32)
        return ".dll";
#elif defined(__APPLE__)
        return ".vst";
#else
        return ".so";
#endif
    }

    std::vector<fs::path> getDefaultPaths() const override {
        std::vector<fs::path> paths;
#if defined(_WIN32)
        paths.push_back("C:\\Program Files\\Steinberg\\VstPlugins");
        paths.push_back("C:\\Program Files\\VstPlugins");
#elif defined(__APPLE__)
        paths.push_back("/Library/Audio/Plug-Ins/VST");
        paths.push_back(getHomeDir() / "Library/Audio/Plug-Ins/VST");
#else
        paths.push_back("/usr/lib/vst");
        paths.push_back("/usr/local/lib/vst");
        paths.push_back(getHomeDir() / ".vst");
#endif
        return paths;
    }

    bool isBundleDirectory() const override {
#if defined(__APPLE__)
        return true;
#else
        return false;
#endif
    }

    bool isPluginPath(const fs::path& path) const override {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == getExtension() && (isBundleDirectory() ? fs::is_directory(path) : fs::is_regular_file(path));
    }
};

// --- AU Traits ---
class AuTraits : public IFormatTraits {
public:
    PluginFormat getFormat() const override { return PluginFormat::AU; }
    std::string getName() const override { return "au"; }
    std::string getExtension() const override { return ".component"; }

    std::vector<fs::path> getDefaultPaths() const override {
        std::vector<fs::path> paths;
#if defined(__APPLE__)
        paths.push_back("/Library/Audio/Plug-Ins/Components");
        paths.push_back(getHomeDir() / "Library/Audio/Plug-Ins/Components");
#endif
        return paths;
    }

    bool isBundleDirectory() const override { return true; }

    bool isPluginPath(const fs::path& path) const override {
#if defined(__APPLE__)
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".component" && fs::is_directory(path);
#else
        (void)path;
        return false; // AU only on Mac
#endif
    }
};

// --- AAX Traits ---
class AaxTraits : public IFormatTraits {
public:
    PluginFormat getFormat() const override { return PluginFormat::AAX; }
    std::string getName() const override { return "aax"; }
    std::string getExtension() const override { return ".aaxplugin"; }

    std::vector<fs::path> getDefaultPaths() const override {
        std::vector<fs::path> paths;
#if defined(_WIN32)
        paths.push_back("C:\\Program Files\\Common Files\\Avid\\Audio\\Plug-Ins");
#elif defined(__APPLE__)
        paths.push_back("/Library/Application Support/Avid/Audio/Plug-Ins");
#endif
        return paths;
    }

    bool isBundleDirectory() const override { return true; }

    bool isPluginPath(const fs::path& path) const override {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".aaxplugin" && fs::is_directory(path);
    }
};

// --- LV2 Traits ---
class Lv2Traits : public IFormatTraits {
public:
    PluginFormat getFormat() const override { return PluginFormat::LV2; }
    std::string getName() const override { return "lv2"; }
    std::string getExtension() const override { return ".lv2"; }

    std::vector<fs::path> getDefaultPaths() const override {
        std::vector<fs::path> paths;
#if defined(__linux__) || defined(__APPLE__)
        paths.push_back("/usr/lib/lv2");
        paths.push_back("/usr/local/lib/lv2");
        paths.push_back(getHomeDir() / ".lv2");
#endif
        return paths;
    }

    bool isBundleDirectory() const override { return true; } // LV2 is always a bundle folder

    bool isPluginPath(const fs::path& path) const override {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".lv2" && fs::is_directory(path);
    }
};

// --- Format Registry Implementation ---

FormatRegistry::FormatRegistry() {
    m_traits.push_back(std::make_unique<Vst3Traits>());
    m_traits.push_back(std::make_unique<ClapTraits>());
    m_traits.push_back(std::make_unique<Vst2Traits>());
    m_traits.push_back(std::make_unique<AuTraits>());
    m_traits.push_back(std::make_unique<AaxTraits>());
    m_traits.push_back(std::make_unique<Lv2Traits>());
}

const std::vector<std::unique_ptr<IFormatTraits>>& FormatRegistry::getAllTraits() const {
    return m_traits;
}

const IFormatTraits* FormatRegistry::getTraits(PluginFormat format) const {
    for (const auto& t : m_traits) {
        if (t->getFormat() == format) {
            return t.get();
        }
    }
    return nullptr;
}

const IFormatTraits* FormatRegistry::getTraits(const std::string& formatName) const {
    std::string lowerName = formatName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    for (const auto& t : m_traits) {
        if (t->getName() == lowerName) {
            return t.get();
        }
    }
    return nullptr;
}

std::vector<const IFormatTraits*> FormatRegistry::parseFormats(const std::string& formatList) const {
    std::vector<const IFormatTraits*> result;
    if (formatList.empty() || formatList == "all") {
        for (const auto& t : m_traits) {
            result.push_back(t.get());
        }
        return result;
    }

    std::stringstream ss(formatList);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), item.end());

        if (const auto* traits = getTraits(item)) {
            result.push_back(traits);
        } else {
            // Unrecognized format
            std::cerr << "Warning: Unrecognized plugin format '" << item << "' ignored.\n";
        }
    }
    return result;
}

} // namespace rps::core
