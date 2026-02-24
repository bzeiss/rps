#include <rps/core/PluginDiscovery.hpp>
#include <iostream>
#include <algorithm>

namespace rps::core {

namespace fs = boost::filesystem;

bool PluginDiscovery::isPluginFile(const fs::path& path, const std::vector<const IFormatTraits*>& formats) {
    for (const auto* traits : formats) {
        if (traits->isPluginPath(path)) {
            return true;
        }
    }
    return false;
}

bool PluginDiscovery::isBundleDirectory(const fs::path& path, const std::vector<const IFormatTraits*>& formats) {
    for (const auto* traits : formats) {
        if (traits->isPluginPath(path) && traits->isBundleDirectory()) {
            return true;
        }
    }
    return false;
}

std::vector<fs::path> PluginDiscovery::findPlugins(
    const std::vector<std::string>& directories,
    const std::vector<const IFormatTraits*>& formats) {
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
                
                if (isPluginFile(entryPath, formats)) {
                    foundPlugins.push_back(entryPath);
                    // Do not recurse into the plugin bundle itself
                    if (isBundleDirectory(entryPath, formats)) {
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
