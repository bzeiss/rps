#include <rps/core/DefaultPaths.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace rps::core {

namespace fs = boost::filesystem;

std::vector<fs::path> DefaultPaths::getPluginDirectories() {
    std::vector<fs::path> paths;

#if defined(_WIN32)
    
    // Helper lambda to get Known Folders on Windows
    auto getKnownFolder = [](int csidl) -> fs::path {
        char path[MAX_PATH];
        if (SHGetFolderPathA(NULL, csidl, NULL, 0, path) == S_OK) {
            return fs::path(path);
        }
        return fs::path();
    };

    fs::path programFiles = getKnownFolder(CSIDL_PROGRAM_FILES);
    fs::path commonFiles = getKnownFolder(CSIDL_PROGRAM_FILES_COMMON);
    fs::path programFilesX86 = getKnownFolder(CSIDL_PROGRAM_FILESX86);
    fs::path commonFilesX86 = getKnownFolder(CSIDL_PROGRAM_FILES_COMMONX86);

    // VST3
    paths.push_back(commonFiles / "VST3");
    paths.push_back(commonFilesX86 / "VST3");

    // CLAP
    paths.push_back(commonFiles / "CLAP");
    paths.push_back(commonFilesX86 / "CLAP");

    // AAX
    paths.push_back(commonFiles / "Avid" / "Audio" / "Plug-Ins");

    // VST2 (Common guess paths since there is no strict standard)
    paths.push_back(programFiles / "Steinberg" / "VstPlugins");
    paths.push_back(programFilesX86 / "Steinberg" / "VstPlugins");
    paths.push_back(programFiles / "VstPlugins");
    paths.push_back(programFilesX86 / "VstPlugins");
    paths.push_back(commonFiles / "VST2");

#elif defined(__APPLE__)

    // VST3
    paths.push_back("/Library/Audio/Plug-Ins/VST3");
    paths.push_back(fs::path(std::getenv("HOME")) / "Library/Audio/Plug-Ins/VST3");

    // CLAP
    paths.push_back("/Library/Audio/Plug-Ins/CLAP");
    paths.push_back(fs::path(std::getenv("HOME")) / "Library/Audio/Plug-Ins/CLAP");

    // AU (Audio Units)
    paths.push_back("/Library/Audio/Plug-Ins/Components");
    paths.push_back(fs::path(std::getenv("HOME")) / "Library/Audio/Plug-Ins/Components");

    // AAX
    paths.push_back("/Library/Application Support/Avid/Audio/Plug-Ins");

    // VST2
    paths.push_back("/Library/Audio/Plug-Ins/VST");
    paths.push_back(fs::path(std::getenv("HOME")) / "Library/Audio/Plug-Ins/VST");

#elif defined(__linux__)

    // Helper to get HOME dir safely
    const char* homeEnv = std::getenv("HOME");
    fs::path home = homeEnv ? fs::path(homeEnv) : fs::path();

    // VST3
    paths.push_back("/usr/lib/vst3");
    paths.push_back("/usr/local/lib/vst3");
    if (!home.empty()) {
        paths.push_back(home / ".vst3");
    }

    // CLAP
    paths.push_back("/usr/lib/clap");
    paths.push_back("/usr/local/lib/clap");
    if (!home.empty()) {
        paths.push_back(home / ".clap");
    }

    // LV2
    paths.push_back("/usr/lib/lv2");
    paths.push_back("/usr/local/lib/lv2");
    if (!home.empty()) {
        paths.push_back(home / ".lv2");
    }

    // VST2
    paths.push_back("/usr/lib/vst");
    paths.push_back("/usr/local/lib/vst");
    if (!home.empty()) {
        paths.push_back(home / ".vst");
    }

#endif

    // Filter out paths that don't actually exist on the system to keep it clean
    std::vector<fs::path> existingPaths;
    for (const auto& p : paths) {
        if (!p.empty() && fs::exists(p) && fs::is_directory(p)) {
            existingPaths.push_back(p);
        }
    }

    return existingPaths;
}

} // namespace rps::core
