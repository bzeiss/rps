#pragma once

#include <string>
#include <vector>
#include <boost/filesystem.hpp>

namespace rps::core {

class PluginDiscovery {
public:
    static std::vector<boost::filesystem::path> findPlugins(const std::vector<std::string>& directories);

private:
    static bool isPluginFile(const boost::filesystem::path& path);
};

} // namespace rps::core
