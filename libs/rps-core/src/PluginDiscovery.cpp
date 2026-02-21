#include <rps/core/PluginDiscovery.hpp>
#include <iostream>
#include <algorithm>

namespace rps::core {

namespace fs = boost::filesystem;

bool PluginDiscovery::isPluginFile(const fs::path& path) {
    if (!fs::is_regular_file(path) && !fs::is_directory(path)) {
        return false;
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // VST3 are folders on Mac, but often .vst3 files on Windows/Linux
    // CLAP are .clap files
    // VST2 are .dll on Windows, .vst on Mac, .so on Linux
    // AU are .component folders on Mac
    // AAX are .aaxplugin folders/files

    return ext == ".vst3" || ext == ".clap" || ext == ".vst" || 
           ext == ".dll" || ext == ".so" || ext == ".component" || 
           ext == ".aaxplugin";
}

std::vector<fs::path> PluginDiscovery::findPlugins(const std::vector<std::string>& directories) {
    std::vector<fs::path> foundPlugins;

    for (const auto& dirStr : directories) {
        fs::path dirPath(dirStr);
        if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
            std::cerr << "Warning: Directory does not exist or is not a directory: " << dirStr << "\n";
            continue;
        }

        try {
            // macOS plugin bundles (e.g. .vst3, .component, .aaxplugin) are directories.
            // We need to carefully traverse to avoid going inside a plugin bundle 
            // and finding its internal binary, since the host treats the bundle root as the plugin.
            for (auto it = fs::recursive_directory_iterator(dirPath); it != fs::recursive_directory_iterator(); ++it) {
                const fs::path& entryPath = it->path();
                
                if (isPluginFile(entryPath)) {
                    foundPlugins.push_back(entryPath);
                    // Do not recurse into the plugin bundle itself
                    if (fs::is_directory(entryPath)) {
                        it.disable_recursion_pending();
                    }
                }
            }
        } catch (const fs::filesystem_error& ex) {
            std::cerr << "Error traversing directory " << dirStr << ": " << ex.what() << "\n";
        }
    }

    return foundPlugins;
}

} // namespace rps::core
